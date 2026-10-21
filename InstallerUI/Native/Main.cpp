#include "Interceptor.h"
#include <iostream>
#include <string>
#include <system_error>
#include <atomic>
#include <iomanip>

int InstallDriver(const wchar_t* directory,bool restore,bool onlyNew=false,const wchar_t* mouseInf=nullptr,const wchar_t* controlInf=nullptr);
int ConfigureControlDriver(const wchar_t* inf,bool remove);
namespace {
void PrintInitializationFailure(const Interceptor::Init_Result& result) {
    if(!result.error)return;
    std::cerr<<"Device initialization failed [Win32 "<<std::dec<<result.error<<" / 0x"<<std::hex<<std::uppercase
        <<std::setw(8)<<std::setfill('0')<<result.error<<std::dec<<"]\n";
    if(!result.error_detail.empty())std::cerr<<"Stage: "<<result.error_detail<<'\n';
    if(!result.error_device.empty()) {
        const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,result.error_device.c_str(),-1,nullptr,0,nullptr,nullptr);
        if(count>0) {
            std::string device(static_cast<std::size_t>(count),'\0');
            if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,result.error_device.c_str(),-1,device.data(),count,nullptr,nullptr)>0)
                std::cerr<<"Device or class: "<<device.c_str()<<'\n';
        }
    }
}
bool UsbXusbBackendLoaded(const IC_CAPABILITIES& capabilities) {
    return (capabilities.controller_mask&IC_CONTROLLER_XUSB)!=0 &&
        (capabilities.xusb_profiles&4u)!=0 && (capabilities.xusb_profiles&3u)!=0;
}
int SetupPolicySelfTest() {
    struct Case {unsigned controllers,profiles;bool expected;};
    constexpr Case cases[]={{2,3,false},{2,7,true},{2,6,true},{0,7,false},{2,4,false},{2,0,false},{15,7,true}};
    for(const auto& test:cases) {
        IC_CAPABILITIES capabilities{};capabilities.controller_mask=test.controllers;capabilities.xusb_profiles=test.profiles;
        if(UsbXusbBackendLoaded(capabilities)!=test.expected)throw std::runtime_error("Installer USB backend policy test failed");
    }
    std::cout<<"Installer backend policy: 7 cases passed; no driver opened or installation performed.\n";
    return 0;
}
std::atomic<bool> quit=false;
HANDLE quitEvent=nullptr;SRWLOCK quitLock=SRWLOCK_INIT;
BOOL WINAPI OnControl(DWORD) {
    quit=true;AcquireSRWLockShared(&quitLock);if(quitEvent)SetEvent(quitEvent);ReleaseSRWLockShared(&quitLock);return TRUE;
}
struct ConsoleStop {
    ConsoleStop(){
        quitEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!quitEvent)throw std::system_error(GetLastError(),std::system_category(),"Create stop event");
        if(!SetConsoleCtrlHandler(OnControl,TRUE)) {
            const auto error=GetLastError();CloseHandle(quitEvent);quitEvent=nullptr;
            throw std::system_error(error,std::system_category(),"Register console handler");
        }
    }
    ~ConsoleStop(){SetConsoleCtrlHandler(OnControl,FALSE);AcquireSRWLockExclusive(&quitLock);auto event=quitEvent;quitEvent=nullptr;ReleaseSRWLockExclusive(&quitLock);CloseHandle(event);}
};

}
int wmain(int argc,wchar_t** argv) {
    using namespace Interceptor;
    try {
        std::wstring mode=argc>1?argv[1]:L"--help";
        if((mode==L"--self-test-setup" || mode==L"--self-test") && argc==2)return SetupPolicySelfTest();
        if(mode==L"--require-usb-backend" && argc==2) {
            try {
                if(UsbXusbBackendLoaded(Get_Capabilities()))return 0;
            }catch(const std::system_error& error) {
                if(error.code().value()!=ERROR_INVALID_FUNCTION && error.code().value()!=ERROR_NOT_SUPPORTED &&
                    error.code().value()!=ERROR_REVISION_MISMATCH)throw;
            }
            std::cout<<"The installed package requires the USB XUSB backend, but the loaded driver does not advertise it. "
                "Restart Windows and reopen this installer before completing associations.\n";
            return ERROR_SUCCESS_REBOOT_REQUIRED;
        }
        if(mode==L"--install-control" && argc==3)return ConfigureControlDriver(argv[2],false);
        if(mode==L"--remove-control" && argc==2)return ConfigureControlDriver(nullptr,true);
        if(mode==L"--install" && argc==3)return InstallDriver(argv[2],false);
        if(mode==L"--associate" && (argc==4 || argc==5))return InstallDriver(argv[2],false,true,argv[3],argc==5?argv[4]:nullptr);
        if(mode==L"--init-all") {
            const bool quiet=argc==3 && std::wstring(argv[2])==L"--quiet";
            if(argc!=2 && !quiet)throw std::invalid_argument("Expected --init-all [--quiet]");
            const auto r=Init_AllDevices();
            std::cout<<"Associated="<<r.associated<<"; already associated="<<r.already_associated<<"; skipped="<<r.skipped
                <<"; restart required="<<r.reboot_required<<"; ready="<<r.ready<<"; error="<<r.error<<'\n';
            PrintInitializationFailure(r);
            return r.error?static_cast<int>(r.error):r.ready?0:ERROR_SUCCESS_REBOOT_REQUIRED;
        }
        if(mode==L"--restore")return InstallDriver(nullptr,true);
        if(mode==L"--status") {
            auto s=Get_Status();
            std::cout<<"ABI "<<s.version<<"; keyboards="<<s.keyboard_devices<<"; mouse="<<s.mouse_devices
                     <<"; HID readers="<<s.controller_hid_devices<<"; HID queued="<<s.controller_hid_count
                     <<"; overflow resets="<<s.overflows<<"; last reset="<<s.last_reset_reason<<"\n";
            try {
                const auto c=Get_Capabilities();std::cout<<"Capabilities v"<<c.version<<"; controller mask="<<c.controller_mask
                    <<"; XUSB profiles="<<c.xusb_profiles<<"; XInputHID profiles="<<c.xinputhid_profiles<<"; GIP profiles="<<c.gip_profiles
                    <<" (compiled profiles, not connected/capturing device counts)\n";
            } catch(const std::system_error& e) {
                if(e.code().value()!=ERROR_INVALID_FUNCTION && e.code().value()!=ERROR_NOT_SUPPORTED)throw;
                std::cout<<"Capabilities endpoint unavailable; native association disabled\n";
            }
            return 0;
        }
        if(mode==L"--send-mouse") {
            const auto init=Init_AllDevices();
            if(!init.ready) {if(init.error)throw std::system_error(static_cast<int>(init.error),std::system_category(),"Init_AllDevices");return ERROR_SUCCESS_REBOOT_REQUIRED;}
            Mouse_Input m{};m.stroke.x=20; // device 0 selects an available mouse mapper.
            auto result=Send_Input(m);
            if(result.error)throw std::system_error(static_cast<int>(result.error),std::system_category(),"Send mouse with temporary activation");
            std::cout<<result.accepted<<" input sent with temporary capture\n";return 0;
        }
        if(mode!=L"--passthrough" && mode!=L"--remap" && mode!=L"--block") {
            std::cout<<"InterceptorExample (run as administrator)\n"
                "  --passthrough [keyboard|mouse|both] : capture and forward unchanged\n"
                "  --remap [keyboard|mouse|both]       : swap A/B scan codes, invert relative X\n"
                "  --block [keyboard|mouse|both]       : capture without Send\n"
                "  --send-mouse                       : move 20 units with temporary capture\n"
                "  --status | --self-test | --init-all [--quiet]\n"
                "  --require-usb-backend              : verify the loaded XUSB backend\n"
                "  --install FOLDER | --restore        : used by installer\n"
                "  --install-control CONTROL_INF | --remove-control : control anchor only\n"
                "  --associate KEYBOARD_INF MOUSE_INF CONTROL_INF : bind using staged packages\n"
                "Physical Ctrl+Alt+Delete revokes ALL capture contexts.\n";return mode==L"--help"?0:ERROR_INVALID_PARAMETER;
        }
        std::uint8_t mask=Both;
        if(argc>2) {
            const std::wstring type=argv[2];
            if(type==L"keyboard")mask=Keyboard;else if(type==L"mouse")mask=Mouse;
            else if(type!=L"both")throw std::invalid_argument("Expected keyboard, mouse or both");
        }
        ConsoleStop consoleStop;
        const auto init=Init_AllDevices();
        if(!init.ready) {if(init.error)throw std::system_error(static_cast<int>(init.error),std::system_category(),"Init_AllDevices");return ERROR_SUCCESS_REBOOT_REQUIRED;}
        std::cout<<"Release held keys/buttons. Capture begins in 2 seconds.\n"
                 <<"Press physical Ctrl+Alt+Delete to stop capture.\n"<<std::flush;
        if(WaitForSingleObject(quitEvent,2000)==WAIT_OBJECT_0 || quit)return 0;
        for(const auto& device:Get_Devices())
            std::cout<<"Device type="<<device.type<<" id="<<device.id<<" VID="<<std::hex<<device.vid<<" PID="<<device.pid<<std::dec<<'\n';
        auto handler=[&](auto& input)->bool {
            using T=std::decay_t<decltype(input)>;
            if constexpr(std::is_same_v<T,Keyboard_Input>) {
                if(!(mask&Keyboard))return true;
                if(mode==L"--remap" && !(input.stroke.flags&(KeyE0|KeyE1))) {
                    if(input.stroke.make_code==0x1E)input.stroke.make_code=0x30;
                    else if(input.stroke.make_code==0x30)input.stroke.make_code=0x1E;
                }
                return mode!=L"--block";
            } else if constexpr(std::is_same_v<T,Mouse_Input>) {
                if(!(mask&Mouse))return true;
                if(mode==L"--remap" && !(input.stroke.flags&MouseAbsolute))
                    input.stroke.x=input.stroke.x==INT32_MIN?INT32_MAX:-input.stroke.x;
                return mode!=L"--block";
            } else return true;
        };
        while(!quit) {
            auto inputs=Get_Input_With_Timeout(INFINITE,quitEvent);
            if(inputs.empty())continue;
            for(auto& event:inputs) {
                if(auto* key=event.Get<Keyboard_Input>())event.blocked=!handler(*key);
                else if(auto* mouse=event.Get<Mouse_Input>())event.blocked=!handler(*mouse);
            }
            const auto sent=Send_Input(inputs);
            if(!sent)throw std::system_error(static_cast<int>(sent.error),std::system_category(),"Forward input batch");
        }
        Destroy_Context();return 0;
    } catch(const std::system_error& e) {
        if(e.code().value()==ERROR_OPERATION_ABORTED) { std::cout<<"Capture revoked; normal input restored.\n";return 0; }
        std::cerr<<e.what()<<"\n";return 1;
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n";return 1; }
}
