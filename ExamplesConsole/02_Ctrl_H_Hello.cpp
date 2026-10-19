#include "Interceptor.h"
#include <iostream>
#include <map>
#include <set>
#include <system_error>
#include <winsvc.h>
#include <vector>

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

using namespace Interceptor;
constexpr std::uint16_t H=0x23;
HANDLE stopEvent=nullptr,finishedEvent=nullptr;
SRWLOCK consoleLock=SRWLOCK_INIT;

bool Stopping() {return WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0;}

BOOL WINAPI ConsoleControl(DWORD signal) {
    if(signal!=CTRL_C_EVENT && signal!=CTRL_BREAK_EVENT && signal!=CTRL_CLOSE_EVENT &&
       signal!=CTRL_LOGOFF_EVENT && signal!=CTRL_SHUTDOWN_EVENT)return FALSE;
    AcquireSRWLockShared(&consoleLock);
    if(stopEvent)SetEvent(stopEvent);
    if(signal==CTRL_CLOSE_EVENT && finishedEvent)WaitForSingleObject(finishedEvent,2500);
    ReleaseSRWLockShared(&consoleLock);
    return TRUE;
}

class ConsoleLifetime {
    DWORD oldMode_=0;
    bool changed_=false;
public:
    ConsoleLifetime() {
        finishedEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!finishedEvent)throw std::system_error(GetLastError(),std::system_category(),"Create completion event");
        stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!stopEvent || !SetConsoleCtrlHandler(ConsoleControl,TRUE)) {
            const auto error=GetLastError();
            if(stopEvent)CloseHandle(stopEvent);
            CloseHandle(finishedEvent);stopEvent=finishedEvent=nullptr;
            throw std::system_error(error,std::system_category(),"Register console cancellation");
        }
        if(GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&oldMode_))
            changed_=SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),(oldMode_|ENABLE_EXTENDED_FLAGS)&~ENABLE_QUICK_EDIT_MODE)!=FALSE;
    }
    ~ConsoleLifetime() {
        if(changed_)SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),oldMode_);
        SetEvent(finishedEvent);
        SetConsoleCtrlHandler(ConsoleControl,FALSE);
        AcquireSRWLockExclusive(&consoleLock);
        const HANDLE stop=stopEvent,finished=finishedEvent;
        stopEvent=finishedEvent=nullptr;
        ReleaseSRWLockExclusive(&consoleLock);
        CloseHandle(stop);CloseHandle(finished);
    }
    ConsoleLifetime(const ConsoleLifetime&)=delete;
    ConsoleLifetime& operator=(const ConsoleLifetime&)=delete;
};

// Release capture before the close handler is told that cleanup is finished.
struct CaptureLifetime {
    ~CaptureLifetime() {try{Destroy_Context();}catch(...){/* Process exit also closes driver handles. */}}
};

bool Plain(const Keyboard_Input& input,std::uint16_t scan) {
    return input.stroke.make_code==scan && !(input.stroke.flags&(KeyE0|KeyE1));
}
std::uint64_t KeyIdentity(const Keyboard_Input& input) {
    return (static_cast<std::uint64_t>(input.device)<<32)|
        (static_cast<std::uint64_t>(input.stroke.flags&(KeyE0|KeyE1))<<16)|input.stroke.make_code;
}

class HelloShortcut {
    bool caps_;
    std::map<std::uint64_t,Keyboard_Input> held_;
    std::set<std::uint64_t> consumedH_;
    bool HasControl() const {
        for(const auto& entry:held_)
            if(entry.second.stroke.make_code==0x1D && !(entry.second.stroke.flags&KeyE1))return true;
        return false;
    }
    bool HasAltOrWin() const {
        for(const auto& entry:held_) {
            const auto& key=entry.second;
            if(key.stroke.make_code==0x38 ||
               ((key.stroke.flags&KeyE0) && (key.stroke.make_code==0x5B || key.stroke.make_code==0x5C)))return true;
        }
        return false;
    }
    static bool Modifier(const Keyboard_Input& key) {
        return key.stroke.make_code==0x1D || Plain(key,0x2A) || Plain(key,0x36);
    }
    static void Emit(std::vector<Keyboard_Input>& output,const Keyboard_Input& source,std::uint16_t scan,bool up) {
        auto key=source;key.stroke.make_code=scan;key.stroke.flags=up?KeyUp:0;output.push_back(key);
    }
    void TypeHello(const Keyboard_Input& source,std::vector<Keyboard_Input>& output) const {
        // Keep the device ID and E0/E1 prefix of every held Ctrl/Shift key.
        // Releasing Ctrl first prevents the generated letters becoming shortcuts.
        for(const auto& entry:held_)if(Modifier(entry.second)) {
            auto release=entry.second;release.stroke.flags|=KeyUp;output.push_back(release);
        }
        if(caps_)Emit(output,source,0x2A,false); // Shift compensates for Caps Lock.
        for(const std::uint16_t scan:{H,std::uint16_t{0x12},std::uint16_t{0x26},std::uint16_t{0x26},std::uint16_t{0x18}}) {
            Emit(output,source,scan,false);Emit(output,source,scan,true);
        }
        if(caps_)Emit(output,source,0x2A,true);
        for(const auto& entry:held_)if(Modifier(entry.second))output.push_back(entry.second);
    }
public:
    explicit HelloShortcut(bool capsLock):caps_(capsLock){}
    void Process(Keyboard_Input input,std::vector<Keyboard_Input>& output) {
        const auto identity=KeyIdentity(input);
        const bool up=(input.stroke.flags&KeyUp)!=0,repeated=held_.find(identity)!=held_.end();
        if(up)held_.erase(identity);else held_[identity]=input;
        if(Plain(input,0x3A) && !up && !repeated)caps_=!caps_;
        // Consume the entire shortcut H press, including repeats and its release.
        if(consumedH_.count(identity)) {if(up)consumedH_.erase(identity);return;}
        if(Plain(input,H) && !up && !repeated && HasControl() && !HasAltOrWin()) {
            consumedH_.insert(identity);TypeHello(input,output);return;
        }
        output.push_back(input);
    }
};

void Backpressure(DWORD error,bool pending,ULONGLONG deadline) {
    if(error && error!=ERROR_BUSY)throw std::system_error(error,std::system_category(),"Send_Input");
    if(!pending)return;
    if(GetTickCount64()>=deadline)throw std::runtime_error("Windows input queue stayed full for two seconds; releasing capture.");
    const auto wait=WaitForSingleObject(stopEvent,1);
    if(wait==WAIT_FAILED)throw std::system_error(GetLastError(),std::system_category(),"Wait for cancellation");
}

void ForwardOthers(Input_Batch& batch,std::size_t wanted) {
    std::size_t accepted=0;const auto deadline=GetTickCount64()+2000;
    while(accepted<wanted && !Stopping()) {
        const auto sent=Send_Input(batch); // The batch remembers already accepted entries.
        if(sent.accepted>wanted-accepted)throw std::runtime_error("Invalid input acceptance count");
        accepted+=sent.accepted;Backpressure(sent.error,accepted<wanted,deadline);
    }
}

void SendKeyboard(std::vector<Keyboard_Input>& output) {
    const auto deadline=GetTickCount64()+2000;
    while(!output.empty() && !Stopping()) {
        const auto sent=Send_Input(output); // Family, device IDs and count are inferred.
        if(sent.accepted>output.size())throw std::runtime_error("Invalid keyboard acceptance count");
        // Retry only the unsent suffix. A complete send needs no shifting.
        if(sent.accepted==output.size())output.clear();
        else if(sent.accepted)output.erase(output.begin(),output.begin()+sent.accepted);
        Backpressure(sent.error,!output.empty(),deadline);
    }
}
}

int wmain(int argc,wchar_t** argv) {
    if(argc==2 && (std::wstring(argv[1])==L"--help" || std::wstring(argv[1])==L"/?")) {
        std::cout<<"Ctrl+H types hello once per press, preserving Ctrl/Shift and Caps Lock.\n"
            <<"Alt/Win combinations pass unchanged. Other input families are forwarded.\n"
            <<"Uses common keyboard scan-code layouts, not universal text/IME injection.\n"
            <<"Install the driver, run as administrator, release all keys, then focus a text editor.\n"
            <<"Ctrl+C or closing the console stops capture; physical Ctrl+Alt+Delete also revokes it.\n";
        return 0;
    }
    if(argc!=1){std::cerr<<"Usage: 02_Ctrl_H_Hello [--help]\n";return 2;}
    try {
        if(!CheckInterceptorInstallation())return ERROR_SERVICE_DOES_NOT_EXIST;
        ConsoleLifetime console;
        const auto init=Init_AllDevices();
        if(init.error)throw std::system_error(init.error,std::system_category(),"Init_AllDevices");
        if(init.reboot_required){std::cerr<<"Restart Windows to complete device initialization.\n";return ERROR_SUCCESS_REBOOT_REQUIRED;}
        if(!init.ready){std::cerr<<"Device initialization is incomplete.\n";return ERROR_NOT_READY;}
        std::cout<<"Ctrl+H types hello. Release all keys/buttons; capture starts in two seconds.\n"
            <<"Then focus the target text editor. Ctrl+C or close stops capture.\n"<<std::flush;
        const auto countdown=WaitForSingleObject(stopEvent,2000);
        if(countdown==WAIT_OBJECT_0)return 0;
        if(countdown==WAIT_FAILED)throw std::system_error(GetLastError(),std::system_category(),"Startup countdown");
        {
            CaptureLifetime capture;
            std::vector<Keyboard_Input> output;output.reserve(512);
            (void)Get_Devices(); // Open the automatic session before sampling Caps Lock.
            HelloShortcut shortcut((GetKeyState(VK_CAPITAL)&1)!=0);
            while(!Stopping()) {
                auto inputs=Get_Input_With_Timeout(INFINITE,stopEvent);
                output.clear();std::size_t untouched=0;
                for(auto& event:inputs) {
                    if(auto* key=event.Get<Keyboard_Input>()) {
                        event.blocked=true;shortcut.Process(*key,output);
                    }else ++untouched;
                }
                ForwardOthers(inputs,untouched);
                SendKeyboard(output);
            }
        }
        std::cout<<"Capture closed; normal input restored.\n";return 0;
    }catch(const std::system_error& error) {
        if(error.code().value()==ERROR_OPERATION_ABORTED){std::cout<<"Capture revoked; normal input restored.\n";return 0;}
        std::cerr<<"Error: "<<error.what()<<'\n';return 1;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
