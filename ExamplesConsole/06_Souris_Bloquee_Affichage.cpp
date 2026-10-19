#include "Interceptor.h"
#include <cstdio>
#include <cwchar>
#include <iostream>
#include <stdexcept>
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

HANDLE stopRequested=nullptr,finished=nullptr;
SRWLOCK consoleLock=SRWLOCK_INIT;

BOOL WINAPI ConsoleControl(DWORD signal) {
    AcquireSRWLockShared(&consoleLock);
    if(stopRequested)SetEvent(stopRequested);
    // Give main time to close capture before Windows terminates a closing console.
    if(signal==CTRL_CLOSE_EVENT && finished)WaitForSingleObject(finished,2500);
    ReleaseSRWLockShared(&consoleLock);
    return TRUE;
}

struct ConsoleLifetime {
    DWORD oldMode=0;bool changedMode=false;
    ConsoleLifetime() {
        finished=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!finished)throw std::system_error(GetLastError(),std::system_category(),"Create finished event");
        stopRequested=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!stopRequested || !SetConsoleCtrlHandler(ConsoleControl,TRUE)) {
            const DWORD error=GetLastError();
            if(stopRequested)CloseHandle(stopRequested);
            CloseHandle(finished);stopRequested=finished=nullptr;
            throw std::system_error(static_cast<int>(error),std::system_category(),"Register console cancellation");
        }
        const HANDLE input=GetStdHandle(STD_INPUT_HANDLE);
        if(GetConsoleMode(input,&oldMode))
            changedMode=SetConsoleMode(input,(oldMode|ENABLE_EXTENDED_FLAGS)&~ENABLE_QUICK_EDIT_MODE)!=FALSE;
    }
    ~ConsoleLifetime() {
        // Every batch has left scope before this destructor runs.
        try {Interceptor::Destroy_Context();}catch(...){}
        if(changedMode)SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),oldMode);
        SetEvent(finished);SetConsoleCtrlHandler(ConsoleControl,FALSE);
        AcquireSRWLockExclusive(&consoleLock);
        const HANDLE stop=stopRequested,done=finished;stopRequested=finished=nullptr;
        ReleaseSRWLockExclusive(&consoleLock);
        CloseHandle(stop);CloseHandle(done);
    }
    ConsoleLifetime(const ConsoleLifetime&)=delete;
    ConsoleLifetime& operator=(const ConsoleLifetime&)=delete;
};

bool Forward(Interceptor::Input_Batch& inputs) {
    const auto deadline=GetTickCount64()+2000;
    for(;;) {
        const auto sent=Interceptor::Send_Input(inputs);
        if(sent)return true;
        if(sent.error!=ERROR_BUSY)
            throw std::system_error(static_cast<int>(sent.error),std::system_category(),"Forward input batch");
        // A retried batch sends only its unaccepted entries. Wait only on backpressure.
        if(GetTickCount64()>=deadline)throw std::runtime_error("Windows input queue stayed full for 2 seconds; capture released.");
        if(WaitForSingleObject(stopRequested,1)==WAIT_OBJECT_0)return false;
    }
}
}

int wmain(int argc,wchar_t** argv) {
    using namespace Interceptor;
    if(argc==2 && (!std::wcscmp(argv[1],L"--help") || !std::wcscmp(argv[1],L"/?"))) {
        std::cout<<"WARNING: MOUSE INPUT IS BLOCKED and displayed here.\nUsage: 06_Souris_Bloquee_Affichage.exe [--help]\n";return 0;
    }
    if(argc!=1){std::cerr<<"Run without arguments, or use --help.\n";return ERROR_INVALID_PARAMETER;}
    try {
        if(!CheckInterceptorInstallation())return ERROR_SERVICE_DOES_NOT_EXIST;
        ConsoleLifetime console;
        std::cout<<"Initializing Interceptor devices...\n"<<std::flush;
        const auto init=Init_AllDevices();
        if(init.error)throw std::system_error(static_cast<int>(init.error),std::system_category(),"Init_AllDevices");
        if(!init.ready) {
            std::cerr<<(init.reboot_required
                ?"Restart Windows, rerun the installer's /install, then launch this example.\n"
                :"Devices are not ready. Check the installer diagnostics.\n");
            return init.reboot_required?ERROR_SUCCESS_REBOOT_REQUIRED:ERROR_NOT_READY;
        }
        std::cout<<"WARNING: MOUSE INPUT IS BLOCKED and displayed here.\nPress Ctrl+C or use physical Ctrl+Alt+Delete to stop. Keyboard input remains usable.\nRun only one capture example at a time.\nRelease all keys/buttons; capture starts in 2 seconds.\n"<<std::flush;
        if(WaitForSingleObject(stopRequested,2000)==WAIT_OBJECT_0)return 0;
        std::cout<<"Capture active.\n"<<std::flush;
        for(;;) {
            auto inputs=Get_Input_With_Timeout(INFINITE,stopRequested);
            if(inputs.empty()) {
                if(WaitForSingleObject(stopRequested,0)==WAIT_OBJECT_0)break;
                continue;
            }
            for(auto& event:inputs)if(event.Get<Mouse_Input>())event.blocked=true;
            // Forward keyboards/controllers before console output; mouse input stays blocked.
            if(!Forward(inputs))break;
            for(auto& event:inputs)if(const auto* mouse=event.Get<Mouse_Input>()) {
                const auto buttons=mouse->stroke.button_flags;
                std::printf("MOUSE seq=%llu dev=%u x=%ld y=%ld %s flags=0x%03X wheel=%d %s%s%s%s%s%s%s%s%s%s\n",
                    static_cast<unsigned long long>(mouse->sequence),mouse->device,
                    static_cast<long>(mouse->stroke.x),static_cast<long>(mouse->stroke.y),
                    (mouse->stroke.flags&MouseAbsolute)?"ABS":"REL",static_cast<unsigned>(buttons),
                    static_cast<int>(static_cast<short>(mouse->stroke.button_data)),
                    (buttons&0x001)?"L-DOWN ":"",(buttons&0x002)?"L-UP ":"",
                    (buttons&0x004)?"R-DOWN ":"",(buttons&0x008)?"R-UP ":"",
                    (buttons&0x010)?"MID-DOWN ":"",(buttons&0x020)?"MID-UP ":"",
                    (buttons&0x040)?"X1-DOWN ":"",(buttons&0x080)?"X1-UP ":"",
                    (buttons&0x100)?"X2-DOWN ":"",(buttons&0x200)?"X2-UP ":"");
            }
            std::fflush(stdout);
        }
    }catch(const std::system_error& error) {
        if(error.code().value()!=ERROR_OPERATION_ABORTED) {
            std::cerr<<"Error: "<<error.what()<<"\nInstall Interceptor and run this EXE as administrator.\n";
            return 1;
        }
        std::cout<<"Capture cancelled by the driver.\n";
    }catch(const std::exception& error){std::cerr<<"Error: "<<error.what()<<'\n';return 1;}
    std::cout<<"Capture closed; normal input restored.\n";
    return 0;
}
