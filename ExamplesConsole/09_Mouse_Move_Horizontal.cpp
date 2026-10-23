#include "Interceptor.h"
#include <algorithm>
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace {
using namespace Interceptor;

void Check(BOOL result,const char* operation) {
    if(!result)throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),operation);
}

bool ParseDelta(std::string_view text,std::int32_t& delta) {
    const auto first=text.find_first_not_of(" \t");
    if(first==std::string_view::npos)return false;
    text=text.substr(first,text.find_last_not_of(" \t")-first+1);
    if(text.front()=='+') {
        text.remove_prefix(1);
        if(text.empty() || text.front()<'0' || text.front()>'9')return false;
    }
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),delta);
    return parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size();
}

// Relative mouse input: no cursor position, screen coordinates or conversion.
Mouse_Input MakeMovement(std::uint32_t device,std::int32_t delta) {
    Mouse_Input input{};
    input.device=device;
    auto& state=input.stroke;
    state.x=delta; // flags=0 means relative movement; Y and buttons stay zero.
    return input;
}

template<class Sender> void SendAll(Sender send,std::size_t count) {
    const auto deadline=GetTickCount64()+2000;
    while(count) {
        const auto result=send();
        if(result.accepted>count)throw std::runtime_error("Invalid Send_Input acceptance count.");
        count-=result.accepted;
        if(result.error && result.error!=ERROR_BUSY)
            throw std::system_error(static_cast<int>(result.error),std::system_category(),"Send_Input");
        if(!count)return;
        if(GetTickCount64()>=deadline)
            throw std::runtime_error("Windows input queue stayed full for two seconds; releasing capture.");
        Sleep(1); // Only on queue backpressure, never in the normal input path.
    }
}

void MoveHorizontal(std::uint32_t device,std::int32_t delta) {
    if(!delta)return;
    const auto input=MakeMovement(device,delta);
    SendAll([&]{return Send_Input(input);},1);
    std::cout<<"Sent relative mouse X = "<<delta<<".\n";
}

BOOL WINAPI ConsoleControl(DWORD signal) {
    if(signal!=CTRL_C_EVENT && signal!=CTRL_BREAK_EVENT)return FALSE;
    // Wake the same loop as Esc so normal scope cleanup restores console mode
    // and releases capture before exiting. No SDK calls on the handler thread.
    INPUT_RECORD record{};
    record.EventType=KEY_EVENT;
    record.Event.KeyEvent.bKeyDown=TRUE;
    record.Event.KeyEvent.wRepeatCount=1;
    record.Event.KeyEvent.wVirtualKeyCode=VK_ESCAPE;
    DWORD written=0;
    return WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE),&record,1,&written) && written==1;
}

class ConsoleMode {
    DWORD previous_=0;
public:
    const HANDLE input=GetStdHandle(STD_INPUT_HANDLE);
    ConsoleMode() {
        Check(GetConsoleMode(input,&previous_),"Open an interactive Windows console (stdin cannot be redirected)");
        // Read key records ourselves: a blocking getline would stop forwarding
        // the captured keyboard, including the keys needed to enter a number.
        Check(SetConsoleMode(input,(previous_|ENABLE_EXTENDED_FLAGS|ENABLE_PROCESSED_INPUT)&
            ~(ENABLE_QUICK_EDIT_MODE|ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT|ENABLE_MOUSE_INPUT|ENABLE_WINDOW_INPUT)),"SetConsoleMode");
        if(!SetConsoleCtrlHandler(ConsoleControl,TRUE)) {
            const auto error=GetLastError();
            SetConsoleMode(input,previous_);
            throw std::system_error(static_cast<int>(error),std::system_category(),"SetConsoleCtrlHandler");
        }
    }
    ~ConsoleMode(){SetConsoleMode(input,previous_);SetConsoleCtrlHandler(ConsoleControl,FALSE);}
    ConsoleMode(const ConsoleMode&)=delete;
    ConsoleMode& operator=(const ConsoleMode&)=delete;
};

struct CaptureLifetime {
    ~CaptureLifetime(){try{Destroy_Context();}catch(...){/* Process exit also closes driver handles. */}}
};

void CheckInstallation() {
    const auto manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    Check(manager!=nullptr,"Check Interceptor installation");
    const auto service=OpenServiceW(manager,IC_SDK_SERVICE,SERVICE_QUERY_STATUS);
    const auto error=service?ERROR_SUCCESS:GetLastError();
    if(service)CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if(error)throw std::system_error(static_cast<int>(error),std::system_category(),
        "This edition's driver is unavailable. Run InterceptorSetup.exe or InterceptorSetupConsole.exe /install");
}
}

int wmain(int argc,wchar_t** argv) try {
    if(argc==2 && (std::wstring_view(argv[1])==L"--help" || std::wstring_view(argv[1])==L"/?")) {
        std::cout<<"09_Mouse_Move_Horizontal\n"
            <<"Install this edition's driver, then run this console example as administrator.\n"
            <<"Enter a signed number and press Enter: 300 = right, -485 = left, 0 = no movement.\n"
            <<"Uses Interceptor Send_Input with relative mouse X counts; Y is zero.\n"
            <<"Raw counts are not screen pixels; Windows or the game applies its sensitivity.\n"
            <<"Physical input is forwarded. Esc, q + Enter or Ctrl+C exits.\n";
        return 0;
    }
    if(argc!=1){std::cerr<<"Usage: 09_Mouse_Move_Horizontal [--help]\n";return 2;}
    CheckInstallation();
    const auto init=Init_AllDevices();
    if(init.error)throw std::system_error(static_cast<int>(init.error),std::system_category(),"Init_AllDevices");
    if(init.reboot_required){std::cerr<<"Restart Windows to complete installation.\n";return ERROR_SUCCESS_REBOOT_REQUIRED;}
    if(!init.ready){std::cerr<<"Device initialization is incomplete.\n";return ERROR_NOT_READY;}
    ConsoleMode console;
    CaptureLifetime capture;
    const auto devices=Get_Devices(); // Opens the automatic capture once.
    const auto mouse=std::find_if(devices.begin(),devices.end(),[](const auto& d){return d.type==static_cast<std::uint32_t>(Device_Type::Mouse);});
    if(mouse==devices.end())throw std::runtime_error("No captured mouse is available. Connect/associate a mouse, then restart this example.");
    std::cout<<"Horizontal mouse movement via Interceptor (mouse ID "<<mouse->id<<").\n"
        <<"Enter relative X counts: 300 = right, -485 = left. Esc or q + Enter exits.\n"
        <<"Physical inputs stay active. Close other capture programs first.\nX > "<<std::flush;
    std::string line;
    bool tooLong=false;
    for(;;) {
        DWORD available=0;
        Check(GetNumberOfConsoleInputEvents(console.input,&available),"Read console event count");
        if(available) {
            INPUT_RECORD records[32]{};DWORD read=0;
            Check(ReadConsoleInputW(console.input,records,std::min<DWORD>(available,32),&read),"ReadConsoleInput");
            for(DWORD i=0;i<read;++i) {
                if(records[i].EventType!=KEY_EVENT || !records[i].Event.KeyEvent.bKeyDown)continue;
                const auto& key=records[i].Event.KeyEvent;
                if(key.wVirtualKeyCode==VK_ESCAPE){std::cout<<"\nClosed; normal input restored.\n";return 0;}
                for(WORD repeat=0;repeat<key.wRepeatCount;++repeat) {
                    const auto ch=key.uChar.UnicodeChar;
                    if(ch==L'\r') {
                        std::cout<<'\n';
                        if(line=="q" || line=="Q")return 0;
                        std::int32_t delta=0;
                        if(tooLong || !ParseDelta(line,delta))
                            std::cout<<"Enter a whole number from -2147483648 to 2147483647.\n";
                        else MoveHorizontal(mouse->id,delta);
                        line.clear();tooLong=false;std::cout<<"X > "<<std::flush;
                    }else if(ch==L'\b') {
                        if(!line.empty()){line.pop_back();std::cout<<"\b \b"<<std::flush;}
                        if(line.empty())tooLong=false;
                    }else if(ch>=L' ' && ch<=L'~') {
                        if(line.size()<64){line.push_back(static_cast<char>(ch));std::cout<<static_cast<char>(ch)<<std::flush;}
                        else tooLong=true;
                    }
                }
            }
        }
        Check(GetNumberOfConsoleInputEvents(console.input,&available),"Read console event count");
        // A console input handle is waitable. Wake for either a new driver batch
        // or console keys; idle uses no polling thread, sleep or timer.
        // Drain driver input even during a large paste into the console.
        auto inputs=Get_Input_With_Timeout(available?0:INFINITE,available?nullptr:console.input);
        SendAll([&]{return Send_Input(inputs);},inputs.size());
    }
}catch(const std::system_error& error) {
    if(error.code().value()==ERROR_OPERATION_ABORTED){std::cout<<"Capture revoked; normal input restored.\n";return 0;}
    std::cerr<<"Error: "<<error.what()<<"\nRun as administrator with this edition's driver installed.\n";return 1;
}catch(const std::exception& error){std::cerr<<"Error: "<<error.what()<<'\n';return 1;}
