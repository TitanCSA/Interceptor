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

void Transform(Hid_State& state,const Options&) {
    // Pair consecutive declared buttons within the same report and collection.
    // A trailing unpaired button remains unchanged. Hats are axes, not buttons.
    for(std::size_t i=0;i+1<state.buttons.size();) {
        auto& a=state.buttons[i];auto& b=state.buttons[i+1];
        if(a.report_id==b.report_id && a.link_collection==b.link_collection && a.usage_page==b.usage_page) {
            std::swap(a.pressed,b.pressed);i+=2;
        } else ++i;
    }
}
void Transform(IC_NATIVE_GAMEPAD_STATE& state,bool gip,const Options&) {
    // A/B, X/Y, LB/RB, Back/Start, LS/RS, Up/Down, Left/Right.
    // Guide is the odd last button; unknown/reserved bits remain untouched.
    static constexpr unsigned xusb[]={12,13,14,15,8,9,5,4,6,7,0,1,2,3};
    static constexpr unsigned gips[]={4,5,6,7,12,13,3,2,14,15,8,9,10,11};
    const auto* bits=gip?gips:xusb;
    const auto original=state.buttons;
    for(std::size_t i=0;i<14;i+=2) {
        const unsigned a=1u<<bits[i],b=1u<<bits[i+1];
        state.buttons=static_cast<std::uint16_t>((state.buttons&~(a|b))|
            ((original&a)?b:0)|((original&b)?a:0));
    }
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
    std::cout<<"Swap buttons in pairs. HID, XUSB, XInputHID and GIP.\n"
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
            std::cout<<"Swap buttons in pairs\n"
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

