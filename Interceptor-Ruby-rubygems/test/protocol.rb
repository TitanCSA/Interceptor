require 'interceptor'
require 'fiddle'
sdk=Interceptor
fixture=ARGV.fetch(0)
mode=Fiddle::Function.new(Fiddle::Handle.new(fixture)['interceptor_test_mode'],[Fiddle::TYPE_INT],Fiddle::TYPE_VOID)
def check(value);raise 'Product Ruby SDK contract failed' unless value;end
kinds=["keyboard", "mouse", "hid", "xinput", "xinputhid", "gip"]
sdk::Interceptor.open(library_path: fixture) do |client|
  client.initialize_devices
  check(client.info[:input_mask]==3 && client.info[:controller_mask]==15)
  check(client.devices.map{|d|d[:kind]}.sort==kinds.sort)
  batch=client.receive(timeout: 0);check(batch.map(&:kind).sort==kinds.sort && batch.all?{|e|e.sequence>(1<<63)})
  batch.each do |event|
    event.field('scan_code').value=0x30 if event.kind=='keyboard'
    event.field('y').value=-7 if event.kind=='mouse'
  end
  check(client.send(batch)==kinds.size);check(client.send(batch)==0)
  current=client.receive(timeout: 0)
  begin;client.send(batch);raise 'Stale batch accepted';rescue ArgumentError;end
  current[0].blocked=true;check(client.send(current)==kinds.size-1)
  retry_batch=client.receive(timeout: 0);mode.call(2)
  begin;client.send(retry_batch);raise 'Partial send missing';rescue sdk::NativeError=>e;check(e.code==170 && e.accepted==1);end
  check(client.send(retry_batch)==kinds.size-1);mode.call(0)
  client.discard(client.receive(timeout: 0))
  (sdk::KINDS.drop(1)-kinds).each do |kind|
    begin;client.inject(kind);raise 'Unsupported family accepted';rescue sdk::NativeError=>e;check(e.code==50);end
  end
  mode.call(1);reader=Thread.new{check(client.receive(timeout: 2000).size.zero?)}
  sleep 0.03;client.cancel;reader.value;mode.call(0)
end
sdk::Interceptor.open { |_| }
puts 'Interceptor Ruby SDK: product masks, edits, blocking, partial sends, stale batches, uint64, cancellation and production DLL passed'
