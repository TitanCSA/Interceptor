#ifndef IC_SDK_INTERNAL
#define IC_SDK_INTERNAL
#endif
#include "Interceptor.h"
// Interceptor SDK implementation. No other project files are required.

// ---- Interceptor.cpp ----

// ---- ContextInternal.h ----
// Wire-compatibility entry point for the archived regression harnesses only.
// Applications use the single public Create_Context() declaration.
namespace Interceptor { namespace Detail {
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
#include <string>
#include <vector>

namespace Interceptor { namespace Policy {
inline bool Equal(const std::wstring& a,const wchar_t* b) {return _wcsicmp(a.c_str(),b)==0;}
inline bool Has(const std::vector<std::wstring>& names,const wchar_t* value) {
    return std::any_of(names.begin(),names.end(),[&](const auto& name){return Equal(name,value);});
}
inline bool ForeignInterceptor(const std::wstring& name) {
    return !Equal(name,IC_SDK_SERVICE) && (Equal(name,L"Interceptor") ||
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
        [](const auto& name){return Equal(name,IC_SDK_SERVICE);});};
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
        if(Equal(name,IC_SDK_SERVICE))continue; // Remove duplicates and correct a previous wrong position.
        if(!remove && transport==IC_CONTROLLER_XINPUTHID && Equal(name,L"xinputhid") && !inserted) {
            out.emplace_back(IC_SDK_SERVICE);inserted=true;
        }
        out.push_back(name);
    }
    if(!remove && !inserted)out.emplace_back(IC_SDK_SERVICE);
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
#define IC_XUSB_MIRROR_HID_PARENT_VALUE L"InterceptorXusbHidParent"
#define IC_XUSB_MIRROR_PARENT_VALUE L"InterceptorXusbParent"
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
    if(error==IC_ERROR_TRIAL_EXPIRED)operation="Interceptor two-hour trial expired; restart Windows to use capture again";
    throw std::system_error(static_cast<int>(error),std::system_category(),operation);
}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle() { if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value); }
    Handle()=default;Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
void Open(Handle& h) {
    h.value=CreateFileW(IC_SDK_DOS_DEVICE,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
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
constexpr auto RestartKey=L"SOFTWARE\\Interceptor\\PendingDeviceRestart";
DWORD PendingRestart(bool& pending) {
    DWORD flag=0,bytes=sizeof(flag);
    const LSTATUS status=RegGetValueW(HKEY_LOCAL_MACHINE,RestartKey,L"Required",RRF_RT_REG_DWORD|RRF_SUBKEY_WOW6464KEY,nullptr,&flag,&bytes);
    pending=status==ERROR_SUCCESS && flag!=0;
    return status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND?ERROR_SUCCESS:static_cast<DWORD>(status);
}
DWORD RememberRestart() {
    HKEY key=nullptr;
    LSTATUS status=RegCreateKeyExW(HKEY_LOCAL_MACHINE,RestartKey,0,nullptr,REG_OPTION_VOLATILE,KEY_SET_VALUE|KEY_WOW64_64KEY,nullptr,&key,nullptr);
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
    if(!_wcsicmp(service.c_str(),IC_SDK_SERVICE)) {++result.already_associated;CheckStarted(device,result);return;}
    if(_wcsicmp(service.c_str(),mouse?L"mouhid":L"kbdhid")) {++result.skipped;return;}
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
       _wcsicmp(Property(set,device,SPDRP_HARDWAREID).c_str(),hardware.c_str())) {
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
            if(!_wcsicmp(name,IC_SDK_SERVICE)) {found=true;if(remove)continue;}
            updated.insert(updated.end(),name,name+wcslen(name)+1);
        }
        if(found==!remove) {
            if(!remove) {
                ++result.already_associated;CheckStarted(device,result);
                if(!ActiveControllerFilter(id))result.reboot_required=true;
            }
            continue;
        }
        if(!remove) {const wchar_t name[]=IC_SDK_SERVICE;updated.insert(updated.end(),std::begin(name),std::end(name));}
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
        if(Policy::Has(gip?upper:lower,IC_SDK_SERVICE)) {
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
    for(const auto* name:{IC_XUSB_MIRROR_HID_PARENT_VALUE,IC_XUSB_MIRROR_PARENT_VALUE}) {
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
        for(const auto* name:{IC_XUSB_MIRROR_HID_PARENT_VALUE,IC_XUSB_MIRROR_PARENT_VALUE}) {
            const auto status=RegQueryValueExW(key.value,name,nullptr,nullptr,nullptr,nullptr);
            if(status==ERROR_SUCCESS||status==ERROR_MORE_DATA)marked=true;
            else if(status!=ERROR_FILE_NOT_FOUND)throw std::system_error(status,std::system_category(),"Read legacy XUSB mirror marker");
        }
        const auto upper=FilterNames(MultiProperty(set.value,d,SPDRP_UPPERFILTERS));
        const auto lower=FilterNames(MultiProperty(set.value,d,SPDRP_LOWERFILTERS));
        if(!marked && ((!Policy::Has(upper,IC_SDK_SERVICE)&&!Policy::Has(lower,IC_SDK_SERVICE)) ||
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
    MutexHandle interprocess{CreateMutexW(nullptr,FALSE,L"Global\\Interceptor.InitAllDevices"),false};
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
