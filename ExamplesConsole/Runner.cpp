#include "Interceptor.h"
#include "Behaviors.h"
#include <atomic>
#include <array>
#include <iostream>
#include <system_error>
#include <conio.h>

namespace Examples {
namespace {
std::atomic<bool> stop{false};
HANDLE stopped=nullptr;
HANDLE stopRequested=nullptr;
SRWLOCK exitLock=SRWLOCK_INIT;
BOOL WINAPI ConsoleControl(DWORD type) {
    stop=true;
    AcquireSRWLockShared(&exitLock);
    if(stopRequested)SetEvent(stopRequested);
    if(type==CTRL_CLOSE_EVENT) {
        if(stopped)WaitForSingleObject(stopped,2500);
    }
    ReleaseSRWLockShared(&exitLock);
    return TRUE;
}
struct ConsoleLifetime {
    DWORD oldMode=0;bool changed=false;
    ConsoleLifetime() {
        stopped=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!stopped)throw std::system_error(GetLastError(),std::system_category(),"CreateEvent");
        stopRequested=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!stopRequested){const auto error=GetLastError();CloseHandle(stopped);stopped=nullptr;throw std::system_error(error,std::system_category(),"CreateEvent");}
        if(!SetConsoleCtrlHandler(ConsoleControl,TRUE)) {
            const auto error=GetLastError();CloseHandle(stopRequested);CloseHandle(stopped);
            stopRequested=stopped=nullptr;
            throw std::system_error(error,std::system_category(),"Register console handler");
        }
        if(GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&oldMode))
            changed=SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),(oldMode|ENABLE_EXTENDED_FLAGS)&~ENABLE_QUICK_EDIT_MODE)!=FALSE;
    }
    ~ConsoleLifetime() {
        if(changed)SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),oldMode);
        SetEvent(stopped);
        SetConsoleCtrlHandler(ConsoleControl,FALSE);
        AcquireSRWLockExclusive(&exitLock);
        const HANDLE event=stopped;stopped=nullptr;
        const HANDLE request=stopRequested;stopRequested=nullptr;
        ReleaseSRWLockExclusive(&exitLock);
        CloseHandle(event);
        CloseHandle(request);
    }
};
void PauseIfOwnConsole() {
    DWORD processes[2],mode=0;
    if(!stop && GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&mode) && GetConsoleProcessList(processes,2)==1) {
        std::cout<<"Press Enter to close.\n"<<std::flush;
        while(_getch()!='\r'){}
    }
}
void SendOutputs(const std::vector<Emission>& out,std::vector<Keyboard_Input>& keys,std::vector<Mouse_Input>& mice) {
    std::size_t index=0;
    while(index<out.size() && !stop) {
        keys.clear();mice.clear();
        const auto kind=out[index].kind;std::size_t n=0;
        while(index+n<out.size() && n<256 && out[index+n].kind==kind) {
            if(kind==Keyboard)keys.push_back(out[index+n].key);else mice.push_back(out[index+n].mouse);
            ++n;
        }
        std::size_t sent=0;const auto deadline=GetTickCount64()+2000;
        while(sent<n && !stop) {
            const auto result=kind==Keyboard?Send_Input(keys):Send_Input(mice);
            if(result.accepted>n-sent)throw std::runtime_error("Invalid send count");
            sent+=result.accepted;
            // The usual complete send needs no shifting. Keep only an actual
            // unaccepted suffix when Windows applies backpressure.
            if(sent<n && result.accepted) {
                if(kind==Keyboard)keys.erase(keys.begin(),keys.begin()+result.accepted);
                else mice.erase(mice.begin(),mice.begin()+result.accepted);
            }
            if(result.error && result.error!=ERROR_BUSY)throw std::system_error(static_cast<int>(result.error),std::system_category(),"Send");
            if(sent<n) {
                if(GetTickCount64()>=deadline)throw std::runtime_error("Windows queue full for 2 seconds: capture released.");
                WaitForSingleObject(stopRequested,1);
            }
        }
        index+=n;
    }
}
int LifetimeTest(Mode mode,int argc,wchar_t** argv) {
    (void)mode;
    (void)Get_Devices();
    if(argc==2 && std::wstring(argv[1])==L"--probe-context")return 0;
    if(argc!=3)throw std::invalid_argument("--lifetime-child requires an event name");
    HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[2]);
    if(!ready)throw std::system_error(GetLastError(),std::system_category(),"OpenEvent");
    SetEvent(ready);CloseHandle(ready);
    Sleep(30000); // Parent kills us. Timeout also releases the context if parent fails.
    return 0;
}
}
int Run(Mode mode,int argc,wchar_t** argv) {
    const bool testing=argc>1;
    try {
        if(testing) {
            const std::wstring argument=argv[1];
            if(argument==L"--probe-context" || argument==L"--lifetime-child")return LifetimeTest(mode,argc,argv);
            throw std::invalid_argument("Run without arguments to use this example.");
        }
        ConsoleLifetime console;
        std::cout<<"Initializing Interceptor devices...\n"<<std::flush;
        const auto init=Init_AllDevices();
        if(!init.ready) {
            if(init.error)throw std::system_error(static_cast<int>(init.error),std::system_category(),"Init_AllDevices");
            return ERROR_SUCCESS_REBOOT_REQUIRED;
        }
        std::cout<<Description(mode)<<"\n\n"
            <<"The Interceptor driver is required. Run only one capture example at a time.\n"
            <<"Recovery: PHYSICAL Ctrl+Alt+Delete. Closing this window also releases capture.\n"
            <<"Release all keys/buttons. Starting in 2 seconds.\n"
            <<"For remapping examples, then focus the target application.\n"<<std::flush;
        WaitForSingleObject(stopRequested,2000);
        if(stop)return 0;
        for(const auto& device:Get_Devices())
            std::cout<<"Device type="<<device.type<<" id="<<device.id<<" VID="<<std::hex<<device.vid<<" PID="<<device.pid<<std::dec<<'\n';
        Behavior behavior(mode,(GetKeyState(VK_CAPITAL)&1)!=0);
        std::vector<Input_Event*> ordered;ordered.reserve(256);
        std::vector<Emission> out;out.reserve(512);
        std::vector<Keyboard_Input> keys;keys.reserve(256);
        std::vector<Mouse_Input> mice;mice.reserve(256);
        std::string log;log.reserve(65536);
        std::cout<<"Capture is active.\n"<<std::flush;
        while(!stop) {
            auto inputs=Get_Input_With_Timeout(INFINITE,stopRequested);
            if(inputs.empty())continue;
            ordered.clear();out.clear();log.clear();
            for(auto& event:inputs) {
                if((event.type==Device_Type::Keyboard && (Mask(mode)&Keyboard)) ||
                   (event.type==Device_Type::Mouse && (Mask(mode)&Mouse))) {
                    event.blocked=true;ordered.push_back(&event);
                }
            }
            // Keyboard/mouse share a sequence counter. Preserve their order
            // for combined behaviours without sorting large report payloads.
            if(Mask(mode)==Both)
                std::sort(ordered.begin(),ordered.end(),[](const auto* a,const auto* b){return a->sequence<b->sequence;});
            for(auto* event:ordered) {
                if(auto* key=event->Get<Keyboard_Input>())behavior.KeyboardInput(*key,out,log);
                else if(auto* mouse=event->Get<Mouse_Input>())behavior.MouseInput(*mouse,out,log);
            }
            const auto forwarded=Send_Input(inputs);
            if(!forwarded)throw std::system_error(static_cast<int>(forwarded.error),std::system_category(),"Forward untouched input");
            SendOutputs(out,keys,mice);
            if(!log.empty()) { std::fwrite(log.data(),1,log.size(),stdout);std::fflush(stdout); }
        }
        Destroy_Context();
        std::cout<<"Capture closed; normal input restored.\n";
    } catch(const std::system_error& e) {
        if(e.code().value()==ERROR_OPERATION_ABORTED) {
            std::cout<<"Capture cancelled by the driver. Normal input restored.\n";
        } else {
            std::cerr<<"Error: "<<e.what()<<"\nInstall Interceptor and run this EXE as administrator.\n";
            if(!testing)PauseIfOwnConsole();return 1;
        }
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<"\n";if(!testing)PauseIfOwnConsole();return 1;
    }
    if(!testing)PauseIfOwnConsole();
    return 0;
}
}
