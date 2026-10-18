#include "ContextAPI.h"
#include "ContextInternal.h"
#include "internal/CaptureAll.h"
#include "NativePolicy.h"
#include "SendBatch.h"
#include "ContextPreparation.h"
#include "InputProcessing.h"
#include "internal/InputRead.h"
#include "internal/DeviceAvailability.h"
#include "internal/ControllerXusbMirror.h"
#include <algorithm>
#include <SetupAPI.h>
#include <hidsdi.h>
#include <hidpi.h>
#include "HidCodec.h"
#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"hid.lib")
#include <mutex>
#include <stdexcept>
#include <system_error>

namespace Interceptor {
namespace {
[[noreturn]] void Fail(const char* operation,DWORD error=GetLastError()) {
    if(error==IC_ERROR_TRIAL_EXPIRED)operation="Interceptor two-hour trial expired; restart Windows to use capture again";
    throw std::system_error(static_cast<int>(error),std::system_category(),operation);
}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle() { if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value); }
    Handle()=default;Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
void Open(Handle& h) {
    h.value=CreateFileW(L"\\\\.\\Interceptor",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h.value==INVALID_HANDLE_VALUE)Fail("Open Interceptor (install driver and run as administrator)");
}
DWORD Io(HANDLE handle,DWORD code,const void* in,DWORD inBytes,void* out,DWORD outBytes) {
    DWORD bytes=0;
    if(!DeviceIoControl(handle,code,const_cast<void*>(in),inBytes,out,outBytes,&bytes,nullptr))Fail("Interceptor DeviceIoControl");
    return bytes;
}
IC_CAPABILITIES Capabilities(HANDLE handle) {
    IC_CAPABILITIES c{};
    const auto bytes=Io(handle,IOCTL_IC_CAPABILITIES,nullptr,0,&c,sizeof(c));
    if(!Policy::ValidCapabilities(c,bytes))Fail("Invalid Interceptor capabilities",ERROR_REVISION_MISMATCH);
    return c;
}
using SourceHandles=std::vector<std::unique_ptr<Handle>>;
void ValidateControllerCapabilities(HANDLE driver,std::uint8_t controllers,std::uint8_t inputs=0) {
    if(!(controllers&~Controller_Hid))return; // Keep compatibility with ABI4 HID-only drivers.
    IC_CAPABILITIES capabilities{};
    try {capabilities=Capabilities(driver);}
    catch(const std::system_error& e) {
        if(e.code().value()==ERROR_INVALID_FUNCTION)Fail("Loaded driver has no native capabilities endpoint",ERROR_NOT_SUPPORTED);
        throw;
    }
    if((controllers&~Policy::SupportedControllers(capabilities)) || (inputs&~capabilities.input_mask))
        Fail("Controller profile unavailable in the loaded driver",ERROR_NOT_SUPPORTED);
}
SourceHandles OpenControllerSources(std::uint8_t controllers,bool availableOnly=false) {
    SourceHandles sources;
    const GUID sourceGuids[]={IC_HID_INTERFACE_GUID,IC_XUSB_INTERFACE_GUID,IC_XINPUTHID_INTERFACE_GUID,IC_XUSB_MIRROR_INTERFACE_GUID};
    const DWORD sourceIoctls[]={IOCTL_IC_HID_SOURCE,IOCTL_IC_XUSB_SOURCE,IOCTL_IC_XINPUTHID_SOURCE,IOCTL_IC_XUSB_MIRROR_SOURCE};
    // GIP is driven by Windows USB URBs. It has no user-mode source handle.
    for(unsigned native=0;native<4;++native)if(controllers&(native==3?Controller_Xinput:1u<<native)) {
        const auto before=sources.size();
        struct Set {HDEVINFO value;~Set(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}};
        Set set{SetupDiGetClassDevsW(&sourceGuids[native],nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
        if(set.value==INVALID_HANDLE_VALUE)Fail("Enumerate controller filters");
        for(DWORD i=0;;++i) {
            SP_DEVICE_INTERFACE_DATA item{};item.cbSize=sizeof(item);
            if(!SetupDiEnumDeviceInterfaces(set.value,nullptr,&sourceGuids[native],i,&item)) {
                if(GetLastError()!=ERROR_NO_MORE_ITEMS)Fail("Enumerate controller interface");break;
            }
            DWORD required=0;SetupDiGetDeviceInterfaceDetailW(set.value,&item,nullptr,0,&required,nullptr);
            if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER || required<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
                Fail("Query controller path size",ERROR_INVALID_DATA);
            std::vector<BYTE> path(required);auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(path.data());
            detail->cbSize=sizeof(*detail);
            if(!SetupDiGetDeviceInterfaceDetailW(set.value,&item,detail,required,nullptr,nullptr))Fail("Query controller path");
            auto source=std::make_unique<Handle>();
            source->value=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
            if(source->value==INVALID_HANDLE_VALUE) {
                const auto error=GetLastError();
                if(availableOnly && (error==ERROR_DEVICE_NOT_CONNECTED || error==ERROR_NOT_READY || error==ERROR_FILE_NOT_FOUND || error==ERROR_GEN_FAILURE))continue;
                Fail("Open controller source",error);
            }
            try {Io(source->value,sourceIoctls[native],nullptr,0,nullptr,0);}
            catch(const std::system_error& error) {
                // Collection filters explicitly report transparent unsupported
                // profiles. They must not disable capture of other supported
                // HID/XInputHID devices. Keep other failures visible, and never
                // omit a native XUSB mirror required for complete isolation.
                if((native==0 || native==2) && error.code().value()==ERROR_NOT_SUPPORTED)continue;
                if(availableOnly && (error.code().value()==ERROR_NOT_SUPPORTED || error.code().value()==ERROR_NOT_READY || error.code().value()==ERROR_DEVICE_NOT_CONNECTED))continue;
                throw;
            }
            sources.push_back(std::move(source));
        }
        if(!availableOnly && sources.size()==before)Fail("No active controller filters for requested transport",ERROR_DEVICE_NOT_CONNECTED);
    }
    return sources;
}
unsigned ControllerSendIndex(DWORD code) noexcept {
    switch(code) {
    case IOCTL_IC_SEND_HID:return 0;
    case IOCTL_IC_SEND_XUSB:return 1;
    case IOCTL_IC_SEND_XINPUTHID:return 2;
    case IOCTL_IC_SEND_GIP:case IOCTL_IC_SEND_GIP_RAW:return 3;
    default:return 4;
    }
}
struct SenderState {
    // Reverse destruction closes the source files before the control handle.
    Handle driver;
    Detail::SourceCache<SourceHandles> sources[4];
    HANDLE Prepare(unsigned index) {
        if(driver.value==INVALID_HANDLE_VALUE)Open(driver);
        if(index<4)sources[index].Prepare([&] {
            const auto controllers=static_cast<std::uint8_t>(1u<<index);
            ValidateControllerCapabilities(driver.value,controllers);
            return OpenControllerSources(controllers);
        });
        return driver.value;
    }
    void Refresh(unsigned index) {
        if(index<4)sources[index].Reset();
        (void)Prepare(index);
    }
};
SenderState& Sender() {thread_local SenderState state;return state;}
DWORD NtError(std::uint32_t status) {
    if(status==0)return ERROR_SUCCESS;
    if(status==0x80000011u)return ERROR_BUSY; // STATUS_DEVICE_BUSY
    if(status==0xC000009Du)return ERROR_DEVICE_NOT_CONNECTED;
    using Convert=ULONG(WINAPI*)(LONG);
    auto module=GetModuleHandleW(L"ntdll.dll");
    if(!module)return ERROR_GEN_FAILURE;
    auto fn=reinterpret_cast<Convert>(GetProcAddress(module,"RtlNtStatusToDosError"));
    return fn?fn(static_cast<LONG>(status)):ERROR_GEN_FAILURE;
}
template<class T> Send_Result Send(DWORD code,const T* data,std::size_t count,HANDLE bound=INVALID_HANDLE_VALUE) {
    if(count && !data)throw std::invalid_argument("Send: null input");
    const auto index=ControllerSendIndex(code);
    return Detail::SendBatches(count,[&](std::size_t offset,std::size_t n)->Send_Result {
        IC_SEND_RESULT result{};
        try {
            const void* payload=data+offset;
            if constexpr(Detail::DecodedHid<T>) {
                // Reused per caller thread; no timer/worker or allocation after warmup.
                thread_local std::vector<Detail::Wire<T>> encoded;
                encoded.resize(n);
                for(std::size_t i=0;i<n;++i) {
                    const auto& input=data[offset+i];encoded[i]=static_cast<const Detail::Wire<T>&>(input);
                    const auto error=Detail::Hid_Codec::Encode(input,encoded[i].report,encoded[i].length,encoded[i].device);
                    if(error)return {0,error};
                }
                payload=encoded.data();
            }
            const DWORD bytes=Io(bound==INVALID_HANDLE_VALUE?Sender().Prepare(index):bound,code,payload,static_cast<DWORD>(n*sizeof(Detail::Wire<T>)),&result,sizeof(result));
            if(bytes!=sizeof(result) || result.accepted>n)Fail("Invalid Interceptor send response",ERROR_INVALID_DATA);
        } catch(const std::system_error& e) {return {0,static_cast<DWORD>(e.code().value())};}
        return {result.accepted,NtError(result.status)};
    },[&]()->DWORD {
        try {Sender().Refresh(index);return ERROR_SUCCESS;}
        catch(const std::system_error& e) {return static_cast<DWORD>(e.code().value());}
    },bound==INVALID_HANDLE_VALUE && index<4);
}
}
struct Input_Processing_Storage {
    std::vector<Keyboard_Input> keyboard;
    std::vector<Mouse_Input> mouse;
    std::vector<Controller_Hid_Input> hid;
    std::vector<Controller_Xinput_Input> xusb;
    std::vector<Controller_XinputHid_Input> xinputhid;
    std::vector<Controller_GIP_Input> gip;
    std::vector<Controller_GIP_Raw_Input> gipRaw;
};
class Context {
public:
    SourceHandles sources;
    Handle driver,events[IC_STREAM_COUNT],revoked;std::uint8_t mask=0;std::mutex readers[IC_STREAM_COUNT];
    std::vector<Detail::Wire<Controller_Hid_Input>> hid;
    std::vector<Controller_Xinput_Input> xusb;
    std::vector<Detail::Wire<Controller_XinputHid_Input>> xinputhid;
    std::vector<Controller_GIP_Input> gip;
    std::vector<Controller_GIP_Raw_Input> gipRaw;
    bool gipRawSupported=false;
    int selectedReadSupport=-1; // Unknown / unavailable / supported, per handle.
    std::mutex processing;
    std::unique_ptr<Input_Processing_Storage> processingStorage;
    ~Context() {if(driver.value!=INVALID_HANDLE_VALUE) {CloseHandle(driver.value);driver.value=INVALID_HANDLE_VALUE;}}
    std::vector<Keyboard_Input> keyboard;
    std::vector<Mouse_Input> mouse;
};
void Context_Deleter::operator()(Context* c) const noexcept { delete c; }
IC_DEVICE_QUERY_RESULT DeviceDetails(HANDLE driver,std::uint32_t type,std::uint32_t token,bool ordinal,std::uint32_t version) {
    IC_DEVICE_QUERY request{version,type,token,ordinal?1u:0u};IC_DEVICE_QUERY_RESULT result{};
    const auto bytes=Io(driver,IOCTL_IC_DEVICE_QUERY,&request,sizeof(request),&result,sizeof(result));
    if(bytes!=sizeof(result) || result.size!=sizeof(result) || result.version!=version ||
       result.type!=type || (ordinal?!result.instance_token:result.instance_token!=token) || result.reserved[2] ||
       (version==IC_DEVICE_QUERY_VERSION && (result.reserved[0] || result.reserved[1])) ||
       (version==IC_DEVICE_STATE_VERSION && type && (result.reserved[0]<IC_DEVICE_PREPARING || result.reserved[0]>IC_DEVICE_UNAVAILABLE)) ||
       result.instance[255])Fail("Invalid Interceptor device details",ERROR_INVALID_DATA);
    return result;
}
Detail::Native_Device_Availability Availability(HANDLE driver,std::uint32_t type,std::uint32_t token,bool ordinal) {
    Detail::Native_Device_Availability state;
    try {state.details=DeviceDetails(driver,type,token,ordinal,IC_DEVICE_STATE_VERSION);state.reported_by_driver=true;}
    catch(const std::system_error& error) {
        const auto code=error.code().value();
        if(code!=ERROR_INVALID_PARAMETER && code!=ERROR_INVALID_FUNCTION && code!=ERROR_NOT_SUPPORTED)throw;
        state.details=DeviceDetails(driver,type,token,ordinal,IC_DEVICE_QUERY_VERSION);
        IcDeviceAvailability(&state.details,0);
    }
    state.state=state.details.reserved[0];state.reasons=state.details.reserved[1];return state;
}
DWORD PreparationError(HANDLE driver,std::uint8_t mask,std::uint8_t controllers,DWORD original) {
    if(original!=ERROR_DEVICE_NOT_CONNECTED && original!=ERROR_NOT_READY)return original;
    // Older kernels called pending sources disconnected. Query exact registered
    // readers, without opening another source/capture, to preserve this distinction.
    bool found=false,pending=false;
    const unsigned streams=mask|(static_cast<unsigned>(controllers)<<2);
    for(unsigned stream=0;stream<6;++stream)if(streams&(1u<<stream)) {
        bool familyPresent=false;
        for(unsigned ordinal=1;ordinal<=IC_MAX_DEVICES;++ordinal) {
            try {
                const auto state=Availability(driver,stream+1,ordinal,true);familyPresent=true;found=true;
                if(state.state!=IC_DEVICE_UNSUPPORTED)pending=true;
            }catch(const std::system_error& error) {
                if(error.code().value()==ERROR_DEVICE_NOT_CONNECTED)break;
                // Unsupported query extensions preserve the real CREATE error.
                if(error.code().value()==ERROR_INVALID_FUNCTION || error.code().value()==ERROR_NOT_SUPPORTED || error.code().value()==ERROR_INVALID_PARAMETER)return original;
                throw;
            }
        }
        if(!familyPresent)return ERROR_DEVICE_NOT_CONNECTED;
    }
    return !found?original:pending?ERROR_NOT_READY:ERROR_NOT_SUPPORTED;
}
static Context_Result CreateImpl(std::uint8_t mask,std::uint8_t controllers,const Detail::Preparation_Options& wait,bool all) {
    if(controllers&~15u)throw std::invalid_argument("Create_Context: invalid controller mask");
    if((!mask && !controllers) || (mask&~Both))throw std::invalid_argument("Create_Context: invalid input mask");
    Context_Result result;result.devices.reserve(IC_MAX_DEVICES);
    std::vector<std::uint64_t> response((sizeof(IC_CREATE_RESULT)+IC_MAX_DEVICES*sizeof(Device_Info)+7)/8);
    Context_Ptr c(new Context);c->mask=static_cast<std::uint8_t>(mask|((controllers&Controller_Hid)?IC_READY_HID:0)|((controllers&Controller_Xinput)?IC_READY_XUSB:0)|((controllers&Controller_XinputHid)?IC_READY_XINPUTHID:0)|((controllers&Controller_GIP)?IC_READY_GIP:0));
    Open(c->driver);
    if(all) {
        const auto capabilities=Capabilities(c->driver.value);
        mask=static_cast<std::uint8_t>(capabilities.input_mask);
        controllers=static_cast<std::uint8_t>(Policy::SupportedControllers(capabilities));
        c->mask=static_cast<std::uint8_t>(mask|(controllers<<2));
    }
    ValidateControllerCapabilities(c->driver.value,controllers,mask);
    if(controllers&Controller_GIP)c->gipRawSupported=(Capabilities(c->driver.value).gip_profiles&IC_PROFILE_GIP_USB_GAMEPAD_RAW)!=0;
    IC_CREATE request{};request.version=IC_VERSION;request.mask=mask;request.controller_mask=controllers;
    for(unsigned i=0;i<IC_STREAM_COUNT;++i)if(c->mask&(1u<<i)) {
        c->events[i].value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!c->events[i].value)Fail("CreateEvent");
    }
    request.keyboard_event=(mask&Keyboard)?reinterpret_cast<std::uint64_t>(c->events[0].value):0;
    request.mouse_event=(mask&Mouse)?reinterpret_cast<std::uint64_t>(c->events[1].value):0;
    c->revoked.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!c->revoked.value)Fail("CreateEvent");
    request.revoked_event=reinterpret_cast<std::uint64_t>(c->revoked.value);
    request.controller_hid_event=(controllers&Controller_Hid)?reinterpret_cast<std::uint64_t>(c->events[2].value):0;
    IC_CREATE_XUSB nativeRequest{request,(controllers&Controller_Xinput)?reinterpret_cast<std::uint64_t>(c->events[3].value):0};
    IC_CREATE_NATIVE extendedRequest{request,nativeRequest.xusb_event,(controllers&Controller_XinputHid)?reinterpret_cast<std::uint64_t>(c->events[4].value):0,(controllers&Controller_GIP)?reinterpret_cast<std::uint64_t>(c->events[5].value):0};
    const bool extended=all || (controllers&(Controller_XinputHid|Controller_GIP))!=0;
    const bool native=(controllers&Controller_Xinput)!=0;
    DWORD bytes=0;bool sourcesOpened=false;
    const auto attempt=[&] {
        try {
            if(!sourcesOpened){c->sources=OpenControllerSources(controllers,all);sourcesOpened=true;}
            bytes=Io(c->driver.value,all?IOCTL_IC_CREATE_ALL:extended?IOCTL_IC_CREATE_NATIVE:native?IOCTL_IC_CREATE_XUSB:IOCTL_IC_CREATE,
                extended?static_cast<const void*>(&extendedRequest):native?static_cast<const void*>(&nativeRequest):&request,
                extended?sizeof(extendedRequest):native?sizeof(nativeRequest):sizeof(request),response.data(),static_cast<DWORD>(response.size()*8));
        }catch(const std::system_error& error) {
            const auto code=static_cast<DWORD>(error.code().value());
            Fail("Create capture context",PreparationError(c->driver.value,mask,controllers,code));
        }
    };
    if(all)attempt();
    else Detail::PrepareContext(attempt,[]{return GetTickCount64();},[&](DWORD delay) {
        if(wait.stop_event) {
            const auto state=WaitForSingleObject(wait.stop_event,delay);
            if(state==WAIT_OBJECT_0)Fail("Capture preparation cancelled",ERROR_OPERATION_ABORTED);
            if(state==WAIT_FAILED)Fail("Wait for capture preparation");
        } else if(delay)Sleep(delay);
    },wait.timeout);
    IC_CREATE_RESULT header{};
    if(bytes<sizeof(header))Fail("Invalid context snapshot",ERROR_INVALID_DATA);
    memcpy(&header,response.data(),sizeof(header));
    if(header.version!=IC_VERSION || header.count>IC_MAX_DEVICES || bytes!=sizeof(header)+header.count*sizeof(Device_Info))
        Fail("Invalid context snapshot",ERROR_INVALID_DATA);
    result.devices.resize(header.count);
    if(header.count)memcpy(result.devices.data(),reinterpret_cast<const BYTE*>(response.data())+sizeof(header),header.count*sizeof(Device_Info));
    result.generation=header.generation;result.context=std::move(c);return result;
}
Context_Result Create_Context() {return CreateImpl(Both,15,Detail::Preparation_Options{},true);}
bool Detail::Raw_Gip_Supported(const Context& context) noexcept {return context.gipRawSupported;}
Context_Result Detail::Create_Selected_Context(std::uint8_t mask,std::uint8_t controllers,const Preparation_Options& wait) {
    return CreateImpl(mask,controllers,wait,false);
}
std::vector<Device_Info> Get_Devices(Context& c) {
    std::vector<std::uint64_t> raw((sizeof(IC_CREATE_RESULT)+IC_MAX_DEVICES*sizeof(Device_Info)+7)/8);
    const auto bytes=Io(c.driver.value,IOCTL_IC_CONTEXT_DEVICES,nullptr,0,raw.data(),static_cast<DWORD>(raw.size()*8));
    IC_CREATE_RESULT header{};if(bytes<sizeof(header))Fail("Invalid device snapshot",ERROR_INVALID_DATA);
    memcpy(&header,raw.data(),sizeof(header));
    if(header.version!=IC_VERSION || header.count>IC_MAX_DEVICES || bytes!=sizeof(header)+header.count*sizeof(Device_Info))Fail("Invalid device snapshot",ERROR_INVALID_DATA);
    std::vector<Device_Info> out(header.count);
    if(header.count)memcpy(out.data(),reinterpret_cast<const BYTE*>(raw.data())+sizeof(header),header.count*sizeof(Device_Info));
    return out;
}
void Destroy_Context(Context_Ptr& c) noexcept { c.reset(); }
IC_CAPABILITIES Get_Capabilities() {Handle h;Open(h);return Capabilities(h.value);}
IC_TRIAL_STATUS Get_Trial_Status() {
    Handle h;Open(h);IC_TRIAL_STATUS result{};
    const auto bytes=Io(h.value,IOCTL_IC_TRIAL_STATUS,nullptr,0,&result,sizeof(result));
    if(bytes!=sizeof(result) || result.size!=sizeof(result) || result.version!=IC_TRIAL_VERSION ||
       result.state>IC_TRIAL_UNAVAILABLE || result.active_mask&~IC_ALL_STREAMS ||
       !result.duration_100ns || result.remaining_100ns>result.duration_100ns ||
       result.reserved[0] || result.reserved[1] || result.reserved[2])Fail("Invalid Interceptor trial status",ERROR_INVALID_DATA);
    return result;
}
IC_DEVICE_QUERY_RESULT Get_Device_Details(std::uint32_t type,std::uint32_t token,bool ordinal) {
    Handle h;Open(h);return DeviceDetails(h.value,type,token,ordinal,IC_DEVICE_QUERY_VERSION);
}
Detail::Native_Device_Availability Get_Device_Availability(std::uint32_t type,std::uint32_t token,bool ordinal) {
    if(!type || type>IC_TYPE_CONTROLLER_GIP || !token || (ordinal && token>IC_MAX_DEVICES))throw std::invalid_argument("Invalid device availability query");
    Handle h;Open(h);return Availability(h.value,type,token,ordinal);
}
void Destroy_Context(Context_Result& c) noexcept {c.context.reset();c.devices.clear();c.generation=0;}
template<class T> void Get(Context& c,std::vector<T>& out,std::vector<Detail::Wire<T>>& scratch,std::uint32_t count,unsigned stream,DWORD code) {
    if(!(c.mask&(1u<<stream)))throw std::invalid_argument("Context does not own this input type");
    std::lock_guard<std::mutex> lock(c.readers[stream]);
    const auto limit=stream>=2?IC_HID_CAPACITY:IC_CAPACITY;
    const auto capacity=count?std::min<std::uint32_t>(count,limit):limit;
    // Reserve before dequeue: an allocation failure must not lose captured input.
    out.reserve(capacity);IC_GET request{IC_VERSION,count};
    if(scratch.size()<capacity)scratch.resize(capacity);
    try {
        DWORD bytes=Io(c.driver.value,code,&request,sizeof(request),scratch.data(),static_cast<DWORD>(capacity*sizeof(Detail::Wire<T>)));
        if(bytes%sizeof(Detail::Wire<T>) || bytes>capacity*sizeof(Detail::Wire<T>))Fail("Invalid Interceptor input response",ERROR_INVALID_DATA);
        Detail::AssignWire(out,scratch.data(),bytes/sizeof(Detail::Wire<T>));
    } catch(...) { out.clear();throw; }
}
void Get_Keyboard_Input(Context& c,std::vector<Keyboard_Input>& out,std::uint32_t count) { Get(c,out,c.keyboard,count,0,IOCTL_IC_GET_KEYBOARD); }
void Get_Mouse_Input(Context& c,std::vector<Mouse_Input>& out,std::uint32_t count) { Get(c,out,c.mouse,count,1,IOCTL_IC_GET_MOUSE); }
std::vector<Keyboard_Input> Get_Keyboard_Input(Context& c,std::uint32_t count) {
    std::vector<Keyboard_Input> out;Get_Keyboard_Input(c,out,count);return out;
}
std::vector<Mouse_Input> Get_Mouse_Input(Context& c,std::uint32_t count) {
    std::vector<Mouse_Input> out;Get_Mouse_Input(c,out,count);return out;
}
Send_Result Send_Input(const Keyboard_Input* input,std::size_t count) { return Send(IOCTL_IC_SEND_KEYBOARD,input,count); }
Send_Result Send_Input(const Mouse_Input* input,std::size_t count) { return Send(IOCTL_IC_SEND_MOUSE,input,count); }
Send_Result Send_Input(Context& c,const Keyboard_Input* input,std::size_t count) { return Send(IOCTL_IC_SEND_OWNED_KEYBOARD,input,count,c.driver.value); }
Send_Result Send_Input(Context& c,const Mouse_Input* input,std::size_t count) { return Send(IOCTL_IC_SEND_OWNED_MOUSE,input,count,c.driver.value); }
template<class T> std::uint32_t GetDirect(Context& c,T* out,std::uint32_t capacity,std::uint32_t count,unsigned stream,DWORD code) {
    if(!(c.mask&(1u<<stream)) || !out || !capacity || capacity>IC_CAPACITY || (count && std::min(count,IC_CAPACITY)>capacity))
        throw std::invalid_argument("Get: invalid stream/buffer/capacity");
    std::lock_guard<std::mutex> lock(c.readers[stream]);
    IC_GET request{IC_VERSION,count};
    if constexpr(Detail::DecodedHid<T>) {
        // The per-family reader lock also protects this context-owned wire
        // buffer. Allocate before dequeue and reuse it across direct reads.
        auto& wire=[&]() -> auto& {
            if constexpr(std::is_same_v<T,Controller_Hid_Input>)return c.hid;
            else return c.xinputhid;
        }();
        if(wire.size()<capacity)wire.resize(capacity);
        const auto bytes=Io(c.driver.value,code,&request,sizeof(request),wire.data(),static_cast<DWORD>(capacity*sizeof(Detail::Wire<T>)));
        if(bytes%sizeof(Detail::Wire<T>)||bytes>capacity*sizeof(Detail::Wire<T>))Fail("Invalid Interceptor input response",ERROR_INVALID_DATA);
        const auto n=bytes/sizeof(Detail::Wire<T>);for(std::size_t i=0;i<n;++i)Detail::AssignWire(out[i],wire[i]);return static_cast<std::uint32_t>(n);
    } else {
        const DWORD bytes=Io(c.driver.value,code,&request,sizeof(request),out,static_cast<DWORD>(capacity*sizeof(T)));
        if(bytes%sizeof(T) || bytes>capacity*sizeof(T))Fail("Invalid Interceptor input response",ERROR_INVALID_DATA);
        return static_cast<std::uint32_t>(bytes/sizeof(T));
    }
}
std::uint32_t Get_Keyboard_Input(Context& c,Keyboard_Input* out,std::uint32_t capacity,std::uint32_t count) { return GetDirect(c,out,capacity,count,0,IOCTL_IC_GET_KEYBOARD); }
std::uint32_t Get_Mouse_Input(Context& c,Mouse_Input* out,std::uint32_t capacity,std::uint32_t count) { return GetDirect(c,out,capacity,count,1,IOCTL_IC_GET_MOUSE); }
void Get_Controller_Hid_Input(Context& c,std::vector<Controller_Hid_Input>& out,std::uint32_t count) {Get(c,out,c.hid,count,2,IOCTL_IC_GET_HID);}
std::vector<Controller_Hid_Input> Get_Controller_Hid_Input(Context& c,std::uint32_t count) {
    std::vector<Controller_Hid_Input> out;Get_Controller_Hid_Input(c,out,count);return out;
}
std::uint32_t Get_Controller_Hid_Input(Context& c,Controller_Hid_Input* out,std::uint32_t capacity,std::uint32_t count) {
    if(capacity>IC_HID_CAPACITY)throw std::invalid_argument("HID capacity exceeds IC_HID_CAPACITY");
    return GetDirect(c,out,capacity,count?std::min(count,IC_HID_CAPACITY):0,2,IOCTL_IC_GET_HID);
}
Send_Result Send_Input(const Controller_Hid_Input* in,std::size_t n) {return Send(IOCTL_IC_SEND_HID,in,n);}
Send_Result Send_Input(Context& c,const Controller_Hid_Input* in,std::size_t n) {return Send(IOCTL_IC_SEND_OWNED_HID,in,n,c.driver.value);}
void Get_Controller_Xinput_Input(Context& c,std::vector<Controller_Xinput_Input>& out,std::uint32_t count){Get(c,out,c.xusb,count,3,IOCTL_IC_GET_XUSB);}
std::vector<Controller_Xinput_Input> Get_Controller_Xinput_Input(Context& c,std::uint32_t count){std::vector<Controller_Xinput_Input> out;Get_Controller_Xinput_Input(c,out,count);return out;}
std::uint32_t Get_Controller_Xinput_Input(Context& c,Controller_Xinput_Input* out,std::uint32_t capacity,std::uint32_t count){if(capacity>IC_XUSB_CAPACITY)throw std::invalid_argument("XUSB capacity");return GetDirect(c,out,capacity,count?std::min(count,IC_XUSB_CAPACITY):0,3,IOCTL_IC_GET_XUSB);}
Send_Result Send_Input(const Controller_Xinput_Input* in,std::size_t n){return Send(IOCTL_IC_SEND_XUSB,in,n);}
Send_Result Send_Input(Context& c,const Controller_Xinput_Input* in,std::size_t n){return Send(IOCTL_IC_SEND_OWNED_XUSB,in,n,c.driver.value);}
void Get_Controller_XinputHid_Input(Context& c,std::vector<Controller_XinputHid_Input>& out,std::uint32_t count){Get(c,out,c.xinputhid,count,4,IOCTL_IC_GET_XINPUTHID);}
std::vector<Controller_XinputHid_Input> Get_Controller_XinputHid_Input(Context& c,std::uint32_t count){std::vector<Controller_XinputHid_Input> out;Get_Controller_XinputHid_Input(c,out,count);return out;}
std::uint32_t Get_Controller_XinputHid_Input(Context& c,Controller_XinputHid_Input* out,std::uint32_t capacity,std::uint32_t count){if(capacity>IC_HID_CAPACITY)throw std::invalid_argument("XInputHID capacity");return GetDirect(c,out,capacity,count?std::min(count,IC_HID_CAPACITY):0,4,IOCTL_IC_GET_XINPUTHID);}
Send_Result Send_Input(const Controller_XinputHid_Input* in,std::size_t n){return Send(IOCTL_IC_SEND_XINPUTHID,in,n);}
Send_Result Send_Input(Context& c,const Controller_XinputHid_Input* in,std::size_t n){return Send(IOCTL_IC_SEND_OWNED_XINPUTHID,in,n,c.driver.value);}
void Get_Controller_GIP_Input(Context& c,std::vector<Controller_GIP_Input>& out,std::uint32_t count){Get(c,out,c.gip,count,5,IOCTL_IC_GET_GIP);}
std::vector<Controller_GIP_Input> Get_Controller_GIP_Input(Context& c,std::uint32_t count){std::vector<Controller_GIP_Input> out;Get_Controller_GIP_Input(c,out,count);return out;}
std::uint32_t Get_Controller_GIP_Input(Context& c,Controller_GIP_Input* out,std::uint32_t capacity,std::uint32_t count){if(capacity>IC_GIP_CAPACITY)throw std::invalid_argument("GIP capacity");return GetDirect(c,out,capacity,count?std::min(count,IC_GIP_CAPACITY):0,5,IOCTL_IC_GET_GIP);}
Send_Result Send_Input(const Controller_GIP_Input* in,std::size_t n){return Send(IOCTL_IC_SEND_GIP,in,n);}
Send_Result Send_Input(Context& c,const Controller_GIP_Input* in,std::size_t n){return Send(IOCTL_IC_SEND_OWNED_GIP,in,n,c.driver.value);}
void Get_Controller_GIP_Raw_Input(Context& c,std::vector<Controller_GIP_Raw_Input>& out,std::uint32_t count) {
    if(!c.gipRawSupported)Fail("Loaded driver has no complete GIP report API",ERROR_NOT_SUPPORTED);
    Get(c,out,c.gipRaw,count,5,IOCTL_IC_GET_GIP_RAW);
}
std::uint32_t Get_Controller_GIP_Raw_Input(Context& c,Controller_GIP_Raw_Input* out,std::uint32_t capacity,std::uint32_t count) {
    if(!c.gipRawSupported)Fail("Loaded driver has no complete GIP report API",ERROR_NOT_SUPPORTED);
    if(capacity>IC_GIP_CAPACITY)throw std::invalid_argument("GIP raw capacity");
    return GetDirect(c,out,capacity,count?std::min(count,IC_GIP_CAPACITY):0,5,IOCTL_IC_GET_GIP_RAW);
}
Send_Result Send_Input(const Controller_GIP_Raw_Input* in,std::size_t n){return Send(IOCTL_IC_SEND_GIP_RAW,in,n);}
Send_Result Send_Input(Context& c,const Controller_GIP_Raw_Input* in,std::size_t n){
    if(!c.gipRawSupported)return {0,ERROR_NOT_SUPPORTED};
    return Send(IOCTL_IC_SEND_OWNED_GIP_RAW,in,n,c.driver.value);
}
std::vector<std::uint8_t> Get_Controller_XinputHid_Descriptor(Context& c,std::uint32_t id) {
    std::vector<std::uint8_t> out(65536);IC_GET request{IC_VERSION,id};
    const auto bytes=Io(c.driver.value,IOCTL_IC_XINPUTHID_DESCRIPTOR,&request,sizeof(request),out.data(),static_cast<DWORD>(out.size()));
    if(!bytes || bytes>out.size())Fail("Invalid XInputHID descriptor response",ERROR_INVALID_DATA);
    out.resize(bytes);return out;
}
std::vector<std::uint8_t> Get_Controller_Hid_Descriptor(Context& c,std::uint32_t id) {
    std::vector<std::uint8_t> out(65536);IC_GET request{IC_VERSION,id};
    const auto bytes=Io(c.driver.value,IOCTL_IC_HID_DESCRIPTOR,&request,sizeof(request),out.data(),static_cast<DWORD>(out.size()));
    if(!bytes || bytes>out.size())Fail("Invalid HID descriptor response",ERROR_INVALID_DATA);
    out.resize(bytes);return out;
}
IC_STATUS Get_Status(Context* c) {
    IC_STATUS status{};Handle transient;
    if(!c)Open(transient);
    DWORD bytes=Io(c?c->driver.value:transient.value,IOCTL_IC_STATUS,nullptr,0,&status,sizeof(status));
    if(bytes!=sizeof(status) || status.version!=IC_VERSION)Fail("Interceptor ABI mismatch",ERROR_REVISION_MISMATCH);
    return status;
}
std::size_t Detail::ProcessInputs(Context& c,const Input_Callbacks& callbacks,DWORD timeout,HANDLE stop) {
    std::unique_lock<std::mutex> lock(c.processing,std::try_to_lock);
    if(!lock.owns_lock())Fail("Another input processing call already uses this capture",ERROR_BUSY);
    try {
    const auto ready=Wait_For_Input(c,timeout,stop);
    if(!ready)return 0;
    if(!c.processingStorage)c.processingStorage=std::make_unique<Input_Processing_Storage>();
    auto& storage=*c.processingStorage;std::size_t processed=0;
#define IC_PROCESS(bit,Name,buffer,callback) if(ready&(bit))processed+=ProcessBatch(storage.buffer, \
    [&](auto& out){Get_##Name##_Input(c,out);}, \
    [&](auto inputs){return Send_Input(c,inputs);},callbacks.callback,callbacks.user)
    IC_PROCESS(Keyboard,Keyboard,keyboard,keyboard);
    IC_PROCESS(Mouse,Mouse,mouse,mouse);
    IC_PROCESS(Ready_Controller_Hid,Controller_Hid,hid,hid);
    IC_PROCESS(Ready_Controller_Xinput,Controller_Xinput,xusb,xusb);
    IC_PROCESS(Ready_Controller_XinputHid,Controller_XinputHid,xinputhid,xinputhid);
    if(c.gipRawSupported){IC_PROCESS(Ready_Controller_GIP,Controller_GIP_Raw,gipRaw,gipRaw);}
    else {IC_PROCESS(Ready_Controller_GIP,Controller_GIP,gip,gip);}
#undef IC_PROCESS
    return processed;
    }catch(...) {
        // Keep the C++ object/events alive for callers already using them.
        // Closing control revokes every family and restores physical input.
        if(c.driver.value!=INVALID_HANDLE_VALUE){CloseHandle(c.driver.value);c.driver.value=INVALID_HANDLE_VALUE;}
        throw;
    }
}
std::uint8_t Wait_For_Input(Context& c,DWORD timeout) {
    return Wait_For_Input(c,timeout,nullptr);
}
std::uint8_t Wait_For_Input(Context& c,DWORD timeout,HANDLE stop_event) {
    HANDLE events[IC_STREAM_COUNT+2]={c.revoked.value};DWORD count=1;
    std::uint8_t streams[IC_STREAM_COUNT+2]{};
    if(stop_event)events[count++]=stop_event;
    for(unsigned i=0;i<IC_STREAM_COUNT;++i)if(c.mask&(1u<<i)) {
        streams[count]=static_cast<std::uint8_t>(1u<<i);events[count++]=c.events[i].value;
    }
    DWORD result=WaitForMultipleObjects(count,events,FALSE,timeout);
    if(result==WAIT_FAILED)Fail("WaitForMultipleObjects");
    if(result==WAIT_OBJECT_0)Fail("Capture revoked",ERROR_OPERATION_ABORTED);
    if(stop_event && result==WAIT_OBJECT_0+1)return 0;
    if(result==WAIT_TIMEOUT)return 0;
    const auto selected=result-WAIT_OBJECT_0;
    if(selected>=count)Fail("Unexpected input wait result",ERROR_INVALID_DATA);
    // Wait-any returns the lowest signalled index. Its stream is already
    // known; only later streams need polling. Earlier arrivals remain signalled
    // for the next receive. Revocation and caller cancellation keep priority.
    std::uint8_t ready=streams[selected];
    for(DWORD i=selected+1;i<count;++i)
        if(WaitForSingleObject(events[i],0)==WAIT_OBJECT_0)ready|=streams[i];
    return ready;
}
}
#include "HidCodec.inl"
#include "AutomaticInput.inl"
#include "ContextMetadata.inl"
