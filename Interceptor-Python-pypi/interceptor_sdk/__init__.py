"""Interceptor: receive, edit state, send. Windows x64, Python 3.10+."""
import ctypes as C
import os
import threading
from functools import wraps
from pathlib import Path
from types import SimpleNamespace

class Error(RuntimeError):
    def __init__(self, code, message, accepted=0):
        super().__init__(message); self.code=code; self.accepted=accepted

class _Error(C.Structure):
    _fields_=[('code',C.c_uint32),('accepted',C.c_uint32),('message',C.c_char*768)]
class _Field(C.Structure):
    _fields_=[('name',C.c_char_p)]+[(n,C.c_int64) for n in ('value','minimum','maximum','physical_minimum','physical_maximum')]+[('normalized',C.c_double)]+[(n,C.c_uint32) for n in ('group','index','usage_page','usage','collection','report_id','data_index','element','flags','units','units_exponent','bit_size')]+[('data',C.POINTER(C.c_ubyte)),('length',C.c_uint32),('reserved',C.c_uint32)]
class _Event(C.Structure):
    _fields_=[('kind',C.c_uint32),('blocked',C.c_uint32),('sequence',C.c_uint64),('timestamp',C.c_uint64),('token',C.c_char_p),('fields',C.POINTER(_Field))]+[(n,C.c_uint32) for n in ('field_count','decode_error','decode_complete','parser_status','device','reserved')]
class _Batch(C.Structure):
    _fields_=[('id',C.c_uint64),('count',C.c_uint32),('reserved',C.c_uint32),('events',C.POINTER(_Event))]
class _Device(C.Structure):
    _fields_=[('kind',C.c_uint32),('id',C.c_uint32)]+[(n,C.c_uint16) for n in ('vid','pid','usage_page','usage')]+[(n,C.c_char_p) for n in ('token','name','hardware_id')]
class _Devices(C.Structure):
    _fields_=[('count',C.c_uint32),('reserved',C.c_uint32),('items',C.POINTER(_Device))]
class _Blob(C.Structure):
    _fields_=[(n,C.c_uint32) for n in ('applicable','format','length','reserved')]+[('data',C.POINTER(C.c_ubyte))]
class _Init(C.Structure):
    _fields_=[(n,C.c_uint32) for n in ('ready','reboot_required','associated','already_associated','skipped','reserved')]
class _Info(C.Structure):
    _fields_=[(n,C.c_uint32) for n in ('installed','active','trial_state','input_mask','controller_mask','device_count','queued','last_reset')]+[(n,C.c_uint64) for n in ('remaining_ms','timestamp_frequency','generation')]
KINDS=('','keyboard','mouse','hid','xinput','xinputhid','gip')
GROUPS={1:'axes',2:'triggers',3:'hats',4:'values',5:'buttons',6:'binary_values'}
def _integer(value):
    if not isinstance(value,int) or not -(1<<63)<=value<(1<<63):raise ValueError("Input value must be an int64 integer")
    return value
def _text(p):return p.decode('utf-8') if p else ''
def _snapshot(value):return SimpleNamespace(**{n:(bool(getattr(value,n)) if n in ('ready','reboot_required','installed','active') else getattr(value,n)) for n,_ in value._fields_ if n!='reserved'})
class State(dict):
    def __getattr__(self,name):
        try:return self[name]
        except KeyError:raise AttributeError(name) from None
    def __setattr__(self,name,value):
        if name not in self:raise AttributeError(name)
        self[name]=value
class Input:
    def __init__(self,event):
        self._kind=KINDS[event.kind];self._token=_text(event.token);self._sequence=event.sequence;self._timestamp=event.timestamp
        self.blocked=bool(event.blocked);self.decode_error=event.decode_error;self.decode_complete=bool(event.decode_complete);self.parser_status=event.parser_status
        self.state=State();self.report=bytearray();self._edits=[]
        for i in range(event.field_count):
            field=event.fields[i]
            if field.group==0:self.state[_text(field.name)]=field.value;self._edits.append((field,_text(field.name),None))
            elif field.group==7:self.report=bytearray(C.string_at(field.data,field.length));self._edits.append((field,'report',None))
            else:
                data=bytearray(C.string_at(field.data,field.length)) if field.length else bytearray()
                item=SimpleNamespace(**{n:getattr(field,n) for n,_ in _Field._fields_ if n not in ('name','data','reserved')},data=data,normalized_valid=bool(field.flags&1),pressed=bool(field.value))
                self.state.setdefault(GROUPS[field.group],[]).append(item);self._edits.append((field,None,item))
    kind=property(lambda self:self._kind);token=property(lambda self:self._token);sequence=property(lambda self:self._sequence);timestamp=property(lambda self:self._timestamp)
    def _commit(self,event):
        event.blocked=bool(self.blocked)
        if self.blocked:return
        for field,name,item in self._edits:
            if name=='report':data=self.report
            elif name is not None:field.value=_integer(self.state[name]);continue
            else:
                field.value=bool(item.pressed) if field.group==5 else _integer(item.value)
                field.normalized=item.normalized;data=item.data
            if len(data)!=field.length:raise ValueError('Input data length is immutable')
            if data:C.memmove(field.data,bytes(data),len(data))
class Batch(tuple):
    def __new__(cls,owner,native):
        return super().__new__(cls,(Input(native.events[n]) for n in range(native.count)))
    def __init__(self,owner,native):
        self._owner=owner;self._ticket=native.id;self._native=native
def _serialized(method):
    @wraps(method)
    def run(self,*args,**kwargs):
        if not self._lock.acquire(blocking=False):raise Error(170,"Interceptor is busy in another operation")
        try:return method(self,*args,**kwargs)
        finally:self._lock.release()
    return run

class Interceptor:
    def __init__(self,library=None):
        self._lock=threading.Lock()
        if os.name!='nt' or C.sizeof(C.c_void_p)!=8:raise OSError('Interceptor requires a 64-bit Windows process')
        root=Path(__file__).resolve();candidates=[root.parent/'InterceptorBridge.dll',root.parents[3]/'runtimes/win-x64/InterceptorBridge.dll']
        chosen=library or os.environ.get('INTERCEPTOR_SDK_NATIVE_PATH') or next((p for p in candidates if p.is_file()),None)
        if chosen is None:raise OSError('InterceptorBridge.dll not found. Download the complete Interceptor release or build it first.')
        self._dll=C.CDLL(str(Path(chosen).resolve()));self._handle=C.c_uint64();self._ticket=0
        signatures={'open':[C.POINTER(C.c_uint64)],'close':[C.c_uint64],'cancel':[C.c_uint64],'initialize':[C.c_uint64,C.POINTER(_Init)],'info_get':[C.c_uint64,C.POINTER(_Info)],'devices_get':[C.c_uint64,C.POINTER(C.POINTER(_Devices))],'receive':[C.c_uint64,C.c_uint32,C.POINTER(C.POINTER(_Batch))],'send':[C.c_uint64,C.c_uint64],'discard':[C.c_uint64,C.c_uint64],'descriptor':[C.c_uint64,C.c_char_p,C.c_uint32,C.POINTER(_Blob)],'inject':[C.c_uint64,C.POINTER(_Event),C.c_uint32]}
        for name,args in signatures.items():f=getattr(self._dll,'interceptor_'+name);f.argtypes=args+[C.POINTER(_Error)];f.restype=C.c_uint32
        self._dll.interceptor_version.restype=C.c_uint32;self._dll.interceptor_sizeof.argtypes=[C.c_uint32];self._dll.interceptor_sizeof.restype=C.c_uint32
        if self._dll.interceptor_version()!=1 or any(self._dll.interceptor_sizeof(i)!=C.sizeof(t) for i,t in enumerate((_Field,_Event,_Batch,_Device,_Error,_Info),1)):raise OSError('Incompatible Interceptor bridge ABI')
        self._call('open',C.byref(self._handle))
    def _call(self,name,*args):
        error=_Error();code=getattr(self._dll,'interceptor_'+name)(*args,C.byref(error))
        if code:raise Error(code,_text(error.message),error.accepted)
        return error.accepted
    def _active(self):
        if not self._handle.value:raise Error(6,'Interceptor is closed')
        return self._handle.value
    @_serialized
    def initialize(self):
        result=_Init();self._call('initialize',self._active(),C.byref(result));return _snapshot(result)
    @_serialized
    def info(self):
        result=_Info();self._call('info_get',self._active(),C.byref(result));return _snapshot(result)
    @_serialized
    def devices(self):
        p=C.POINTER(_Devices)();self._call('devices_get',self._active(),C.byref(p))
        return [SimpleNamespace(kind=KINDS[d.kind],id=d.id,vid=d.vid,pid=d.pid,usage_page=d.usage_page,usage=d.usage,token=_text(d.token),name=_text(d.name),hardware_id=_text(d.hardware_id)) for d in (p.contents.items[i] for i in range(p.contents.count))]
    @_serialized
    def receive(self,timeout=None):
        if timeout is not None and (not isinstance(timeout,int) or not 0<=timeout<=0xfffffffe):raise ValueError('Timeout must be milliseconds >= 0, or None for infinite')
        p=C.POINTER(_Batch)();self._call('receive',self._active(),0xffffffff if timeout is None else timeout,C.byref(p));self._ticket=p.contents.id;return Batch(self,p.contents)
    @_serialized
    def send(self,inputs):
        if not isinstance(inputs,Batch) or inputs._owner is not self or inputs._ticket!=self._ticket or inputs._native.id!=inputs._ticket:raise ValueError('Send expects the current batch returned by this Interceptor')
        self._active()
        if len(inputs)!=inputs._native.count:raise ValueError('Do not resize a batch; set input.blocked instead')
        for i,event in enumerate(inputs):event._commit(inputs._native.events[i])
        return self._call('send',self._handle,inputs._ticket)
    @_serialized
    def discard(self,inputs):
        if not isinstance(inputs,Batch) or inputs._owner is not self or inputs._ticket!=self._ticket or inputs._native.id!=inputs._ticket:raise ValueError('Stale or foreign batch')
        self._call('discard',self._active(),inputs._ticket)
    def cancel(self):self._call('cancel',self._active())
    @_serialized
    def descriptor(self,token,report=False):
        b=_Blob();self._call('descriptor',self._active(),token.encode(),bool(report),C.byref(b));return SimpleNamespace(applicable=bool(b.applicable),format=b.format,data=bytes(C.string_at(b.data,b.length)))
    @_serialized
    def inject(self,kind,token=None,report=None,**state):
        if kind not in KINDS[1:]:raise ValueError('Unknown input kind')
        fields=(_Field*(len(state)+(report is not None)))();buffers=[]
        for i,(name,value) in enumerate(state.items()):fields[i].name=name.encode();fields[i].value=_integer(value)
        if report is not None:
            data=(C.c_ubyte*len(report)).from_buffer_copy(report);buffers.append(data);f=fields[-1];f.name=b'report';f.group=7;f.data=data;f.length=len(report)
        e=_Event();e.kind=KINDS.index(kind);e.token=token.encode() if token else None;e.fields=fields;e.field_count=len(fields)
        return self._call('inject',self._active(),C.byref(e),1)
    def close(self):
        if self._handle.value:
            try:self._call('cancel',self._handle)
            except Error:pass
            with self._lock:
                if self._handle.value:
                    try:self._call('close',self._handle)
                    finally:self._handle.value=0;self._ticket=0
    def __enter__(self):return self
    def __exit__(self,*_):self.close()
    def __del__(self):
        if hasattr(self,'_handle'):
            try:self.close()
            except Exception:pass

Client = Interceptor
