# frozen_string_literal: true
require 'fiddle'
require 'thread'

module InterceptorSDK
  VERSION = '2.0.0'
  KINDS = ['', 'keyboard', 'mouse', 'hid', 'xinput', 'xinputhid', 'gip'].freeze
  class NativeError < StandardError
    attr_reader :code, :accepted
    def initialize(code, message, accepted = 0)
      @code, @accepted = code, accepted
      super(message)
    end
  end
  Field = Struct.new(:value, :normalized, :data, :metadata, keyword_init: true)
  Input = Struct.new(:kind, :token, :sequence, :timestamp, :fields, :metadata, :blocked, keyword_init: true) do
    def field(name)
      found = fields.select { |f| f.metadata[:name] == name }
      raise ArgumentError, 'Field is absent or ambiguous; use descriptor metadata' unless found.length == 1
      found.first
    end
  end
  class Batch
    include Enumerable
    attr_reader :events
    def initialize(events) = @events = events.freeze
    def each(&block) = @events.each(&block)
    def [](index) = @events[index]
    def size = @events.size
    alias length size
  end

  # All pointer/layout operations stay private; callers edit copied field values.
  class Native
    I = Fiddle::TYPE_INT
    Q = Fiddle::TYPE_LONG_LONG
    P = Fiddle::TYPE_VOIDP
    SPECS = {
      version: [], sizeof: [I], open: [P,P], close: [Q,P], initialize: [Q,P,P],
      info_get: [Q,P,P], devices_get: [Q,P,P], receive: [Q,I,P,P], send: [Q,Q,P],
      discard: [Q,Q,P], cancel: [Q,P], descriptor: [Q,P,I,P,P], inject: [Q,P,I,P]
    }.freeze
    @libraries = {}
    @libraries_mutex = Mutex.new
    def self.load(path)
      @libraries_mutex.synchronize { @libraries[path] ||= new(path) }
    end
    def initialize(path)
      @library = Fiddle.dlopen(path)
      @functions = SPECS.to_h { |name, args| [name, Fiddle::Function.new(@library["interceptor_#{name}"], args, I)] }
    end
    def call(name, *arguments) = @functions.fetch(name).call(*arguments)
    def self.buffer(length)
      pointer = Fiddle::Pointer.malloc([length,1].max, Fiddle::RUBY_FREE)
      pointer[0,[length,1].max] = "\0" * [length,1].max
      pointer
    end
    def self.u32(p, off) = p[off,4].unpack1('L<')
    def self.u64(p, off) = p[off,8].unpack1('Q<')
    def self.i64(p, off) = p[off,8].unpack1('q<')
    def self.pointer(p, off) = Fiddle::Pointer.new(u64(p,off))
    def self.text(p) = p.null? ? '' : p.to_s.force_encoding(Encoding::UTF_8)
    def self.signed64(value) = value >= (1 << 63) ? value - (1 << 64) : value
  end

  class Interceptor
    def initialize(library_path: nil)
      raise LoadError, 'Interceptor requires Windows x64 Ruby' unless /mswin|mingw/ =~ RUBY_PLATFORM && Fiddle::SIZEOF_VOIDP == 8
      path = library_path || ENV['INTERCEPTOR_SDK_NATIVE_PATH'] || File.expand_path('../native/InterceptorBridge.dll', __dir__)
      @native = Native.load(File.expand_path(path)); @mutex = Mutex.new; @session = 0; @current = nil
      raise LoadError, 'Incompatible native ABI' unless @native.call(:version) == 1 && [120,64,24,40,776,56].each_with_index.all? { |size,index| @native.call(:sizeof,index+1) == size }
      out = Native.buffer(8); checked(:open,out); @session = Native.u64(out,0)
    end
    def checked(name, *arguments)
      error = Native.buffer(776)
      code = @native.call(name, *arguments, error)
      raise NativeError.new(code, error[8,768].split("\0",2).first.force_encoding('UTF-8'), Native.u32(error,4)) unless code.zero?
      Native.u32(error,4)
    end
    private :checked
    def active
      raise IOError, 'Interceptor is closed' if @session.zero?
      Native.signed64(@session)
    end
    private :active
    def initialize_devices
      @mutex.synchronize do
        out = Native.buffer(24); checked(:initialize,active,out)
        %i[ready reboot_required associated already_associated skipped].each_with_index.to_h { |name,index| [name,Native.u32(out,index*4)] }
      end
    end
    def info
      @mutex.synchronize do
        out = Native.buffer(56); checked(:info_get,active,out)
        result = %i[installed active trial_state input_mask controller_mask device_count queued last_reset].each_with_index.to_h { |name,index| [name,Native.u32(out,index*4)] }
        %i[remaining_ms timestamp_frequency generation].each_with_index { |name,index| result[name] = Native.u64(out,32+index*8) }
        result
      end
    end
    def devices
      @mutex.synchronize do
        out = Native.buffer(8); checked(:devices_get,active,out); list = Native.pointer(out,0); items = Native.pointer(list,8)
        Array.new(Native.u32(list,0)) do |i|
          d = items + i*40
          result = { kind: KINDS[Native.u32(d,0)], id: Native.u32(d,4) }
          %i[vid pid usage_page usage].each_with_index { |name,index| result[name] = d[8+index*2,2].unpack1('S<') }
          %i[token name hardware_id].each_with_index { |name,index| result[name] = Native.text(Native.pointer(d,16+index*8)) }
          result
        end
      end
    end
    def receive(timeout: nil)
      raise ArgumentError, 'Invalid timeout' unless timeout.nil? || timeout.is_a?(Integer) && timeout.between?(0,0xfffffffe)
      @mutex.synchronize do
        @current = nil; @header = nil
        out = Native.buffer(8); checked(:receive,active,timeout.nil? ? -1 : (timeout >= (1 << 31) ? timeout - (1 << 32) : timeout),out)
        @header = Native.pointer(out,0); @ticket = Native.u64(@header,0); @views = Native.pointer(@header,16)
        events = Array.new(Native.u32(@header,8)) do |i|
          e = @views + i*64; fs = Native.pointer(e,32)
          fields = Array.new(Native.u32(e,40)) do |j|
            f = fs + j*120
            m = {name: Native.text(Native.pointer(f,0)), minimum: Native.i64(f,16), maximum: Native.i64(f,24), physical_minimum: Native.i64(f,32), physical_maximum: Native.i64(f,40)}
            %i[group index usage_page usage collection report_id data_index element flags units units_exponent bit_size].each_with_index { |name,index| m[name] = Native.u32(f,56+index*4) }
            bytes = Native.u32(f,112)
            Field.new(value: Native.i64(f,8), normalized: f[48,8].unpack1('E'), data: bytes.zero? ? ''.b : Native.pointer(f,104)[0,bytes], metadata: m.freeze)
          end.freeze
          Input.new(kind: KINDS[Native.u32(e,0)], token: Native.text(Native.pointer(e,24)), sequence: Native.u64(e,8), timestamp: Native.u64(e,16), fields: fields,
            metadata: %i[decode_error decode_complete parser_status device].each_with_index.to_h { |name,index| [name,Native.u32(e,44+index*4)] }.freeze, blocked: Native.u32(e,4) != 0)
        end
        @issued_fields = events.map(&:fields); @current = Batch.new(events)
      end
    end
    def validate(batch)
      active
      raise ArgumentError, 'Stale or foreign batch' unless batch.equal?(@current) && @header && Native.u64(@header,0) == @ticket && batch.size == Native.u32(@header,8)
      batch.each_with_index { |event,index| raise ArgumentError, 'Do not replace/reorder fields' unless event.fields.equal?(@issued_fields[index]) }
    end
    private :validate
    def send(batch)
      @mutex.synchronize do
        validate(batch)
        batch.each_with_index do |event,i|
          fs = Native.pointer(@views+i*64,32)
          event.fields.each_with_index do |field,j|
            raise ArgumentError, 'Field value must fit signed 64 bits' unless field.value.is_a?(Integer) && field.value.between?(-(1<<63),(1<<63)-1)
            raise ArgumentError, 'Normalized values must be finite' unless field.normalized.is_a?(Numeric) && field.normalized.finite?
            raise ArgumentError, 'Do not resize binary/report data' unless field.data.is_a?(String) && field.data.bytesize == Native.u32(fs+j*120,112)
          end
        end
        batch.each_with_index do |event,i|
          e = @views+i*64; e[4,4] = [event.blocked ? 1 : 0].pack('L<'); fs = Native.pointer(e,32)
          event.fields.each_with_index do |field,j|
            f=fs+j*120;f[8,8]=[field.value].pack('q<');f[48,8]=[field.normalized].pack('E')
            Native.pointer(f,104)[0,field.data.bytesize]=field.data unless field.data.empty?
          end
        end
        checked(:send,active,Native.signed64(@ticket))
      end
    end
    def discard(batch) = @mutex.synchronize { validate(batch); checked(:discard,active,Native.signed64(@ticket)); nil }
    def cancel = (checked(:cancel,active); nil)
    def descriptor(token, report: false)
      raise ArgumentError, 'Token contains NUL' if token.include?("\0")
      @mutex.synchronize do
        out=Native.buffer(24);checked(:descriptor,active,token+"\0",report ? 1 : 0,out);length=Native.u32(out,8)
        { applicable: Native.u32(out,0)!=0, format: Native.u32(out,4), data: length.zero? ? ''.b : Native.pointer(out,16)[0,length] }
      end
    end
    def inject(kind, state = {}, token: nil, report: nil)
      k=KINDS.index(kind);raise ArgumentError, 'Unknown input kind' unless k && k>0
      @mutex.synchronize do
        count=state.size+(report.nil? ? 0 : 1);fields=Native.buffer(count*120);strings=[]
        state.each_with_index do |(name,value),i|
          raise ArgumentError, 'Invalid state name or value' unless name.is_a?(String) && !name.include?("\0") && value.is_a?(Integer) && value.between?(-(1<<63),(1<<63)-1)
          name_pointer=Fiddle::Pointer[name+"\0"];strings << name_pointer;f=fields+i*120;f[0,8]=[name_pointer.to_i].pack('Q<');f[8,8]=[value].pack('q<')
        end
        unless report.nil?
          raise ArgumentError, 'Report must be a byte string' unless report.is_a?(String)
          name=Fiddle::Pointer["report\0"];data=Fiddle::Pointer[report];strings.push(name,data);f=fields+state.size*120;f[0,8]=[name.to_i].pack('Q<');f[56,4]=[7].pack('L<');f[104,8]=[data.to_i].pack('Q<');f[112,4]=[report.bytesize].pack('L<')
        end
        event=Native.buffer(64);event[0,4]=[k].pack('L<');event[32,8]=[fields.to_i].pack('Q<');event[40,4]=[count].pack('L<')
        if token
          raise ArgumentError, 'Token contains NUL' if token.include?("\0")
          t=Fiddle::Pointer[token+"\0"];strings << t;event[24,8]=[t.to_i].pack('Q<')
        end
        checked(:inject,active,event,1)
      end
    end
    def close
      return if @session.zero?
      begin; cancel; rescue NativeError; end
      @mutex.synchronize do
        unless @session.zero?
          id=@session;@session=0;@current=nil;@header=nil;checked(:close,Native.signed64(id))
        end
      end
      nil
    end
    def self.open(**options)
      io=new(**options)
      return io unless block_given?
      begin; yield io; ensure; io.close; end
    end
  end
end
