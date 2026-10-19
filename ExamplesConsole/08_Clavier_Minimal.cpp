#include "Interceptor.h"
#include <iostream>
#include <system_error>
#include <winsvc.h>

namespace {
bool CheckInterceptorInstallation() {
    const SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if(!manager)throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Check Interceptor installation");
    const SC_HANDLE service=OpenServiceW(manager,L"Interceptor",SERVICE_QUERY_STATUS);
    const DWORD error=service?ERROR_SUCCESS:GetLastError();
    if(service)CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if(error==ERROR_SERVICE_DOES_NOT_EXIST) {
        std::cerr<<"Interceptor is not installed.\n"
            <<"Run InterceptorSetup.exe, or InterceptorSetupConsole.exe /install, then launch this example again.\n";
        // Keep a double-clicked console visible; shells and redirected runs do not pause.
        DWORD processes[2]{},mode=0;
        if(GetConsoleProcessList(processes,2)==1 && GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&mode)) {
            std::cerr<<"Press Enter to close..."<<std::flush;
            std::cin.get();
        }
        return false;
    }
    if(error)throw std::system_error(static_cast<int>(error),std::system_category(),"Could not verify Interceptor installation");
    return true;
}
}

int wmain() {
    using namespace Interceptor;
    try {
        if(!CheckInterceptorInstallation())return ERROR_SERVICE_DOES_NOT_EXIST;
        const auto init=Init_AllDevices();
        if(!init.ready)return static_cast<int>(init.error?init.error:ERROR_SUCCESS_REBOOT_REQUIRED);
        for(;;) {
            // The library opens/reuses its session automatically. The batch
            // borrows reusable storage until the end of this iteration.
            auto inputs=Get_Input_With_Timeout(INFINITE,nullptr);
            for(auto& event:inputs) {
                if(auto* key=event.Get<Keyboard_Input>()) {
                    if(!(key->stroke.flags&(KeyE0|KeyE1)) && key->stroke.make_code==0x23)
                        key->stroke.make_code=0x24;
                }
            }
            const auto sent=Send_Input(inputs);
            if(!sent)throw std::system_error(static_cast<int>(sent.error),std::system_category(),"Forward input batch");
        }
    }catch(const std::system_error& e) {
        if(e.code().value()==ERROR_OPERATION_ABORTED)return 0;
        std::cerr<<e.what()<<'\n';return 1;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    return 0;
}
