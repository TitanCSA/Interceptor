#include "Interceptor.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <winsvc.h>

namespace {
using namespace Interceptor;
// HID descriptors do not always label which axes are sticks or triggers.
// auto: X/Y + Z/Rz if available, otherwise X/Y + Rx/Ry.
// Override ambiguous descriptors with --hid-layout xy-rxry or xy-zrz.
enum class Layout { Auto, RxRy, ZRz };
enum class Role { Other, Horizontal, Vertical, Trigger };
struct Options { Layout layout=Layout::Auto; double exponent=2.0; };
bool SameGroup(const Hid_Value& a,const Hid_Value& b) {
    return a.report_id==b.report_id && a.link_collection==b.link_collection && a.element==b.element;
}
bool Has(const Hid_State& state,const Hid_Value& axis,unsigned usage) {
    for(const auto& other:state.axes)
        if(other.usage_page==1 && other.usage==usage && SameGroup(axis,other))return true;
    return false;
}
Role AxisRole(const Hid_State& state,const Hid_Value& a,Layout layout) {
    if(a.usage_page!=1)return Role::Other;
    if(a.usage==0x30)return Role::Horizontal; // X
    if(a.usage==0x31)return Role::Vertical;   // Y, including repeated Y controls
    const bool zr=layout==Layout::ZRz || (layout==Layout::Auto && Has(state,a,0x32) && Has(state,a,0x35));
    if(zr) {
        if(a.usage==0x32)return Role::Horizontal;
        if(a.usage==0x35)return Role::Vertical;
        if(a.usage==0x33 || a.usage==0x34)return Role::Trigger;
    } else {
        if(a.usage==0x33)return Role::Horizontal;
        if(a.usage==0x34)return Role::Vertical;
        // A lone Z is often a combined trigger. Do not guess its encoding.
        if((layout!=Layout::Auto || (Has(state,a,0x32)&&Has(state,a,0x35))) &&
           (a.usage==0x32 || a.usage==0x35))return Role::Trigger;
    }
    return Role::Other;
}
bool Editable(const Hid_Value& a) {
    return a.normalized_valid && !a.relative && !a.is_null &&
        a.maximum>a.minimum && std::isfinite(a.normalized);
}

void Transform(Hid_State& state,const Options& options) {
    for(auto& trigger:state.triggers)
        if(Editable(trigger))trigger.normalized=trigger.normalized>0.0?1.0:0.0;
    for(auto& a:state.axes)
        if(Editable(a)&&AxisRole(state,a,options.layout)==Role::Trigger)
            a.normalized=a.value>a.minimum?1.0:-1.0;
    // Digital triggers are already binary and remain unchanged.
}
void Transform(IC_NATIVE_GAMEPAD_STATE& state,bool gip,const Options&) {
    const auto maximum=static_cast<std::uint16_t>(gip?1023:255);
    state.left_trigger=state.left_trigger?maximum:0;
    state.right_trigger=state.right_trigger?maximum:0;
}

// One loop for every controller family. No per-packet logging, timer or FPS cap.
int Run(const Options& options) {
    const auto manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if(!manager)throw std::system_error(GetLastError(),std::system_category(),"Check driver installation");
    const auto service=OpenServiceW(manager,L"Interceptor",SERVICE_QUERY_STATUS);
    const DWORD error=service?ERROR_SUCCESS:GetLastError();
    if(service)CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if(error==ERROR_SERVICE_DOES_NOT_EXIST) {
        std::cerr<<"Interceptor is not installed.\n"
            <<"Run InterceptorSetup.exe or InterceptorSetupConsole.exe /install first.\n";
        return static_cast<int>(error);
    }
    if(error)throw std::system_error(error,std::system_category(),"Check driver installation");
    const auto init=Init_AllDevices();
    if(!init.ready) {
        std::cerr<<"Initialization: error="<<init.error<<", restart="<<init.reboot_required
                 <<" "<<init.error_detail<<"\n";
        return static_cast<int>(init.error?init.error:ERROR_SUCCESS_REBOOT_REQUIRED);
    }
    DWORD mode=0;const auto console=GetStdHandle(STD_INPUT_HANDLE);
    if(GetConsoleMode(console,&mode))SetConsoleMode(console,(mode|ENABLE_EXTENDED_FLAGS)&~ENABLE_QUICK_EDIT_MODE);
    std::cout<<"Instant full-pressure triggers. HID, XUSB, XInputHID and GIP.\n"
             <<"Run as administrator. Close this process / Ctrl+C to restore passthrough.\n";
    for(;;) {
        auto inputs=Get_Input_With_Timeout(INFINITE);
        for(auto& event:inputs) {
            if(auto* hid=event.Get<Controller_Hid_Input>())Transform(hid->state,options);
            else if(auto* xusb=event.Get<Controller_Xinput_Input>())Transform(xusb->state,false,options);
            else if(auto* xih=event.Get<Controller_XinputHid_Input>())Transform(xih->state,options);
            else if(auto* gip=event.Get<Controller_GIP_Input>())Transform(gip->state,true,options);
        }
        const auto sent=Send_Input(inputs);
        if(!sent || sent.accepted!=inputs.size())
            throw std::system_error(sent.error?sent.error:ERROR_WRITE_FAULT,std::system_category(),"Send_Input (incomplete batch)");
    }
}
} // namespace

int wmain(int argc,wchar_t** argv) try {
    Options options;
    for(int i=1;i<argc;++i) {
        const std::wstring argument=argv[i];
        if(argument==L"--help") {
            std::cout<<"Instant full-pressure triggers\n"
                <<"Options: --hid-layout auto|xy-rxry|xy-zrz\n";
            return 0; // No driver access for help.
        }
        if(argument==L"--hid-layout" && i+1<argc) {
            const std::wstring layout=argv[++i];
            if(layout==L"auto")options.layout=Layout::Auto;
            else if(layout==L"xy-rxry")options.layout=Layout::RxRy;
            else if(layout==L"xy-zrz")options.layout=Layout::ZRz;
            else throw std::invalid_argument("Unknown HID layout.");
        }
        else throw std::invalid_argument("Unknown argument. Use --help.");
    }
    return Run(options);
} catch(const std::exception& error) {
    std::cerr<<error.what()<<"\nCapture released; passthrough restored.\n";
    return 1;
}

