#include "Interceptor.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>

// Compile this file with Interceptor.cpp. No other example source is required.
namespace {
using namespace Interceptor;

constexpr std::uint16_t H=0x23,J=0x24,K=0x25;
constexpr std::uint16_t Side1Down=0x40,Side1Up=0x80,Side2Down=0x100,Side2Up=0x200;
constexpr std::uint16_t SideFlags=Side1Down|Side1Up|Side2Down|Side2Up;
std::atomic<bool> stop{false};
HANDLE stopRequested=nullptr,stopped=nullptr;
SRWLOCK exitLock=SRWLOCK_INIT;

BOOL WINAPI ConsoleControl(DWORD type) {
    if(type!=CTRL_C_EVENT && type!=CTRL_BREAK_EVENT && type!=CTRL_CLOSE_EVENT &&
       type!=CTRL_LOGOFF_EVENT && type!=CTRL_SHUTDOWN_EVENT)return FALSE;
    stop=true;
    AcquireSRWLockShared(&exitLock);
    if(stopRequested)SetEvent(stopRequested);
    if(type==CTRL_CLOSE_EVENT && stopped)WaitForSingleObject(stopped,2500);
    ReleaseSRWLockShared(&exitLock);
    return TRUE;
}

class ConsoleLifetime {
    DWORD oldMode=0;
    bool changed=false;
public:
    ConsoleLifetime() {
        stopped=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!stopped)throw std::system_error(GetLastError(),std::system_category(),"Create completion event");
        stopRequested=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!stopRequested) {
            const auto error=GetLastError();CloseHandle(stopped);stopped=nullptr;
            throw std::system_error(error,std::system_category(),"Create stop event");
        }
        if(!SetConsoleCtrlHandler(ConsoleControl,TRUE)) {
            const auto error=GetLastError();CloseHandle(stopRequested);CloseHandle(stopped);
            stopRequested=stopped=nullptr;
            throw std::system_error(error,std::system_category(),"Register console handler");
        }
        if(GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&oldMode))
            changed=SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),(oldMode|ENABLE_EXTENDED_FLAGS)&~ENABLE_QUICK_EDIT_MODE)!=FALSE;
    }
    ConsoleLifetime(const ConsoleLifetime&)=delete;
    ConsoleLifetime& operator=(const ConsoleLifetime&)=delete;
    ~ConsoleLifetime() {
        if(changed)SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),oldMode);
        // Session cleanup has already run. Wake a close handler before taking
        // its exclusive lock, then close handles only after all handlers exit.
        SetEvent(stopped);
        SetConsoleCtrlHandler(ConsoleControl,FALSE);
        AcquireSRWLockExclusive(&exitLock);
        const HANDLE done=stopped,request=stopRequested;
        stopped=stopRequested=nullptr;
        ReleaseSRWLockExclusive(&exitLock);
        CloseHandle(done);CloseHandle(request);
    }
};

struct SessionCleanup {
    ~SessionCleanup() noexcept {
        // Public optional release; no Context handle or internal API is used.
        // This guard outlives each borrowed batch, including on exceptions.
        try { Destroy_Context(); } catch(...) {}
    }
};

bool WaitForStop(DWORD milliseconds) {
    const auto result=WaitForSingleObject(stopRequested,milliseconds);
    if(result==WAIT_FAILED)throw std::system_error(GetLastError(),std::system_category(),"Wait for stop");
    return result==WAIT_OBJECT_0 || stop.load();
}

struct Emission {
    std::uint8_t kind;
    union { Keyboard_Input key;Mouse_Input mouse; };
    explicit Emission(const Keyboard_Input& input):kind(Keyboard),key(input){}
    explicit Emission(const Mouse_Input& input):kind(Mouse),mouse(input){}
};

bool Plain(const Keyboard_Input& input,std::uint16_t scan) {
    return input.stroke.make_code==scan && !(input.stroke.flags&(KeyE0|KeyE1));
}

std::int32_t InvertX(std::int32_t value,bool absolute) {
    if(absolute)return 65535-std::clamp(value,std::int32_t{0},std::int32_t{65535});
    return value==std::numeric_limits<std::int32_t>::min()
        ?std::numeric_limits<std::int32_t>::max():-value;
}

class Remapper {
    // Each physical keyboard/button owns its hold independently. Releasing
    // one owner cannot release H or K while another owner still needs it.
    std::set<std::uint32_t> heldH,heldJ,heldK;
    std::set<std::uint64_t> sideTriggers;
    bool outputH=false,outputK=false;

    static void EmitKey(std::vector<Emission>& out,const Keyboard_Input& source,std::uint16_t scan,bool up) {
        auto key=source;
        key.stroke.make_code=scan;key.stroke.flags=up?KeyUp:0;
        // Aggregated H/K always use the same default keyboard routing. All
        // other keyboard metadata is retained; original forwarded packets keep
        // their device IDs. A mouse source has only sequence/time metadata.
        key.device=0;key.stroke.unit_id=0;
        out.emplace_back(key);
    }
    void UpdateHK(const Keyboard_Input& source,std::vector<Emission>& out) {
        const bool trigger=!heldJ.empty() || !sideTriggers.empty();
        const bool wantH=trigger || !heldH.empty(),wantK=trigger || !heldK.empty();
        if(wantH!=outputH)EmitKey(out,source,H,!wantH);
        if(wantK!=outputK)EmitKey(out,source,K,!wantK);
        outputH=wantH;outputK=wantK;
    }
public:
    void KeyboardInput(const Keyboard_Input& input,std::vector<Emission>& out) {
        std::set<std::uint32_t>* owners=nullptr;
        if(Plain(input,H))owners=&heldH;
        else if(Plain(input,J))owners=&heldJ;
        else if(Plain(input,K))owners=&heldK;
        if(!owners) { out.emplace_back(input);return; }

        const bool up=(input.stroke.flags&KeyUp)!=0;
        const bool repeated=owners->count(input.device)!=0;
        if(up)owners->erase(input.device);else owners->insert(input.device);
        UpdateHK(input,out);
        // Preserve typematic repeats: repeated J makes both H and K repeat;
        // a physical H or K repeats only its own logical key.
        if(repeated && !up) {
            if(Plain(input,H)||Plain(input,J))EmitKey(out,input,H,false);
            if(Plain(input,K)||Plain(input,J))EmitKey(out,input,K,false);
        }
        // H/K are represented by the balanced aggregate; J itself is blocked.
    }
    void MouseInput(Mouse_Input input,std::vector<Emission>& out) {
        input.stroke.x=InvertX(input.stroke.x,(input.stroke.flags&MouseAbsolute)!=0);
        const auto flags=input.stroke.button_flags;
        for(unsigned button=0;button<2;++button) {
            const auto token=(static_cast<std::uint64_t>(input.device)<<8)|button;
            if(flags&(Side1Down<<(button*2)))sideTriggers.insert(token);
            if(flags&(Side1Up<<(button*2)))sideTriggers.erase(token);
        }
        Keyboard_Input source{};source.sequence=input.sequence;source.timestamp=input.timestamp;
        UpdateHK(source,out);
        input.stroke.button_flags&=static_cast<std::uint16_t>(~SideFlags);
        // Omit an otherwise empty relative packet, but retain wheel data,
        // attributes, raw buttons and extra information if they are present.
        if(!input.stroke.flags && !input.stroke.button_flags && !input.stroke.button_data &&
           !input.stroke.raw_buttons && !input.stroke.extra_information && !input.stroke.x && !input.stroke.y)return;
        out.emplace_back(input);
    }
};

void CheckSendError(DWORD error) {
    if(error && error!=ERROR_BUSY)
        throw std::system_error(static_cast<int>(error),std::system_category(),"Send_Input");
}

bool RetryPause(ULONGLONG deadline) {
    if(stop)return false;
    if(GetTickCount64()>=deadline)
        throw std::runtime_error("Input queue remained full for 2 seconds; capture is being released.");
    // Only backpressure uses this bounded delay. Idle capture waits on events.
    return !WaitForStop(1);
}

void ForwardOtherFamilies(Input_Batch& inputs) {
    std::size_t remaining=0;
    for(const auto& event:inputs)if(!event.blocked)++remaining;
    const auto deadline=GetTickCount64()+2000;
    while(remaining && !stop) {
        const auto result=Send_Input(inputs);
        if(result.accepted>remaining)throw std::runtime_error("Invalid accepted input count");
        remaining-=result.accepted;
        CheckSendError(result.error);
        if(remaining && !result.error)throw std::runtime_error("Incomplete successful batch send");
        // The public batch tracks accepted entries itself. Retrying it never
        // resends an accepted controller report or a blocked keyboard/mouse.
        if(remaining && !RetryPause(deadline))return;
    }
}

void SendOutputs(const std::vector<Emission>& out,std::vector<Keyboard_Input>& keys,std::vector<Mouse_Input>& mice) {
    std::size_t index=0;
    while(index<out.size() && !stop) {
        keys.clear();mice.clear();
        const auto kind=out[index].kind;
        std::size_t count=0;
        // Batch only adjacent emissions from the same family, so a generated
        // key remains on the correct side of the associated mouse movement.
        while(index+count<out.size() && count<256 && out[index+count].kind==kind) {
            if(kind==Keyboard)keys.push_back(out[index+count].key);
            else mice.push_back(out[index+count].mouse);
            ++count;
        }
        std::size_t remaining=count;
        const auto deadline=GetTickCount64()+2000;
        while(remaining && !stop) {
            const auto result=kind==Keyboard?Send_Input(keys):Send_Input(mice);
            if(result.accepted>remaining)throw std::runtime_error("Invalid accepted output count");
            remaining-=result.accepted;
            CheckSendError(result.error);
            if(remaining) {
                // Typed arrays do not track acceptance. Keep only the actual
                // unaccepted suffix; the common complete send needs no erase.
                if(result.accepted) {
                    if(kind==Keyboard)keys.erase(keys.begin(),keys.begin()+result.accepted);
                    else mice.erase(mice.begin(),mice.begin()+result.accepted);
                }
                if(!RetryPause(deadline))return;
            }
        }
        index+=count;
    }
}

int Run() {
    std::cout<<"Initializing Interceptor devices...\n"<<std::flush;
    const auto init=Init_AllDevices();
    if(!init.ready) {
        if(init.error)throw std::system_error(static_cast<int>(init.error),std::system_category(),"Init_AllDevices");
        if(init.reboot_required) {
            std::cout<<"Restart Windows to use the newly associated devices, then run this example again.\n";
            return ERROR_SUCCESS_REBOOT_REQUIRED;
        }
        throw std::system_error(ERROR_NOT_READY,std::system_category(),"Interceptor devices are not ready");
    }
    std::cout<<"Invert mouse X; J or X1/X2 holds H+K. J and side-button actions are blocked.\n"
        <<"Physical H/K and overlapping triggers remain balanced; key repeats are preserved.\n"
        <<"Run one capture example at a time as administrator.\n"
        <<"Recovery: PHYSICAL Ctrl+Alt+Delete, Ctrl+C in this console, or close the window.\n"
        <<"Release all keys/buttons. Starting in 2 seconds; then focus the target application.\n"<<std::flush;
    if(WaitForStop(2000))return 0;

    SessionCleanup cleanup;
    for(const auto& device:Get_Devices())
        std::cout<<"Device type="<<device.type<<" id="<<device.id<<" VID="<<std::hex<<device.vid<<" PID="<<device.pid<<std::dec<<'\n';
    Remapper remapper;
    std::vector<Input_Event*> ordered;ordered.reserve(256);
    std::vector<Emission> out;out.reserve(512);
    std::vector<Keyboard_Input> keys;keys.reserve(256);
    std::vector<Mouse_Input> mice;mice.reserve(256);
    std::cout<<"Capture is active.\n"<<std::flush;
    while(!stop) {
        auto inputs=Get_Input_With_Timeout(INFINITE,stopRequested);
        if(stop)break;
        if(inputs.empty())continue;
        ordered.clear();out.clear();
        for(auto& event:inputs) {
            if(event.type==Device_Type::Keyboard || event.type==Device_Type::Mouse) {
                event.blocked=true;ordered.push_back(&event);
            }
        }
        // Only keyboard/mouse share this sequence counter. Sort descriptors,
        // not large controller reports; untouched families retain their data.
        std::sort(ordered.begin(),ordered.end(),[](const auto* a,const auto* b){return a->sequence<b->sequence;});
        for(auto* event:ordered) {
            if(const auto* key=event->Get<Keyboard_Input>())remapper.KeyboardInput(*key,out);
            else if(const auto* mouse=event->Get<Mouse_Input>())remapper.MouseInput(*mouse,out);
            else throw std::runtime_error("Keyboard/mouse event has an unexpected payload");
        }
        ForwardOtherFamilies(inputs);
        SendOutputs(out,keys,mice);
    }
    return 0;
}
}

int wmain(int argc,wchar_t** argv) {
    // --help performs no initialization, capture, installation or driver I/O.
    if(argc==2 && std::wstring(argv[1])==L"--help") {
        std::cout<<"04_Inverse_X_J_Boutons_HK\n"
            <<"Run without arguments: invert mouse X; hold H+K while J or X1/X2 is down.\n"
            <<"Absolute X is inverted in 0..65535; relative X is negated with saturation.\n"
            <<"Requires an installed Interceptor driver and an administrator console.\n"
            <<"Physical Ctrl+Alt+Delete or closing the application restores normal input.\n";
        return 0;
    }
    if(argc!=1) { std::cerr<<"Use --help or run without arguments.\n";return 1; }
    try {
        ConsoleLifetime console;
        const int result=Run();
        if(!result)std::cout<<"Capture closed; normal input restored.\n";
        return result;
    } catch(const std::system_error& error) {
        if(error.code().value()==ERROR_OPERATION_ABORTED) {
            std::cout<<"Capture cancelled; normal input restored.\n";return 0;
        }
        std::cerr<<"Error: "<<error.what()<<"\nInstall Interceptor and run this EXE as administrator.\n";
    } catch(const std::exception& error) {
        std::cerr<<"Error: "<<error.what()<<"\nCapture has been released.\n";
    }
    return 1;
}
