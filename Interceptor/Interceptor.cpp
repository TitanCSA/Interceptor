#ifndef IC_SDK_INTERNAL
#define IC_SDK_INTERNAL
#endif
#include "Interceptor.h"
// Interceptor SDK implementation. No other project files are required.

// ---- Interceptor.cpp ----

// ---- ContextInternal.h ----
// Wire-compatibility entry point for the archived regression harnesses only.
// Applications use the single public Create_Context() declaration.
namespace Interceptor {
namespace {
struct EditionTarget {
    const wchar_t* service;
    const wchar_t* endpoint;
    const wchar_t* restartKey;
    const wchar_t* mutexName;
    const wchar_t* xusbHidParent;
    const wchar_t* xusbParent;
};
constexpr EditionTarget DemoTarget{L"Interceptor", L"\\\\.\\Interceptor",
    L"SOFTWARE\\Interceptor\\PendingDeviceRestart", L"Global\\Interceptor.InitAllDevices",
    L"InterceptorXusbHidParent", L"InterceptorXusbParent"};
constexpr EditionTarget ReleaseTarget{L"Interceptor", L"\\\\.\\Interceptor",
    L"SOFTWARE\\Interceptor\\PendingDeviceRestart", L"Global\\Interceptor.InitAllDevices",
    L"InterceptorXusbHidParent", L"InterceptorXusbParent"};
thread_local const EditionTarget* selectedTarget=nullptr;
const EditionTarget& AssociationTarget() noexcept {return selectedTarget?*selectedTarget:DemoTarget;}
class EditionScope {
    const EditionTarget* previous=selectedTarget;
public:
    explicit EditionScope(const EditionTarget* target) noexcept {selectedTarget=target;}
    ~EditionScope() {selectedTarget=previous;}
    EditionScope(const EditionScope&)=delete;
    EditionScope& operator=(const EditionScope&)=delete;
};
}
 namespace Detail {
struct Preparation_Options {DWORD timeout=0;HANDLE stop_event=nullptr;};
Context_Result Create_Selected_Context(std::uint8_t mask,std::uint8_t controllers,const Preparation_Options& wait={});
}}

// ---- internal/CaptureAll.h ----

// Optional ABI4 extension. Legacy CREATE endpoints retain their exact contract.
// This endpoint reserves all compiled families, including empty/pending ones.
// Preparation completes asynchronously as each source becomes capturable.
#define IOCTL_IC_CREATE_ALL IC_IOCTL(0xA2E)
#define IOCTL_IC_CONTEXT_DEVICES IC_IOCTL(0xA2F)

// ---- NativePolicy.h ----
// Shared by initialization and its pure tests. No device/registry access here.

// ---- internal/XusbMirrorIdentity.h ----

// xusb22 assigns the IG component to the translated HID view; it is not
// necessarily 00, nor the ViGEm target index. Parse exactly two hex digits and
// a component boundary. Parent identity and descriptor validation remain
// separate mandatory checks; the group alone never associates two devices.
static __inline int IcXusbMirrorHex(uint16_t c) {
    if(c>='0'&&c<='9')return (int)(c-'0');
    if(c>='a'&&c<='f')return (int)(c-'a'+10);
    if(c>='A'&&c<='F')return (int)(c-'A'+10);
    return -1;
}
// Parse the product component only, never a substring or a truncated ID.
// VID/PID identify a device; the xusb22 service, wired compatible IDs and
// verified mirror descriptor establish its supported protocol separately.
static __inline int IcXusbProduct(const uint16_t* text,uint32_t chars,int usb,uint16_t* vid,uint16_t* pid) {
    const char* prefix=usb?"USB\\VID_":"HID\\VID_";
    uint32_t i;uint16_t v=0,p=0;
    if(!text || chars<21)return 0;
    for(i=0;i<21;++i) {
        uint16_t c=text[i];int digit;
        if(c>='a'&&c<='z')c=(uint16_t)(c-'a'+'A');
        if((i>=8 && i<12)||(i>=17 && i<21)) {
            digit=IcXusbMirrorHex(c);if(digit<0)return 0;
            if(i<12)v=(uint16_t)((v<<4)|digit);else p=(uint16_t)((p<<4)|digit);
        } else if(c!=(uint16_t)(i<8?prefix[i]:"&PID_"[i-12]))return 0;
    }
    if(chars>21 && text[21]!='&' && text[21]!='\\')return 0;
    if(vid)*vid=v;if(pid)*pid=p;return 1;
}
static __inline int IcXusbMirrorGroup(const uint16_t* text,uint32_t chars,int usb) {
    uint32_t i;int high,low;
    if(chars<27 || !IcXusbProduct(text,chars,usb,0,0))return -1;
    for(i=21;i<25;++i){uint16_t c=text[i];if(c>='a'&&c<='z')c=(uint16_t)(c-'a'+'A');if(c!=(uint16_t)"&IG_"[i-21])return -1;}
    high=IcXusbMirrorHex(text[25]);low=IcXusbMirrorHex(text[26]);
    if(high<0||low<0||(chars>27&&text[27]!='&'&&text[27]!='\\'))return -1;
    return high*16+low;
}
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace Interceptor { namespace Policy {
inline bool Equal(const std::wstring& a,const wchar_t* b) {return _wcsicmp(a.c_str(),b)==0;}
inline bool Has(const std::vector<std::wstring>& names,const wchar_t* value) {
    return std::any_of(names.begin(),names.end(),[&](const auto& name){return Equal(name,value);});
}
inline bool ForeignInterceptor(const std::wstring& name) {
    return !Equal(name,AssociationTarget().service) && (Equal(name,L"Interceptor") ||
        Equal(name,L"InterceptorKeyboardMouse") || Equal(name,L"InterceptorGamepadController"));
}
inline bool ForeignFilters(const std::vector<std::wstring>& names) {
    return std::any_of(names.begin(),names.end(),[](const auto& name){return ForeignInterceptor(name);});
}
enum class ClassAssociation { PerDevice, Conflict };
inline ClassAssociation ClassFilterAssociation(std::uint32_t transport,
    const std::vector<std::wstring>& deviceUpper,const std::vector<std::wstring>& deviceLower,
    const std::vector<std::wstring>& classUpper,const std::vector<std::wstring>& classLower) {
    if(ForeignFilters(deviceUpper)||ForeignFilters(deviceLower)||ForeignFilters(classUpper)||ForeignFilters(classLower))
        return ClassAssociation::Conflict;
    const auto count=[](const auto& names){return std::count_if(names.begin(),names.end(),
        [](const auto& name){return Equal(name,AssociationTarget().service);});};
    const auto upper=count(classUpper),lower=count(classLower);
    if(!upper&&!lower)return ClassAssociation::PerDevice;
    // All native associations are per-instance. Legacy XUSB class references
    // are migrated before association; leaving one would load the filter twice.
    (void)transport;
    return ClassAssociation::Conflict;
}
inline bool ValidCapabilities(const IC_CAPABILITIES& c,std::size_t bytes) {
    return bytes==sizeof(c) && c.size==sizeof(c) && c.version==IC_CAPABILITIES_VERSION && c.abi_version==IC_VERSION &&
        !(c.input_mask&~IC_BOTH) && !(c.controller_mask&~15u) && !c.flags && !c.reserved[0] && !c.reserved[1] && !c.reserved[2] &&
        c.max_devices && c.max_devices<=IC_MAX_DEVICES && c.queue_capacity==IC_CAPACITY && c.max_send==IC_MAX_SEND &&
        c.hid_report_max==(c.controller_mask?IC_HID_REPORT_MAX:0);
}
inline std::uint32_t SupportedControllers(const IC_CAPABILITIES& c) {
    auto mask=c.controller_mask&IC_CONTROLLER_HID;
    if(c.xusb_profiles&(IC_PROFILE_XUSB_WIRED_028E|IC_PROFILE_XUSB_WIRED_COMPATIBLE))mask|=c.controller_mask&IC_CONTROLLER_XUSB;
    if(c.xinputhid_profiles&IC_PROFILE_XINPUTHID_GENERIC6)mask|=c.controller_mask&IC_CONTROLLER_XINPUTHID;
    if(c.gip_profiles&IC_PROFILE_GIP_USB_GAMEPAD14)mask|=c.controller_mask&IC_CONTROLLER_GIP;
    return mask;
}
inline bool UsbXusbBackend(const IC_CAPABILITIES& c) {
    return (c.controller_mask&IC_CONTROLLER_XUSB) &&
        (c.xusb_profiles&IC_PROFILE_XUSB_USB_FILTER) &&
        (c.xusb_profiles&(IC_PROFILE_XUSB_WIRED_028E|IC_PROFILE_XUSB_WIRED_COMPATIBLE));
}
inline bool Usb028e(const std::wstring& id) {
    constexpr auto prefix=L"USB\\VID_045E&PID_028E";constexpr std::size_t length=21;
    return id.size()>=length && !_wcsnicmp(id.c_str(),prefix,length) &&
        (id.size()==length || id[length]==L'&' || id[length]==L'\\');
}
struct NativeNode {
    std::wstring instance,service,deviceClass;
    std::vector<std::wstring> hardware,compatible,upper;
    std::uint32_t propertyFlags=UINT32_MAX;
    bool blockedAncestry=true;
};
inline bool SameProduct(const std::wstring& a,bool aUsb,const std::wstring& b,bool bUsb) {
    uint16_t av=0,ap=0,bv=0,bp=0;
    return a.size()<=UINT32_MAX && b.size()<=UINT32_MAX &&
        IcXusbProduct(reinterpret_cast<const uint16_t*>(a.data()),static_cast<uint32_t>(a.size()),aUsb,&av,&ap) &&
        IcXusbProduct(reinterpret_cast<const uint16_t*>(b.data()),static_cast<uint32_t>(b.size()),bUsb,&bv,&bp) && av==bv && ap==bp;
}
inline bool XusbProfileAvailable(const NativeNode& n,const IC_CAPABILITIES& c) {
    return (c.xusb_profiles&IC_PROFILE_XUSB_WIRED_COMPATIBLE) ||
        ((c.xusb_profiles&IC_PROFILE_XUSB_WIRED_028E) && Usb028e(n.instance));
}
inline std::uint32_t Transport(const NativeNode& n) {
    const bool usb=!_wcsnicmp(n.instance.c_str(),L"USB\\",4);
    const bool hid=!_wcsnicmp(n.instance.c_str(),L"HID\\",4);
    if(usb && Equal(n.deviceClass,L"{D61CA365-5AF4-4486-998B-9DB4734C6CA3}") && Equal(n.service,L"xusb22") &&
       std::any_of(n.hardware.begin(),n.hardware.end(),[&](const auto& id){return SameProduct(n.instance,true,id,true);}) &&
       (Usb028e(n.instance) || (Has(n.compatible,L"USB\\MS_COMP_XUSB10") &&
                               Has(n.compatible,L"USB\\Class_ff&SubClass_5d&Prot_01"))))return IC_CONTROLLER_XUSB;
    if(hid && n.propertyFlags==6 && !n.blockedAncestry &&
       Equal(n.deviceClass,L"{745A17A0-74D3-11D0-B6FE-00A0C90F57DA}") && Has(n.upper,L"xinputhid")) {
        for(const auto& ids:{&n.hardware,&n.compatible})for(const auto& id:*ids)
            if(Equal(id,L"HID_DEVICE_UP:0001_U:0004") || Equal(id,L"HID_DEVICE_UP:0001_U:0005") || Equal(id,L"HID_DEVICE_UP:0001_U:0008"))return IC_CONTROLLER_XINPUTHID;
    }
    if(usb && Equal(n.deviceClass,L"{05F5CFE2-4733-4950-A6BB-07AAD01A3A84}") && Equal(n.service,L"dc1-controller") &&
       Has(n.compatible,L"USB\\MS_COMP_XGIP10"))return IC_CONTROLLER_GIP;
    return 0;
}
inline bool ComponentPrefix(const std::wstring& id,const wchar_t* prefix) {
    const auto length=wcslen(prefix);
    return id.size()>=length && !_wcsnicmp(id.c_str(),prefix,length) &&
        (id.size()==length || id[length]==L'&' || id[length]==L'\\');
}
inline int XusbMirrorGroup(const std::wstring& id,bool usb=false) {
    if(id.size()>UINT32_MAX)return -1;
    return IcXusbMirrorGroup(reinterpret_cast<const uint16_t*>(id.data()),static_cast<uint32_t>(id.size()),usb?1:0);
}
inline bool XusbMirror(const NativeNode& collection,const NativeNode& hid,const NativeNode& xusb) {
    const int group=XusbMirrorGroup(collection.instance);
    if(group<0 || XusbMirrorGroup(hid.instance,true)!=group ||
       !SameProduct(collection.instance,false,hid.instance,true) || !SameProduct(hid.instance,true,xusb.instance,true) ||
       !Equal(collection.deviceClass,L"{745A17A0-74D3-11D0-B6FE-00A0C90F57DA}") ||
       Has(collection.upper,L"xinputhid") ||
       !Equal(hid.service,L"HidUsb") ||
       !Equal(hid.deviceClass,L"{745A17A0-74D3-11D0-B6FE-00A0C90F57DA}") || Transport(xusb)!=IC_CONTROLLER_XUSB)return false;
    const bool gamepad=Has(collection.hardware,L"HID_DEVICE_UP:0001_U:0005") || Has(collection.compatible,L"HID_DEVICE_UP:0001_U:0005");
    return gamepad && std::any_of(hid.hardware.begin(),hid.hardware.end(),[&](const auto& id){return XusbMirrorGroup(id,true)==group && SameProduct(id,true,hid.instance,true);}) &&
        std::any_of(collection.hardware.begin(),collection.hardware.end(),[&](const auto& id){return XusbMirrorGroup(id)==group && SameProduct(id,false,collection.instance,false);});
}
inline std::vector<std::wstring> Filters(const std::vector<std::wstring>& original,std::uint32_t transport,bool remove) {
    std::vector<std::wstring> out;out.reserve(original.size()+1);bool inserted=false;
    for(const auto& name:original) {
        if(Equal(name,AssociationTarget().service))continue; // Remove duplicates and correct a previous wrong position.
        if(!remove && transport==IC_CONTROLLER_XINPUTHID && Equal(name,L"xinputhid") && !inserted) {
            out.emplace_back(AssociationTarget().service);inserted=true;
        }
        out.push_back(name);
    }
    if(!remove && !inserted)out.emplace_back(AssociationTarget().service);
    return out;
}
}}

// ---- SendBatch.h ----
#include <algorithm>
#include <utility>

namespace Interceptor::Detail {
// A failed factory destroys its partial resources before returning. The cache
// only becomes ready after the complete source set has been prepared.
template<class Resources> class SourceCache {
    Resources resources_;
    bool ready_=false;
public:
    template<class Factory> void Prepare(Factory&& factory) {
        if(ready_)return;
        auto replacement=factory();
        resources_=std::move(replacement);ready_=true;
    }
    void Reset() noexcept {resources_.clear();ready_=false;}
};
inline bool SourceUnavailable(DWORD error) noexcept {
    return error==ERROR_DEVICE_NOT_CONNECTED || error==ERROR_NOT_READY;
}
// send(offset,count) reports an accepted prefix. Refresh is allowed once per
// public call, only for a remaining suffix; a committed input is never resent.
template<class Submit,class Refresh>
Send_Result SendBatches(std::size_t count,Submit&& send,Refresh&& refresh,bool canRefresh) {
    std::size_t accepted=0;bool refreshed=false;
    while(accepted<count) {
        const auto n=std::min<std::size_t>(IC_MAX_SEND,count-accepted);
        const auto result=send(accepted,n);
        if(result.accepted>n)return {accepted,ERROR_INVALID_DATA};
        accepted+=result.accepted;
        if(result.error) {
            if(canRefresh && !refreshed && accepted<count && SourceUnavailable(result.error)) {
                refreshed=true;
                const DWORD error=refresh();
                if(error)return {accepted,error};
                continue;
            }
            return {accepted,result.error};
        }
        if(result.accepted!=n)return {accepted,ERROR_INVALID_DATA};
    }
    return {accepted,ERROR_SUCCESS};
}
}

// ---- ContextPreparation.h ----
#include <Windows.h>
#include <algorithm>
#include <system_error>

namespace Interceptor { namespace Detail {
// Shared by CREATE and host-safe fault tests. Never restart a revoked capture:
// this loop is used only before a context has been successfully acquired.
template<class Attempt,class Clock,class Pause>
void PrepareContext(Attempt attempt,Clock now,Pause pause,DWORD timeout) {
    const auto started=now();
    for(;;) {
        // A zero-duration pause checks cancellation before the first attempt.
        pause(0);
        try {attempt();return;}
        catch(const std::system_error& error) {
            if(error.code().value()!=ERROR_NOT_READY || !timeout)throw;
            const auto elapsed=now()-started;
            if(elapsed>=timeout)throw std::system_error(ERROR_TIMEOUT,std::system_category(),"Device present but capture preparation timed out");
            pause(static_cast<DWORD>(std::min<ULONGLONG>(100,timeout-elapsed)));
        }
    }
}
}}

// ---- InputProcessing.h ----
#include <system_error>

namespace Interceptor::Detail {
// Shared with the real processor and its fault-injection tests. A partial
// send is never replayed: the public owning wrapper closes capture on error.
template<class T,class Read,class Send>
std::size_t ProcessBatch(std::vector<T>& inputs,Read&& read,Send&& send,bool (*handler)(void*,T&),void* user) {
    read(inputs);
    const auto received=inputs.size();std::size_t kept=0;
    for(std::size_t i=0;i<received;++i) {
        if(handler && !handler(user,inputs[i]))continue;
        if(kept!=i)inputs[kept]=inputs[i];
        ++kept;
    }
    if(kept) {
        const auto result=send(Detail::Input_View<const T>(inputs.data(),kept));
        if(result.error || result.accepted!=kept)
            throw std::system_error(static_cast<int>(result.error?result.error:ERROR_WRITE_FAULT),std::system_category(),"Forward processed input");
    }
    return received;
}
}

// ---- internal/InputRead.h ----
// Selected input-read request. Queue implementation is private to the driver.
#define IC_GET_PEEK 1u
typedef struct IC_GET_SELECT {IC_GET base;uint32_t device,flags;} IC_GET_SELECT;
#if defined(__cplusplus)
static_assert(sizeof(IC_GET_SELECT)==16,"Selected GET request ABI");
#endif

// ---- internal/DeviceAvailability.h ----

// One interpretation for all six backends. Ready describes capture startup,
// never recent activity. A GIP report is required to validate its wire shape,
// not to prove the user moved a stick. An idle established source stays ready.
static __inline void IcDeviceAvailability(IC_DEVICE_QUERY_RESULT* result,uint32_t extra) {
    const uint32_t f=result->flags;uint32_t reasons=extra,state;
    if(!(f&1u) || extra) {
        if(result->type<=IC_TYPE_MOUSE) {
            if(!(f&16u))reasons|=IC_DEVICE_WAIT_CONNECTION;
            if(!(f&2u) || !(f&1u))reasons|=IC_DEVICE_WAIT_READER;
        } else {
            if(!(f&4u))reasons|=IC_DEVICE_WAIT_POWER;
            if(!(f&2u))reasons|=IC_DEVICE_WAIT_READER;
            if(result->type==IC_TYPE_CONTROLLER_GIP) {
                if(!(f&128u))reasons|=IC_DEVICE_WAIT_HELLO;
                if(!(f&512u))reasons|=IC_DEVICE_WAIT_METADATA;
                if(!(f&8u))reasons|=IC_DEVICE_WAIT_FIRST_REPORT;
                if((f&64u) || ((f&512u) && !(f&1024u)))reasons|=IC_DEVICE_PROFILE_REJECTED;
            } else if(result->type==IC_TYPE_CONTROLLER_HID || result->type==IC_TYPE_CONTROLLER_XINPUTHID) {
                if(!(f&16u))reasons|=IC_DEVICE_WAIT_PROFILE;
                if(f&64u)reasons|=IC_DEVICE_TRANSPARENT;
            } else if(!(f&1u) && !extra)reasons|=IC_DEVICE_WAIT_READER;
        }
    }
    if((f&1u) && !reasons)state=IC_DEVICE_READY;
    else if(reasons&IC_DEVICE_PROFILE_REJECTED)state=IC_DEVICE_UNSUPPORTED;
    else if(reasons&(IC_DEVICE_WAIT_POWER|IC_DEVICE_TRANSPARENT))state=IC_DEVICE_UNAVAILABLE;
    else state=IC_DEVICE_PREPARING;
    result->reserved[0]=state;result->reserved[1]=reasons;
    // In both query versions, bit 0 must agree with actual capture readiness.
    if(state!=IC_DEVICE_READY)result->flags&=~1u;
}

// ---- internal/ControllerXusbMirror.h ----
// Private source for the legacy HID child of a native XUSB gamepad. It does not
// expose a second capture queue: its lifetime and delivery follow XUSB ownership.
#define IC_XUSB_MIRROR_INTERFACE_GUID {0x4bdfd98d,0x5b54,0x4ba0,{0x9e,0x75,0x0b,0xc7,0x37,0xa8,0xbd,0x51}}
#define IOCTL_IC_XUSB_MIRROR_SOURCE IC_IOCTL(0xA15)
#define IC_XUSB_MIRROR_INSTANCE_CHARS 200u
#include <algorithm>
#include <SetupAPI.h>
#include <hidsdi.h>
#include <hidpi.h>

// ---- HidCodec.h ----
#include <hidsdi.h>
#include <hidpi.h>
#include <array>
namespace Interceptor::Detail {
// The replaceable parser boundary is used by pure tests. Production uses HidP.
struct Hid_Parser {
    decltype(&HidP_GetCaps) caps=HidP_GetCaps;
    decltype(&HidP_GetValueCaps) values=HidP_GetValueCaps;
    decltype(&HidP_GetButtonCaps) buttons=HidP_GetButtonCaps;
    decltype(&HidP_GetUsageValueArray) array=HidP_GetUsageValueArray;
    decltype(&HidP_GetData) data=HidP_GetData;
    decltype(&HidP_SetData) setData=HidP_SetData;
    decltype(&HidP_MaxDataListLength) maxData=HidP_MaxDataListLength;
    decltype(&HidP_GetLinkCollectionNodes) collections=HidP_GetLinkCollectionNodes;
};
inline Hid_Parser hidParser;
struct Hid_Codec {
    struct Value {HIDP_VALUE_CAPS cap;USHORT usage,index;std::uint32_t bit_offset=0;NTSTATUS array_status=HIDP_STATUS_SUCCESS;Hid_Value_Kind kind=Hid_Value_Kind::Value;};
    struct Button {USHORT page,usage,link;UCHAR report;USHORT index;bool trigger=false;};
    struct Report {
        std::vector<std::size_t> values,buttons;
        // Exact output reservations avoid geometric allocation/copy growth on
        // freshly returned public inputs. Index by Hid_Value_Kind, then binary.
        std::array<std::size_t,5> counts{};
    };
    std::vector<std::uint8_t> descriptor;
    std::vector<Value> valueDefs;
    std::vector<Button> buttonDefs;
    std::array<Report,256> reports;
    HIDP_CAPS caps{};
    ULONG maxDataCount=0;
    Hid_Parser parser;
    static std::shared_ptr<const Hid_Codec> Create(std::vector<std::uint8_t>);
    static void Decode(const std::shared_ptr<const Hid_Codec>&,Hid_Decoded_Input&,const std::uint8_t*,std::uint32_t,std::uint32_t);
    static DWORD Encode(const Hid_Decoded_Input&,std::uint8_t*,std::uint32_t,std::uint32_t);
};
}
#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"hid.lib")
#include <mutex>
#include <stdexcept>
#include <system_error>

namespace Interceptor {
namespace {
[[noreturn]] void Fail(const char* operation,DWORD error=GetLastError()) {
    if(error==IC_ERROR_LICENSE_REQUIRED)operation="Activate Interceptor Release before starting capture";
    if(error==IC_ERROR_TRIAL_EXPIRED)operation="Interceptor one-hour trial expired; restart Windows to use capture again";
    throw std::system_error(static_cast<int>(error),std::system_category(),operation);
}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    const EditionTarget* target=nullptr;
    ~Handle() { if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value); }
    Handle()=default;Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
bool MissingEndpoint(DWORD error) noexcept {
    return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND;
}
void Open(Handle& h) {
    const auto openTarget=[&](const EditionTarget& target){
        h.value=CreateFileW(target.endpoint,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h.value!=INVALID_HANDLE_VALUE)h.target=&target;
    };
    if(selectedTarget)openTarget(*selectedTarget);
    else {
        openTarget(ReleaseTarget);
        if(h.value==INVALID_HANDLE_VALUE && MissingEndpoint(GetLastError()) &&
           wcscmp(ReleaseTarget.endpoint,DemoTarget.endpoint)!=0)openTarget(DemoTarget);
    }
    if(h.value==INVALID_HANDLE_VALUE)Fail("Open Interceptor Demo or Release");
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
bool ValidateControllerCapabilities(HANDLE driver,std::uint8_t controllers,std::uint8_t inputs=0) {
    if(!(controllers&~Controller_Hid))return false; // Keep compatibility with ABI4 HID-only drivers.
    IC_CAPABILITIES capabilities{};
    try {capabilities=Capabilities(driver);}
    catch(const std::system_error& e) {
        if(e.code().value()==ERROR_INVALID_FUNCTION)Fail("Loaded driver has no native capabilities endpoint",ERROR_NOT_SUPPORTED);
        throw;
    }
    if((controllers&~Policy::SupportedControllers(capabilities)) || (inputs&~capabilities.input_mask))
        Fail("Controller profile unavailable in the loaded driver",ERROR_NOT_SUPPORTED);
    return Policy::UsbXusbBackend(capabilities);
}
SourceHandles OpenControllerSources(std::uint8_t controllers,bool availableOnly=false,bool usbXusb=false) {
    SourceHandles sources;
    const GUID sourceGuids[]={IC_HID_INTERFACE_GUID,IC_XUSB_INTERFACE_GUID,IC_XINPUTHID_INTERFACE_GUID,IC_XUSB_MIRROR_INTERFACE_GUID};
    const DWORD sourceIoctls[]={IOCTL_IC_HID_SOURCE,IOCTL_IC_XUSB_SOURCE,IOCTL_IC_XINPUTHID_SOURCE,IOCTL_IC_XUSB_MIRROR_SOURCE};
    const auto sourceFailure=[](unsigned native,const char* operation,DWORD error) {
        static constexpr const char* names[]={"Controller HID collection","XInput / XUSB parent",
            "XInputHID collection","XInput / XUSB mandatory HID mirror"};
        const auto message=std::string(names[native])+": "+operation+" (Win32 "+std::to_string(error)+")";
        Fail(message.c_str(),error);
    };
    // USB XUSB and GIP are driven by native USB IN requests. Readiness and
    // capture use the central DEVICE_QUERY / CREATE_ALL endpoints; opening a
    // lower filter's interface would traverse xusb22 and is not a source pump.
    // The legacy upper backend retains its old files for older installed ABI4
    // packages; it is never migrated into a lower filter by this SDK.
    for(unsigned native=0;native<4;++native)if(!(usbXusb&&(native==1||native==3)) &&
        (controllers&(native==3?Controller_Xinput:1u<<native))) {
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
                const bool optionalHid=native==0 || native==2;
                if(availableOnly && (error==ERROR_DEVICE_NOT_CONNECTED || error==ERROR_FILE_NOT_FOUND ||
                    (optionalHid && (error==ERROR_NOT_READY || error==ERROR_GEN_FAILURE))))continue;
                sourceFailure(native,"open controller source (CreateFile)",error);
            }
            try {Io(source->value,sourceIoctls[native],nullptr,0,nullptr,0);}
            catch(const std::system_error& error) {
                // An open collection file is also the shared reader's lifetime.
                // In CREATE_ALL, a temporarily unready HID/XInputHID collection
                // or XUSB mirror must stay OPEN so D0/reader preparation can
                // attach it later. Returning an empty set or throwing here
                // closes that file and prevents the pending reader progressing.
                // CREATE_ALL still gates capture on the driver's actual ready
                // state; retaining the file does not claim complete isolation.
                const auto code=static_cast<DWORD>(error.code().value());
                const bool optionalHid=native==0 || native==2;
                if(availableOnly && native!=1 && code==ERROR_NOT_READY) {
                    sources.push_back(std::move(source));continue;
                }
                if(optionalHid && code==ERROR_NOT_SUPPORTED)continue;
                if(availableOnly && code==ERROR_DEVICE_NOT_CONNECTED)continue;
                // A native parent's SOURCE query completes its bootstrap/arm
                // handshake, so its failures remain fatal, including NOT_READY.
                // Access, unsupported mirror and other real failures also keep
                // their exact operation/error and roll back the partial set.
                sourceFailure(native,"start controller source (SOURCE IOCTL)",code);
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
            const bool usbXusb=ValidateControllerCapabilities(driver.value,controllers);
            return OpenControllerSources(controllers,false,usbXusb);
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
    std::atomic<bool> multipleReady{false}; // Relaxed hint; Windows remains authoritative.
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
    if((mask&~IC_PRODUCT_INPUT_MASK) || (controllers&~IC_PRODUCT_CONTROLLER_MASK))
        throw std::invalid_argument("Create_Context: input family is excluded from this product");
    if(controllers&~15u)throw std::invalid_argument("Create_Context: invalid controller mask");
    if((!mask && !controllers) || (mask&~Both))throw std::invalid_argument("Create_Context: invalid input mask");
    Context_Result result;result.devices.reserve(IC_MAX_DEVICES);
    std::vector<std::uint64_t> response((sizeof(IC_CREATE_RESULT)+IC_MAX_DEVICES*sizeof(Device_Info)+7)/8);
    Context_Ptr c(new Context);c->mask=static_cast<std::uint8_t>(mask|((controllers&Controller_Hid)?IC_READY_HID:0)|((controllers&Controller_Xinput)?IC_READY_XUSB:0)|((controllers&Controller_XinputHid)?IC_READY_XINPUTHID:0)|((controllers&Controller_GIP)?IC_READY_GIP:0));
    Open(c->driver);
    const EditionScope scope(c->driver.target);
    if(all) {
        const auto capabilities=Capabilities(c->driver.value);
        mask=static_cast<std::uint8_t>(capabilities.input_mask&IC_PRODUCT_INPUT_MASK);
        controllers=static_cast<std::uint8_t>(Policy::SupportedControllers(capabilities)&IC_PRODUCT_CONTROLLER_MASK);
        c->mask=static_cast<std::uint8_t>(mask|(controllers<<2));
    }
    const bool usbXusb=ValidateControllerCapabilities(c->driver.value,controllers,mask);
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
        // Preserve the source's family, operation and original startup error.
        // Only the CREATE request itself needs availability-error refinement.
        if(!sourcesOpened){c->sources=OpenControllerSources(controllers,all,usbXusb);sourcesOpened=true;}
        try {
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
Context_Result Create_Context() {return CreateImpl(IC_PRODUCT_INPUT_MASK,IC_PRODUCT_CONTROLLER_MASK,Detail::Preparation_Options{},true);}
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

namespace Detail {IC_LICENSE_STATUS Activation_Status();}
namespace {
bool VerificationStatusValid(const IC_LICENSE_STATUS& status){
    const auto nonzero=[](const auto& bytes){for(auto value:bytes)if(value)return true;return false;};
    return status.size==sizeof(status)&&status.version==IC_LICENSE_VERSION&&status.state<=IC_LICENSE_ACTIVE&&
        (status.state!=IC_LICENSE_ACTIVE||(!status.error&&status.generation&&nonzero(status.key_id)&&nonzero(status.customer_id)));
}
bool ReadLicenseStatus(HANDLE handle,IC_LICENSE_STATUS& status) {
    DWORD bytes=0;
    if(!DeviceIoControl(handle,IOCTL_IC_LICENSE_STATUS,nullptr,0,&status,sizeof(status),&bytes,nullptr)){
        const auto error=GetLastError();
        if(error==ERROR_INVALID_FUNCTION || error==ERROR_NOT_SUPPORTED)return false;
        Fail("Read Interceptor activation status",error);
    }
    if(bytes!=sizeof(status) || status.size!=sizeof(status) || status.version!=IC_LICENSE_VERSION ||
       status.state>IC_LICENSE_ACTIVE)Fail("Invalid Interceptor activation status",ERROR_INVALID_DATA);
    return true;
}
}
IC_LICENSE_STATUS Get_License_Status() {
    Handle h;
    try{Open(h);}
    catch(const std::system_error& error){
        if(error.code().value()!=ERROR_ACCESS_DENIED)throw;
        return Detail::Activation_Status();
    }
    IC_LICENSE_STATUS status{};
    if(!ReadLicenseStatus(h.value,status))Fail("The current edition does not use activation keys",ERROR_NOT_SUPPORTED);
    return status;
}
Driver_Use_Status Get_Driver_Use_Status() {
    Handle h;Open(h);
    IC_STATUS protocol{};
    const auto bytes=Io(h.value,IOCTL_IC_STATUS,nullptr,0,&protocol,sizeof(protocol));
    if(bytes!=sizeof(protocol) || protocol.version!=IC_VERSION)Fail("Update the Interceptor driver",ERROR_REVISION_MISMATCH);
    IC_LICENSE_STATUS authorization{};
    if(ReadLicenseStatus(h.value,authorization))
        return {Driver_Edition::Release,authorization.state==IC_LICENSE_ACTIVE?
            Driver_Use_State::Ready:Driver_Use_State::ActivationRequired};
    IC_TRIAL_STATUS trial{};
    DWORD size=0;
    if(!DeviceIoControl(h.value,IOCTL_IC_TRIAL_STATUS,nullptr,0,&trial,sizeof(trial),&size,nullptr)){
        const auto error=GetLastError();
        if(error!=ERROR_INVALID_FUNCTION && error!=ERROR_NOT_SUPPORTED)Fail("Read Interceptor demo status",error);
        return {Driver_Edition::Demo,Driver_Use_State::Ready};
    }
    if(size!=sizeof(trial) || trial.size!=sizeof(trial) || trial.version!=IC_TRIAL_VERSION)
        Fail("Update the Interceptor demo driver",ERROR_REVISION_MISMATCH);
    return {Driver_Edition::Demo,trial.state==IC_TRIAL_EXPIRED?Driver_Use_State::TrialExpired:Driver_Use_State::Ready};
}

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
    // An empty tail needs one syscall. After finding another ready family,
    // individual checks avoid repeated multi-object scans of a dense tail.
    DWORD next=selected+1;
    if(count-next>1 && !c.multipleReady.load(std::memory_order_relaxed)) {
        const auto tail=WaitForMultipleObjects(count-next,events+next,FALSE,0);
        if(tail==WAIT_TIMEOUT)return ready;
        if(tail==WAIT_FAILED)Fail("WaitForMultipleObjects");
        const auto offset=tail-WAIT_OBJECT_0;
        if(offset>=count-next)Fail("Unexpected input wait result",ERROR_INVALID_DATA);
        next+=offset;ready|=streams[next++];
    }
    for(;next<count;++next)
        if(WaitForSingleObject(events[next],0)==WAIT_OBJECT_0)ready|=streams[next];
    c.multipleReady.store((ready&(ready-1))!=0,std::memory_order_relaxed);
    return ready;
}
}

// ---- HidCodec.inl ----
// Descriptor-driven SDK decoding. Never opens a device or starts a capture.
#include <cmath>
#include <limits>
#include <unordered_set>
namespace Interceptor::Detail {
namespace {
std::int64_t HidMinimum(const HIDP_VALUE_CAPS& c){return c.LogicalMin;}
std::int64_t HidMaximum(const HIDP_VALUE_CAPS& c){return c.LogicalMin<0?static_cast<std::int64_t>(c.LogicalMax):static_cast<std::uint32_t>(c.LogicalMax);}
std::int64_t HidSigned(std::uint32_t raw,unsigned bits,bool sign) {
    const auto mask=bits==32?0xffffffffull:(1ull<<bits)-1;
    const auto value=static_cast<std::uint64_t>(raw)&mask;
    return sign&&(value&(1ull<<(bits-1)))?static_cast<std::int64_t>(value)-static_cast<std::int64_t>(mask+1):static_cast<std::int64_t>(value);
}
double HidNormalized(std::int64_t value,std::int64_t minimum,std::int64_t maximum) {
    if(minimum<0&&maximum>0)return value<0?-static_cast<double>(value)/static_cast<double>(minimum):static_cast<double>(value)/static_cast<double>(maximum);
    return 2.0*static_cast<double>(value-minimum)/static_cast<double>(maximum-minimum)-1.0;
}
std::int64_t HidLogical(double value,std::int64_t minimum,std::int64_t maximum) {
    const double raw=minimum<0&&maximum>0?(value<0?-value*static_cast<double>(minimum):value*static_cast<double>(maximum)):
        static_cast<double>(minimum)+(value+1.0)*0.5*static_cast<double>(maximum-minimum);
    return static_cast<std::int64_t>(std::llround(raw));
}
std::uint32_t HidBits(const std::uint8_t* bytes,std::size_t bit,unsigned count) {
    // At most five byte loads for a possibly unaligned 32-bit field. No
    // unaligned typed loads, full-word overreads or host-endian assumptions.
    std::uint64_t raw=0;const auto shift=static_cast<unsigned>(bit%8);
    for(unsigned i=0;i<(shift+count+7)/8;++i)raw|=static_cast<std::uint64_t>(bytes[bit/8+i])<<(i*8);
    return static_cast<std::uint32_t>((raw>>shift)&((1ull<<count)-1));
}
std::uint32_t HidBits(const std::vector<std::uint8_t>& bytes,std::size_t bit,unsigned count) {return HidBits(bytes.data(),bit,count);}
void HidWriteBits(std::uint8_t* bytes,std::size_t bit,unsigned count,std::uint32_t value) {
    while(count) {
        const auto shift=static_cast<unsigned>(bit%8),take=(std::min)(8u-shift,count);
        const auto mask=static_cast<std::uint8_t>(((1u<<take)-1)<<shift);
        auto& byte=bytes[bit/8];byte=static_cast<std::uint8_t>((byte&~mask)|((value<<shift)&mask));
        value>>=take;bit+=take;count-=take;
    }
}
bool HidTriggerUsage(USHORT page,USHORT usage) {return page==2&&usage==0xC0;}
std::vector<std::uint8_t> HidTriggerCollections(const std::vector<HIDP_LINK_COLLECTION_NODE>& nodes) {
    // Resolve each parent edge once, including malformed cycles. Walking the
    // same deep ancestry for every usage would multiply descriptor setup cost.
    std::vector<std::uint8_t> flags(nodes.size());std::vector<USHORT> path;
    for(std::size_t root=0;root<nodes.size();++root) {
        if(flags[root])continue;
        path.clear();auto link=root;bool trigger=false;
        while(link<nodes.size()&&!flags[link]) {
            flags[link]=3;path.push_back(static_cast<USHORT>(link));const auto& node=nodes[link];
            if(node.LinkUsagePage==5&&node.LinkUsage==0x39){trigger=true;break;}
            if(!link||node.Parent==link)break;
            link=node.Parent;
        }
        if(link<nodes.size()&&flags[link]==2)trigger=true;
        for(const auto item:path)flags[item]=trigger?2:1;
    }
    return flags;
}
bool HidTriggerCollection(const std::vector<std::uint8_t>& flags,USHORT link) {return link<flags.size()&&flags[link]==2;}
DWORD HidParseError(NTSTATUS status){return status==HIDP_STATUS_NOT_IMPLEMENTED||status==HIDP_STATUS_NOT_VALUE_ARRAY?ERROR_NOT_SUPPORTED:ERROR_INVALID_DATA;}
}
std::shared_ptr<const Hid_Codec> Hid_Codec::Create(std::vector<std::uint8_t> bytes) {
    auto codec=std::make_shared<Hid_Codec>();codec->descriptor=std::move(bytes);codec->parser=hidParser;
    if(codec->descriptor.empty())Fail("Empty HID parser data",ERROR_INVALID_DATA);
    auto data=reinterpret_cast<PHIDP_PREPARSED_DATA>(codec->descriptor.data());
    if(codec->parser.caps(data,&codec->caps)!=HIDP_STATUS_SUCCESS||!codec->caps.InputReportByteLength||codec->caps.InputReportByteLength>IC_HID_REPORT_MAX)
        Fail("Invalid HID parser data",ERROR_INVALID_DATA);
    std::vector<HIDP_LINK_COLLECTION_NODE> collections(codec->caps.NumberLinkCollectionNodes);
    if(!collections.empty()) {
        ULONG count=static_cast<ULONG>(collections.size());
        if(codec->parser.collections(collections.data(),&count,data)!=HIDP_STATUS_SUCCESS||count>collections.size())
            Fail("HID collection capabilities",ERROR_INVALID_DATA);
        collections.resize(count);
    }
    const auto triggerCollections=HidTriggerCollections(collections);
    codec->maxDataCount=codec->parser.maxData(HidP_Input,data);
    if(codec->maxDataCount>65536)Fail("Invalid HID data count",ERROR_INVALID_DATA);
    USHORT count=codec->caps.NumberInputValueCaps;std::vector<HIDP_VALUE_CAPS> values(count);
    if(count&&(codec->parser.values(HidP_Input,values.data(),&count,data)!=HIDP_STATUS_SUCCESS||count>values.size()))Fail("HID value capabilities",ERROR_INVALID_DATA);
    values.resize(count);std::unordered_set<std::uint64_t> known;
    for(const auto& cap:values) {
        if(cap.IsAlias)continue;
        if(!cap.BitSize||!cap.ReportCount||
            (cap.IsRange&&(cap.Range.UsageMax<cap.Range.UsageMin||cap.Range.DataIndexMax<cap.Range.DataIndexMin||
             cap.Range.UsageMax-cap.Range.UsageMin!=cap.Range.DataIndexMax-cap.Range.DataIndexMin)))Fail("Invalid HID value range",ERROR_INVALID_DATA);
        const unsigned first=cap.IsRange?cap.Range.UsageMin:cap.NotRange.Usage,last=cap.IsRange?cap.Range.UsageMax:first;
        for(unsigned usage=first;usage<=last;++usage) {
            const auto index=cap.IsRange?static_cast<USHORT>(cap.Range.DataIndexMin+usage-first):cap.NotRange.DataIndex;
            if(!known.insert((static_cast<std::uint64_t>(cap.ReportID)<<32)|index).second)continue;
            if(codec->valueDefs.size()>=262144)Fail("HID capability expansion too large",ERROR_NOT_SUPPORTED);
            Hid_Value_Kind kind=Hid_Value_Kind::Value;
            if(HidTriggerUsage(cap.UsagePage,static_cast<USHORT>(usage))||HidTriggerCollection(triggerCollections,cap.LinkCollection))kind=Hid_Value_Kind::Trigger;
            else if(cap.UsagePage==1&&usage>=0x30&&usage<=0x38)kind=Hid_Value_Kind::Axis;
            else if(cap.UsagePage==1&&usage==0x39)kind=Hid_Value_Kind::Hat;
            codec->reports[cap.ReportID].values.push_back(codec->valueDefs.size());
            auto& reservation=codec->reports[cap.ReportID].counts[cap.BitSize>32?4:static_cast<std::size_t>(kind)];
            reservation+=cap.BitSize>32?1u:cap.IsRange?1u:cap.ReportCount;
            if(reservation>static_cast<std::size_t>(codec->caps.InputReportByteLength)*8)Fail("HID scalar expansion too large",ERROR_INVALID_DATA);
            Value definition{cap,static_cast<USHORT>(usage),index};definition.kind=kind;codec->valueDefs.push_back(definition);
        }
    }
    count=codec->caps.NumberInputButtonCaps;std::vector<HIDP_BUTTON_CAPS> buttons(count);
    if(count&&(codec->parser.buttons(HidP_Input,buttons.data(),&count,data)!=HIDP_STATUS_SUCCESS||count>buttons.size()))Fail("HID button capabilities",ERROR_INVALID_DATA);
    buttons.resize(count);known.clear();
    for(const auto& cap:buttons) {
        if(cap.IsAlias)continue;
        if(cap.IsRange&&(cap.Range.UsageMax<cap.Range.UsageMin||cap.Range.DataIndexMax<cap.Range.DataIndexMin||
            cap.Range.UsageMax-cap.Range.UsageMin!=cap.Range.DataIndexMax-cap.Range.DataIndexMin))Fail("Invalid HID button range",ERROR_INVALID_DATA);
        const unsigned first=cap.IsRange?cap.Range.UsageMin:cap.NotRange.Usage,last=cap.IsRange?cap.Range.UsageMax:first;
        for(unsigned usage=first;usage<=last;++usage) {
            const auto index=cap.IsRange?static_cast<USHORT>(cap.Range.DataIndexMin+usage-first):cap.NotRange.DataIndex;
            if(!known.insert((static_cast<std::uint64_t>(cap.ReportID)<<32)|index).second)continue;
            if(codec->buttonDefs.size()>=262144)Fail("HID button expansion too large",ERROR_NOT_SUPPORTED);
            codec->reports[cap.ReportID].buttons.push_back(codec->buttonDefs.size());codec->buttonDefs.push_back({cap.UsagePage,static_cast<USHORT>(usage),cap.LinkCollection,cap.ReportID,index,HidTriggerUsage(cap.UsagePage,static_cast<USHORT>(usage))||HidTriggerCollection(triggerCollections,cap.LinkCollection)});
        }
    }
    // Locate array bits once using synthetic tagged reports, never hardware I/O.
    // Windows' SetUsageValueArray rejects element sizes not divisible by eight.
    // Validated offsets allow exact encoding of those arrays as well.
    const auto reportBits=static_cast<unsigned>(codec->caps.InputReportByteLength)*8;
    for(auto& def:codec->valueDefs) {
        const auto& c=def.cap;const auto arrayCount=c.IsRange?1u:c.ReportCount;
        if(arrayCount==1&&c.BitSize<=32)continue;
        const auto bits=static_cast<unsigned>(c.BitSize)*arrayCount;
        if(!bits||bits>reportBits){def.array_status=HIDP_STATUS_INVALID_REPORT_LENGTH;continue;}
        if(std::any_of(codec->valueDefs.begin(),codec->valueDefs.end(),[&](const Value& other){return &other!=&def&&(!other.cap.IsRange&&other.cap.ReportCount>1)&&(!c.LinkCollection||other.cap.LinkCollection==c.LinkCollection)&&other.cap.ReportID==c.ReportID&&other.cap.UsagePage==c.UsagePage&&other.usage==def.usage;})){
            def.array_status=HIDP_STATUS_NOT_IMPLEMENTED;continue;
        }
        std::vector<std::uint8_t> probe(codec->caps.InputReportByteLength),packed((bits+7)/8);
        unsigned start=0,planes=0;while((1u<<planes)<reportBits)++planes;
        std::vector<std::vector<std::uint8_t>> patterns;
        for(unsigned plane=0;plane<planes;++plane) {
            std::fill(probe.begin(),probe.end(),std::uint8_t{});probe[0]=c.ReportID;
            for(unsigned bit=8;bit<reportBits;++bit)if((bit>>plane)&1u)probe[bit/8]|=static_cast<std::uint8_t>(1u<<(bit%8));
            std::fill(packed.begin(),packed.end(),std::uint8_t{});
            def.array_status=codec->parser.array(HidP_Input,c.UsagePage,c.LinkCollection,def.usage,reinterpret_cast<PCHAR>(packed.data()),static_cast<USHORT>(packed.size()),data,reinterpret_cast<PCHAR>(probe.data()),static_cast<ULONG>(probe.size()));
            if(def.array_status!=HIDP_STATUS_SUCCESS)break;
            if(packed[0]&1u)start|=1u<<plane;
            patterns.push_back(packed);
        }
        if(def.array_status!=HIDP_STATUS_SUCCESS)continue;
        if(start<8||start+bits>reportBits){def.array_status=HIDP_STATUS_INVALID_PREPARSED_DATA;continue;}
        for(unsigned plane=0;plane<planes;++plane)for(unsigned bit=0;bit<bits;++bit)
            if(HidBits(patterns[plane],bit,1)!=(((start+bit)>>plane)&1u))def.array_status=HIDP_STATUS_INVALID_PREPARSED_DATA;
        def.bit_offset=start;
    }
    return codec;
}
void Hid_Codec::Decode(const std::shared_ptr<const Hid_Codec>& codec,Hid_Decoded_Input& out,const std::uint8_t* report,std::uint32_t length,std::uint32_t source) {
    auto& state=out.state;state.axes.clear();state.triggers.clear();state.hats.clear();state.values.clear();state.buttons.clear();state.binary_values.clear();out.decode_complete=false;out.decode_error=ERROR_SUCCESS;out.parser_status=0;
    out.codec_=codec;out.source_id_=source;
    if(!codec||!report||length!=codec->caps.InputReportByteLength){out.decode_error=ERROR_INVALID_DATA;return;}
    out.report_id_=report[0];const auto& layout=codec->reports[report[0]];
    if(layout.values.empty()&&layout.buttons.empty()){out.decode_error=ERROR_NOT_SUPPORTED;return;}
    state.axes.reserve(layout.counts[static_cast<std::size_t>(Hid_Value_Kind::Axis)]);
    state.triggers.reserve(layout.counts[static_cast<std::size_t>(Hid_Value_Kind::Trigger)]);
    state.hats.reserve(layout.counts[static_cast<std::size_t>(Hid_Value_Kind::Hat)]);
    state.values.reserve(layout.counts[static_cast<std::size_t>(Hid_Value_Kind::Value)]);
    state.buttons.reserve(layout.buttons.size());state.binary_values.reserve(layout.counts[4]);
    auto data=reinterpret_cast<PHIDP_PREPARSED_DATA>(const_cast<std::uint8_t*>(codec->descriptor.data()));
    auto buffer=reinterpret_cast<PCHAR>(const_cast<std::uint8_t*>(report));
    const auto failure=[&](NTSTATUS status){if(!out.decode_error){out.decode_error=HidParseError(status);out.parser_status=static_cast<std::uint32_t>(status);}};
    thread_local std::vector<HIDP_DATA> parsed;parsed.resize(codec->maxDataCount);ULONG parsedCount=codec->maxDataCount;
    const auto dataStatus=codec->maxDataCount?codec->parser.data(HidP_Input,parsed.data(),&parsedCount,data,buffer,length):HIDP_STATUS_SUCCESS;
    bool dataValid=dataStatus==HIDP_STATUS_SUCCESS&&parsedCount<=parsed.size();
    if(!dataValid){failure(dataStatus==HIDP_STATUS_SUCCESS?HIDP_STATUS_BUFFER_TOO_SMALL:dataStatus);parsedCount=0;}
    parsed.resize(parsedCount);
    const auto byIndex=[](const HIDP_DATA& a,const HIDP_DATA& b){return a.DataIndex<b.DataIndex;};
    if(!std::is_sorted(parsed.begin(),parsed.end(),byIndex))std::sort(parsed.begin(),parsed.end(),byIndex);
    if(std::adjacent_find(parsed.begin(),parsed.end(),[](const HIDP_DATA& a,const HIDP_DATA& b){return a.DataIndex==b.DataIndex;})!=parsed.end()) {
        failure(HIDP_STATUS_INVALID_PREPARSED_DATA);parsed.clear();dataValid=false;
    }
    std::size_t cursor=0;USHORT previous=0;
    const auto field=[&](USHORT index)->const HIDP_DATA* {
        // Sequential data-index queries are linear overall. Nonsequential
        // layouts use a bounded scan followed by logarithmic lookup.
        if(index<previous)cursor=0;previous=index;
        unsigned scanned=0;while(cursor<parsed.size()&&parsed[cursor].DataIndex<index&&scanned++<4)++cursor;
        if(cursor<parsed.size()&&parsed[cursor].DataIndex<index)cursor=static_cast<std::size_t>(std::lower_bound(parsed.begin()+cursor,parsed.end(),index,[](const HIDP_DATA& item,USHORT wanted){return item.DataIndex<wanted;})-parsed.begin());
        return cursor<parsed.size()&&parsed[cursor].DataIndex==index?&parsed[cursor]:nullptr;
    };
    for(const auto index:layout.values) {
        const auto& def=codec->valueDefs[index];const auto& c=def.cap;
        const unsigned count=c.IsRange?1u:c.ReportCount;
        const auto bits=static_cast<std::size_t>(count)*c.BitSize;
        if(bits>static_cast<std::size_t>(length)*8){failure(HIDP_STATUS_INVALID_REPORT_LENGTH);continue;}
        ULONG raw=0;NTSTATUS status=HIDP_STATUS_SUCCESS;
        if(count>1||c.BitSize>32){status=def.array_status;if(status==HIDP_STATUS_SUCCESS&&def.bit_offset+bits>static_cast<std::size_t>(length)*8)status=HIDP_STATUS_INVALID_REPORT_LENGTH;}
        else {const auto* value=field(def.index);if(value)raw=value->RawValue;else status=HIDP_STATUS_DATA_INDEX_NOT_FOUND;}
        if(status!=HIDP_STATUS_SUCCESS){failure(status);continue;}
        if(c.BitSize>32) {
            Hid_Binary_Value value;value.usage_page=c.UsagePage;value.usage=def.usage;value.link_collection=c.LinkCollection;value.report_id=c.ReportID;value.bit_size=c.BitSize;value.count=count;value.data_index=def.index;
            value.definition_=index;value.data.resize((bits+7)/8);
            for(std::size_t bit=0;bit<bits;bit+=8)value.data[bit/8]=static_cast<std::uint8_t>(HidBits(report,def.bit_offset+bit,static_cast<unsigned>((std::min)(std::size_t{8},bits-bit))));
            value.original_=value.data;state.binary_values.push_back(std::move(value));continue;
        }
        for(unsigned element=0;element<count;++element) {
            Hid_Value v;v.usage_page=c.UsagePage;v.usage=def.usage;v.link_collection=c.LinkCollection;v.report_id=c.ReportID;v.element=element;v.bit_size=c.BitSize;v.data_index=def.index;
            v.relative=!c.IsAbsolute;v.has_null=c.HasNull!=FALSE;v.minimum=HidMinimum(c);v.maximum=HidMaximum(c);
            v.physical_minimum=c.PhysicalMin;v.physical_maximum=c.PhysicalMin<0?static_cast<std::int64_t>(c.PhysicalMax):static_cast<std::uint32_t>(c.PhysicalMax);
            v.units=c.Units;v.units_exponent=c.UnitsExp;
            v.value=HidSigned(count>1?HidBits(report,def.bit_offset+static_cast<std::size_t>(element)*c.BitSize,c.BitSize):raw,c.BitSize,c.LogicalMin<0);
            v.original_=v.value;v.definition_=static_cast<std::uint32_t>(index);v.original_element_=element;v.is_null=v.has_null&&(v.value<v.minimum||v.value>v.maximum);
            v.kind=def.kind;
            const bool valid=!v.is_null&&v.value>=v.minimum&&v.value<=v.maximum;
            if(!v.is_null&&!valid)failure(HIDP_STATUS_VALUE_OUT_OF_RANGE);
            v.normalized_valid=valid&&!v.relative&&v.maximum>v.minimum&&(v.kind==Hid_Value_Kind::Axis||v.kind==Hid_Value_Kind::Trigger);
            if(v.normalized_valid)v.normalized=v.kind==Hid_Value_Kind::Trigger?
                static_cast<double>(v.value-v.minimum)/static_cast<double>(v.maximum-v.minimum):HidNormalized(v.value,v.minimum,v.maximum);
            v.original_normalized_=v.normalized;
            auto& destination=v.kind==Hid_Value_Kind::Axis?state.axes:v.kind==Hid_Value_Kind::Trigger?state.triggers:v.kind==Hid_Value_Kind::Hat?state.hats:state.values;
            destination.push_back(v);
        }
    }
    if(dataValid)for(const auto i:layout.buttons) {
        const auto& button=codec->buttonDefs[i];const auto* value=field(button.index);
        Hid_Button b;b.usage_page=button.page;b.usage=button.usage;b.link_collection=button.link;b.report_id=button.report;b.data_index=button.index;
        b.trigger=button.trigger;b.pressed=value&&value->On;b.original_=b.pressed;b.definition_=i;state.buttons.push_back(b);
    }
    out.decode_complete=out.decode_error==ERROR_SUCCESS;
}
std::shared_ptr<const Hid_Codec> Create_Hid_Decoder(std::vector<std::uint8_t> preparsed) {
    if(preparsed.empty()||preparsed.size()>65536)Fail("Invalid passive HID parser data size",ERROR_INVALID_DATA);
    return Hid_Codec::Create(std::move(preparsed));
}
void Decode_Hid_Input(const std::shared_ptr<const Hid_Codec>& decoder,Hid_Decoded_Input& output,
                      const std::uint8_t* report,std::uint32_t length,std::uint32_t source) {
    if(length>IC_HID_REPORT_MAX){Hid_Input_Access::Clear(output);output.decode_error=ERROR_INVALID_DATA;return;}
    Hid_Codec::Decode(decoder,output,report,length,source);
}
DWORD Hid_Codec::Encode(const Hid_Decoded_Input& in,std::uint8_t* report,std::uint32_t length,std::uint32_t source) {
    const auto& state=in.state;
    if(!in.codec_) {
        if(!state.axes.empty()||!state.triggers.empty()||!state.hats.empty()||!state.values.empty()||!state.buttons.empty()||!state.binary_values.empty())return ERROR_INVALID_DATA;
        return ERROR_SUCCESS; // Explicit raw reports remain supported.
    }
    const auto& codec=*in.codec_;
    if(!report||length!=codec.caps.InputReportByteLength||report[0]!=in.report_id_||source!=in.source_id_)return ERROR_INVALID_DATA;
    auto data=reinterpret_cast<PHIDP_PREPARSED_DATA>(const_cast<std::uint8_t*>(codec.descriptor.data()));auto buffer=reinterpret_cast<PCHAR>(report);
    thread_local std::vector<HIDP_DATA> scalarEdits;scalarEdits.clear();
    // Each control is authoritative in one dynamic state list. Only edited
    // fields are encoded; unknown bytes and explicit raw edits remain intact.
    for(const auto* list:{&state.axes,&state.triggers,&state.hats,&state.values})for(const auto& v:*list) {
        auto value=v.value;
        if(v.element!=v.original_element_)return ERROR_INVALID_DATA;
        if(value==v.original_&&v.normalized==v.original_normalized_)continue;
        if(v.definition_>=codec.valueDefs.size())return ERROR_INVALID_DATA;
        const auto& def=codec.valueDefs[v.definition_];const auto& c=def.cap;
        if(c.ReportID!=report[0]||!c.BitSize||c.BitSize>32||v.element!=v.original_element_||
            v.usage_page!=c.UsagePage||v.usage!=def.usage||v.link_collection!=c.LinkCollection||v.report_id!=c.ReportID||
            v.bit_size!=c.BitSize||v.data_index!=def.index||v.kind!=def.kind)return ERROR_INVALID_DATA;
        const auto minimum=HidMinimum(c),maximum=HidMaximum(c);
        if(v.normalized!=v.original_normalized_) {
            const bool trigger=def.kind==Hid_Value_Kind::Trigger;
            if((!trigger&&def.kind!=Hid_Value_Kind::Axis)||!c.IsAbsolute||maximum<=minimum||
                !v.normalized_valid||!std::isfinite(v.normalized)||v.normalized<(trigger?0.0:-1.0)||v.normalized>1.0)return ERROR_INVALID_PARAMETER;
            const auto normalized=trigger?static_cast<std::int64_t>(std::llround(static_cast<double>(minimum)+v.normalized*static_cast<double>(maximum-minimum))):HidLogical(v.normalized,minimum,maximum);
            if(value!=v.original_&&value!=normalized)return ERROR_INVALID_PARAMETER;
            value=normalized;
        }
        if(value==v.original_)continue;
        const auto representableMin=c.LogicalMin<0?-(1ll<<(c.BitSize-1)):0;
        const auto representableMax=c.LogicalMin<0?(1ll<<(c.BitSize-1))-1:static_cast<std::int64_t>((1ull<<c.BitSize)-1);
        if(value<representableMin||value>representableMax||(!c.HasNull&&(value<minimum||value>maximum)))return ERROR_INVALID_PARAMETER;
        const unsigned count=c.IsRange?1u:c.ReportCount;
        if(count>1) {
            const auto bits=static_cast<std::size_t>(count)*c.BitSize;if(v.element>=count||bits>static_cast<std::size_t>(length)*8)return ERROR_INVALID_DATA;
            if(def.array_status!=HIDP_STATUS_SUCCESS||def.bit_offset+bits>static_cast<std::size_t>(length)*8)return ERROR_INVALID_DATA;
            const auto bit=def.bit_offset+static_cast<std::size_t>(v.element)*c.BitSize;
            HidWriteBits(report,bit,c.BitSize,static_cast<std::uint32_t>(value));
        } else {HIDP_DATA item{};item.DataIndex=def.index;item.RawValue=static_cast<ULONG>(value);scalarEdits.push_back(item);}
    }
    if(!scalarEdits.empty()) {
        ULONG count=static_cast<ULONG>(scalarEdits.size());
        const auto status=codec.parser.setData(HidP_Input,scalarEdits.data(),&count,data,buffer,length);
        if(status!=HIDP_STATUS_SUCCESS)return HidParseError(status);
        if(count!=scalarEdits.size())return ERROR_INVALID_DATA;
    }
    for(const auto& v:state.binary_values) {
        if(v.data==v.original_)continue;
        if(v.definition_>=codec.valueDefs.size()||v.data.size()!=v.original_.size())return ERROR_INVALID_DATA;
        const auto& def=codec.valueDefs[v.definition_];const auto& c=def.cap;
        const auto bits=static_cast<std::size_t>(c.IsRange?1u:c.ReportCount)*c.BitSize;
        if(c.ReportID!=report[0]||v.data.size()!=(bits+7)/8||bits>static_cast<std::size_t>(length)*8||
            v.usage_page!=c.UsagePage||v.usage!=def.usage||v.link_collection!=c.LinkCollection||v.report_id!=c.ReportID||
            v.bit_size!=c.BitSize||v.data_index!=def.index||v.count!=(c.IsRange?1u:c.ReportCount))return ERROR_INVALID_DATA;
        if(bits%8&&((v.data.back()^v.original_.back())&static_cast<std::uint8_t>(0xffu<<(bits%8))))return ERROR_INVALID_PARAMETER;
        if(def.array_status!=HIDP_STATUS_SUCCESS||def.bit_offset+bits>static_cast<std::size_t>(length)*8)return ERROR_INVALID_DATA;
        for(std::size_t n=0;n<bits;n+=8)HidWriteBits(report,def.bit_offset+n,static_cast<unsigned>((std::min)(std::size_t{8},bits-n)),v.data[n/8]);
    }
    // One scan: release immediately, queue presses until all slots are free.
    // The scalar scratch is no longer needed and can hold the pending presses.
    scalarEdits.clear();
    for(const auto& b:state.buttons) {
        if(b.pressed==b.original_)continue;
        if(b.definition_>=codec.buttonDefs.size())return ERROR_INVALID_DATA;
        const auto& def=codec.buttonDefs[b.definition_];
        if(def.report!=report[0]||b.usage_page!=def.page||b.usage!=def.usage||b.link_collection!=def.link||
            b.report_id!=def.report||b.data_index!=def.index||b.trigger!=def.trigger)return ERROR_INVALID_DATA;
        HIDP_DATA item{};item.DataIndex=def.index;item.On=b.pressed?TRUE:FALSE;
        if(b.pressed){scalarEdits.push_back(item);continue;}
        ULONG count=1;
        const auto status=codec.parser.setData(HidP_Input,&item,&count,data,buffer,length);
        if(status!=HIDP_STATUS_SUCCESS&&status!=HIDP_STATUS_BUTTON_NOT_PRESSED)return HidParseError(status);
        if(status==HIDP_STATUS_SUCCESS&&count!=1)return ERROR_INVALID_DATA;
    }
    if(!scalarEdits.empty()) {
        ULONG count=static_cast<ULONG>(scalarEdits.size());
        const auto status=codec.parser.setData(HidP_Input,scalarEdits.data(),&count,data,buffer,length);
        if(status!=HIDP_STATUS_SUCCESS)return HidParseError(status);
        if(count!=scalarEdits.size())return ERROR_INVALID_DATA;
    }
    return ERROR_SUCCESS;
}
}

// ---- AutomaticInput.inl ----
// Included once by Interceptor.cpp, after the explicit-context implementation.
#include <array>
#include <bitset>
#include <exception>
#include <cstring>
#include <objbase.h>
#pragma comment(lib,"ole32.lib")

namespace Interceptor {
namespace Detail {
struct Captured_Device : Device {
    std::uint32_t source=0;
};
Device PublicDevice(const Device_Info& info) {
    Device device;
    device.id=info.id;device.type=info.type;device.flags=info.flags;
    device.vid=info.vid;device.pid=info.pid;device.revision=info.revision;
    device.usage_page=info.usage_page;device.usage=info.usage;
    device.input_bytes=info.input_bytes;device.output_bytes=info.output_bytes;device.feature_bytes=info.feature_bytes;
    std::copy(std::begin(info.name),std::end(info.name),std::begin(device.name));
    std::copy(std::begin(info.hardware_id),std::end(info.hardware_id),std::begin(device.hardware_id));
    return device;
}
struct Automatic_State {
    struct Codec_Cache {std::string token;std::shared_ptr<const Hid_Codec> codec;DWORD error=ERROR_SUCCESS;};
    std::recursive_mutex mutex;
    Context_Result capture;
    Input_Processing_Storage storage;
    std::vector<Input_Event> events;
    std::vector<Captured_Device> devices;
    std::string token;
    std::array<std::array<int,IC_MAX_DEVICES+1>,IC_STREAM_COUNT> deviceIndex{};
    std::array<std::array<Codec_Cache,IC_MAX_DEVICES+1>,2> codecs;
    std::uint64_t batch=0;
    bool activeBatch=false;
    DWORD fault=ERROR_SUCCESS;

    void IndexDevices() noexcept {
        for(auto& family:deviceIndex)family.fill(-1);
        for(std::size_t i=0;i<devices.size();++i) {
            const auto& d=devices[i];
            if(d.type>=1 && d.type<=IC_STREAM_COUNT && d.id<=IC_MAX_DEVICES)
                deviceIndex[d.type-1][d.id]=static_cast<int>(i);
        }
    }
    const Captured_Device* Find(Device_Type type,std::uint32_t id) const noexcept {
        const auto family=static_cast<std::uint32_t>(type);
        if(id<=IC_MAX_DEVICES) {
            const auto index=deviceIndex[family-1][id];
            return index<0?nullptr:&devices[static_cast<std::size_t>(index)];
        }
        for(const auto& d:devices)if(d.type==family && d.id==id)return &d;
        return nullptr;
    }
    Context& Ensure() {
        if(fault)Fail("Automatic capture stopped; release it with Destroy_Context before restarting",fault);
        if(!capture) {
            auto created=Create_Context();
            GUID guid{};
            if(FAILED(CoCreateGuid(&guid)))Fail("Create capture identity",ERROR_GEN_FAILURE);
            static constexpr char hex[]="0123456789abcdef";
            std::string nextToken="ic:capture:";nextToken.reserve(nextToken.size()+32);
            std::array<std::uint8_t,sizeof(GUID)> guidBytes{};std::memcpy(guidBytes.data(),&guid,sizeof(guid));
            for(auto byte:guidBytes) {
                nextToken.push_back(hex[byte>>4]);nextToken.push_back(hex[byte&15]);
            }
            auto nextDevices=Identify(created.devices,nextToken);
            token=std::move(nextToken);devices=std::move(nextDevices);
            for(auto& family:codecs)for(auto& cached:family)cached=Codec_Cache{};
            IndexDevices();
            capture=std::move(created);
        }
        return capture.Get();
    }
    std::vector<Captured_Device> Identify(const std::vector<Device_Info>& source,const std::string& session) const {
        std::vector<Captured_Device> result;result.reserve(source.size());
        std::bitset<IC_STREAM_COUNT*(IC_MAX_DEVICES+1)> seen;
        for(const auto& info:source) {
            if(!info.id || info.id>IC_MAX_DEVICES || !info.instance_token || info.type<1 || info.type>IC_STREAM_COUNT ||
                !((IC_PRODUCT_INPUT_MASK|(IC_PRODUCT_CONTROLLER_MASK<<2))&(1u<<(info.type-1))))
                Fail("Invalid captured device identity",ERROR_INVALID_DATA);
            const auto slot=(info.type-1)*(IC_MAX_DEVICES+1)+info.id;
            if(seen.test(slot))Fail("Duplicate captured device identity",ERROR_INVALID_DATA);
            seen.set(slot);
            const auto* known=capture?Find(static_cast<Device_Type>(info.type),info.id):nullptr;
            Captured_Device device;static_cast<Device&>(device)=PublicDevice(info);device.source=info.instance_token;
            device.token=known && known->source==info.instance_token?known->token:
                session+":"+std::to_string(info.type)+":"+std::to_string(info.instance_token)+":"+std::to_string(info.id);
            // The token contains family, source identity and local ID. Unique
            // (family, ID) pairs therefore cannot produce duplicate tokens.
            result.push_back(std::move(device));
        }
        return result;
    }
    std::vector<Captured_Device> SnapshotDevices(Context& context) {
        auto snapshot=Identify(Get_Devices(context),token);
        if(!activeBatch){devices=snapshot;IndexDevices();}
        return snapshot;
    }
    void ReleaseResources() noexcept {
        storage=Input_Processing_Storage{};
        std::vector<Input_Event>{}.swap(events);
        std::vector<Captured_Device>{}.swap(devices);
        std::string{}.swap(token);
        for(auto& family:codecs)for(auto& cached:family)cached=Codec_Cache{};
        IndexDevices();
    }
    // A failure must not silently reacquire interception on the next call.
    void Abort(DWORD error) noexcept {
        fault=error?error:ERROR_GEN_FAILURE;
        Destroy_Context(capture);
        if(!activeBatch)ReleaseResources();
    }
    void AbortException() noexcept {
        try {throw;}
        catch(const std::system_error& e){Abort(static_cast<DWORD>(e.code().value()));}
        catch(...){Abort(ERROR_GEN_FAILURE);}
    }
    void RequireReader() const {
        if(activeBatch)Fail("Finish the previous Input_Batch before receiving again",ERROR_BUSY);
    }
    template<class T> void DecodeInputs(Context& context,std::vector<T>& inputs) {
        if constexpr(DecodedHid<T>) {
            constexpr auto type=std::is_same_v<T,Controller_Hid_Input>?Device_Type::Controller_Hid:Device_Type::Controller_XinputHid;
            bool refreshed=false;
            for(auto& input:inputs) {
                const auto* device=Find(type,input.device);
                if(!device&&!refreshed){(void)SnapshotDevices(context);refreshed=true;device=Find(type,input.device);}
                if(!device||!input.device||input.device>IC_MAX_DEVICES){input.decode_error=ERROR_NOT_FOUND;continue;}
                input.token=device->token;auto& cache=codecs[std::is_same_v<T,Controller_Hid_Input>?0:1][input.device];
                if(cache.token!=device->token) {
                    cache=Codec_Cache{};cache.token=device->token;
                    try {
                        auto descriptor=[&]{if constexpr(std::is_same_v<T,Controller_Hid_Input>)return Get_Controller_Hid_Descriptor(context,input.device);else return Get_Controller_XinputHid_Descriptor(context,input.device);}();
                        cache.codec=Hid_Codec::Create(std::move(descriptor));
                    }catch(const std::system_error& error){cache.error=static_cast<DWORD>(error.code().value());}
                }
                if(cache.error){input.decode_error=cache.error;continue;}
                Hid_Codec::Decode(cache.codec,input,input.report,input.length,input.device);
            }
        }
    }
    template<class T> bool ValidTokens(Detail::Input_View<const T> inputs) const {
        if constexpr(DecodedHid<T>)for(const auto& input:inputs)if(!input.token.empty()) {
            const auto* device=Find(std::is_same_v<T,Controller_Hid_Input>?Device_Type::Controller_Hid:Device_Type::Controller_XinputHid,input.device);
            if(!device||device->token!=input.token)return false;
        }
        return true;
    }
    template<class T> void Append(std::vector<T>& inputs,Device_Type type) {
        for(auto& input:inputs) {
            Input_Event event;
            const auto& header=[&]() -> const auto& {
                if constexpr(std::is_same_v<T,Controller_GIP_Raw_Input>)return input.input;
                else return input;
            }();
            event.payload_=&input;event.owner_=this;event.batch_=batch;
            event.index_=events.size();event.type=type;event.device=header.device;
            event.sequence=header.sequence;event.timestamp=header.timestamp;
            events.push_back(event);
        }
    }
    Input_Batch Receive(std::unique_lock<std::recursive_mutex> lock,DWORD timeout,HANDLE stop) {
        RequireReader();
        auto& context=Ensure();
        try {
            const auto ready=Wait_For_Input(context,timeout,stop);
            if(!ready)return {};
            // Exactly the signalled families are read. GET buffers and the
            // descriptor array retain their capacity between calls.
            events.clear();++batch;
#define IC_AUTO_READ(bit,Name,buffer,type) if(ready&(bit)) {Get_##Name##_Input(context,storage.buffer);DecodeInputs(context,storage.buffer);Append(storage.buffer,Device_Type::type);}
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE
            IC_AUTO_READ(Keyboard,Keyboard,keyboard,Keyboard);
            IC_AUTO_READ(Mouse,Mouse,mouse,Mouse);
#endif
#if IC_PRODUCT_HAS_CONTROLLERS
            IC_AUTO_READ(Ready_Controller_Hid,Controller_Hid,hid,Controller_Hid);
            IC_AUTO_READ(Ready_Controller_Xinput,Controller_Xinput,xusb,Controller_Xinput);
            IC_AUTO_READ(Ready_Controller_XinputHid,Controller_XinputHid,xinputhid,Controller_XinputHid);
            if(context.gipRawSupported) {IC_AUTO_READ(Ready_Controller_GIP,Controller_GIP_Raw,gipRaw,Controller_GIP);}
            else {IC_AUTO_READ(Ready_Controller_GIP,Controller_GIP,gip,Controller_GIP);}
#endif
#undef IC_AUTO_READ
            if(events.empty())return {};
            bool refresh=false;
            for(const auto& event:events)if(!Find(event.type,event.device)){refresh=true;break;}
            if(refresh){(void)SnapshotDevices(context);}
            for(auto& event:events)event.device_info=Find(event.type,event.device);
            activeBatch=true;
            Input_Batch result;
            result.state_=this;result.lock_=std::move(lock);
            result.inputs_={events.data(),events.size()};
            result.exceptions_=std::uncaught_exceptions();
            return result;
        }catch(...){AbortException();throw;}
    }
    bool Valid(const Input_Event& event) const noexcept {
        return activeBatch && event.owner_==this && event.batch_==batch && event.index_<events.size()
            && event.payload_==events[event.index_].payload_;
    }
    Send_Result SendEvents(Detail::Input_View<Input_Event> inputs) {
        if(inputs.empty())return {};
        for(const auto& event:inputs)if(!Valid(event))return {0,ERROR_OPERATION_ABORTED};
        if(fault || !capture)return {0,fault?fault:ERROR_OPERATION_ABORTED};
        std::size_t accepted=0;
        for(std::size_t i=0;i<inputs.size();) {
            auto& event=inputs[i];
            if(event.blocked || events[event.index_].sent_){++i;continue;}
            std::size_t run=1;
            const auto result=std::visit([&](auto* first)->Send_Result {
                using T=std::remove_pointer_t<decltype(first)>;
                while(i+run<inputs.size()) {
                    auto& next=inputs[i+run];
                    const auto pointer=std::get_if<T*>(&next.payload_);
                    if(next.blocked || events[next.index_].sent_ || !pointer || *pointer!=first+run)break;
                    ++run;
                }
                return Send_Input(capture.Get(),Detail::Input_View<const T>(first,run));
            },event.payload_);
            for(std::size_t n=0;n<result.accepted;++n)events[inputs[i+n].index_].sent_=true;
            accepted+=result.accepted;
            if(result.error || result.accepted!=run) {
                const auto error=result.error?result.error:ERROR_WRITE_FAULT;
                if(error!=ERROR_BUSY)Abort(error);
                return {accepted,error};
            }
            i+=run;
        }
        return {accepted,ERROR_SUCCESS};
    }
    static Send_Result SendBatch(Input_Batch& inputs);
};
Automatic_State& Automatic(){static Automatic_State state;return state;}
struct Automatic_Lease {
    Automatic_State& state;
    std::unique_lock<std::recursive_mutex> lock;
    Automatic_Lease():state(Automatic()),lock(state.mutex,std::try_to_lock) {
        if(!lock.owns_lock())Fail("Automatic Interceptor session is being used by another thread",ERROR_BUSY);
    }
};
bool Stopped(HANDLE event) {
    if(!event)return false;
    const auto result=WaitForSingleObject(event,0);
    if(result==WAIT_FAILED)Fail("Check stop event");
    return result==WAIT_OBJECT_0;
}
Send_Result Automatic_State::SendBatch(Input_Batch& inputs) {
    if(inputs.empty())return {};
    Automatic_Lease lease;
    if(inputs.state_!=&lease.state)return {0,ERROR_OPERATION_ABORTED};
    return lease.state.SendEvents(inputs.inputs_);
}
}

void Input_Batch::Release() noexcept {
    if(state_) {
        // A transformation exception outside the SDK still restores physical
        // input while unwinding, even if the application catches it later.
        if(std::uncaught_exceptions()>exceptions_)state_->Abort(ERROR_OPERATION_ABORTED);
        state_->activeBatch=false;
        if(state_->fault)state_->ReleaseResources();
        state_=nullptr;inputs_={};
    }
    lock_=std::unique_lock<std::recursive_mutex>{};
}
Input_Batch::~Input_Batch(){Release();}
Input_Batch::Input_Batch(Input_Batch&& other) noexcept
    :state_(other.state_),lock_(std::move(other.lock_)),inputs_(other.inputs_),exceptions_(other.exceptions_) {
    other.state_=nullptr;other.inputs_={};
}
Input_Batch& Input_Batch::operator=(Input_Batch&& other) noexcept {
    if(this!=&other){Release();state_=other.state_;lock_=std::move(other.lock_);inputs_=other.inputs_;exceptions_=other.exceptions_;other.state_=nullptr;other.inputs_={};}
    return *this;
}
Input_Batch Get_Input_With_Timeout(DWORD timeout,HANDLE stopEvent) {
    if(Detail::Stopped(stopEvent))return {};
    Detail::Automatic_Lease lease;
    return lease.state.Receive(std::move(lease.lock),timeout,stopEvent);
}
std::vector<Device> Get_Devices() {
    Detail::Automatic_Lease lease;auto& state=lease.state;auto& context=state.Ensure();
    // Return a separate snapshot: metadata referenced by a live batch is stable.
    try{const auto snapshot=state.SnapshotDevices(context);return {snapshot.begin(),snapshot.end()};}
    catch(...){state.AbortException();throw;}
}
Context_Info Get_Context() {
    Detail::Automatic_Lease lease;auto& state=lease.state;auto& context=state.Ensure();
    Context_Info result;result.token=state.token;result.generation=state.capture.generation;
    try{const auto snapshot=state.SnapshotDevices(context);result.devices.assign(snapshot.begin(),snapshot.end());}
    catch(...){state.AbortException();throw;}
    result.active=true;
    try{result.status=Get_Status(&context);result.active=result.status.active_mask!=0;}
    catch(const std::system_error& e){result.status_error=static_cast<DWORD>(e.code().value());}
    try{result.capabilities=Capabilities(context.driver.value);}
    catch(const std::system_error& e){result.capabilities_error=static_cast<DWORD>(e.code().value());}
    try{result.trial=Get_Trial_Status();}
    catch(const std::system_error& e){result.trial_error=static_cast<DWORD>(e.code().value());}
    return result;
}
void Destroy_Context() {
    Detail::Automatic_Lease lease;auto& state=lease.state;state.RequireReader();
    Destroy_Context(state.capture);state.fault=ERROR_SUCCESS;state.ReleaseResources();
}
Send_Result Send_Input(Input_Batch& inputs){return Detail::Automatic_State::SendBatch(inputs);}
Send_Result Send_Input(Input_Event& input) {
    Detail::Automatic_Lease lease;
    return lease.state.SendEvents({&input,1});
}
namespace Detail {
template<class T> void SelectedRead(Context& context,std::vector<T>& out,std::vector<Wire<T>>& scratch,
    unsigned stream,DWORD code,std::uint32_t device,bool consume) {
    if(!(context.mask&(1u<<stream)))throw std::invalid_argument("Context does not own this input family");
    if constexpr(std::is_same_v<T,Controller_GIP_Raw_Input>)
        if(!context.gipRawSupported)Fail("Loaded driver has no complete GIP report API",ERROR_NOT_SUPPORTED);
    if(context.selectedReadSupport==0)
        Fail("Loaded driver does not support selected or non-consuming input reads",ERROR_NOT_SUPPORTED);
    std::lock_guard<std::mutex> lock(context.readers[stream]);
    const auto capacity=stream<2?IC_CAPACITY:IC_HID_CAPACITY;
    out.reserve(capacity);if(scratch.size()<capacity)scratch.resize(capacity);
    const IC_GET_SELECT request{{IC_VERSION,0},device,consume?0u:IC_GET_PEEK};
    DWORD bytes=0;
    try {bytes=Io(context.driver.value,code,&request,sizeof(request),scratch.data(),static_cast<DWORD>(capacity*sizeof(Wire<T>)));}
    catch(const std::system_error& e) {
        const auto error=e.code().value();
        if(context.selectedReadSupport<0 && (error==ERROR_INVALID_PARAMETER || error==ERROR_INVALID_FUNCTION || error==ERROR_NOT_SUPPORTED)) {
            context.selectedReadSupport=0;
            Fail("Loaded driver does not support selected or non-consuming input reads",ERROR_NOT_SUPPORTED);
        }
        throw;
    }
    if(bytes%sizeof(Wire<T>) || bytes>capacity*sizeof(Wire<T>))Fail("Invalid selected input response",ERROR_INVALID_DATA);
    context.selectedReadSupport=1;
    AssignWire(out,scratch.data(),bytes/sizeof(Wire<T>));
}
}
#define IC_AUTO_IMPL(Name,Type,stream,buffer,readIoctl) \
std::vector<Type> Get_##Name##_Input(std::uint32_t deviceId,bool consume) { \
    Detail::Automatic_Lease lease;auto& state=lease.state;state.RequireReader();auto& context=state.Ensure(); \
    std::vector<Type> out; \
    try{ \
        if(!deviceId && consume)Get_##Name##_Input(context,out); \
        else Detail::SelectedRead(context,out,context.buffer,stream,readIoctl,deviceId,consume); \
        state.DecodeInputs(context,out); \
    }catch(const std::system_error& e){if(e.code().value()!=ERROR_NOT_SUPPORTED)state.AbortException();throw;} \
    catch(...){state.AbortException();throw;} \
    return out; \
} \
Send_Result Detail::SendAuto(Detail::Input_View<const Type> inputs) { \
    if(inputs.empty())return {}; \
    Detail::Automatic_Lease lease;auto& state=lease.state;auto& context=state.Ensure(); \
    if(!state.ValidTokens(inputs))return {0,ERROR_NOT_FOUND}; \
    const auto result=Send_Input(context,inputs.data(),inputs.size()); \
    if(result.error && result.error!=ERROR_BUSY)state.Abort(result.error);return result; \
}
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE
IC_AUTO_IMPL(Keyboard,Keyboard_Input,0,keyboard,IOCTL_IC_GET_KEYBOARD)
IC_AUTO_IMPL(Mouse,Mouse_Input,1,mouse,IOCTL_IC_GET_MOUSE)
#endif
#if IC_PRODUCT_HAS_CONTROLLERS
IC_AUTO_IMPL(Controller_Hid,Controller_Hid_Input,2,hid,IOCTL_IC_GET_HID)
IC_AUTO_IMPL(Controller_Xinput,Controller_Xinput_Input,3,xusb,IOCTL_IC_GET_XUSB)
IC_AUTO_IMPL(Controller_XinputHid,Controller_XinputHid_Input,4,xinputhid,IOCTL_IC_GET_XINPUTHID)
IC_AUTO_IMPL(Controller_GIP,Controller_GIP_Input,5,gip,IOCTL_IC_GET_GIP)
IC_AUTO_IMPL(Controller_GIP_Raw,Controller_GIP_Raw_Input,5,gipRaw,IOCTL_IC_GET_GIP_RAW)
#endif
#undef IC_AUTO_IMPL
IC_STATUS Get_Status() {
    Detail::Automatic_Lease lease;
    return Get_Status(lease.state.capture?&lease.state.capture.Get():nullptr);
}
}

// ---- ContextMetadata.inl ----
// User-side metadata only. Included after AutomaticInput.inl.

// ---- UsbReportDescriptor.inl ----
// Read-only USB descriptor queries, only on explicit metadata calls.
#include <cfgmgr32.h>
#include <usbioctl.h>
#include <usbiodef.h>
#pragma comment(lib,"cfgmgr32.lib")
namespace Interceptor { namespace Detail {
inline constexpr GUID usbHubGuid{0xf18a0e88,0xc30c,0x11d0,{0x88,0x15,0x00,0xa0,0xc9,0x06,0xbe,0xd8}};
struct HidReportLocation {std::uint16_t interface_number=0,length=0;};
HidReportLocation FindHidReport(const std::vector<std::uint8_t>& config,int requestedInterface) {
    if(config.size()<9 || config[0]<9 || config[1]!=2 || IcNativeRead16(config.data()+2)!=config.size())
        Fail("Invalid USB configuration descriptor",ERROR_INVALID_DATA);
    int current=-1;bool hid=false;HidReportLocation found;unsigned matches=0;
    for(std::size_t cursor=0;cursor<config.size();) {
        const auto length=config[cursor];
        if(length<2 || length>config.size()-cursor)Fail("Truncated USB descriptor",ERROR_INVALID_DATA);
        const auto* entry=config.data()+cursor;
        if(entry[1]==4) {
            if(length<9)Fail("Truncated USB interface descriptor",ERROR_INVALID_DATA);
            current=entry[2];hid=entry[3]==0 && entry[5]==3 && (requestedInterface<0 || current==requestedInterface);
        } else if(entry[1]==0x21 && hid) {
            if(length<6 || 6u+3u*entry[5]>length)Fail("Truncated USB HID descriptor",ERROR_INVALID_DATA);
            for(unsigned i=0;i<entry[5];++i)if(entry[6+3*i]==0x22) {
                const auto bytes=IcNativeRead16(entry+7+3*i);
                if(!bytes)Fail("Empty HID report descriptor",ERROR_INVALID_DATA);
                found={static_cast<std::uint16_t>(current),bytes};++matches;
            }
        }
        cursor+=length;
    }
    if(matches!=1)Fail("USB HID interface absent or ambiguous",ERROR_NOT_SUPPORTED);
    return found;
}
DWORD UsbDescriptorIo(HANDLE hub,DWORD code,void* buffer,DWORD size) {
    Handle event;event.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!event.value)Fail("Create USB descriptor event");
    OVERLAPPED operation{};operation.hEvent=event.value;DWORD bytes=0;
    if(!DeviceIoControl(hub,code,buffer,size,buffer,size,&bytes,&operation)) {
        if(GetLastError()!=ERROR_IO_PENDING)Fail("Read USB descriptor");
        const auto wait=WaitForSingleObject(event.value,2000);
        if(wait!=WAIT_OBJECT_0) {
            const auto error=wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();
            CancelIoEx(hub,&operation);
            // Storage must remain alive until the cancelled request completes.
            (void)GetOverlappedResult(hub,&operation,&bytes,TRUE);
            Fail("Wait for USB descriptor",error);
        }
        if(!GetOverlappedResult(hub,&operation,&bytes,FALSE))Fail("Complete USB descriptor read");
    }
    if(bytes>size)Fail("USB response exceeds buffer",ERROR_INVALID_DATA);
    return bytes;
}
std::vector<std::uint8_t> UsbGetDescriptor(HANDLE hub,ULONG port,UCHAR type,UCHAR index,USHORT interfaceNumber,USHORT length) {
    const auto header=static_cast<DWORD>(FIELD_OFFSET(USB_DESCRIPTOR_REQUEST,Data));
    std::vector<std::uint8_t> buffer(header+length);
    auto* request=reinterpret_cast<USB_DESCRIPTOR_REQUEST*>(buffer.data());request->ConnectionIndex=port;
    request->SetupPacket.bmRequest=0x80;request->SetupPacket.bRequest=6;
    request->SetupPacket.wValue=static_cast<USHORT>((type<<8)|index);
    request->SetupPacket.wIndex=interfaceNumber;request->SetupPacket.wLength=length;
    const auto bytes=UsbDescriptorIo(hub,IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(bytes<header || bytes-header>length)Fail("Invalid USB descriptor extent",ERROR_INVALID_DATA);
    return {buffer.begin()+header,buffer.begin()+bytes};
}
int InterfaceFromInstance(const std::wstring& instance) {
    const auto hex=[](wchar_t c)->int {if(c>=L'0'&&c<=L'9')return c-L'0';if(c>=L'a'&&c<=L'f')return c-L'a'+10;if(c>=L'A'&&c<=L'F')return c-L'A'+10;return -1;};
    for(std::size_t i=0;i+6<=instance.size();++i)if(instance[i]==L'&' &&
        (instance[i+1]==L'M'||instance[i+1]==L'm')&&(instance[i+2]==L'I'||instance[i+2]==L'i')&&instance[i+3]==L'_') {
        const int a=hex(instance[i+4]),b=hex(instance[i+5]);if(a>=0&&b>=0)return a*16+b;
    }
    return -1;
}
std::vector<std::uint8_t> ReadUsbReportDescriptor(const Device& device,const Device_Details& details) {
    if(!(details.flags&IC_QUERY_IDENTITY_VALID) || !details.instance[0])Fail("HID source identity unavailable",ERROR_NOT_SUPPORTED);
    std::wstring instance;for(auto c:details.instance){if(!c)break;instance.push_back(static_cast<wchar_t>(c));}
    int interfaceNumber=InterfaceFromInstance(instance);DEVINST node=0;
    const auto located=CM_Locate_DevNodeW(&node,instance.data(),CM_LOCATE_DEVNODE_NORMAL);
    if(located!=CR_SUCCESS)Fail("Locate HID devnode",CM_MapCrToWin32Err(located,ERROR_NOT_SUPPORTED));
    struct DeviceSet {HDEVINFO value;~DeviceSet(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}};
    DeviceSet hubs{SetupDiGetClassDevsW(&usbHubGuid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
    if(hubs.value==INVALID_HANDLE_VALUE)Fail("Enumerate USB hubs");
    for(unsigned depth=0;depth<32;++depth) {
        DEVINST parent=0;if(CM_Get_Parent(&parent,node,0)!=CR_SUCCESS)break;
        for(DWORD i=0;;++i) {
            SP_DEVICE_INTERFACE_DATA entry{};entry.cbSize=sizeof(entry);
            if(!SetupDiEnumDeviceInterfaces(hubs.value,nullptr,&usbHubGuid,i,&entry)) {
                if(GetLastError()!=ERROR_NO_MORE_ITEMS)Fail("Enumerate USB hub interface");break;
            }
            SP_DEVINFO_DATA info{};info.cbSize=sizeof(info);DWORD required=0;
            SetupDiGetDeviceInterfaceDetailW(hubs.value,&entry,nullptr,0,&required,&info);
            if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER || required<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))Fail("USB hub path extent",ERROR_INVALID_DATA);
            std::vector<std::uint8_t> path(required);auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(path.data());detail->cbSize=sizeof(*detail);
            if(!SetupDiGetDeviceInterfaceDetailW(hubs.value,&entry,detail,required,nullptr,&info))Fail("USB hub path");
            if(info.DevInst!=parent)continue;
            ULONG type=0,port=0,length=sizeof(port);
            const auto address=CM_Get_DevNode_Registry_PropertyW(node,CM_DRP_ADDRESS,&type,&port,&length,0);
            if(address!=CR_SUCCESS || type!=REG_DWORD || length!=sizeof(port) || !port)Fail("USB port address unavailable",ERROR_NOT_SUPPORTED);
            Handle hub;hub.value=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
            if(hub.value==INVALID_HANDLE_VALUE)Fail("Open USB hub for descriptor query");
            std::vector<std::uint8_t> connection(sizeof(USB_NODE_CONNECTION_INFORMATION_EX)+32*sizeof(USB_PIPE_INFO));
            auto* state=reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(connection.data());state->ConnectionIndex=port;
            if(UsbDescriptorIo(hub.value,IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,connection.data(),static_cast<DWORD>(connection.size()))<sizeof(*state) ||
                state->ConnectionStatus!=DeviceConnected || !state->CurrentConfigurationValue ||
                (device.vid && device.vid!=state->DeviceDescriptor.idVendor) || (device.pid && device.pid!=state->DeviceDescriptor.idProduct))
                Fail("USB source no longer matches capture",ERROR_DEVICE_NOT_CONNECTED);
            for(unsigned configuration=0;configuration<state->DeviceDescriptor.bNumConfigurations;++configuration) {
                const auto prefix=UsbGetDescriptor(hub.value,port,2,static_cast<UCHAR>(configuration),0,9);
                if(prefix.size()!=9 || prefix[0]<9 || prefix[1]!=2)Fail("Invalid USB configuration prefix",ERROR_INVALID_DATA);
                if(prefix[5]!=state->CurrentConfigurationValue)continue;
                const auto size=IcNativeRead16(prefix.data()+2);
                if(size<9)Fail("Invalid USB configuration size",ERROR_INVALID_DATA);
                const auto config=UsbGetDescriptor(hub.value,port,2,static_cast<UCHAR>(configuration),0,size);
                const auto report=FindHidReport(config,interfaceNumber);
                // The hub API forces recipient 0x80; hardware requiring 0x81
                // may reject it. Never substitute reconstructed/preparsed data.
                auto data=UsbGetDescriptor(hub.value,port,0x22,0,report.interface_number,report.length);
                if(data.size()!=report.length)Fail("Incomplete hardware HID report descriptor",ERROR_INVALID_DATA);
                return data;
            }
            Fail("Active USB configuration absent",ERROR_NOT_SUPPORTED);
        }
        node=parent;wchar_t name[MAX_DEVICE_ID_LEN]{};
        if(interfaceNumber<0 && CM_Get_Device_IDW(node,name,MAX_DEVICE_ID_LEN,0)==CR_SUCCESS)interfaceNumber=InterfaceFromInstance(name);
    }
    Fail("HID raw descriptor unavailable on this transport",ERROR_NOT_SUPPORTED);
}
using RawReportReader=std::vector<std::uint8_t>(*)(const Device&,const Device_Details&);
RawReportReader rawReportReader=ReadUsbReportDescriptor;
}}
namespace Interceptor {
namespace Detail {
Captured_Device ResolveToken(Automatic_State& state,Context& context,const std::string& token) {
    for(const auto& device:state.devices)if(device.token==token)return device;
    const auto snapshot=state.SnapshotDevices(context);
    for(const auto& device:snapshot)if(device.token==token)return device;
    return {};
}
Captured_Device ExistingDevice(Automatic_State& state,const std::string& token) {
    if(token.empty())Fail("Device token must not be empty",ERROR_INVALID_PARAMETER);
    if(state.fault)Fail("Automatic capture is stopped",state.fault);
    if(!state.capture)Fail("Device token has no current capture",ERROR_NOT_FOUND);
    const auto device=ResolveToken(state,state.capture.Get(),token);
    if(device.token.empty())Fail("Unknown or stale device token",ERROR_NOT_FOUND);
    return device;
}
Device_Details PublicDetails(const Device& device,const IC_DEVICE_QUERY_RESULT& raw) {
    Device_Details result;result.token=device.token;
    result.size=raw.size;result.version=raw.version;result.type=raw.type;result.hid_profiles=raw.hid_profiles;result.flags=raw.flags;
    result.last_read_status=raw.last_read_status;result.last_usb_status=raw.last_usb_status;
    result.physical_reports=raw.physical_reports;result.report_bytes=raw.report_bytes;
    result.metadata_bytes=raw.metadata_bytes;result.metadata_input_bytes=raw.metadata_input_bytes;
    std::copy(std::begin(raw.reserved),std::end(raw.reserved),std::begin(result.reserved));
    std::copy(std::begin(raw.instance),std::end(raw.instance),std::begin(result.instance));
    return result;
}
Descriptor_Result IdentifyDescriptor(Automatic_State& state,Context& context,const std::string& token) {
    Descriptor_Result result;const auto device=ResolveToken(state,context,token);result.device=device;
    if(result.device.token.empty()){result.error=ERROR_NOT_FOUND;return result;}
    try{result.details=PublicDetails(device,DeviceDetails(context.driver.value,device.type,device.source,false,IC_DEVICE_QUERY_VERSION));}
    catch(const std::system_error& e){result.details_error=static_cast<DWORD>(e.code().value());}
    return result;
}
}
Device_Details Get_Device_Details(const std::string& token) {
    if(token.empty())Fail("Device token must not be empty",ERROR_INVALID_PARAMETER);
    Detail::Automatic_Lease lease;auto& state=lease.state;
    const auto device=Detail::ExistingDevice(state,token);
    return Detail::PublicDetails(device,DeviceDetails(state.capture.Get().driver.value,device.type,device.source,false,IC_DEVICE_QUERY_VERSION));
}
Device_Availability Get_Device_Availability(const std::string& token) {
    if(token.empty())Fail("Device token must not be empty",ERROR_INVALID_PARAMETER);
    Detail::Automatic_Lease lease;auto& state=lease.state;
    const auto device=Detail::ExistingDevice(state,token);
    const auto native=Availability(state.capture.Get().driver.value,device.type,device.source,false);
    Device_Availability result;result.details=Detail::PublicDetails(device,native.details);
    result.state=native.state;result.reasons=native.reasons;result.reported_by_driver=native.reported_by_driver;
    return result;
}
Descriptor_Result Get_descriptor(const std::string& token) {
    if(token.empty()){Descriptor_Result result;result.error=ERROR_INVALID_PARAMETER;return result;}
    Detail::Automatic_Lease lease;auto& state=lease.state;auto& context=state.Ensure();
    auto result=Detail::IdentifyDescriptor(state,context,token);if(!result)return result;
    try {
        switch(result.device.type) {
        case IC_TYPE_CONTROLLER_HID:
            result.format=Descriptor_Format::Hid_Preparsed;result.data=Get_Controller_Hid_Descriptor(context,result.device.id);break;
        case IC_TYPE_CONTROLLER_XINPUTHID:
            result.format=Descriptor_Format::Hid_Preparsed;result.data=Get_Controller_XinputHid_Descriptor(context,result.device.id);break;
        case IC_TYPE_CONTROLLER_GIP: {
            result.format=Descriptor_Format::Gip_Metadata;result.data.resize(4096);
            const IC_GET request{IC_VERSION,result.device.id};
            const auto bytes=Io(context.driver.value,IOCTL_IC_GIP_DESCRIPTOR,&request,sizeof(request),result.data.data(),static_cast<DWORD>(result.data.size()));
            if(!bytes || bytes>result.data.size())Fail("Invalid GIP metadata response",ERROR_INVALID_DATA);
            result.data.resize(bytes);break;
        }
        default:break;
        }
    }catch(const std::system_error& e){result.error=static_cast<DWORD>(e.code().value());result.data.clear();}
    return result;
}
Report_Descriptor Get_Report_Descriptor(const std::string& token) {
    if(token.empty()){Report_Descriptor result;result.error=ERROR_INVALID_PARAMETER;return result;}
    Detail::Automatic_Lease lease;auto& state=lease.state;auto& context=state.Ensure();
    auto result=Detail::IdentifyDescriptor(state,context,token);if(!result)return result;
    if(result.device.type==IC_TYPE_CONTROLLER_XUSB || result.device.type==IC_TYPE_CONTROLLER_GIP) {
        result.applicable=false;result.error=ERROR_NOT_SUPPORTED;return result;
    }
    result.format=Descriptor_Format::Hid_Report;
    if(result.details_error){result.error=result.details_error;return result;}
    try{
        const auto before=Get_Status(&context);
        if(!(before.active_mask&(1u<<(result.device.type-1))))Fail("Capture no longer owns HID source",ERROR_OPERATION_ABORTED);
        result.data=Detail::rawReportReader(result.device,result.details);
        const auto after=Get_Status(&context);
        if(after.generation!=before.generation || after.active_mask!=before.active_mask)Fail("Capture changed during descriptor query",ERROR_OPERATION_ABORTED);
        if(result.data.empty())Fail("Empty hardware HID report descriptor",ERROR_INVALID_DATA);
    }
    catch(const std::system_error& e){result.error=static_cast<DWORD>(e.code().value());result.data.clear();}
    return result;
}
}

// ---- InitDevices.cpp ----
#include <SetupAPI.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <commctrl.h>
#include <initguid.h>
#include <devguid.h>
#include <string>
#include <cwctype>
#include <iterator>
#include <vector>
#include <mutex>
#include <system_error>
#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"newdev.lib")
#pragma comment(lib,"cfgmgr32.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace Interceptor { namespace Init_Implementation {
namespace {
struct DeviceSet {
    HDEVINFO value=INVALID_HANDLE_VALUE;
    ~DeviceSet(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}
};
struct DriverList {
    HDEVINFO set;SP_DEVINFO_DATA* device;
    ~DriverList(){SetupDiDestroyDriverInfoList(set,device,SPDIT_COMPATDRIVER);}
};
struct MutexHandle {
    HANDLE value=nullptr;bool owned=false;
    ~MutexHandle(){if(owned)ReleaseMutex(value);if(value)CloseHandle(value);}
};
// A volatile key survives application exits but is discarded by a full Windows
// restart. Otherwise a second EXE could miss a reboot requested by the first.
DWORD PendingRestart(bool& pending) {
    DWORD flag=0,bytes=sizeof(flag);
    const LSTATUS status=RegGetValueW(HKEY_LOCAL_MACHINE,AssociationTarget().restartKey,L"Required",RRF_RT_REG_DWORD|RRF_SUBKEY_WOW6464KEY,nullptr,&flag,&bytes);
    pending=status==ERROR_SUCCESS && flag!=0;
    return status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND?ERROR_SUCCESS:static_cast<DWORD>(status);
}
DWORD RememberRestart() {
    HKEY key=nullptr;
    LSTATUS status=RegCreateKeyExW(HKEY_LOCAL_MACHINE,AssociationTarget().restartKey,0,nullptr,REG_OPTION_VOLATILE,KEY_SET_VALUE|KEY_WOW64_64KEY,nullptr,&key,nullptr);
    if(status!=ERROR_SUCCESS)return static_cast<DWORD>(status);
    const DWORD flag=1;status=RegSetValueExW(key,L"Required",0,REG_DWORD,reinterpret_cast<const BYTE*>(&flag),sizeof(flag));
    RegCloseKey(key);return static_cast<DWORD>(status);
}
void RecordError(Init_Result& result,DWORD error,const char* detail=nullptr,const std::wstring& device={}) {
    if(!result.error) {
        result.error=error?error:ERROR_GEN_FAILURE;
        if(detail)result.error_detail=detail;
        result.error_device=device;
    }
}
bool IsAdministrator() {
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;PSID sid=nullptr;BOOL member=FALSE;
    if(!AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid))return false;
    const BOOL ok=CheckTokenMembership(nullptr,sid,&member);FreeSid(sid);return ok && member;
}
std::wstring Property(HDEVINFO set,SP_DEVINFO_DATA& device,DWORD property) {
    DWORD size=0,type=0;
    if(!SetupDiGetDeviceRegistryPropertyW(set,&device,property,&type,nullptr,0,&size) && GetLastError()!=ERROR_INSUFFICIENT_BUFFER)
        throw std::system_error(GetLastError(),std::system_category(),"Read device property");
    std::vector<wchar_t> data(size/sizeof(wchar_t)+2,0);
    if(!SetupDiGetDeviceRegistryPropertyW(set,&device,property,&type,reinterpret_cast<BYTE*>(data.data()),size,nullptr))
        throw std::system_error(GetLastError(),std::system_category(),"Read device property");
    return data.data(); // First hardware ID is sufficient to check the HID bus.
}
void CheckStarted(SP_DEVINFO_DATA& device,Init_Result& result) {
    ULONG status=0,problem=0;
    if(CM_Get_DevNode_Status(&status,&problem,device.DevInst,0)!=CR_SUCCESS) {RecordError(result,ERROR_DEVICE_NOT_CONNECTED);return;}
    if(problem==CM_PROB_NEED_RESTART)result.reboot_required=true;
    else if(!(status&DN_STARTED) || (status&DN_HAS_PROBLEM))RecordError(result,ERROR_NOT_READY);
}
bool ForeignStackFilters(HDEVINFO set,SP_DEVINFO_DATA& device,Init_Result& result,const std::wstring& id);
void Associate(HDEVINFO set,SP_DEVINFO_DATA& device,bool mouse,Init_Result& result) {
    const auto hardware=Property(set,device,SPDRP_HARDWAREID);
    if(_wcsnicmp(hardware.c_str(),L"HID\\",4)) {++result.skipped;return;}
    const auto service=Property(set,device,SPDRP_SERVICE);
    if(Policy::ForeignInterceptor(service)) {
        ++result.skipped;RecordError(result,ERROR_SHARING_VIOLATION);
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        (void)SetupDiGetDeviceInstanceIdW(set,&device,instance,_countof(instance),nullptr);
        result.skipped_devices.push_back({instance,L"Another Interceptor product owns this mapper; uninstall that product first",ERROR_SHARING_VIOLATION});
        return;
    }
    wchar_t instance[MAX_DEVICE_ID_LEN]{};
    if(!SetupDiGetDeviceInstanceIdW(set,&device,instance,_countof(instance),nullptr)) {
        RecordError(result,GetLastError());return;
    }
    if(ForeignStackFilters(set,device,result,instance))return;
    if(SetupDetail::IsMapperPaused(instance,AssociationTarget().service)){++result.skipped;return;}
    if(!_wcsicmp(service.c_str(),AssociationTarget().service)) {++result.already_associated;CheckStarted(device,result);return;}
    if(_wcsicmp(service.c_str(),mouse?L"mouhid":L"kbdhid")) {++result.skipped;return;}
    if(!SetupDetail::UsesInboxInputMapper(set,device,mouse)) {
        ++result.skipped;
        result.skipped_devices.push_back({instance,L"The manufacturer owns this input driver package",ERROR_NOT_SUPPORTED});
        return;
    }
    const auto profile=SetupDetail::ProbeMapperProfile(set,device,mouse);
    if(!profile.supported) {
        ++result.skipped;result.skipped_devices.push_back({instance,profile.reason,profile.error});return;
    }
    SP_DEVINSTALL_PARAMS_W params{};params.cbSize=sizeof(params);
    if(!SetupDiGetDeviceInstallParamsW(set,&device,&params)) {RecordError(result,GetLastError());return;}
    params.Flags|=DI_QUIETINSTALL;
    if(!SetupDiSetDeviceInstallParamsW(set,&device,&params) || !SetupDiBuildDriverInfoList(set,&device,SPDIT_COMPATDRIVER)) {RecordError(result,GetLastError());return;}
    DriverList list{set,&device};SP_DRVINFO_DATA_W selected{};bool found=false;
    for(DWORD index=0;;++index) {
        SP_DRVINFO_DATA_W candidate{};candidate.cbSize=sizeof(candidate);
        if(!SetupDiEnumDriverInfoW(set,&device,SPDIT_COMPATDRIVER,index,&candidate)) {
            if(GetLastError()!=ERROR_NO_MORE_ITEMS) {RecordError(result,GetLastError());return;}
            break;
        }
        if(_wcsicmp(candidate.ProviderName,L"Interceptor Project") ||
           _wcsicmp(candidate.Description,mouse?L"Interceptor HID Mouse Mapper":L"Interceptor HID Keyboard Mapper"))continue;
        if(!found || candidate.DriverVersion>selected.DriverVersion ||
           (candidate.DriverVersion==selected.DriverVersion && CompareFileTime(&candidate.DriverDate,&selected.DriverDate)>0)) {selected=candidate;found=true;}
    }
    if(!found) {RecordError(result,ERROR_NOT_FOUND);return;}
    // A different installer or PnP may have changed the instance since the
    // metadata probe. Preserve a newly selected custom driver rather than bind.
    if(_wcsicmp(Property(set,device,SPDRP_SERVICE).c_str(),service.c_str()) ||
       _wcsicmp(Property(set,device,SPDRP_HARDWAREID).c_str(),hardware.c_str()) ||
       !SetupDetail::UsesInboxInputMapper(set,device,mouse)) {
        ++result.skipped;result.skipped_devices.push_back({instance,L"Mapper identity/service changed before association",ERROR_RETRY});return;
    }
    BOOL reboot=FALSE;
    // Exact devnode, not an update of every instance sharing a hardware ID.
    const BOOL installed=DiInstallDevice(nullptr,set,&device,&selected,DIIDFLAG_NOFINISHINSTALLUI,&reboot);
    const DWORD error=installed?ERROR_SUCCESS:GetLastError();
    result.reboot_required|=reboot!=FALSE;
    if(SetupDiGetDeviceInstallParamsW(set,&device,&params))result.reboot_required|=(params.Flags&(DI_NEEDREBOOT|DI_NEEDRESTART))!=0;
    if(!installed) {RecordError(result,error);return;}
    ++result.associated;
    if(!result.reboot_required)CheckStarted(device,result);
}

// ---- HidDevices.inl ----
// Included by InitDevices.cpp: shares its RAII wrappers and error reporting.
// Changes only per-instance UpperFilters, never the HID class-wide filter list.

// ---- internal/ControllerDiscovery.h ----
#include <wchar.h>
static __inline int IcNativeControllerService(const wchar_t* name) {
    return name && (!_wcsnicmp(name,L"xusb",4) || !_wcsnicmp(name,L"xinputhid",9) ||
                    !_wcsnicmp(name,L"xboxgip",7) || !_wcsnicmp(name,L"gip",3));
}
// Pure predicate shared with installer regression tests. A usage can occur in
// either hardware IDs or compatible IDs; do not assume Windows puts it in both.
static __inline unsigned IcControllerCollectionProfile(const wchar_t* id) {
    unsigned page=0,usage=0,i;
    if(!id || wcslen(id)!=25 || _wcsnicmp(id,L"HID_DEVICE_UP:",14) || _wcsnicmp(id+18,L"_U:",3))return 0;
    for(i=0;i<8;++i) {
        wchar_t c=id[(i<4?14:17)+i];unsigned digit;
        if(c>=L'0' && c<=L'9')digit=c-L'0';else if(c>=L'A' && c<=L'F')digit=c-L'A'+10;
        else if(c>=L'a' && c<=L'f')digit=c-L'a'+10;else return 0;
        if(i<4)page=(page<<4)|digit;else usage=(usage<<4)|digit;
    }
    return IcHidGameProfile((unsigned short)page,(unsigned short)usage);
}
static __inline int IcControllerCollectionId(const wchar_t* id) {
    return IcControllerCollectionProfile(id)!=0;
}
std::vector<wchar_t> MultiProperty(HDEVINFO set,SP_DEVINFO_DATA& device,DWORD property) {
    DWORD bytes=0,type=0;
    if(!SetupDiGetDeviceRegistryPropertyW(set,&device,property,&type,nullptr,0,&bytes)) {
        const auto error=GetLastError();
        if(error==ERROR_INVALID_DATA || error==ERROR_FILE_NOT_FOUND)return {0,0};
        if(error!=ERROR_INSUFFICIENT_BUFFER)throw std::system_error(error,std::system_category(),"Read HID property");
    }
    std::vector<wchar_t> result(bytes/sizeof(wchar_t)+2,0);
    if(bytes && !SetupDiGetDeviceRegistryPropertyW(set,&device,property,&type,reinterpret_cast<BYTE*>(result.data()),bytes,nullptr))
        throw std::system_error(GetLastError(),std::system_category(),"Read HID property");
    if(bytes && type!=REG_MULTI_SZ)throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Expected multi-string HID property");
    return result;
}
bool ControllerCollection(HDEVINFO set,SP_DEVINFO_DATA& device,unsigned profiles=IC_HID_PROFILE_DESKTOP) {
    bool game=false;
    for(const auto property:{SPDRP_HARDWAREID,SPDRP_COMPATIBLEIDS}) {
        const auto ids=MultiProperty(set,device,property);
        for(const wchar_t* id=ids.data();*id;id+=wcslen(id)+1) {
            if(IcControllerCollectionProfile(id)&profiles)return true;
            if(!_wcsicmp(id,L"HID_DEVICE_SYSTEM_GAME"))game=true;
        }
    }
    // Some miniports publish the Windows game-controller marker without a
    // usable UP/U identity. Inspect only metadata of this exact collection;
    // never infer its protocol from its manufacturer or friendly name.
    return game && SetupDetail::ProbeControllerProfile(set,device,profiles).supported;
}
bool NativeXbox(DEVINST node) {
    // The HID view of an Xbox controller is not its native game-input path.
    for(unsigned depth=0;depth<32;++depth) {
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if(CM_Get_Device_IDW(node,id,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS)return true;
        for(auto* p=id;*p;++p)*p=static_cast<wchar_t>(towupper(*p));
        if(wcsstr(id,L"&IG_") || wcsstr(id,L"GIP\\"))return true;
        wchar_t service[256]{};ULONG type=0,bytes=sizeof(service)-sizeof(wchar_t);
        const auto query=CM_Get_DevNode_Registry_PropertyW(node,CM_DRP_SERVICE,&type,service,&bytes,0);
        if(query==CR_SUCCESS) {
            if(IcNativeControllerService(service) || !_wcsicmp(service,L"dc1-controller"))return true;
        } else if(query!=CR_NO_SUCH_VALUE)return true; // Fail closed on ambiguous ancestry.
        // xinputhid is commonly an UpperFilter, not the function Service.
        // Treating that node as plain HID would leave native XInput bypassing us.
        for(const auto property:{CM_DRP_UPPERFILTERS,CM_DRP_LOWERFILTERS}) {
            ULONG required=0;
            const auto first=CM_Get_DevNode_Registry_PropertyW(node,property,&type,nullptr,&required,0);
            if(first==CR_NO_SUCH_VALUE)continue;
            if(first!=CR_BUFFER_SMALL || !required || required>65536 || required%sizeof(wchar_t))return true;
            std::vector<wchar_t> filters(required/sizeof(wchar_t)+2,0);
            if(CM_Get_DevNode_Registry_PropertyW(node,property,&type,filters.data(),&required,0)!=CR_SUCCESS || type!=REG_MULTI_SZ)return true;
            for(const auto* name=filters.data();*name;name+=wcslen(name)+1)
                if(IcNativeControllerService(name))return true;
        }
        DEVINST parent=0;const auto result=CM_Get_Parent(&parent,node,0);
        if(result==CR_NO_SUCH_DEVNODE)return false;
        if(result!=CR_SUCCESS)return true;
        node=parent;
    }
    return true;
}
void WriteFilters(HDEVINFO set,SP_DEVINFO_DATA& device,const std::vector<wchar_t>& filters) {
    const bool empty=!filters[0];
    if(!SetupDiSetDeviceRegistryPropertyW(set,&device,SPDRP_UPPERFILTERS,
        empty?nullptr:reinterpret_cast<const BYTE*>(filters.data()),empty?0:static_cast<DWORD>(filters.size()*sizeof(wchar_t))))
        throw std::system_error(GetLastError(),std::system_category(),"Write per-device HID filters");
}
bool ActiveControllerFilter(const std::wstring& id,const GUID& guid=IC_HID_INTERFACE_GUID) {
    DeviceSet set{SetupDiGetClassDevsW(&guid,id.c_str(),nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
    if(set.value==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Query active controller filter");
    SP_DEVICE_INTERFACE_DATA item{};item.cbSize=sizeof(item);
    if(!SetupDiEnumDeviceInterfaces(set.value,nullptr,&guid,0,&item)) {
        if(GetLastError()==ERROR_NO_MORE_ITEMS)return false;
        throw std::system_error(GetLastError(),std::system_category(),"Query controller interface");
    }
    return (item.Flags&SPINT_ACTIVE)!=0;
}
std::vector<std::wstring> FilterNames(const std::vector<wchar_t>& values);
bool NativeClassFilterMissing(const GUID& deviceClass,DWORD property);
std::vector<std::wstring> NativeClassFilters(const GUID& deviceClass,DWORD property,
    decltype(&SetupDiGetClassRegistryPropertyW) query=SetupDiGetClassRegistryPropertyW,
    bool (*missing)(const GUID&,DWORD)=NativeClassFilterMissing);
bool ForeignStackFilters(HDEVINFO set,SP_DEVINFO_DATA& device,Init_Result& result,const std::wstring& id) {
    const bool foreign=Policy::ForeignFilters(FilterNames(MultiProperty(set,device,SPDRP_UPPERFILTERS))) ||
        Policy::ForeignFilters(FilterNames(MultiProperty(set,device,SPDRP_LOWERFILTERS))) ||
        Policy::ForeignFilters(NativeClassFilters(device.ClassGuid,SPCRP_UPPERFILTERS)) ||
        Policy::ForeignFilters(NativeClassFilters(device.ClassGuid,SPCRP_LOWERFILTERS));
    if(foreign) {
        ++result.skipped;RecordError(result,ERROR_SHARING_VIOLATION);
        result.skipped_devices.push_back({id,L"Another Interceptor product filters this stack; uninstall that product first",ERROR_SHARING_VIOLATION});
    }
    return foreign;
}
void AssociateControllers(Init_Result& result,bool remove) {
    unsigned profiles=IC_HID_PROFILE_DESKTOP;
    if(!remove)try{profiles=Get_Device_Details().hid_profiles;}
    catch(const std::system_error&){/* Old ABI4 drivers retain their original HID contract. */}
    // Snapshot IDs; restarting a devnode can invalidate live enumeration.
    std::vector<std::wstring> targets;
    DeviceSet snapshot{SetupDiGetClassDevsW(&GUID_DEVCLASS_HIDCLASS,nullptr,nullptr,remove?0:DIGCF_PRESENT)};
    if(snapshot.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());return;}
    for(DWORD i=0;;++i) {
        SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);
        if(!SetupDiEnumDeviceInfo(snapshot.value,i,&d)) {if(GetLastError()!=ERROR_NO_MORE_ITEMS)RecordError(result,GetLastError());break;}
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if(!SetupDiGetDeviceInstanceIdW(snapshot.value,&d,id,MAX_DEVICE_ID_LEN,nullptr)) {RecordError(result,GetLastError());continue;}
        targets.emplace_back(id);
    }
    for(const auto& id:targets)try {
        DeviceSet set{SetupDiCreateDeviceInfoList(nullptr,nullptr)};
        if(set.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());continue;}
        SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
        if(!SetupDiOpenDeviceInfoW(set.value,id.c_str(),nullptr,0,&device)) {RecordError(result,GetLastError());continue;}
        if(!remove && (NativeXbox(device.DevInst) || !ControllerCollection(set.value,device,profiles)))continue;
        if(!remove && ForeignStackFilters(set.value,device,result,id))continue;
        const auto original=MultiProperty(set.value,device,SPDRP_UPPERFILTERS);
        std::vector<wchar_t> updated;bool found=false;
        for(const auto* name=original.data();*name;name+=wcslen(name)+1) {
            if(!_wcsicmp(name,AssociationTarget().service)) {found=true;if(remove)continue;}
            updated.insert(updated.end(),name,name+wcslen(name)+1);
        }
        if(found==!remove) {
            if(!remove) {
                ++result.already_associated;CheckStarted(device,result);
                if(!ActiveControllerFilter(id))result.reboot_required=true;
            }
            continue;
        }
        if(!remove) {const auto* name=AssociationTarget().service;updated.insert(updated.end(),name,name+wcslen(name)+1);}
        updated.push_back(0);if(updated.size()==1)updated.push_back(0);
        WriteFilters(set.value,device,updated);
        ++result.associated;
        // Defer stack reconstruction to an explicit reboot. This avoids restarting
        // another application's controller while it has pending requests open.
        // An absent instance needs no immediate restart when removing its filter.
        ULONG status=0,problem=0;
        if(CM_Get_DevNode_Status(&status,&problem,device.DevInst,0)==CR_SUCCESS && (status&DN_STARTED))result.reboot_required=true;
        else if(!remove)result.reboot_required=true;
    } catch(const std::system_error& e) {RecordError(result,static_cast<DWORD>(e.code().value()),e.what(),id);}
}

// ---- NativeDevices.inl ----
// Included by InitDevices.cpp after HidDevices.inl. Per-instance associations
// only; never install a class-wide filter or replace a native function service.
std::vector<std::wstring> FilterNames(const std::vector<wchar_t>& values) {
    std::vector<std::wstring> out;
    for(auto* p=values.data();*p;p+=wcslen(p)+1)out.emplace_back(p);
    return out;
}
bool NativeClassFilterMissing(const GUID& deviceClass,DWORD property) {
    const HKEY key=SetupDiOpenClassRegKeyExW(&deviceClass,KEY_QUERY_VALUE,DIOCR_INSTALLER,nullptr,nullptr);
    if(key==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Open native setup-class key");
    DWORD bytes=0;const auto status=RegQueryValueExW(key,property==SPCRP_UPPERFILTERS?L"UpperFilters":L"LowerFilters",
        nullptr,nullptr,nullptr,&bytes);RegCloseKey(key);
    if(status==ERROR_FILE_NOT_FOUND)return true;
    if(status!=ERROR_SUCCESS)throw std::system_error(status,std::system_category(),"Check missing native class filters");
    return false;
}
std::vector<std::wstring> NativeClassFilters(const GUID& deviceClass,DWORD property,
    decltype(&SetupDiGetClassRegistryPropertyW) query,
    bool (*missing)(const GUID&,DWORD)) {
    // Query the setup-class property through SetupAPI, never derive a registry
    // path from device text. Missing filters are distinct from an unreadable or
    // malformed value; only the former means no class association.
    if(property!=SPCRP_UPPERFILTERS && property!=SPCRP_LOWERFILTERS)
        throw std::system_error(ERROR_INVALID_PARAMETER,std::system_category(),"Invalid native class filter property");
    DWORD bytes=0,type=0;
    const BOOL queried=query(&deviceClass,property,&type,nullptr,0,&bytes,nullptr,nullptr);
    const DWORD error=queried?ERROR_SUCCESS:GetLastError();
    if(!queried && (error==ERROR_INVALID_DATA || error==ERROR_FILE_NOT_FOUND)) {
        // ERROR_INVALID_DATA is also possible for invalid registry data. Verify
        // absence instead of interpreting a corrupt existing value as empty.
        if(missing(deviceClass,property))return {};
        throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Unreadable existing native class filters");
    }
    if((!queried && error!=ERROR_INSUFFICIENT_BUFFER) || bytes<sizeof(wchar_t) ||
       bytes>65536 || bytes%sizeof(wchar_t))
        throw std::system_error(static_cast<int>(!queried && error!=ERROR_INSUFFICIENT_BUFFER?error:ERROR_INVALID_DATA),
            std::system_category(),"Read native class filters size");
    std::vector<wchar_t> data(bytes/sizeof(wchar_t));const DWORD capacity=bytes;
    if(!query(&deviceClass,property,&type,reinterpret_cast<BYTE*>(data.data()),capacity,&bytes,nullptr,nullptr))
        throw std::system_error(GetLastError(),std::system_category(),"Read native class filters");
    if(type!=REG_MULTI_SZ || bytes<sizeof(wchar_t) || bytes>capacity || bytes%sizeof(wchar_t))
        throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Invalid native class filter type/size");
    const auto length=bytes/sizeof(wchar_t);
    // An empty REG_MULTI_SZ is one empty string (a single NUL). Nonempty
    // sequences still require the final string terminator plus the list NUL.
    if(length==1) {
        if(!data[0])return {};
        throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Unterminated native class filters");
    }
    if(data[length-1] || data[length-2])
        throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Unterminated native class filters");
    std::vector<std::wstring> result;std::size_t at=0;
    while(at<length && data[at]) {
        const auto end=std::find(data.begin()+at,data.begin()+length,L'\0');
        result.emplace_back(data.begin()+at,end);at=static_cast<std::size_t>(end-data.begin())+1;
    }
    if(std::any_of(data.begin()+at,data.begin()+length,[](wchar_t c){return c!=0;}))
        throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Trailing native class filter data");
    return result;
}
std::uint32_t NativePropertyFlags(HDEVINFO set,SP_DEVINFO_DATA& device) {
    // This is xinputhid's per-devnode SOFTWARE key. Never set/guess these flags:
    // GIP's translated child normally has 14, while our generic profile needs 6.
    HKEY key=SetupDiOpenDevRegKey(set,&device,DICS_FLAG_GLOBAL,0,DIREG_DRV,KEY_QUERY_VALUE);
    if(key==INVALID_HANDLE_VALUE)return UINT32_MAX;
    DWORD value=0,type=0,bytes=sizeof(value);
    const auto status=RegQueryValueExW(key,L"DevicePropertyFlags",nullptr,&type,reinterpret_cast<BYTE*>(&value),&bytes);
    RegCloseKey(key);
    return status==ERROR_SUCCESS && type==REG_DWORD && bytes==sizeof(value)?value:UINT32_MAX;
}
bool NativeControllerParent(DEVINST node) {
    DEVINST parent=0;const auto result=CM_Get_Parent(&parent,node,0);
    if(result==CR_NO_SUCH_DEVNODE)return false;
    return result!=CR_SUCCESS || NativeXbox(parent);
}
void WriteNativeFilters(HDEVINFO set,SP_DEVINFO_DATA& device,DWORD property,const std::vector<std::wstring>& values) {
    std::vector<wchar_t> bytes;
    for(const auto& value:values)bytes.insert(bytes.end(),value.c_str(),value.c_str()+value.size()+1);
    bytes.push_back(0);if(bytes.size()==1)bytes.push_back(0);
    if(!SetupDiSetDeviceRegistryPropertyW(set,&device,property,values.empty()?nullptr:reinterpret_cast<const BYTE*>(bytes.data()),
        values.empty()?0:static_cast<DWORD>(bytes.size()*sizeof(wchar_t))))
        throw std::system_error(GetLastError(),std::system_category(),"Write per-device native controller filters");
}
template<class Write> bool ConfigureXusbUsbFilters(Init_Result& result,
    const std::vector<std::wstring>& upper,const std::vector<std::wstring>& lower,Write&& write) {
    const auto cleaned=Policy::Filters(upper,0,true);
    const auto installed=Policy::Filters(lower,IC_CONTROLLER_XUSB,false);
    bool changed=false;
    // Remove the obsolete upper registration first. If a later write fails,
    // the next boot is transparent rather than loading this service twice.
    if(cleaned!=upper){write(SPDRP_UPPERFILTERS,cleaned);result.reboot_required=true;changed=true;}
    if(installed!=lower){write(SPDRP_LOWERFILTERS,installed);result.reboot_required=true;changed=true;}
    return changed;
}
void AssociateNativeControllers(Init_Result& result,const IC_CAPABILITIES* capabilities,bool remove) {
    const GUID classes[]={GUID_DEVCLASS_HIDCLASS,
        {0xd61ca365,0x5af4,0x4486,{0x99,0x8b,0x9d,0xb4,0x73,0x4c,0x6c,0xa3}},
        {0x05f5cfe2,0x4733,0x4950,{0xa6,0xbb,0x07,0xaa,0xd0,0x1a,0x3a,0x84}}};
    std::vector<std::wstring> targets;
    for(const auto& cls:classes) {
        DeviceSet set{SetupDiGetClassDevsW(&cls,nullptr,nullptr,remove?0:DIGCF_PRESENT)};
        if(set.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());continue;}
        for(DWORD index=0;;++index) {
            SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
            if(!SetupDiEnumDeviceInfo(set.value,index,&device)) {if(GetLastError()!=ERROR_NO_MORE_ITEMS)RecordError(result,GetLastError());break;}
            wchar_t id[MAX_DEVICE_ID_LEN]{};
            if(!SetupDiGetDeviceInstanceIdW(set.value,&device,id,MAX_DEVICE_ID_LEN,nullptr)) {RecordError(result,GetLastError());continue;}
            targets.emplace_back(id);
        }
    }
    for(const auto& id:targets)try {
        DeviceSet set{SetupDiCreateDeviceInfoList(nullptr,nullptr)};
        if(set.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());continue;}
        SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
        if(!SetupDiOpenDeviceInfoW(set.value,id.c_str(),nullptr,0,&device)) {RecordError(result,GetLastError());continue;}
        const auto upper=FilterNames(MultiProperty(set.value,device,SPDRP_UPPERFILTERS));
        const auto lower=FilterNames(MultiProperty(set.value,device,SPDRP_LOWERFILTERS));
        if(remove) {
            // Includes absent instances and legacy placement. Remove only our
            // exact service name, preserving every other name and its order.
            bool changed=false;
            for(const auto property:{SPDRP_UPPERFILTERS,SPDRP_LOWERFILTERS}) {
                const auto& original=property==SPDRP_UPPERFILTERS?upper:lower;
                const auto updated=Policy::Filters(original,0,true);
                if(updated!=original){WriteNativeFilters(set.value,device,property,updated);changed=true;}
            }
            if(changed) {
                ++result.associated;ULONG state=0,problem=0;
                if(CM_Get_DevNode_Status(&state,&problem,device.DevInst,0)==CR_SUCCESS && (state&DN_STARTED))result.reboot_required=true;
            }
            continue;
        }
        Policy::NativeNode node;node.instance=id;node.upper=upper;
        node.deviceClass=Property(set.value,device,SPDRP_CLASSGUID);
        try {node.service=Property(set.value,device,SPDRP_SERVICE);}
        catch(const std::system_error& e) {if(e.code().value()!=ERROR_INVALID_DATA && e.code().value()!=ERROR_FILE_NOT_FOUND)throw;}
        node.hardware=FilterNames(MultiProperty(set.value,device,SPDRP_HARDWAREID));
        node.compatible=FilterNames(MultiProperty(set.value,device,SPDRP_COMPATIBLEIDS));
        if(Policy::Has(upper,L"xinputhid")) {node.propertyFlags=NativePropertyFlags(set.value,device);node.blockedAncestry=NativeControllerParent(device.DevInst);}
        const auto transport=Policy::Transport(node);if(!transport) {if(Policy::Has(upper,L"xinputhid"))++result.skipped;continue;}
        if(ForeignStackFilters(set.value,device,result,id))continue;
        if(!capabilities || !(Policy::SupportedControllers(*capabilities)&transport)) {++result.skipped;continue;}
        if(transport==IC_CONTROLLER_XUSB && !Policy::XusbProfileAvailable(node,*capabilities)){++result.skipped;continue;}
        if(transport==IC_CONTROLLER_XUSB && !Policy::UsbXusbBackend(*capabilities)) {
            ++result.skipped;result.skipped_devices.push_back({id,L"Legacy XUSB backend: install the current driver package before migrating to a USB lower filter",ERROR_NOT_SUPPORTED});continue;
        }
        const auto classUpper=NativeClassFilters(device.ClassGuid,SPCRP_UPPERFILTERS);
        const auto classLower=NativeClassFilters(device.ClassGuid,SPCRP_LOWERFILTERS);
        const auto placement=Policy::ClassFilterAssociation(transport,upper,lower,classUpper,classLower);
        if(placement==Policy::ClassAssociation::Conflict) {
            RecordError(result,ERROR_INVALID_DATA,"Conflicting native class/device filter placement",id);continue;
        }
        if(transport==IC_CONTROLLER_XUSB) {
            if(ConfigureXusbUsbFilters(result,upper,lower,[&](DWORD property,const auto& filters){WriteNativeFilters(set.value,device,property,filters);}))++result.associated;
            else {
                ++result.already_associated;CheckStarted(device,result);
                const GUID guid=IC_XUSB_INTERFACE_GUID;if(!ActiveControllerFilter(id,guid))result.reboot_required=true;
            }
            continue;
        }
        const bool gip=transport==IC_CONTROLLER_GIP;
        if(Policy::Has(gip?upper:lower,AssociationTarget().service)) {
            // Do not introduce two Interceptor instances on one stack. A legacy
            // wrong-side association needs explicit uninstall/reinstall first.
            RecordError(result,ERROR_INVALID_DATA,"Interceptor registered on the unexpected native filter side",id);continue;
        }
        const auto property=gip?SPDRP_LOWERFILTERS:SPDRP_UPPERFILTERS;
        const auto& original=gip?lower:upper;
        const auto updated=Policy::Filters(original,transport,false);
        if(original==updated) {
            ++result.already_associated;CheckStarted(device,result);
            const GUID gipGuid=IC_GIP_INTERFACE_GUID,xusbGuid=IC_XUSB_INTERFACE_GUID,xihGuid=IC_XINPUTHID_INTERFACE_GUID;
            const auto& guid=gip?gipGuid:transport==IC_CONTROLLER_XUSB?xusbGuid:xihGuid;
            if(!ActiveControllerFilter(id,guid))result.reboot_required=true;
        } else {
            WriteNativeFilters(set.value,device,property,updated);++result.associated;
            // Pending I/O may belong to other applications. Only the explicit
            // restart reconstructs the stack; never restart a controller here.
            result.reboot_required=true;
        }
    } catch(const std::system_error& e) {RecordError(result,static_cast<DWORD>(e.code().value()),e.what(),id);}
}

// ---- XusbMirrors.inl ----
// Cleanup is authorized by this product's own identity marker or filter, not
// by unrelated ancestry. Historical HID views may have classless ghost parents.
struct MirrorRegistry {
    HKEY value;
    ~MirrorRegistry(){if(value!=INVALID_HANDLE_VALUE)RegCloseKey(value);}
};
bool RemoveMirrorIdentity(HKEY key,Init_Result& result,bool started,decltype(&RegDeleteValueW) remove=RegDeleteValueW) {
    bool changed=false;
    for(const auto* name:{AssociationTarget().xusbHidParent,AssociationTarget().xusbParent}) {
        const auto status=remove(key,name);
        if(status==ERROR_SUCCESS){changed=true;result.reboot_required|=started;}
        else if(status!=ERROR_FILE_NOT_FOUND)throw std::system_error(status,std::system_category(),"Remove legacy XUSB mirror identity");
    }
    return changed;
}
void CleanupLegacyXusbMirrors(Init_Result& result) {
    // Include absent instances, but touch only our own legacy mirror state.
    DeviceSet snapshot{SetupDiGetClassDevsW(&GUID_DEVCLASS_HIDCLASS,nullptr,nullptr,0)};
    if(snapshot.value==INVALID_HANDLE_VALUE){RecordError(result,GetLastError());return;}
    std::vector<std::wstring> ids;
    for(DWORD index=0;;++index) {
        SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);
        if(!SetupDiEnumDeviceInfo(snapshot.value,index,&d)){if(GetLastError()!=ERROR_NO_MORE_ITEMS)RecordError(result,GetLastError());break;}
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if(!SetupDiGetDeviceInstanceIdW(snapshot.value,&d,id,MAX_DEVICE_ID_LEN,nullptr)){RecordError(result,GetLastError());continue;}
        if(Policy::XusbMirrorGroup(id)>=0)ids.emplace_back(id);
    }
    for(const auto& id:ids)try {
        DeviceSet set{SetupDiCreateDeviceInfoList(nullptr,nullptr)};SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);
        if(set.value==INVALID_HANDLE_VALUE || !SetupDiOpenDeviceInfoW(set.value,id.c_str(),nullptr,0,&d)){RecordError(result,GetLastError());continue;}
        MirrorRegistry key{SetupDiOpenDevRegKey(set.value,&d,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_QUERY_VALUE|KEY_SET_VALUE)};
        if(key.value==INVALID_HANDLE_VALUE){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_INVALID_DATA)continue;throw std::system_error(error,std::system_category(),"Open legacy XUSB mirror key");}
        bool marked=false;
        for(const auto* name:{AssociationTarget().xusbHidParent,AssociationTarget().xusbParent}) {
            const auto status=RegQueryValueExW(key.value,name,nullptr,nullptr,nullptr,nullptr);
            if(status==ERROR_SUCCESS||status==ERROR_MORE_DATA)marked=true;
            else if(status!=ERROR_FILE_NOT_FOUND)throw std::system_error(status,std::system_category(),"Read legacy XUSB mirror marker");
        }
        const auto upper=FilterNames(MultiProperty(set.value,d,SPDRP_UPPERFILTERS));
        const auto lower=FilterNames(MultiProperty(set.value,d,SPDRP_LOWERFILTERS));
        if(!marked && ((!Policy::Has(upper,AssociationTarget().service)&&!Policy::Has(lower,AssociationTarget().service)) ||
           Policy::Has(upper,L"xinputhid")))continue;
        ULONG state=0,problem=0;const bool started=CM_Get_DevNode_Status(&state,&problem,d.DevInst,0)==CR_SUCCESS&&(state&DN_STARTED);
        bool changed=false;
        for(const auto property:{SPDRP_UPPERFILTERS,SPDRP_LOWERFILTERS}) {
            const auto& original=property==SPDRP_UPPERFILTERS?upper:lower;
            const auto cleaned=Policy::Filters(original,0,true);
            if(cleaned!=original){WriteNativeFilters(set.value,d,property,cleaned);changed=true;result.reboot_required|=started;}
        }
        changed=RemoveMirrorIdentity(key.value,result,started)||changed;
        if(changed)++result.associated;
    }catch(const std::system_error& error){RecordError(result,static_cast<DWORD>(error.code().value()),error.what(),id);}
}
void CleanupLegacyXusbClassFilters(Init_Result& result) {
    // Historical laboratory/legacy XNA class placement is no longer a capture
    // contract. Remove only this service from this exact class, preserving all
    // other products and the original order. Never create a new class filter.
    const GUID xna={0xd61ca365,0x5af4,0x4486,{0x99,0x8b,0x9d,0xb4,0x73,0x4c,0x6c,0xa3}};
    const HKEY existing=SetupDiOpenClassRegKeyExW(&xna,KEY_QUERY_VALUE,DIOCR_INSTALLER,nullptr,nullptr);
    if(existing==INVALID_HANDLE_VALUE){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND||error==ERROR_INVALID_CLASS)return;RecordError(result,error);return;}
    RegCloseKey(existing);
    for(const auto property:{SPCRP_UPPERFILTERS,SPCRP_LOWERFILTERS})try {
        const auto original=NativeClassFilters(xna,property);
        const auto cleaned=Policy::Filters(original,0,true);
        if(cleaned==original)continue;
        std::vector<wchar_t> data;
        for(const auto& name:cleaned)data.insert(data.end(),name.c_str(),name.c_str()+name.size()+1);
        data.push_back(0);if(data.size()==1)data.push_back(0);
        if(!SetupDiSetClassRegistryPropertyW(&xna,property,cleaned.empty()?nullptr:reinterpret_cast<const BYTE*>(data.data()),cleaned.empty()?0:static_cast<DWORD>(data.size()*sizeof(wchar_t)),nullptr,nullptr))
            throw std::system_error(GetLastError(),std::system_category(),"Remove legacy XUSB class filter");
        ++result.associated;result.reboot_required=true;
    }catch(const std::system_error& error){RecordError(result,static_cast<DWORD>(error.code().value()),error.what(),L"Setup class {D61CA365-5AF4-4486-998B-9DB4734C6CA3}");}
}
void CleanupLegacyXusb(Init_Result& result) {
    CleanupLegacyXusbClassFilters(result);CleanupLegacyXusbMirrors(result);
}
}

static Init_Result Initialize() {
    static std::mutex mutex;std::lock_guard<std::mutex> guard(mutex);
    Init_Result result;
    if(!IsAdministrator()) {RecordError(result,ERROR_ACCESS_DENIED,"Initialization requires administrator rights");return result;}
    Handle driver;
    try{Open(driver);}catch(const std::system_error& error){result.error=static_cast<DWORD>(error.code().value());return result;}
    const EditionScope scope(driver.target);
    MutexHandle interprocess{CreateMutexW(nullptr,FALSE,AssociationTarget().mutexName),false};
    if(!interprocess.value) {RecordError(result,GetLastError(),"Create initialization mutex");return result;}
    const DWORD wait=WaitForSingleObject(interprocess.value,30000);
    if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED) {RecordError(result,wait==WAIT_TIMEOUT?ERROR_BUSY:GetLastError(),"Wait for another initialization to finish");return result;}
    interprocess.owned=true;
    result.error=PendingRestart(result.reboot_required);
    if(result.error) {result.error_detail="Read pending Windows restart state";return result;}
    if(result.reboot_required)return result;
    // Snapshot instance IDs before changing stacks; updating one device can
    // invalidate an ongoing enumeration. Reopen and recheck each instance.
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE
    struct Target {std::wstring id;bool mouse;};std::vector<Target> targets;
    const GUID* classes[]={&GUID_DEVCLASS_KEYBOARD,&GUID_DEVCLASS_MOUSE};
    for(unsigned kind=0;kind<2;++kind) {
        DeviceSet devices{SetupDiGetClassDevsW(classes[kind],nullptr,nullptr,DIGCF_PRESENT)};
        if(devices.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());continue;}
        for(DWORD index=0;;++index) {
            SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
            if(!SetupDiEnumDeviceInfo(devices.value,index,&device)) {if(GetLastError()!=ERROR_NO_MORE_ITEMS)RecordError(result,GetLastError());break;}
            wchar_t id[MAX_DEVICE_ID_LEN]{};
            if(!SetupDiGetDeviceInstanceIdW(devices.value,&device,id,MAX_DEVICE_ID_LEN,nullptr)) {RecordError(result,GetLastError());continue;}
            targets.push_back({id,kind!=0});
        }
    }
    for(const auto& target:targets) {
        DeviceSet devices{SetupDiCreateDeviceInfoList(nullptr,nullptr)};
        if(devices.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());break;}
        SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
        if(!SetupDiOpenDeviceInfoW(devices.value,target.id.c_str(),nullptr,0,&device)) {RecordError(result,GetLastError());continue;}
        try {Associate(devices.value,device,target.mouse,result);}
        catch(const std::system_error& e) {RecordError(result,static_cast<DWORD>(e.code().value()),e.what(),target.id);}
    }
    #endif
#if IC_PRODUCT_HAS_CONTROLLERS
    if(!result.error && !result.reboot_required) {
        // Never attach the HID filter when an older keyboard-only driver is loaded.
        try {
            if(Get_Status().version!=IC_VERSION)throw std::system_error(ERROR_REVISION_MISMATCH,std::system_category());
            IC_CAPABILITIES capabilities{};bool available=false;
            try {capabilities=Get_Capabilities();available=true;}
            catch(const std::system_error& e) {
                // ABI4 keyboard/HID drivers predate the immutable endpoint.
                // Missing endpoint permits no native association whatsoever.
                if(e.code().value()!=ERROR_INVALID_FUNCTION && e.code().value()!=ERROR_NOT_SUPPORTED)throw;
            }
            if(available&&Policy::UsbXusbBackend(capabilities))CleanupLegacyXusb(result);
            if(!result.error)AssociateControllers(result,false);
            if(!result.error)AssociateNativeControllers(result,available?&capabilities:nullptr,false);
        }
        catch(const std::system_error& e) {RecordError(result,static_cast<DWORD>(e.code().value()),e.what());}
    }
    #endif
    if(result.reboot_required) {
        const DWORD error=RememberRestart();if(error)RecordError(result,error,"Record required Windows restart");
    }
    if(!result.error && !result.reboot_required) {
        try {if(Get_Status().version!=IC_VERSION)RecordError(result,ERROR_REVISION_MISMATCH,"Verify driver ABI after association");}
        catch(const std::system_error& e) {RecordError(result,static_cast<DWORD>(e.code().value()),e.what());}
    }
    result.ready=!result.error && !result.reboot_required;
    return result;
}
// Installer helper. Caller must be elevated and serialize setup operations.
static Init_Result RestoreFilters() {
    Init_Result result;
    if(!IsAdministrator()) {RecordError(result,ERROR_ACCESS_DENIED,"Restoration requires administrator rights");return result;}
#if IC_PRODUCT_HAS_CONTROLLERS
    AssociateControllers(result,true);AssociateNativeControllers(result,nullptr,true);CleanupLegacyXusb(result);
#endif
    return result;
}

}

Init_Result Init_AllDevices() {return Init_Implementation::Initialize();}
Init_Result Restore_Controller_Hid_Filters() {return Init_Implementation::RestoreFilters();}
}

#include "Interceptor.h"
#include <shlobj.h>
#include <cfgmgr32.h>
#include <cwchar>
#include <set>
#include <string_view>
#define IC_SDK_BUILD_VERSION "1.6.1.3"
#include <map>
#include <charconv>
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"uuid.lib")

namespace Interceptor {
namespace {
std::string InstalledDriverVersions(const wchar_t* service) {
    constexpr ULONG flags = CM_GETIDLIST_FILTER_SERVICE | CM_GETIDLIST_FILTER_PRESENT;
    // DEVPKEY_Device_DriverVersion identifies the installed INF version for a device.
    constexpr DEVPROPKEY driverVersion{{0xa8b865dd,0x2e3d,0x4094,{0xad,0x97,0xe5,0x93,0xa7,0x0c,0x75,0xd6}},3};
    std::vector<wchar_t> ids;
    CONFIGRET status = CR_BUFFER_SMALL;
    for (int attempt = 0; attempt < 2 && status == CR_BUFFER_SMALL; ++attempt) {
        ULONG count = 0;
        status = CM_Get_Device_ID_List_SizeW(&count,service,flags);
        if (status != CR_SUCCESS) break;
        if (count <= 1) return "not detected";
        ids.resize(count);
        status = CM_Get_Device_ID_ListW(service,ids.data(),count,flags);
    }
    if (status != CR_SUCCESS) return "unavailable";

    std::set<std::string> versions;
    bool unreadable = false;
    for (wchar_t* id = ids.data(); *id; id += std::wcslen(id) + 1) {
        DEVINST device{};
        DEVPROPTYPE type{};
        wchar_t value[64]{};
        ULONG bytes = sizeof(value);
        if (CM_Locate_DevNodeW(&device,id,CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS ||
            CM_Get_DevNode_PropertyW(device,&driverVersion,&type,reinterpret_cast<BYTE*>(value),&bytes,0) != CR_SUCCESS ||
            type != DEVPROP_TYPE_STRING || bytes < 2 * sizeof(wchar_t) || bytes > sizeof(value) ||
            bytes % sizeof(wchar_t) != 0 || value[bytes / sizeof(wchar_t) - 1] != L'\0') {
            unreadable = true;
            continue;
        }
        const std::wstring_view version(value,bytes / sizeof(wchar_t) - 1);
        if (version.find_first_not_of(L"0123456789.") != std::wstring_view::npos) {
            unreadable = true;
            continue;
        }
        std::string ascii;
        ascii.reserve(version.size());
        for (wchar_t character : version) ascii += static_cast<char>(character);
        versions.insert(std::move(ascii));
    }
    std::string result;
    for (const auto& version : versions) {
        if (!result.empty()) result += ", ";
        result += version;
    }
    if (unreadable) result += result.empty() ? "unavailable" : ", unavailable on another device";
    return result.empty() ? "not detected" : result;
}

void AppendDriverVersions(std::string& message) {
    const DWORD previous = GetLastError();
    try {
        std::string details = "\nInterceptor Release SDK: " IC_SDK_BUILD_VERSION "\nInterceptor driver (installed): ";
        details += InstalledDriverVersions(L"Interceptor");
        message += details;
    } catch (const std::bad_alloc&) {
        // Preserve the original error if diagnostics cannot allocate memory.
    }
    SetLastError(previous);
}

enum class DriverState { Unknown, Absent, Stopped, Running };
DriverState QueryDriverState(const wchar_t* name) {
    SC_HANDLE manager = OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if (!manager) return DriverState::Unknown;
    SC_HANDLE service = OpenServiceW(manager,name,SERVICE_QUERY_STATUS);
    DriverState result = DriverState::Unknown;
    if (service) {
        SERVICE_STATUS status{};
        if (QueryServiceStatus(service,&status))
            result = status.dwCurrentState == SERVICE_RUNNING ? DriverState::Running : DriverState::Stopped;
        CloseServiceHandle(service);
    } else if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
        result = DriverState::Absent;
    }
    CloseServiceHandle(manager);
    return result;
}

const char* ActivationOpenDiagnostic() {
    switch (QueryDriverState(L"Interceptor")) {
    case DriverState::Absent:

        return "The Interceptor driver service was not found. Run the Release installer and restart if requested.";
    case DriverState::Stopped:
        return "The Interceptor driver is registered, but Windows has not started it. Run the Release installer diagnostics.";
    case DriverState::Running:
        return "The Interceptor driver is running, but its activation interface is unavailable. Repair the Release installation and restart if requested.";
    default:
        return "Windows could not open the Interceptor activation interface. Check the Release installation and restart if requested.";
    }
}
}
}
// ---- Licensing/Core.h ----
#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace license {
using Bytes = std::vector<std::uint8_t>;
struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
struct HttpError : Error {
    std::uint32_t status;
    std::string responseCode;
    HttpError(std::uint32_t value, std::string code, std::string detail = {});
};
std::string Base64(const Bytes& bytes);
Bytes Decode64(std::string_view text, std::size_t maximum = 65536);
std::string Hex(const Bytes& bytes);
Bytes DecodeHex(std::string_view text, std::size_t exact);
Bytes LicenseKeyIdentifier(std::string_view text);
struct Value {
    enum class Kind { String, Number, Boolean, Null } kind = Kind::Null;
    std::string text;
};
using Object = std::map<std::string, Value>;
Object ParseObject(std::string_view json);
HttpError ParseHttpError(std::uint32_t status, std::string_view body);
// Printable ASCII only, bounded, with activation-key tokens redacted.
std::string SanitizeDiagnostic(std::string_view text, std::size_t maximum = 512);
std::string String(const Object& value, const char* name, std::size_t maximum = 65536);
std::uint64_t Number(const Object& value, const char* name);
std::string Quote(std::string_view text);
std::string Json(const std::map<std::string, std::string>& values);
struct Challenge {
    std::array<std::uint8_t, 32> boot{};
    std::array<std::uint8_t, 32> request{};
    std::uint32_t maximumSeconds = 0;
    std::uint32_t flags = 0;
};
Challenge ParseChallenge(const Bytes& raw);
struct AuthorizationReply {
    std::string key;
    std::string customer;
    Bytes authorization;
    std::uint64_t generation = 0;
};
AuthorizationReply ParseAuthorizationReply(std::string_view text, const Challenge& challenge, const Bytes& keyIdentifier);
}

// ---- Licensing/Platform.h ----
#include <windows.h>
#include <winioctl.h>
#include <string>

namespace license {
std::string WindowsDiagnostic(const char* action, unsigned long code);
struct WindowsError : Error {
    unsigned long code;
    std::string phase,reason;
    unsigned long secureFailureFlags=0,extendedError=0;
    bool retryable=false,failoverEligible=false;
    WindowsError(std::string action,unsigned long value):Error(WindowsDiagnostic(action.c_str(),value)),code(value){}
    WindowsError(std::string action,unsigned long value,std::string phaseValue,std::string reasonValue,
        unsigned long secureFlags=0,unsigned long extended=0,bool canRetry=false);
};
[[noreturn]] void WinError(const char* action, unsigned long code = GetLastError());
class Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept:h(value) {}
    ~Handle() { if(h!=INVALID_HANDLE_VALUE&&h!=nullptr)CloseHandle(h); }
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:h(other.h) {other.h=INVALID_HANDLE_VALUE;}
    HANDLE get() const noexcept {return h;}
};
std::wstring Widen(std::string_view text);
Bytes Random(std::size_t count);
bool IsSystem();
bool IsAdministrator();
class Storage {
    std::wstring folder;
public:
    Storage();
    std::wstring Path(const wchar_t* name) const;
    void Remove(const wchar_t* name) const;
    Handle Lock() const;
};
// One shared monotonic deadline covers every request in a verification. Zero
// starts a 25-second budget. Only authenticated, duplicated caller tokens belong
// here; impersonation is confined to the network and restored before returning.
std::string Post(const wchar_t* endpoint, std::string_view json, ULONGLONG absoluteDeadline=0, HANDLE callerNetworkToken=nullptr, unsigned route=0);
std::string DiagnoseNetwork(ULONGLONG absoluteDeadline=0, HANDLE callerNetworkToken=nullptr);
class Driver {
    Handle handle;
public:
    Driver();
    Bytes Challenge() const;
    std::string Status() const;
    void Apply(const Bytes& lease) const;
    void Clear() const;
};
std::wstring ModulePath();
}

// ---- Licensing/Core.cpp ----
#include <algorithm>
#include <charconv>
#include <limits>

namespace license {
std::string SanitizeDiagnostic(std::string_view text,std::size_t maximum) {
    if(text.size()>maximum||!std::all_of(text.begin(),text.end(),[](unsigned char c){return c>=32&&c<127;}))return {};
    std::string result(text);std::size_t offset=0;
    while((offset=result.find("icv4_",offset))!=std::string::npos){
        auto end=offset+5;
        while(end<result.size()&&((result[end]>='a'&&result[end]<='z')||(result[end]>='A'&&result[end]<='Z')||(result[end]>='0'&&result[end]<='9')||result[end]=='_'||result[end]=='-'||result[end]=='.'))++end;
        result.replace(offset,end-offset,"[redacted]");offset+=10;
    }
    return result;
}
namespace {
const char* HttpExplanation(std::uint32_t status,std::string_view code) noexcept {
    if(code=="invalid_license")return "The activation key was not accepted.";
    if(code=="key_disabled"||code=="license_disabled")return "The activation key has been revoked.";
    if(code=="customer_disabled")return "The customer account is disabled.";
    if(status==408)return "The license server timed out while handling the request.";
    if(status==429)return "The license server received too many requests; try again later.";
    if(status==404)return "The licensing endpoint was not found; check that the current verification agent is installed.";
    if(status>=500)return "The license server or its gateway is temporarily unavailable.";
    return "The license server refused this verification request.";
}
std::string HttpDiagnostic(std::uint32_t status,std::string_view code,std::string_view suppliedDetail) {
    const auto detail=SanitizeDiagnostic(suppliedDetail,256);
    return std::string(HttpExplanation(status,code))+" (HTTP "+std::to_string(status)+", "+std::string(code)+")"+(detail.empty()?"":": "+detail);
}
}
HttpError::HttpError(std::uint32_t value, std::string code, std::string detail)
    :Error(HttpDiagnostic(value,code,detail)),status(value),responseCode(std::move(code)){}
namespace {
constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::uint32_t U32(const Bytes& b, std::size_t p) {
    return std::uint32_t(b.at(p)) | (std::uint32_t(b.at(p + 1)) << 8) |
        (std::uint32_t(b.at(p + 2)) << 16) | (std::uint32_t(b.at(p + 3)) << 24);
}
std::uint64_t U64(const Bytes& b, std::size_t p) {
    return U32(b, p) | (std::uint64_t(U32(b, p + 4)) << 32);
}
bool EqualAt(const Bytes& b, std::size_t offset, const Bytes& v) {
    return offset <= b.size() && v.size() <= b.size() - offset &&
        std::equal(v.begin(), v.end(), b.begin() + offset);
}
class Parser {
    std::string_view s; std::size_t p = 0;
    [[noreturn]] void Bad() const { throw Error("Invalid or unsupported JSON response"); }
    void Ws() { while(p<s.size()&&(s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r')) ++p; }
    bool Take(char c) { Ws(); if(p<s.size()&&s[p]==c) { ++p; return true; } return false; }
    unsigned Digit(char c) { if(c>='0'&&c<='9')return unsigned(c-'0'); if(c>='a'&&c<='f')return unsigned(c-'a'+10); if(c>='A'&&c<='F')return unsigned(c-'A'+10); Bad(); }
    std::string Text() {
        if(!Take('"')) Bad(); std::string out;
        while(p<s.size()) {
            unsigned char c = static_cast<unsigned char>(s[p++]);
            if(c=='"') return out;
            if(c<32||c>=128) Bad();
            if(c=='\\') {
                if(p==s.size()) Bad(); c=static_cast<unsigned char>(s[p++]);
                switch(c) {
                case '"': case '\\': case '/': out.push_back(char(c)); break;
                case 'b':out.push_back('\b');break; case 'f':out.push_back('\f');break;
                case 'n':out.push_back('\n');break; case 'r':out.push_back('\r');break; case 't':out.push_back('\t');break;
                case 'u': {
                    if(s.size()-p<4)Bad(); unsigned u=0;
                    for(unsigned i=0;i<4;++i)u=(u<<4)|Digit(s[p++]);
                    // Protocol field names and values are ASCII. Reject other encodings.
                    if(u>=128)Bad(); out.push_back(char(u)); break;
                }
                default:Bad();
                }
            } else out.push_back(char(c));
        } Bad();
    }
    Value Val() {
        Ws(); if(p==s.size())Bad();
        if(s[p]=='"')return {Value::Kind::String,Text()};
        if(s[p]>='0'&&s[p]<='9') {
            auto start=p++; if(s[start]=='0'&&p<s.size()&&s[p]>='0'&&s[p]<='9')Bad();
            while(p<s.size()&&s[p]>='0'&&s[p]<='9')++p;
            return {Value::Kind::Number,std::string(s.substr(start,p-start))};
        }
        if(s.substr(p,4)=="true") { p+=4; return {Value::Kind::Boolean,"true"}; }
        if(s.substr(p,5)=="false") { p+=5; return {Value::Kind::Boolean,"false"}; }
        if(s.substr(p,4)=="null") { p+=4; return {}; } Bad();
    }
public:
    explicit Parser(std::string_view input):s(input) { if(s.size()>65536)Bad(); }
    Object Run() {
        Object out; if(!Take('{'))Bad();
        if(!Take('}')) for(;;) {
            auto key=Text(); if(key.empty()||key.size()>80||!Take(':'))Bad();
            if(!out.emplace(key,Val()).second)Bad();
            if(out.size()>32)Bad(); if(Take('}'))break; if(!Take(','))Bad();
        }
        Ws();if(p!=s.size())Bad(); return out;
    }
};
}
std::string Base64(const Bytes& b) {
    std::string s; s.reserve((b.size()+2)/3*4);
    for(std::size_t i=0;i<b.size();i+=3) {
        unsigned v=unsigned(b[i])<<16; if(i+1<b.size())v|=unsigned(b[i+1])<<8; if(i+2<b.size())v|=b[i+2];
        s.push_back(alphabet[(v>>18)&63]);s.push_back(alphabet[(v>>12)&63]);
        s.push_back(i+1<b.size()?alphabet[(v>>6)&63]:'=');s.push_back(i+2<b.size()?alphabet[v&63]:'=');
    } return s;
}
Bytes Decode64(std::string_view s,std::size_t maximum) {
    if(s.size()%4||s.size()>(maximum+2)/3*4)throw Error("Invalid base64 length");
    Bytes b;b.reserve(s.size()/4*3);
    for(std::size_t i=0;i<s.size();i+=4) {
        unsigned v=0; unsigned padding=0;
        for(unsigned j=0;j<4;++j) {
            char c=s[i+j]; if(c=='=') { if(j<2||i+4!=s.size())throw Error("Invalid base64 padding"); ++padding;v<<=6; }
            else { if(padding)throw Error("Invalid base64 padding"); const char* a=std::find(std::begin(alphabet),std::end(alphabet)-1,c);
                if(a==std::end(alphabet)-1)throw Error("Invalid base64 character");v=(v<<6)|unsigned(a-alphabet); }
        }
        if(padding>2||(padding==2&&(v&65535))||(padding==1&&(v&255)))throw Error("Noncanonical base64");
        b.push_back(std::uint8_t(v>>16));if(padding<2)b.push_back(std::uint8_t(v>>8));if(padding<1)b.push_back(std::uint8_t(v));
    }
    if(b.size()>maximum)throw Error("Decoded data exceeds limit");return b;
}
std::string Hex(const Bytes& b) { constexpr char h[]="0123456789abcdef"; std::string s;for(auto c:b) {s.push_back(h[c>>4]);s.push_back(h[c&15]);}return s; }
Bytes DecodeHex(std::string_view s,std::size_t exact) {
    if(s.size()!=exact*2)throw Error("Invalid identifier length");Bytes b;
    auto nibble=[](char c)->unsigned { if(c>='0'&&c<='9')return unsigned(c-'0');if(c>='a'&&c<='f')return unsigned(c-'a'+10);throw Error("Invalid lowercase hex identifier");};
    for(std::size_t i=0;i<s.size();i+=2)b.push_back(std::uint8_t((nibble(s[i])<<4)|nibble(s[i+1])));return b;
}
Bytes LicenseKeyIdentifier(std::string_view text) {
    if(text.size()!=81||text.substr(0,5)!="icv4_"||text[37]!='.')throw Error("Invalid license key format");
    const auto identifier=DecodeHex(text.substr(5,32),16);
    for(char c:text.substr(38)){
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'))
            throw Error("Invalid license key format");
    }
    if(std::string_view("AEIMQUYcgkosw048").find(text.back())==std::string_view::npos)
        throw Error("Invalid license key format");
    return identifier;
}
Object ParseObject(std::string_view json) {return Parser(json).Run();}
std::string String(const Object& v,const char* n,std::size_t maximum) {
    auto p=v.find(n);if(p==v.end()||p->second.kind!=Value::Kind::String||p->second.text.size()>maximum)throw Error(std::string("Invalid field: ")+n);
    return p->second.text;
}
std::uint64_t Number(const Object& v,const char* n) {
    auto p=v.find(n);if(p==v.end()||p->second.kind!=Value::Kind::Number)throw Error(std::string("Invalid number: ")+n);
    std::uint64_t out=0;auto r=std::from_chars(p->second.text.data(),p->second.text.data()+p->second.text.size(),out);
    if(r.ec!=std::errc()||r.ptr!=p->second.text.data()+p->second.text.size())throw Error("Number overflow");return out;
}
HttpError ParseHttpError(std::uint32_t status,std::string_view body) {
    std::string code="request_denied",detail;
    try {
        const auto object=ParseObject(body);const auto supplied=String(object,"error",80);
        if(!supplied.empty()&&supplied.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")==std::string::npos)code=supplied;
        if(object.count("message")) {
            auto text=String(object,"message",256);
            // Diagnostics are a bounded protocol field, never raw HTML,
            // response bodies, terminal escapes or an activation request.
            detail=SanitizeDiagnostic(text,256);
        }
    }catch(const Error&){} // Keep a validated error code even if detail is malformed.
    return HttpError(status,std::move(code),std::move(detail));
}
std::string Quote(std::string_view s) {std::string out="\"";for(unsigned char c:s) {if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c>=32&&c<127)out+=char(c);else {constexpr char h[]="0123456789abcdef";out+="\\u00";out+=h[c>>4];out+=h[c&15];}}return out+'"';}
std::string Json(const std::map<std::string,std::string>& v) {std::string out="{";for(const auto& p:v){if(out.size()>1)out+=',';out+=Quote(p.first)+':'+Quote(p.second);}return out+'}';}
Challenge ParseChallenge(const Bytes& b) {
    if(b.size()!=88||U32(b,0)!=0x35434349||U32(b,4)!=88||U32(b,8)!=5||U32(b,12)!=1||U32(b,16)!=0||U32(b,20)!=1)throw Error("Invalid driver verification challenge; install/restart the current licensed driver");
    for(const std::size_t offset:{24u,56u})if(std::all_of(b.begin()+offset,b.begin()+offset+32,[](std::uint8_t v){return v==0;}))throw Error("Driver verification nonce is zero");
    Challenge c;c.maximumSeconds=U32(b,16);c.flags=U32(b,20);std::copy_n(b.begin()+24,32,c.boot.begin());std::copy_n(b.begin()+56,32,c.request.begin());return c;
}
AuthorizationReply ParseAuthorizationReply(std::string_view s,const Challenge& c,const Bytes& identifier) {
    const auto obj=ParseObject(s);if(obj.size()!=5)throw Error("Unexpected authorization response fields");
    AuthorizationReply r;r.key=String(obj,"key_id",32);r.customer=String(obj,"customer_id",32);
    const auto key=DecodeHex(r.key,16),customer=DecodeHex(r.customer,16);
    r.authorization=Decode64(String(obj,"authorization",512),384);r.generation=Number(obj,"generation");
    if(String(obj,"active_until",32)!="driver_reload"||c.maximumSeconds!=0||c.flags!=1||r.generation==0)throw Error("The authorization duration or generation policy is invalid");
    if(r.authorization.size()!=384)throw Error("The signed authorization must contain exactly 384 bytes");
    if(identifier.size()!=16||key!=identifier)throw Error("The server authorization key identifier does not match the supplied activation key");
    if(std::all_of(customer.begin(),customer.end(),[](std::uint8_t v){return v==0;}))throw Error("The server authorization customer identifier is empty");
    const auto& b=r.authorization;
    if(U32(b,0)!=0x354c4349||b[4]!=5||b[5]!=0||b[6]!=128||b[7]!=0||U32(b,8)!=1)throw Error("The signed authorization header is incompatible with this driver protocol or product");
    if(U32(b,12)!=0||U64(b,120)!=0)throw Error("The signed authorization contains an unsupported duration or reserved field");
    if(U64(b,16)!=r.generation)throw Error("The signed authorization generation does not match the server response");
    if(!EqualAt(b,24,Bytes(c.boot.begin(),c.boot.end())))throw Error("The signed authorization belongs to a different driver load");
    if(!EqualAt(b,56,Bytes(c.request.begin(),c.request.end())))throw Error("The signed authorization belongs to a different or replaced verification request");
    if(!EqualAt(b,88,key))throw Error("The signed authorization key identifier does not match this activation key");
    if(!EqualAt(b,104,customer))throw Error("The signed authorization customer identifier does not match the server response");
    return r;
}
}

// ---- Licensing/HttpTransport.cpp ----
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <exception>
#include <memory>
#include <utility>
#ifdef VDEVICE_HTTP_TESTING
#include "HttpTransportTesting.h"
#endif

#ifndef INTERCEPTOR_LICENSE_SERVER_URL
#define INTERCEPTOR_LICENSE_SERVER_URL L"https://licences.configcsa.com"
#endif

namespace license {
namespace {
#ifdef VDEVICE_HTTP_TESTING
using Api=transport_test::Api;
Api currentApi;
std::atomic<long> liveContexts{0};
std::wstring fixtureOrigin;
#else
struct Api {
    decltype(&WinHttpOpen) open=&WinHttpOpen;
    decltype(&WinHttpConnect) connect=&WinHttpConnect;
    decltype(&WinHttpOpenRequest) openRequest=&WinHttpOpenRequest;
    decltype(&WinHttpSetOption) setOption=&WinHttpSetOption;
    decltype(&WinHttpQueryOption) queryOption=&WinHttpQueryOption;
    decltype(&WinHttpSetTimeouts) setTimeouts=&WinHttpSetTimeouts;
    decltype(&WinHttpSetStatusCallback) setStatusCallback=&WinHttpSetStatusCallback;
    decltype(&WinHttpSendRequest) send=&WinHttpSendRequest;
    decltype(&WinHttpReceiveResponse) receive=&WinHttpReceiveResponse;
    decltype(&WinHttpQueryHeaders) queryHeaders=&WinHttpQueryHeaders;
    decltype(&WinHttpQueryDataAvailable) queryData=&WinHttpQueryDataAvailable;
    decltype(&WinHttpReadData) read=&WinHttpReadData;
    decltype(&WinHttpCloseHandle) close=&WinHttpCloseHandle;
};
#endif
Api SnapshotApi(){
#ifdef VDEVICE_HTTP_TESTING
    return currentApi;
#else
    return {};
#endif
}
enum class Phase {Configuration,Proxy,Dns,Connect,Tls,Send,Receive,Read};
const char* PhaseName(Phase phase) noexcept {
    switch(phase){
    case Phase::Configuration:return "configuration";
    case Phase::Proxy:return "proxy";
    case Phase::Dns:return "dns";
    case Phase::Connect:return "connect";
    case Phase::Tls:return "tls";
    case Phase::Send:return "send";
    case Phase::Receive:return "receive";
    case Phase::Read:return "read";
    }
    return "network";
}
const char* FailureReason(DWORD code,DWORD secure) noexcept {
    if((secure&WINHTTP_CALLBACK_STATUS_FLAG_CERT_REVOKED)||code==ERROR_WINHTTP_SECURE_CERT_REVOKED)return "certificate_revoked";
    if((secure&WINHTTP_CALLBACK_STATUS_FLAG_CERT_CN_INVALID)||code==ERROR_WINHTTP_SECURE_CERT_CN_INVALID)return "certificate_hostname_mismatch";
    if((secure&WINHTTP_CALLBACK_STATUS_FLAG_CERT_DATE_INVALID)||code==ERROR_WINHTTP_SECURE_CERT_DATE_INVALID)return "certificate_date_invalid";
    if((secure&WINHTTP_CALLBACK_STATUS_FLAG_CERT_WRONG_USAGE)||code==ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE)return "certificate_wrong_usage";
    constexpr DWORD untrustedAndRevocation=WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA|WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED;
    if((secure&untrustedAndRevocation)==untrustedAndRevocation)return "certificate_untrusted_revocation_unreachable";
    if((secure&WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA)||code==ERROR_WINHTTP_SECURE_INVALID_CA)return "certificate_untrusted";
    if((secure&WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED)||code==ERROR_WINHTTP_SECURE_CERT_REV_FAILED)return "certificate_revocation_unreachable";
    if(secure&WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CERT)return "certificate_invalid";
    if(code==ERROR_WINHTTP_SECURE_INVALID_CERT)return "certificate_invalid";
    if((secure&WINHTTP_CALLBACK_STATUS_FLAG_SECURITY_CHANNEL_ERROR)||code==ERROR_WINHTTP_SECURE_CHANNEL_ERROR)return "tls_handshake_failed";
    switch(code){
    case ERROR_WINHTTP_SECURE_FAILURE:return "tls_validation_failed";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:return "dns_failure";
    case ERROR_WINHTTP_CANNOT_CONNECT:return "connection_failed";
    case ERROR_WINHTTP_CONNECTION_ERROR:return "connection_interrupted";
    case ERROR_WINHTTP_RESEND_REQUEST:return "request_resend_required";
    case ERROR_WINHTTP_TIMEOUT:case ERROR_TIMEOUT:case WAIT_TIMEOUT:return "operation_timeout";
    case ERROR_WINHTTP_AUTODETECTION_FAILED:case ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT:case ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT:return "proxy_configuration_failed";
    case ERROR_WINHTTP_LOGIN_FAILURE:return "proxy_authentication_required";
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:return "client_certificate_required";
    case ERROR_OPERATION_ABORTED:case ERROR_WINHTTP_OPERATION_CANCELLED:return "network_cancelled";
    default:return "network_error";
    }
}
bool Retryable(DWORD code,DWORD secure) noexcept {
    // A retry must get a fresh challenge. No TLS validation, proxy policy or
    // server refusal is a transient transport error.
    return secure==0&&(code==ERROR_WINHTTP_NAME_NOT_RESOLVED||code==ERROR_WINHTTP_CONNECTION_ERROR||code==ERROR_WINHTTP_CANNOT_CONNECT||
        code==ERROR_WINHTTP_RESEND_REQUEST||code==ERROR_WINHTTP_TIMEOUT);
}
bool TlsFailure(DWORD code,DWORD secure) noexcept {
    if(secure)return true;
    switch(code){
    case ERROR_WINHTTP_SECURE_FAILURE:case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:return true;
    default:return false;
    }
}
struct Http {
    HINTERNET h=nullptr;
    const Api* api;
    Http(HINTERNET value,const Api& functions):h(value),api(&functions){}
    ~Http(){if(h)api->close(h);}
    Http(const Http&)=delete;
    Http& operator=(const Http&)=delete;
};
// Every buffer passed to an asynchronous API belongs to this context. The
// caller owns one reference and WinHTTP owns one until HANDLE_CLOSING, its last
// callback. Cancelling a deadline never frees an in-flight payload/read buffer.
struct Context {
    std::atomic<unsigned> references{1};
    Handle event;
    std::atomic<DWORD> completion{0},error{0},secure{0},bytes{0};
    std::atomic<Phase> phase{Phase::Configuration};
    std::string payload;
    std::array<char,8192> buffer{};
    explicit Context(std::string_view body):event(CreateEventW(nullptr,TRUE,FALSE,nullptr)),payload(body){
        if(!event.get())WinError("Create HTTPS completion event");
#ifdef VDEVICE_HTTP_TESTING
        ++liveContexts;
#endif
    }
    ~Context(){
#ifdef VDEVICE_HTTP_TESTING
        --liveContexts;
#endif
        if(!payload.empty())SecureZeroMemory(payload.data(),payload.size());
    }
    void AddRef() noexcept {references.fetch_add(1,std::memory_order_relaxed);}
    void Release() noexcept {if(references.fetch_sub(1,std::memory_order_acq_rel)==1)delete this;}
};
void CALLBACK Status(HINTERNET,DWORD_PTR value,DWORD status,void* information,DWORD size) noexcept {
    auto* context=reinterpret_cast<Context*>(value);if(!context)return;
    if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING){context->Release();return;}
    switch(status){
    case WINHTTP_CALLBACK_STATUS_DETECTING_PROXY:context->phase=Phase::Proxy;return;
    case WINHTTP_CALLBACK_STATUS_RESOLVING_NAME:context->phase=Phase::Dns;return;
    case WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER:context->phase=Phase::Connect;return;
    case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER:context->phase=Phase::Tls;return;
    case WINHTTP_CALLBACK_STATUS_SENDING_REQUEST:context->phase=Phase::Send;return;
    case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
        if(information&&size==sizeof(DWORD))context->secure.fetch_or(*static_cast<DWORD*>(information));
        context->phase=Phase::Tls;return;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        if(information&&size==sizeof(WINHTTP_ASYNC_RESULT))context->error=static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
        else context->error=ERROR_INVALID_DATA;
        break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        if(information&&size==sizeof(DWORD))context->bytes=*static_cast<DWORD*>(information);
        else context->error=ERROR_INVALID_DATA;
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:context->bytes=size;break;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:break;
    default:return;
    }
    context->completion.store(status,std::memory_order_release);
    SetEvent(context->event.get());
}
struct ReleaseContext {void operator()(Context* context) const noexcept {if(context)context->Release();}};
class Request {
    Api api;
    Http session,connection;
    HINTERNET request=nullptr;
    std::unique_ptr<Context,ReleaseContext> context;
    ULONGLONG deadline,overallDeadline;
    unsigned route;
    [[noreturn]]void Fail(const char* action,DWORD code,bool expired=false){
        DWORD extended=0,size=sizeof(extended);
        // Preserve the failing API result before a diagnostic query can replace
        // GetLastError. Schannel details are an extra, never the native error.
        if(request&&!api.queryOption(request,WINHTTP_OPTION_EXTENDED_ERROR,&extended,&size))extended=0;
        const DWORD secure=context?context->secure.load():0;
        const auto phase=context?context->phase.load():Phase::Configuration;
        // Kernel wait expiration and GetTickCount64 can straddle a clock tick.
        // An expired wait for the overall budget is terminal even if the next
        // sampled tick has not advanced yet.
        const bool exhausted=GetTickCount64()>=overallDeadline||(expired&&deadline==overallDeadline);
        const char* reason=expired?(exhausted?"network_deadline":"operation_timeout"):FailureReason(code,secure);
        SetLastError(code);
        WindowsError failure(action,code,PhaseName(phase),reason,secure,extended,!exhausted&&Retryable(code,secure));
        // A certificate failure remains permanent for this origin. A second,
        // independently authenticated, fixed owner-controlled origin may still
        // work when DNS returned an interception/wrong-server certificate.
        failure.failoverEligible=route==0&&!exhausted&&(Retryable(code,secure)||TlsFailure(code,secure));
        throw failure;
    }
    void Deadline(){if(GetTickCount64()>=deadline)Fail("Complete license HTTPS request within its total deadline",ERROR_WINHTTP_TIMEOUT,true);}
    void Option(HINTERNET handle,DWORD option,DWORD value,const char* action){
        if(!api.setOption(handle,option,&value,sizeof(value))){const DWORD code=GetLastError();Fail(action,code);}
        Deadline();
    }
    void Begin(Phase phase){
        Deadline();context->phase=phase;context->completion=0;context->error=0;context->bytes=0;
        if(!ResetEvent(context->event.get())){const DWORD code=GetLastError();Fail("Reset HTTPS completion event",code);}
    }
    void Complete(BOOL started,DWORD wanted,const char* action){
        if(!started){const DWORD code=GetLastError();if(code!=ERROR_IO_PENDING)Fail(action,code);}
        const ULONGLONG now=GetTickCount64();if(now>=deadline)Fail(action,ERROR_WINHTTP_TIMEOUT,true);
        const DWORD remaining=static_cast<DWORD>(std::min<ULONGLONG>(deadline-now,MAXDWORD-1));
        const DWORD wait=WaitForSingleObject(context->event.get(),remaining);
        if(wait==WAIT_TIMEOUT)Fail(action,ERROR_WINHTTP_TIMEOUT,true);
        if(wait!=WAIT_OBJECT_0){const DWORD code=GetLastError();Fail("Wait for HTTPS completion",code);}
        const DWORD error=context->error.load();if(error)Fail(action,error);
        if(context->completion.load(std::memory_order_acquire)!=wanted)Fail("Validate HTTPS completion",ERROR_INVALID_DATA);
        Deadline();
    }
public:
    Request(const std::wstring& host,INTERNET_PORT port,const wchar_t* verb,const wchar_t* endpoint,std::string_view body,ULONGLONG until,ULONGLONG overallUntil,unsigned selectedRoute)
        :api(SnapshotApi()),session(nullptr,api),connection(nullptr,api),context(new Context(body)),deadline(until),overallDeadline(overallUntil),route(selectedRoute){
        try {
            Deadline();
            session.h=api.open(L"InterceptorReleaseLicense/5.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC);
            if(!session.h){const DWORD code=GetLastError();Fail("Open caller HTTPS session",code);}
            Deadline();
            if(!api.setTimeouts(session.h,5000,5000,5000,10000)){const DWORD code=GetLastError();Fail("Set HTTPS phase timeouts",code);}
            // Windows versions that do not know TLS 1.3 reject the option. Only
            // that capability error permits retrying with TLS 1.2 alone.
            // The backup uses TLS 1.2 for proxies that fail a TLS 1.3 ClientHello.
            DWORD protocols=WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2|(route==0?0x00002000:0); // TLS 1.3
            if(!api.setOption(session.h,WINHTTP_OPTION_SECURE_PROTOCOLS,&protocols,sizeof(protocols))){
                const DWORD code=GetLastError();
                if(code!=ERROR_INVALID_PARAMETER&&code!=ERROR_NOT_SUPPORTED&&code!=ERROR_WINHTTP_INVALID_OPTION)Fail("Require TLS 1.2 or TLS 1.3",code);
                Option(session.h,WINHTTP_OPTION_SECURE_PROTOCOLS,WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2,"Require TLS 1.2");
            }
            // Keep WinHTTP's default alternate-address attempts within our deadline.
            BOOL fastFallback=TRUE;
            if(!api.setOption(session.h,WINHTTP_OPTION_IPV6_FAST_FALLBACK,&fastFallback,sizeof(fastFallback))){
                const DWORD code=GetLastError();
                if(code!=ERROR_INVALID_PARAMETER&&code!=ERROR_NOT_SUPPORTED&&code!=ERROR_WINHTTP_INVALID_OPTION)Fail("Enable IPv6 to IPv4 fast fallback",code);
            }
            connection.h=api.connect(session.h,host.c_str(),port,0);
            if(!connection.h){const DWORD code=GetLastError();Fail("Connect license HTTPS origin",code);}
            Deadline();
            request=api.openRequest(connection.h,verb,endpoint,nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
            if(!request){const DWORD code=GetLastError();Fail("Open license HTTPS request",code);}
            Option(request,WINHTTP_OPTION_REDIRECT_POLICY,WINHTTP_OPTION_REDIRECT_POLICY_NEVER,"Disable HTTPS redirects");
            Option(request,WINHTTP_OPTION_AUTOLOGON_POLICY,WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH,"Disable automatic Windows credential use");
            Option(request,WINHTTP_OPTION_ENABLE_FEATURE,WINHTTP_ENABLE_SSL_REVOCATION,"Require certificate revocation checking");
            // No SECURITY_FLAG_IGNORE_* and no SSL_REVERT_IMPERSONATION flags:
            // hostname, trust, validity and revocation use the caller context.
            const DWORD notifications=WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES|
                WINHTTP_CALLBACK_FLAG_SECURE_FAILURE|WINHTTP_CALLBACK_FLAG_DETECTING_PROXY|
                WINHTTP_CALLBACK_FLAG_RESOLVE_NAME|WINHTTP_CALLBACK_FLAG_CONNECT_TO_SERVER|WINHTTP_CALLBACK_FLAG_SEND_REQUEST;
            if(api.setStatusCallback(request,Status,notifications,0)==WINHTTP_INVALID_STATUS_CALLBACK){const DWORD code=GetLastError();Fail("Observe HTTPS completion and TLS validation",code);}
            DWORD_PTR value=reinterpret_cast<DWORD_PTR>(context.get());
            context->AddRef();
            if(!api.setOption(request,WINHTTP_OPTION_CONTEXT_VALUE,&value,sizeof(value))){
                const DWORD code=GetLastError();context->Release();Fail("Attach HTTPS completion context",code);
            }
        }catch(...){if(request){api.close(request);request=nullptr;}throw;}
    }
    ~Request(){if(request)api.close(request);}
    Request(const Request&)=delete;
    std::string Run(bool post){
        Begin(Phase::Send);
        const wchar_t* headers=post?L"Content-Type: application/json\r\nAccept: application/json\r\n":L"Accept: application/json\r\n";
        Complete(api.send(request,headers,static_cast<DWORD>(-1),context->payload.empty()?WINHTTP_NO_REQUEST_DATA:context->payload.data(),
            static_cast<DWORD>(context->payload.size()),static_cast<DWORD>(context->payload.size()),reinterpret_cast<DWORD_PTR>(context.get())),
            WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,"Send license HTTPS request");
        Begin(Phase::Receive);
        Complete(api.receive(request,nullptr),WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,"Receive license HTTPS response");
        DWORD status=0,size=sizeof(status);
        if(!api.queryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)){
            const DWORD code=GetLastError();Fail("Read HTTPS response status",code);
        }
        Deadline();
        if(status==407)Fail("Authenticate caller proxy access",ERROR_WINHTTP_LOGIN_FAILURE);
        std::string body;
        for(;;){
            Begin(Phase::Read);
            Complete(api.queryData(request,nullptr),WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE,"Read HTTPS response availability");
            const DWORD available=context->bytes.load();if(!available)break;
            if(available>65536-body.size())throw Error("HTTPS response exceeds limit");
            const DWORD amount=std::min<DWORD>(available,static_cast<DWORD>(context->buffer.size()));
            Begin(Phase::Read);
            Complete(api.read(request,context->buffer.data(),amount,nullptr),WINHTTP_CALLBACK_STATUS_READ_COMPLETE,"Read HTTPS response body");
            const DWORD read=context->bytes.load();if(read>amount)Fail("Validate HTTPS response length",ERROR_INVALID_DATA);
            if(!read)break;body.append(context->buffer.data(),read);
        }
        if(status!=200)throw ParseHttpError(status,body);
        return body;
    }
};
class NetworkIdentity {
    std::unique_ptr<Handle> previous;
    bool active=false;
public:
    explicit NetworkIdentity(HANDLE caller){
        if(!caller)return;
        TOKEN_TYPE type{};SECURITY_IMPERSONATION_LEVEL level{};DWORD size=0;
        if(!GetTokenInformation(caller,TokenType,&type,sizeof(type),&size)||
            (type==TokenImpersonation&&!GetTokenInformation(caller,TokenImpersonationLevel,&level,sizeof(level),&size))){
            const DWORD code=GetLastError();throw WindowsError("Read authenticated caller network token",code,"identity","caller_identity_invalid");
        }
        if(type!=TokenImpersonation||level<SecurityImpersonation)throw WindowsError("Require authenticated caller impersonation",ERROR_BAD_IMPERSONATION_LEVEL,"identity","caller_identity_invalid");
        HANDLE raw=nullptr;
        if(!OpenThreadToken(GetCurrentThread(),TOKEN_QUERY|TOKEN_IMPERSONATE,TRUE,&raw)){
            const DWORD code=GetLastError();if(code!=ERROR_NO_TOKEN)throw WindowsError("Preserve service network identity",code,"identity","caller_identity_invalid");
        }
        if(raw)previous=std::make_unique<Handle>(raw);
        if(!ImpersonateLoggedOnUser(caller)){const DWORD code=GetLastError();throw WindowsError("Use authenticated caller network identity",code,"identity","caller_identity_invalid");}
        active=true;
    }
    void Restore(){
        if(!active)return;
        const BOOL restored=previous?SetThreadToken(nullptr,previous->get()):RevertToSelf();
        if(!restored){
            // Microsoft requires termination after failed RevertToSelf: no
            // catch handler or later driver operation may run as the caller.
            TerminateProcess(GetCurrentProcess(),ERROR_CANNOT_IMPERSONATE);
            std::terminate();
        }
        active=false;
    }
    ~NetworkIdentity(){if(active)Restore();}
};
std::string Exchange(const wchar_t* endpoint,std::string_view body,bool post,ULONGLONG absoluteDeadline,HANDLE caller,unsigned route){
    const ULONGLONG now=GetTickCount64();
    const ULONGLONG overallDeadline=absoluteDeadline?std::min(absoluteDeadline,now+25000):now+25000;
    const ULONGLONG deadline=route==0?std::min(overallDeadline,now+12000):overallDeadline;
    if(body.size()>65536||!endpoint||endpoint[0]!=L'/'||wcslen(endpoint)>256||wcsstr(endpoint,L"..")||wcspbrk(endpoint,L"?#\\\r\n"))throw Error("Invalid HTTPS request");
    if(route>1||(route==1&&wcscmp(endpoint,L"/v1/verify")&&wcscmp(endpoint,L"/health")))throw Error("Invalid fixed HTTPS licensing route");
    std::wstring base=route==0?INTERCEPTOR_LICENSE_SERVER_URL:L"https://configcsa.com";
#ifdef VDEVICE_HTTP_TESTING
    if(!fixtureOrigin.empty())base=fixtureOrigin;
#endif
    URL_COMPONENTS url{};url.dwStructSize=sizeof(url);
    url.dwHostNameLength=url.dwUrlPathLength=url.dwUserNameLength=url.dwPasswordLength=url.dwExtraInfoLength=static_cast<DWORD>(-1);
    if(!WinHttpCrackUrl(base.c_str(),0,0,&url)||url.nScheme!=INTERNET_SCHEME_HTTPS||url.dwUserNameLength||url.dwPasswordLength||url.dwExtraInfoLength||!url.dwHostNameLength||
        (url.dwUrlPathLength&&std::wstring(url.lpszUrlPath,url.dwUrlPathLength)!=L"/"))throw Error("A fixed production HTTPS licensing origin must be configured at build time");
    const std::wstring host(url.lpszHostName,url.dwHostNameLength);if(host==L"licenses.invalid")throw Error("Production licensing endpoint is not configured");
    const std::wstring path=(route==1?std::wstring(L"/driver-activation"):std::wstring())+endpoint;
    NetworkIdentity identity(caller);
    try {
        std::string result;
        {Request request(host,url.nPort,post?L"POST":L"GET",path.c_str(),body,deadline,overallDeadline,route);result=request.Run(post);}
        identity.Restore();return result;
    }catch(...){
        const auto failure=std::current_exception();
        identity.Restore();
        try{std::rethrow_exception(failure);}catch(const WindowsError& error){SetLastError(error.code);throw;}
    }
}
// The server's health object deliberately contains a product array. The strict
// authorization parser accepts flat objects only, so health has its own bounded
// JSON reader: validate the complete document, skip unknown nested fields, and
// check the two health protocol fields without loosening authorization parsing.
class HealthJson {
    std::string_view text;
    std::size_t position=0;
    unsigned values=0;
    [[noreturn]]void Invalid(){throw Error("License HTTPS health response is invalid or incompatible");}
    void Space(){while(position<text.size()&&(text[position]==' '||text[position]=='\r'||text[position]=='\n'||text[position]=='\t'))++position;}
    bool Take(char value){Space();if(position<text.size()&&text[position]==value){++position;return true;}return false;}
    void Need(char value){if(!Take(value))Invalid();}
    unsigned Hex4(){
        unsigned value=0;for(unsigned i=0;i<4;++i){if(position==text.size())Invalid();const char digit=text[position++];value<<=4;
            if(digit>='0'&&digit<='9')value+=digit-'0';else if(digit>='a'&&digit<='f')value+=digit-'a'+10;else if(digit>='A'&&digit<='F')value+=digit-'A'+10;else Invalid();}
        return value;
    }
    std::string Text(){
        Need('"');std::string result;
        while(position<text.size()){
            const unsigned char value=static_cast<unsigned char>(text[position++]);if(value=='"')return result;if(value<32)Invalid();
            if(value!='\\'){result.push_back(static_cast<char>(value));continue;}
            if(position==text.size())Invalid();const char escape=text[position++];
            switch(escape){
            case '"':case '\\':case '/':result.push_back(escape);break;
            case 'b':result.push_back('\b');break;case 'f':result.push_back('\f');break;
            case 'n':result.push_back('\n');break;case 'r':result.push_back('\r');break;case 't':result.push_back('\t');break;
            case 'u':{
                unsigned code=Hex4();
                if(code>=0xd800&&code<=0xdbff){if(position+2>text.size()||text[position++]!='\\'||text[position++]!='u')Invalid();const unsigned low=Hex4();if(low<0xdc00||low>0xdfff)Invalid();code=0x10000;}
                else if(code>=0xdc00&&code<=0xdfff)Invalid();
                result.push_back(code<=127?static_cast<char>(code):'?');break;
            }
            default:Invalid();
            }
        }
        Invalid();
    }
    std::string_view Number(){
        Space();const auto start=position;if(position<text.size()&&text[position]=='-')++position;
        if(position==text.size())Invalid();
        if(text[position]=='0')++position;
        else {if(text[position]<'1'||text[position]>'9')Invalid();while(position<text.size()&&text[position]>='0'&&text[position]<='9')++position;}
        if(position<text.size()&&text[position]=='.'){++position;const auto begin=position;while(position<text.size()&&text[position]>='0'&&text[position]<='9')++position;if(begin==position)Invalid();}
        if(position<text.size()&&(text[position]=='e'||text[position]=='E')){++position;if(position<text.size()&&(text[position]=='+'||text[position]=='-'))++position;const auto begin=position;while(position<text.size()&&text[position]>='0'&&text[position]<='9')++position;if(begin==position)Invalid();}
        return text.substr(start,position-start);
    }
    void Skip(unsigned depth){
        if(depth>16||++values>10000)Invalid();Space();if(position==text.size())Invalid();
        if(text[position]=='"'){Text();return;}
        if(Take('{')){if(Take('}'))return;do{Text();Need(':');Skip(depth+1);}while(Take(','));Need('}');return;}
        if(Take('[')){if(Take(']'))return;do{Skip(depth+1);}while(Take(','));Need(']');return;}
        for(const auto literal:{std::string_view("true"),std::string_view("false"),std::string_view("null")})if(text.substr(position,literal.size())==literal){position+=literal.size();return;}
        Number();
    }
public:
    explicit HealthJson(std::string_view body):text(body){}
    void Validate(){
        if(text.size()>65536)Invalid();Widen(text); // also reject malformed UTF-8
        bool status=false,protocol=false;Need('{');if(Take('}'))Invalid();
        do{
            const auto name=Text();Need(':');
            if(name=="status"){if(status||Text()!="ok")Invalid();status=true;}
            else if(name=="protocol"){if(protocol||Number()!="5")Invalid();protocol=true;}
            else Skip(1);
        }while(Take(','));
        Need('}');Space();if(position!=text.size()||!status||!protocol)Invalid();
    }
};
void ValidateHealth(std::string_view body){HealthJson(body).Validate();}
}
std::string Post(const wchar_t* endpoint,std::string_view body,ULONGLONG deadline,HANDLE caller,unsigned route){return Exchange(endpoint,body,true,deadline,caller,route);}
std::string DiagnoseNetwork(ULONGLONG deadline,HANDLE caller){
    if(!deadline)deadline=GetTickCount64()+25000;
    std::string body,primaryIssue;unsigned route=0;
    try{body=Exchange(L"/health",{},false,deadline,caller,0);}catch(const WindowsError& error){
        if(!error.failoverEligible)throw;
        primaryIssue=error.reason+" (Windows error "+std::to_string(error.code)+", secure flags "+std::to_string(error.secureFailureFlags)+")";route=1;
        try{body=Exchange(L"/health",{},false,deadline,caller,1);}catch(const WindowsError& backup){
            WindowsError combined("Primary endpoint: "+primaryIssue+"; fixed compatibility health check",backup.code,backup.phase,backup.reason,backup.secureFailureFlags,backup.extendedError,backup.retryable);
            SetLastError(backup.code);throw combined;
        }
    }
    ValidateHealth(body);
    return SanitizeDiagnostic(std::string("License HTTPS health check succeeded (HTTP 200) on ")+(route?"the fixed compatibility endpoint":"the primary endpoint")+
        "; Windows verified the server certificate, hostname and revocation policy using the caller network context."+(route?" Primary endpoint reason: "+primaryIssue+".":""));
}
#ifdef VDEVICE_HTTP_TESTING
namespace transport_test {
void SetApi(const Api& api){currentApi=api;}
void ResetApi(){currentApi={};}
long LiveContexts(){return liveContexts.load();}
std::string ProbeHealthRoute(unsigned route){const auto body=Exchange(L"/health",{},false,0,nullptr,route);ValidateHealth(body);return body;}
void SetFixtureOrigin(std::wstring origin){fixtureOrigin=std::move(origin);}
std::string ProbeFixture(ULONGLONG deadline){return Exchange(L"/health",{},false,deadline,nullptr,0);}
}
#endif
}

// ---- Licensing/WindowsErrors.inc ----
#include <sstream>
#include <locale>

namespace license {
std::string WindowsDiagnostic(const char* action,unsigned long code) {
    const char* detail="Windows rejected this operation; use the native error code for further diagnosis.";
    switch(code){
    case ERROR_FILE_NOT_FOUND:detail="The required file, installed driver or service endpoint was not found.";break;
    case ERROR_PATH_NOT_FOUND:detail="The required installation path was not found.";break;
    case ERROR_ACCESS_DENIED:detail="Access was denied; run the application as administrator.";break;
    case ERROR_INVALID_HANDLE:detail="The driver or communication handle is no longer valid.";break;
    case ERROR_NOT_ENOUGH_MEMORY:case ERROR_OUTOFMEMORY:detail="Windows does not have enough memory to complete this operation.";break;
    case ERROR_INVALID_DATA:detail="The received data is malformed, incompatible or was rejected during validation.";break;
    case ERROR_NOT_READY:case ERROR_DEVICE_NOT_CONNECTED:detail="The required driver or device is not ready or connected.";break;
    case ERROR_SHARING_VIOLATION:detail="Another process is using the required resource; wait for its operation to finish.";break;
    case ERROR_NOT_SUPPORTED:detail="This operation is not supported by the installed Windows component or driver.";break;
    case ERROR_INVALID_PARAMETER:detail="An operation argument or the activation key format is invalid.";break;
    case ERROR_BROKEN_PIPE:case ERROR_NO_DATA:case ERROR_PIPE_NOT_CONNECTED:detail="The verification process or licensing service closed its communication channel.";break;
    case ERROR_PIPE_BUSY:detail="Another verification is using the licensing service; try again after it finishes.";break;
    case ERROR_MOD_NOT_FOUND:detail="A required Windows module or installed component could not be loaded.";break;
    case WAIT_TIMEOUT:case ERROR_TIMEOUT:detail="The operation timed out before it completed.";break;
    case ERROR_OPERATION_ABORTED:case ERROR_CANCELLED:detail="The operation was cancelled before it completed.";break;
    case ERROR_SERVICE_REQUEST_TIMEOUT:detail="The licensing service did not start or respond within the allowed time.";break;
    case ERROR_SERVICE_DISABLED:detail="The licensing service is disabled; repair the installation or enable its manual startup.";break;
    case ERROR_SERVICE_DOES_NOT_EXIST:detail="The licensing service is not installed; run the current Interceptor Keyboard & Mouse Release installer.";break;
    case ERROR_SERVICE_CANNOT_ACCEPT_CTRL:case ERROR_SERVICE_NOT_ACTIVE:detail="The licensing service is stopping or is not ready; retry after the current operation finishes.";break;
    case ERROR_SERVICE_MARKED_FOR_DELETE:detail="The old licensing service is pending removal; restart Windows before installing it again.";break;
    case ERROR_RETRY:detail="The operation could not finish now; try again later.";break;
    case IC_ERROR_LICENSE_REQUIRED:detail="The loaded driver has no valid authorization; complete Verification() before opening a capture.";break;
    case ERROR_WINHTTP_TIMEOUT:detail="The connection to the license server timed out; check Internet access and proxy settings.";break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:detail="Windows could not resolve the license server name; check DNS, Internet access and proxy settings.";break;
    case ERROR_WINHTTP_CANNOT_CONNECT:detail="Windows could not connect to the license server; check the network, firewall and proxy.";break;
    case ERROR_WINHTTP_CONNECTION_ERROR:detail="The connection to the license server was interrupted before the response completed.";break;
    case ERROR_WINHTTP_SECURE_FAILURE:detail="Windows rejected the HTTPS security check; check the clock, trusted certificates and certificate-revocation connectivity.";break;
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:detail="The HTTPS certificate is expired or not yet valid; check the Windows date and time.";break;
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:detail="The HTTPS certificate does not match the license server name; check DNS and HTTPS interception software.";break;
    case ERROR_WINHTTP_SECURE_INVALID_CA:detail="The HTTPS certificate chain is not trusted by Windows; update trusted root certificates and check HTTPS interception software.";break;
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:detail="Windows could not verify whether the HTTPS certificate was revoked; check certificate-revocation network access.";break;
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:detail="The HTTPS certificate has been revoked; Windows refused the connection.";break;
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:detail="Windows could not establish the required TLS connection with the license server.";break;
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:detail="The HTTPS certificate is not valid for authenticating a web server.";break;
    case ERROR_WINHTTP_AUTODETECTION_FAILED:case ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT:detail="Windows could not discover or download the proxy configuration; check network and proxy settings.";break;
    case ERROR_WINHTTP_LOGIN_FAILURE:detail="The proxy or network authentication failed; check the proxy access policy for the caller.";break;
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:detail="The network or proxy unexpectedly requested a client certificate; check its access policy.";break;
    }
    std::ostringstream text;text.imbue(std::locale::classic());text<<action<<" failed: "<<detail<<" (Windows error "<<code<<", 0x"<<std::hex<<code<<").";return text.str();
}
[[noreturn]]void WinError(const char* action,unsigned long code) {throw WindowsError(action,code);}
WindowsError::WindowsError(std::string action,unsigned long value,std::string phaseValue,std::string reasonValue,
    unsigned long secureFlags,unsigned long extended,bool canRetry)
    :Error(SanitizeDiagnostic(WindowsDiagnostic(action.c_str(),value)+" [phase="+phaseValue+", reason="+reasonValue+
        ", secure_failure_flags="+std::to_string(secureFlags)+", extended_error="+std::to_string(extended)+"]")),
     code(value),phase(std::move(phaseValue)),reason(std::move(reasonValue)),secureFailureFlags(secureFlags),extendedError(extended),retryable(canRetry){}
std::wstring Widen(std::string_view s) {if(s.empty())return {};if(s.size()>65536)throw Error("Text exceeds limit");int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);if(!n)WinError("Decode UTF-8");std::wstring v(static_cast<std::size_t>(n),L'\0');if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),v.data(),n))WinError("Decode UTF-8");return v;}
}

// ---- Licensing/SdkVerification.inc ----

#pragma comment(lib,"winhttp.lib")

namespace Interceptor {
namespace {
// Keep this Win32 name in sync with the activation link in Shared/Product.h.
constexpr auto ActivationDevice=L"\\\\.\\Global\\InterceptorActivation";
constexpr ULONGLONG VerificationBudgetMs=25000;

class ActivationDriver {
    Handle handle;
public:
    ActivationDriver() {
        handle.value=CreateFileW(ActivationDevice,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle.value==INVALID_HANDLE_VALUE)license::WinError("Open the Interceptor activation interface");
    }
    license::Bytes Challenge() const {
        license::Bytes bytes(sizeof(IC_LICENSE_CHALLENGE));
        if(Io(handle.value,IOCTL_IC_LICENSE_CHALLENGE,nullptr,0,bytes.data(),sizeof(IC_LICENSE_CHALLENGE))!=bytes.size())
            license::WinError("Read a complete driver challenge",ERROR_INVALID_DATA);
        return bytes;
    }
    IC_LICENSE_STATUS Apply(const license::Bytes& authorization) const {
        IC_LICENSE_STATUS status{};
        if(Io(handle.value,IOCTL_IC_LICENSE_APPLY,authorization.data(),static_cast<DWORD>(authorization.size()),
            &status,sizeof(status))!=sizeof(status))
            license::WinError("Read the driver's authorization confirmation",ERROR_INVALID_DATA);
        return status;
    }
    IC_LICENSE_STATUS Status() const {
        IC_LICENSE_STATUS status{};
        if(Io(handle.value,IOCTL_IC_LICENSE_STATUS,nullptr,0,&status,sizeof(status))!=sizeof(status)||
            status.size!=sizeof(status)||status.version!=IC_LICENSE_VERSION||status.state>IC_LICENSE_ACTIVE||
            (status.state==IC_LICENSE_ACTIVE&&(!status.generation||status.error)))
            license::WinError("Read the driver activation status",ERROR_INVALID_DATA);
        return status;
    }
};

struct VerificationBody {
    std::string value;
    VerificationBody(std::string_view key,const license::Bytes& challenge) {
        const auto encoded=license::Base64(challenge);
        value.reserve(256);
        value.append("{\"license_key\":\"").append(key);
        value.append("\",\"challenge\":\"").append(encoded).append("\"}");
    }
    ~VerificationBody(){if(!value.empty())SecureZeroMemory(value.data(),value.size());}
};

void SetWindowsFailure(Verification_Result& result,DWORD code) {
    result.error=code?code:ERROR_GEN_FAILURE;
    result.reason="windows_error";
    result.message=license::WindowsDiagnostic("Interceptor verification",result.error);
    if(result.stage=="driver_open"&&(code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND)){
        result.reason="activation_interface_unavailable";
        result.message+=" Install the current Interceptor Release driver and restart if requested.";
    } else if(code==ERROR_ACCESS_DENIED)result.reason="access_denied";
    else if(code==ERROR_RETRY)result.reason="concurrent_verification";
    else if(code==ERROR_TIMEOUT)result.reason="operation_timeout";
}

std::string VerificationFailureDetail(std::string_view detail,std::string_view key) {
    auto safe=license::SanitizeDiagnostic(detail);
    if(key.size()==81){
        const auto secret=key.substr(38);
        std::size_t pos=0;
        while((pos=safe.find(secret,pos))!=std::string::npos){
            safe.replace(pos,secret.size(),"[redacted]");
            pos+=10;
        }
    }
    return safe.empty()?"Verification failed; see the stage and Windows error.":safe;
}

template<class Driver,class Post>
Verification_Result VerifyDirect(const std::string& key,Post&& post) {
    Verification_Result result;result.stage="key_format";
    std::string primaryFailure;
    try {
        const auto identifier=license::LicenseKeyIdentifier(key);
        result.stage="driver_open";
        Driver driver;
        const auto deadline=GetTickCount64()+VerificationBudgetMs;
        unsigned route=0;
        bool retriedConcurrentApply=false;
        for(;;){
            result.stage="driver_challenge";
            if(GetTickCount64()>=deadline)license::WinError("Complete verification within its deadline",ERROR_TIMEOUT);
            const auto raw=driver.Challenge();
            const auto challenge=license::ParseChallenge(raw);
            const VerificationBody body(key,raw);

            result.stage="server_verification";
            std::string reply;
            try {reply=post(body.value,deadline,route);}
            catch(const license::WindowsError& failure){
                if(route||!failure.failoverEligible||GetTickCount64()>=deadline)throw;
                primaryFailure="Primary HTTPS endpoint: "+failure.reason+" (Windows "+std::to_string(failure.code)+"). ";
                route=1;continue;
            }catch(const license::HttpError& failure){
                if(route||(failure.status!=502&&failure.status!=503&&failure.status!=504)||GetTickCount64()>=deadline)throw;
                primaryFailure="Primary HTTPS endpoint returned HTTP "+std::to_string(failure.status)+". ";
                route=1;continue;
            }

            result.stage="authorization_response";
            const auto authorization=license::ParseAuthorizationReply(reply,challenge,identifier);
            result.stage="driver_apply";
            IC_LICENSE_STATUS status{};
            try{status=driver.Apply(authorization.authorization);}
            catch(const std::system_error& failure){
                if(failure.code().value()!=ERROR_RETRY||retriedConcurrentApply||GetTickCount64()>=deadline)throw;
                retriedConcurrentApply=true;continue;
            }

            result.stage="driver_confirmation";
            if(!VerificationStatusValid(status)||status.state!=IC_LICENSE_ACTIVE||
                status.generation!=authorization.generation||
                license::Hex(license::Bytes(std::begin(status.key_id),std::end(status.key_id)))!=authorization.key||
                license::Hex(license::Bytes(std::begin(status.customer_id),std::end(status.customer_id)))!=authorization.customer)
                license::WinError("Validate the driver's authorization confirmation",ERROR_INVALID_DATA);
            result.stage="complete";result.reason="ok";result.activated=true;
            result.message=primaryFailure+"Activated until driver reload or Windows restart.";
            return result;
        }
    }catch(const license::WindowsError& failure){
        SetWindowsFailure(result,failure.code);
        if(!failure.reason.empty())result.reason=failure.reason;
        if(result.reason!="activation_interface_unavailable")result.message=VerificationFailureDetail(failure.what(),key);
    }catch(const license::HttpError& failure){
        result.error=failure.status==401||failure.status==403?ERROR_ACCESS_DENIED:
            failure.status==408||failure.status==429||failure.status>=500?ERROR_RETRY:ERROR_INVALID_DATA;
        result.reason=failure.responseCode;result.message=VerificationFailureDetail(failure.what(),key);
    }catch(const std::system_error& failure){
        SetWindowsFailure(result,static_cast<DWORD>(failure.code().value()));
    }catch(const license::Error& failure){
        result.error=ERROR_INVALID_DATA;
        result.reason=result.stage=="authorization_response"?"invalid_authorization":"invalid_verification_data";
        result.message=VerificationFailureDetail(failure.what(),key);
        if(result.stage=="key_format"){
            result.error=ERROR_INVALID_PARAMETER;result.reason="invalid_key_format";
            result.message="Expected the complete 81-character icv4_ activation key, with canonical base64url encoding.";
        }
    }catch(const std::bad_alloc&){
        result.error=ERROR_NOT_ENOUGH_MEMORY;result.reason="out_of_memory";
    }catch(...){
        result.error=ERROR_GEN_FAILURE;result.reason="local_error";
        result.message="Unexpected local verification failure.";
    }
    if(!primaryFailure.empty())result.message=primaryFailure+result.message;
    return result;
}
}

Verification_Result Verification(const std::string& licenseKey) {
    auto result=VerifyDirect<ActivationDriver>(licenseKey,[](std::string_view body,ULONGLONG deadline,unsigned route){
        return license::Post(L"/v1/verify",body,deadline,nullptr,route);
    });
    if(!result) {
        if(result.reason=="activation_interface_unavailable") {
            try{result.message=ActivationOpenDiagnostic();}catch(const std::bad_alloc&){}
        }
        AppendDriverVersions(result.message);
    }
    SetLastError(result.error);
    return result;
}

namespace Detail {
IC_LICENSE_STATUS Activation_Status(){
    try{return ActivationDriver().Status();}
    catch(const license::WindowsError& failure){
        std::string detail(failure.what());
        AppendDriverVersions(detail);
        throw std::system_error(static_cast<int>(failure.code),std::system_category(),detail);
    }
}
std::string Diagnose_License_Network(){
    try{return license::DiagnoseNetwork();}
    catch(const license::WindowsError& failure){
        throw std::system_error(static_cast<int>(failure.code),std::system_category(),failure.what());
    }
}
}
}


namespace Interceptor {
std::vector<Input_Driver_Binding> Get_Input_Driver_Bindings() {
    std::vector<Input_Driver_Binding> bindings;
    for(const auto* cls:{&GUID_DEVCLASS_MOUSE,&GUID_DEVCLASS_KEYBOARD}){
        const auto set=SetupDiGetClassDevsW(cls,nullptr,nullptr,DIGCF_PRESENT);
        if(set==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Enumerate input drivers");
        struct Guard {
            HDEVINFO set;
            ~Guard(){SetupDiDestroyDeviceInfoList(set);}
        } guard{set};
        for(DWORD index=0;;++index){
            SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
            if(!SetupDiEnumDeviceInfo(set,index,&device)){
                const auto error=GetLastError();
                if(error==ERROR_NO_MORE_ITEMS)break;
                throw std::system_error(error,std::system_category(),"Enumerate input driver instance");
            }
            wchar_t instance[MAX_DEVICE_ID_LEN]{}, service[256]{};
            if(!SetupDiGetDeviceInstanceIdW(set,&device,instance,_countof(instance),nullptr) ||
               _wcsnicmp(instance,L"HID\\",4))continue;
            if(!SetupDiGetDeviceRegistryPropertyW(set,&device,SPDRP_SERVICE,nullptr,
                 reinterpret_cast<BYTE*>(service),sizeof(service),nullptr))continue;
            Input_Driver_Binding row;
            row.instance=instance;
            row.service=service;
            row.inf=SetupDetail::InstalledMapperProperty(set,device,L"InfPath");
            row.mouse=cls==&GUID_DEVCLASS_MOUSE;
            row.original_requested=SetupDetail::IsMapperPaused(instance,L"Interceptor") ||
                SetupDetail::IsMapperPaused(instance,L"Interceptor");
            for(const auto property:{SPDRP_UPPERFILTERS,SPDRP_LOWERFILTERS}){
                DWORD size=0;
                SetupDiGetDeviceRegistryPropertyW(set,&device,property,nullptr,nullptr,0,&size);
                if(size)row.has_device_filters=true;
            }
            if(!_wcsicmp(row.service.c_str(),L"Interceptor") || !_wcsicmp(row.service.c_str(),L"Interceptor"))row.mode=Input_Driver_Mode::Interceptor;
            else if(SetupDetail::UsesInboxInputMapper(set,device,row.mouse) && !row.has_device_filters)
                row.mode=Input_Driver_Mode::Windows;
            else if(!row.inf.empty() && (!_wcsicmp(row.service.c_str(),L"mouhid") || !_wcsicmp(row.service.c_str(),L"kbdhid")))
                row.mode=Input_Driver_Mode::Manufacturer;
            bindings.push_back(std::move(row));
        }
    }
    return bindings;
}

std::uint32_t Set_Input_Driver_Mode(const std::wstring& instance,bool use_original) {
    if(instance.size()<7 || instance.size()>=MAX_DEVICE_ID_LEN || _wcsnicmp(instance.c_str(),L"HID\\",4) ||
       instance.find_first_of(L"\" \t\r\n")!=std::wstring::npos || instance.back()==L'\\' ||
       instance.find(L'\0')!=std::wstring::npos)return ERROR_INVALID_PARAMETER;
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;
    PSID admins=nullptr;
    if(!AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,
        0,0,0,0,0,0,&admins))return GetLastError();
    BOOL administrator=FALSE;
    const BOOL checked=CheckTokenMembership(nullptr,admins,&administrator);
    const DWORD checkError=checked?ERROR_SUCCESS:GetLastError();
    FreeSid(admins);
    if(checkError)return checkError;
    if(!administrator)return ERROR_ELEVATION_REQUIRED;

    const wchar_t* product=nullptr;
    try {
        Handle driver;
        Open(driver);
        product=driver.target->service;
    } catch(const std::system_error& error){return static_cast<DWORD>(error.code().value());}
    wchar_t programs[MAX_PATH]{}, system[MAX_PATH]{};
    DWORD bytes=sizeof(programs);
    const auto read=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
        L"ProgramFilesDir",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,programs,&bytes);
    if(read!=ERROR_SUCCESS)return static_cast<DWORD>(read);
    const auto length=GetSystemDirectoryW(system,_countof(system));
    if(!length)return GetLastError();
    if(length>=_countof(system))return ERROR_INSUFFICIENT_BUFFER;
    const auto script=std::wstring(programs)+L"\\"+product+L"\\Recovery\\SetMapperMode.ps1";
    const auto interpreter=std::wstring(system)+L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    const auto attributes=GetFileAttributesW(script.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES)return GetLastError();
    if(attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))return ERROR_INVALID_DATA;
    auto command=L"\""+interpreter+L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+
        script+L"\" -Mode "+(use_original?L"Original":L"Interceptor")+L" -Instance \""+instance+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);
    PROCESS_INFORMATION child{};
    if(!CreateProcessW(interpreter.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
        nullptr,nullptr,&startup,&child))return GetLastError();
    CloseHandle(child.hThread);
    const DWORD wait=WaitForSingleObject(child.hProcess,INFINITE);
    DWORD result=0;
    if(wait!=WAIT_OBJECT_0 || !GetExitCodeProcess(child.hProcess,&result))result=GetLastError();
    CloseHandle(child.hProcess);
    return result;
}
}
