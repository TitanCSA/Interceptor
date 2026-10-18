#pragma once
// Shared by initialization and its pure tests. No device/registry access here.
#include "Interceptor.h"
#include "internal/XusbMirrorIdentity.h"
#include <algorithm>
#include <string>
#include <vector>

namespace Interceptor { namespace Policy {
inline bool Equal(const std::wstring& a,const wchar_t* b) {return _wcsicmp(a.c_str(),b)==0;}
inline bool Has(const std::vector<std::wstring>& names,const wchar_t* value) {
    return std::any_of(names.begin(),names.end(),[&](const auto& name){return Equal(name,value);});
}
enum class ClassAssociation { PerDevice, ExistingXusbUpper, Conflict };
inline ClassAssociation ClassFilterAssociation(std::uint32_t transport,
    const std::vector<std::wstring>& deviceUpper,const std::vector<std::wstring>& deviceLower,
    const std::vector<std::wstring>& classUpper,const std::vector<std::wstring>& classLower) {
    const auto count=[](const auto& names){return std::count_if(names.begin(),names.end(),
        [](const auto& name){return Equal(name,L"Interceptor");});};
    const auto upper=count(classUpper),lower=count(classLower);
    if(!upper&&!lower)return ClassAssociation::PerDevice;
    // The native XUSB dispatch supports an upper filter above xusb22. A class
    // registration and a device registration would load our service twice.
    // XInputHID requires device-list ordering before xinputhid; GIP dispatch
    // currently requires the per-device LowerFilters property. Class placement
    // alone cannot establish either of those supported contracts.
    if(transport==IC_CONTROLLER_XUSB && upper==1 && !lower &&
       !count(deviceUpper) && !count(deviceLower))return ClassAssociation::ExistingXusbUpper;
    return ClassAssociation::Conflict;
}
inline bool ValidCapabilities(const IC_CAPABILITIES& c,std::size_t bytes) {
    return bytes==sizeof(c) && c.size==sizeof(c) && c.version==IC_CAPABILITIES_VERSION && c.abi_version==IC_VERSION &&
        !(c.input_mask&~IC_BOTH) && !(c.controller_mask&~15u) && !c.flags && !c.reserved[0] && !c.reserved[1] && !c.reserved[2] &&
        c.max_devices && c.max_devices<=IC_MAX_DEVICES && c.queue_capacity==IC_CAPACITY && c.max_send==IC_MAX_SEND &&
        c.hid_report_max==IC_HID_REPORT_MAX;
}
inline std::uint32_t SupportedControllers(const IC_CAPABILITIES& c) {
    auto mask=c.controller_mask&IC_CONTROLLER_HID;
    if(c.xusb_profiles&(IC_PROFILE_XUSB_WIRED_028E|IC_PROFILE_XUSB_WIRED_COMPATIBLE))mask|=c.controller_mask&IC_CONTROLLER_XUSB;
    if(c.xinputhid_profiles&IC_PROFILE_XINPUTHID_GENERIC6)mask|=c.controller_mask&IC_CONTROLLER_XINPUTHID;
    if(c.gip_profiles&IC_PROFILE_GIP_USB_GAMEPAD14)mask|=c.controller_mask&IC_CONTROLLER_GIP;
    return mask;
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
        if(Equal(name,L"Interceptor"))continue; // Remove duplicates and correct a previous wrong position.
        if(!remove && transport==IC_CONTROLLER_XINPUTHID && Equal(name,L"xinputhid") && !inserted) {
            out.emplace_back(L"Interceptor");inserted=true;
        }
        out.push_back(name);
    }
    if(!remove && !inserted)out.emplace_back(L"Interceptor");
    return out;
}
}}
