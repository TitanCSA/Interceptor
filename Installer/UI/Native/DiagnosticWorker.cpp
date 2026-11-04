#include "Interceptor.h"
#pragma execution_character_set("utf-8")
// Finite, explicit diagnostic worker. No installation or association. Closing
// this process also closes its non-inherited capture handle in the kernel.
#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <type_traits>
#include <system_error>
#include <vector>
#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"cfgmgr32.lib")
#pragma comment(lib,"hid.lib")
using namespace Interceptor;
namespace {
std::string Utf8(const std::wstring& s){if(s.empty())return {};int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);if(!n)throw std::runtime_error("UTF-16 conversion");std::string r(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),r.data(),n,nullptr,nullptr);return r;}
std::string Escape(const std::string& s){std::string r;for(unsigned char c:s){if(c=='"'||c=='\\'){r+='\\';r+=c;}else if(c=='\n')r+="\\n";else if(c=='\r')r+="\\r";else if(c=='\t')r+="\\t";else if(c>=32)r+=c;}return r;}
void Emit(const char* check,const char* status,const std::string& message,unsigned progress){std::printf("{\"check\":\"%s\",\"status\":\"%s\",\"message\":\"%s\",\"progress\":%u}\n",check,status,Escape(message).c_str(),progress);}
std::string Hex(DWORD n){char b[16]{};sprintf_s(b,"0x%08lX",n);return b;}
std::string ErrorText(const std::system_error& error){
    wchar_t message[1024]{};const auto code=static_cast<DWORD>(error.code().value());
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,code,0,message,_countof(message),nullptr);
    std::string operation=error.what();auto separator=operation.find(": ");if(separator!=operation.npos)operation.resize(separator);
    return operation+" ; "+Utf8(message);
}
struct Handle{HANDLE value=INVALID_HANDLE_VALUE;explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;};
std::wstring RawInstance(std::wstring path){if(path.rfind(L"\\\\?\\",0)==0)path.erase(0,4);auto end=path.rfind(L"#{");if(end!=path.npos)path.resize(end);std::replace(path.begin(),path.end(),L'#',L'\\');return path;}
bool SameInstanceOrChild(const std::wstring& raw,const std::wstring& wanted){
    if(!_wcsicmp(raw.c_str(),wanted.c_str()))return true;
    DEVINST node=0;auto id=raw;
    if(CM_Locate_DevNodeW(&node,id.data(),CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS)return false;
    for(unsigned depth=0;depth<12;++depth){DEVINST parent=0;if(CM_Get_Parent(&parent,node,0)!=CR_SUCCESS)return false;wchar_t b[MAX_DEVICE_ID_LEN]{};if(CM_Get_Device_IDW(parent,b,_countof(b),0)!=CR_SUCCESS)return false;if(!_wcsicmp(b,wanted.c_str()))return true;node=parent;}return false;
}
std::vector<std::wstring> LegacyCandidates(const Device_Info& device){
    // Old ABI4 builds lack per-token instance IDs. Never choose among identical
    // products: exact first hardware ID + device class must identify ONE devnode.
    static const GUID classes[]={
        {0x4D36E96B,0xE325,0x11CE,{0xBF,0xC1,0x08,0x00,0x2B,0xE1,0x03,0x18}},
        {0x4D36E96F,0xE325,0x11CE,{0xBF,0xC1,0x08,0x00,0x2B,0xE1,0x03,0x18}},
        {0x745A17A0,0x74D3,0x11D0,{0xB6,0xFE,0x00,0xA0,0xC9,0x0F,0x57,0xDA}},
        {0xD61CA365,0x5AF4,0x4486,{0x99,0x8B,0x9D,0xB4,0x73,0x4C,0x6C,0xA3}},
        {0x745A17A0,0x74D3,0x11D0,{0xB6,0xFE,0x00,0xA0,0xC9,0x0F,0x57,0xDA}},
        {0x05F5CFE2,0x4733,0x4950,{0xA6,0xBB,0x07,0xAA,0xD0,0x1A,0x3A,0x84}}};
    std::vector<std::wstring> result;if(!device.hardware_id[0]||device.type<1||device.type>6)return result;
    const auto set=SetupDiGetClassDevsW(&classes[device.type-1],nullptr,nullptr,DIGCF_PRESENT);if(set==INVALID_HANDLE_VALUE)return result;
    struct Close{HDEVINFO set;~Close(){SetupDiDestroyDeviceInfoList(set);}} close{set};
    for(DWORD i=0;;++i){SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);if(!SetupDiEnumDeviceInfo(set,i,&d))break;wchar_t hardware[4096]{},id[MAX_DEVICE_ID_LEN]{};DWORD type=0;
        if(!SetupDiGetDeviceRegistryPropertyW(set,&d,SPDRP_HARDWAREID,&type,reinterpret_cast<BYTE*>(hardware),sizeof(hardware)-4,nullptr)||type!=REG_MULTI_SZ||_wcsicmp(hardware,reinterpret_cast<const wchar_t*>(device.hardware_id)))continue;
        if(SetupDiGetDeviceInstanceIdW(set,&d,id,_countof(id),nullptr))result.emplace_back(id);
    }return result;
}
struct Observed{std::wstring instance;DWORD type=0;USHORT key=0,keyFlags=0;LONG x=0;bool nativeXValid=false;short nativeX=0;std::vector<BYTE> report;};
template<class Next,class Dispatch> bool PumpBounded(Next next,Dispatch dispatch){
    for(unsigned n=0;n<256;++n){if(!next(true))return true;dispatch();}
    return !next(false);
}
template<class Pump,class Events> bool PrepareProbe(Pump pump,Events& events){
    const bool drained=pump();events.clear();return drained;
}
class Observer {
    HWND window_=nullptr;
    std::map<HANDLE,std::wstring> names_;
    struct Descriptor {std::vector<BYTE> data;std::vector<HIDP_VALUE_CAPS> values;};
    std::map<HANDLE,Descriptor> descriptors_;
    std::vector<BYTE> rawBuffer_;
    std::deque<Observed> events_;
    static LRESULT CALLBACK Proc(HWND w,UINT message,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<Observer*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(message==WM_NCCREATE){self=static_cast<Observer*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(self&&message==WM_INPUT){try{self->Read(reinterpret_cast<HRAWINPUT>(lp));}catch(...){self->error=ERROR_INVALID_DATA;}}return DefWindowProcW(w,message,wp,lp);}
    void Read(HRAWINPUT input){
        UINT n=0;if(GetRawInputData(input,RID_INPUT,nullptr,&n,sizeof(RAWINPUTHEADER))==UINT(-1)||n<sizeof(RAWINPUTHEADER)||n>65536){error=GetLastError();return;}
        rawBuffer_.resize(n);const auto got=GetRawInputData(input,RID_INPUT,rawBuffer_.data(),&n,sizeof(RAWINPUTHEADER));
        if(got==UINT(-1)){error=GetLastError();return;}
        if(got<sizeof(RAWINPUTHEADER)||got>rawBuffer_.size()){error=ERROR_INVALID_DATA;return;}n=got;
        const auto* raw=reinterpret_cast<const RAWINPUT*>(rawBuffer_.data());auto handle=raw->header.hDevice;
        auto name=names_.find(handle);if(name==names_.end()){
            UINT chars=0;if(GetRawInputDeviceInfoW(handle,RIDI_DEVICENAME,nullptr,&chars)==UINT(-1)||!chars||chars>4096)return;
            std::vector<wchar_t> path(chars+1);if(GetRawInputDeviceInfoW(handle,RIDI_DEVICENAME,path.data(),&chars)==UINT(-1))return;
            name=names_.emplace(handle,RawInstance(path.data())).first;
        }
        Observed e;e.instance=name->second;e.type=raw->header.dwType;
        if(e.type==RIM_TYPEKEYBOARD && n>=sizeof(RAWINPUTHEADER)+sizeof(RAWKEYBOARD)){e.key=raw->data.keyboard.MakeCode;e.keyFlags=raw->data.keyboard.Flags;}
        else if(e.type==RIM_TYPEMOUSE && n>=sizeof(RAWINPUTHEADER)+sizeof(RAWMOUSE))e.x=raw->data.mouse.lLastX;
        else if(e.type==RIM_TYPEHID && n>=sizeof(RAWINPUTHEADER)+8){
            const auto& hid=raw->data.hid;const SIZE_T available=n-sizeof(RAWINPUTHEADER)-8;
            if(!hid.dwSizeHid||hid.dwSizeHid>IC_HID_REPORT_MAX||!hid.dwCount||hid.dwCount>available/hid.dwSizeHid)return;
            auto descriptor=descriptors_.find(handle);
            if(descriptor==descriptors_.end()){
                Descriptor cached;UINT bytes=0;
                if(GetRawInputDeviceInfoW(handle,RIDI_PREPARSEDDATA,nullptr,&bytes)!=UINT(-1)&&bytes&&bytes<=65536){
                    cached.data.resize(bytes);
                    if(GetRawInputDeviceInfoW(handle,RIDI_PREPARSEDDATA,cached.data.data(),&bytes)!=UINT(-1)&&bytes&&bytes<=cached.data.size()){
                        cached.data.resize(bytes);auto pp=reinterpret_cast<PHIDP_PREPARSED_DATA>(cached.data.data());HIDP_CAPS caps{};
                        if(HidP_GetCaps(pp,&caps)==HIDP_STATUS_SUCCESS&&caps.NumberInputValueCaps<=1024){
                            USHORT count=caps.NumberInputValueCaps;cached.values.resize(count);
                            if(count&&HidP_GetValueCaps(HidP_Input,cached.values.data(),&count,pp)!=HIDP_STATUS_SUCCESS)cached.values.clear();
                            else cached.values.resize(count);
                        }
                    }else cached.data.clear();
                }
                // Cache failures as well: metadata queries never run per report.
                descriptor=descriptors_.emplace(handle,std::move(cached)).first;
            }
            for(DWORD i=0;i<hid.dwCount;++i){
                auto value=e;value.report.assign(hid.bRawData+static_cast<SIZE_T>(i)*hid.dwSizeHid,hid.bRawData+static_cast<SIZE_T>(i+1)*hid.dwSizeHid);
                if(!descriptor->second.values.empty()){
                    auto pp=reinterpret_cast<PHIDP_PREPARSED_DATA>(descriptor->second.data.data());
                    for(const auto& v:descriptor->second.values){if(v.UsagePage!=1||!v.IsAbsolute||!v.BitSize||v.BitSize>32||v.LogicalMax<=v.LogicalMin||v.ReportID!=value.report[0])continue;
                            const auto lo=v.IsRange?v.Range.UsageMin:v.NotRange.Usage,hi=v.IsRange?v.Range.UsageMax:v.NotRange.Usage;if(lo>0x30||hi<0x30)continue;
                            ULONG rawValue=0;if(HidP_GetUsageValue(HidP_Input,1,v.LinkCollection,0x30,&rawValue,pp,reinterpret_cast<PCHAR>(value.report.data()),static_cast<ULONG>(value.report.size()))!=HIDP_STATUS_SUCCESS)continue;
                            const auto bits=v.BitSize;long long signedValue=rawValue;if(bits<32){rawValue&=(1u<<bits)-1;signedValue=rawValue;}
                            if(v.LogicalMin<0&&(rawValue&(1u<<(bits-1))))signedValue-=1LL<<bits;
                            if(signedValue<v.LogicalMin||signedValue>v.LogicalMax)continue;
                            value.nativeX=static_cast<short>((signedValue-v.LogicalMin)*65535LL/(static_cast<long long>(v.LogicalMax)-v.LogicalMin)-32768);value.nativeXValid=true;break;
                    }
                }
                Push(std::move(value));
            }
            return;
        }else return;
        Push(std::move(e));
    }
    void Push(Observed e){if(events_.size()==256){events_.pop_front();++dropped;}events_.push_back(std::move(e));}
public:
    DWORD error=0;unsigned dropped=0;
    Observer(){WNDCLASSW cls{};cls.lpfnWndProc=Proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"InterceptorDiagnosticObserver149";if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::system_error(GetLastError(),std::system_category(),"Register observer");window_=CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,this);if(!window_)throw std::system_error(GetLastError(),std::system_category(),"Create observer");
        std::vector<RAWINPUTDEVICE> regs;for(USHORT usage:std::array<USHORT,6>{2,6,4,5,8,15})regs.push_back({1,usage,RIDEV_INPUTSINK,window_});for(USHORT usage=1;usage<=12;++usage)regs.push_back({2,usage,RIDEV_INPUTSINK,window_});for(USHORT usage:std::array<USHORT,3>{32,33,36})regs.push_back({2,usage,RIDEV_INPUTSINK,window_});for(USHORT usage=1;usage<=4;++usage)regs.push_back({4,usage,RIDEV_INPUTSINK,window_});for(USHORT usage=1;usage<=3;++usage)regs.push_back({5,usage,RIDEV_INPUTSINK,window_});
        if(!RegisterRawInputDevices(regs.data(),static_cast<UINT>(regs.size()),sizeof(RAWINPUTDEVICE))){const auto code=GetLastError();DestroyWindow(window_);window_=nullptr;throw std::system_error(code,std::system_category(),"Register Raw Input observer");}
    }
    ~Observer(){if(window_)DestroyWindow(window_);}
    bool Pump(){MSG m{};return PumpBounded([&](bool remove){return PeekMessageW(&m,nullptr,0,0,remove?PM_REMOVE:PM_NOREMOVE)!=FALSE;},[&]{TranslateMessage(&m);DispatchMessageW(&m);});}
    bool BeginModification(){return PrepareProbe([&]{return Pump();},events_);}
    const std::deque<Observed>& Events()const{return events_;}
};
struct Evidence {
    DWORD type=0,id=0;std::wstring instance;unsigned physical=0,forwarded=0;bool transformed=false,observed=false;
    USHORT key=0;LONG x=0;short nativeX=0;std::vector<BYTE> report;ULONGLONG modifiedAt=0;
};
bool ModifyMouse(Mouse_Input& in,LONG& expected){
    // Probe motion only. Never duplicate a click, wheel event or physical delta.
    in.stroke.button_flags=0;in.stroke.button_data=0;in.stroke.raw_buttons=0;
    if(in.stroke.flags&MouseAbsolute){
        if(in.stroke.x<0||in.stroke.x>65535)return false;
        in.stroke.x=in.stroke.x==65535?65534:in.stroke.x+1;
    }else {in.stroke.x=1;in.stroke.y=0;}
    expected=in.stroke.x;return true;
}
bool Foreground(HWND w){return w&&IsWindow(w)&&GetAncestor(GetForegroundWindow(),GA_ROOT)==GetAncestor(w,GA_ROOT);}
void CheckSend(const Send_Result& r,size_t n){if(r.error||r.accepted!=n)throw std::system_error(r.error?r.error:ERROR_WRITE_FAULT,std::system_category(),"Send / suffix not retried");}
template<class Observation> void Observe(Observation& observer,Evidence& e){observer.Pump();if(!e.transformed||e.observed)return;for(const auto& v:observer.Events()){
    if(!SameInstanceOrChild(v.instance,e.instance))continue;
    if(e.type==1 && v.type==RIM_TYPEKEYBOARD&&v.key==e.key&&!(v.keyFlags&RI_KEY_BREAK))e.observed=true;
    else if(e.type==2&&v.type==RIM_TYPEMOUSE&&v.x==e.x)e.observed=true;
    else if(e.type==3&&v.type==RIM_TYPEHID&&v.report==e.report)e.observed=true;
    else if(e.type>=4&&v.type==RIM_TYPEHID&&v.nativeXValid&&v.nativeX==e.nativeX)e.observed=true;
}}
void TestObservationBoundary(){
    struct QueuedObserver {
        std::deque<Observed> queued,events;Observed current;
        bool Pump(){return PumpBounded([&](bool remove){if(queued.empty())return false;if(remove){current=std::move(queued.front());queued.pop_front();}return true;},[&]{if(events.size()==256)events.pop_front();events.push_back(std::move(current));});}
        bool BeginModification(){return PrepareProbe([&]{return Pump();},events);}
        const std::deque<Observed>& Events()const{return events;}
    };
    for(unsigned count:{257u,512u,513u,769u,1025u}){
        QueuedObserver observer;Observed matching;matching.instance=L"HID\\TEST\\target";matching.type=RIM_TYPEKEYBOARD;matching.key=0x76;
        observer.queued.assign(count,matching);Evidence evidence;evidence.type=1;evidence.instance=matching.instance;evidence.key=matching.key;
        // The first bounded flush must never establish a boundary while even
        // one old matching WM_INPUT remains queued. Forwarding can continue.
        bool clean=observer.BeginModification();
        if(clean||observer.queued.size()!=count-256||!observer.events.empty())throw std::runtime_error("Queued pre-probe input must defer the modification");
        unsigned attempts=1;
        while(!clean&&attempts++<8){Observe(observer,evidence);if(evidence.observed)throw std::runtime_error("Old input validated an unissued probe");clean=observer.BeginModification();}
        if(!clean||!observer.queued.empty()||!observer.events.empty())throw std::runtime_error("Probe boundary must drain and discard all older observations");
        evidence.transformed=true;Observe(observer,evidence);
        if(evidence.observed)throw std::runtime_error("Pre-probe matching input falsely validated the probe");
        observer.queued.push_back(matching);Observe(observer,evidence);
        if(!evidence.observed)throw std::runtime_error("A genuine post-probe observation must validate the probe");
    }
}
bool ModifyHid(Controller_Hid_Input& input,const std::vector<BYTE>& descriptor,bool xOnly=false,short* normalized=nullptr){
    auto pp=reinterpret_cast<PHIDP_PREPARSED_DATA>(const_cast<BYTE*>(descriptor.data()));HIDP_CAPS caps{};
    if(descriptor.empty()||HidP_GetCaps(pp,&caps)!=HIDP_STATUS_SUCCESS||caps.NumberInputValueCaps>1024)return false;
    USHORT n=caps.NumberInputValueCaps;std::vector<HIDP_VALUE_CAPS> values(n);
    if(!n||HidP_GetValueCaps(HidP_Input,values.data(),&n,pp)!=HIDP_STATUS_SUCCESS)return false;
    for(unsigned i=0;i<n;++i){const auto& v=values[i];if(v.ReportID!=input.report[0]||!v.IsAbsolute||v.IsAlias||!v.BitSize||v.BitSize>32||v.LogicalMax<=v.LogicalMin||v.BitField&1)continue;
        const auto lo=v.IsRange?v.Range.UsageMin:v.NotRange.Usage,hi=v.IsRange?v.Range.UsageMax:v.NotRange.Usage;
        if(xOnly&&(v.UsagePage!=1||lo>0x30||hi<0x30))continue;
        const auto usage=xOnly?static_cast<USAGE>(0x30):lo;ULONG original=0;
        if(HidP_GetUsageValue(HidP_Input,v.UsagePage,v.LinkCollection,usage,&original,pp,reinterpret_cast<PCHAR>(input.report),input.length)!=HIDP_STATUS_SUCCESS)continue;
        long long signedValue=original;if(v.BitSize<32){original&=(1u<<v.BitSize)-1;signedValue=original;}
        if(v.LogicalMin<0&&(original&(1u<<(v.BitSize-1))))signedValue-=1LL<<v.BitSize;
        if(signedValue<v.LogicalMin||signedValue>v.LogicalMax)continue;
        const auto changed=signedValue==v.LogicalMax?signedValue-1:signedValue+1;
        const ULONG value=static_cast<ULONG>(changed);
        if(HidP_SetUsageValue(HidP_Input,v.UsagePage,v.LinkCollection,usage,value,pp,reinterpret_cast<PCHAR>(input.report),input.length)==HIDP_STATUS_SUCCESS){if(normalized)*normalized=static_cast<short>((changed-v.LogicalMin)*65535LL/(static_cast<long long>(v.LogicalMax)-v.LogicalMin)-32768);return true;}
    }return false;
}
template<class T> const T& Source(const T& input){return input;}
const Controller_GIP_Input& Source(const Controller_GIP_Raw_Input& input){return input.input;}
template<class T,class Getter,class Sender,class Modify>
void Drain(std::vector<T>& buffer,Getter get,Sender send,Modify modify,Evidence& e,HWND owner,bool permit,bool (*foreground)(HWND)=Foreground){
    const auto n=get(buffer.data(),static_cast<uint32_t>(buffer.size()));
    uint32_t begin=0;bool modified=false;
    for(uint32_t i=0;i<n;++i){auto& in=buffer[i];if(Source(in).device!=e.id)continue;++e.physical;
        if(!e.transformed&&permit&&foreground(owner)){auto changed=in;if(modify(changed)){
            // Preserve probe insertion order while forwarding each contiguous
            // original run in one request, rather than one IOCTL per packet.
            if(i>begin){CheckSend(send(buffer.data()+begin,i-begin),i-begin);e.forwarded+=i-begin;}begin=i;
            CheckSend(send(&changed,1),1);
            if constexpr(std::is_same_v<T,Keyboard_Input>){changed.stroke.flags|=KeyUp;CheckSend(send(&changed,1),1);}
            if constexpr(std::is_same_v<T,Mouse_Input>){if(!(changed.stroke.flags&MouseAbsolute)){changed.stroke.x=-changed.stroke.x;CheckSend(send(&changed,1),1);}}
            e.transformed=true;e.modifiedAt=GetTickCount64();modified=true;
        }}
    }
    if(n>begin){CheckSend(send(buffer.data()+begin,n-begin),n-begin);e.forwarded+=n-begin;}
    if(modified&&owner)Emit("modification_send","info","Send accepted the modified report; waiting for the independent Windows consumer.",60);
}
void TestForwarding(){
    std::vector<Keyboard_Input> buffer(256);Evidence evidence;evidence.id=2;
    unsigned calls=0,originals=0,modifications=0;
    const auto get=[&](auto* packets,uint32_t capacity){
        for(uint32_t i=0;i<capacity;++i){packets[i].device=1+i%2;packets[i].sequence=i;}
        return capacity;
    };
    const auto send=[&](const auto* packets,size_t count)->Send_Result{
        ++calls;for(size_t i=0;i<count;++i){if(packets[i].sequence!=i)throw std::runtime_error("Diagnostic forwarding order");++originals;}
        return {count,0};
    };
    const auto modify=[&](auto&){++modifications;return false;};
    for(unsigned i=0;i<10000;++i)Drain(buffer,get,send,modify,evidence,nullptr,false);
    if(calls!=10000||originals!=2560000||evidence.physical!=1280000||evidence.forwarded!=2560000||modifications)
        throw std::runtime_error("Diagnostic batches must forward all originals with one request and no inactive probe");
    Drain(buffer,[](auto*,uint32_t){return 0u;},send,modify,evidence,nullptr,false);
    if(calls!=10000)throw std::runtime_error("Empty diagnostic batch must not send");
    bool rejected=false;
    try{Drain(buffer,get,[](const auto*,size_t n)->Send_Result{return {n-1,0};},modify,evidence,nullptr,false);}
    catch(const std::system_error& failure){rejected=failure.code().value()==ERROR_WRITE_FAULT;}
    if(!rejected)throw std::runtime_error("Partial diagnostic send must release capture instead of duplicating a prefix");
    // Independent expected output: prefix, balanced F24 probe, then every
    // original packet including the target, in exactly the captured order.
    Evidence probe;probe.id=2;std::vector<Keyboard_Input> emitted;unsigned probeCalls=0;
    Drain(buffer,[](auto* p,uint32_t n){for(uint32_t i=0;i<n;++i){p[i]={};p[i].device=i==17?2:1;p[i].sequence=i;p[i].stroke.make_code=0x23;}return n;},
        [&](const auto* p,size_t n)->Send_Result{++probeCalls;emitted.insert(emitted.end(),p,p+n);return {n,0};},
        [](auto& p){p.stroke.make_code=0x76;return true;},probe,nullptr,true,[](HWND){return true;});
    if(probeCalls!=4||emitted.size()!=258||probe.physical!=1||probe.forwarded!=256||!probe.transformed)
        throw std::runtime_error("Batched diagnostic probe counts");
    for(unsigned i=0;i<256;++i){const auto& p=emitted[i+(i>=17?2:0)];if(p.sequence!=i||p.stroke.make_code!=0x23||p.stroke.flags)
        throw std::runtime_error("Diagnostic probe reordered or modified an original");}
    if(emitted[17].stroke.make_code!=0x76||emitted[17].stroke.flags||emitted[18].stroke.make_code!=0x76||emitted[18].stroke.flags!=KeyUp)
        throw std::runtime_error("Diagnostic keyboard probe must remain balanced before target original");
    std::vector<Controller_GIP_Raw_Input> raw(2);Evidence gip;gip.id=3;
    Drain(raw,[](auto* p,uint32_t){p[0].input.device=3;p[1].input.device=4;p[0].report[63]=0xA5;return 2u;},
        [](const auto* p,size_t n)->Send_Result{if(n!=2||p[0].report[63]!=0xA5)throw std::runtime_error("GIP raw extension lost");return {n,0};},
        [](auto&){return false;},gip,nullptr,false);
    if(gip.physical!=1||gip.forwarded!=2)throw std::runtime_error("GIP raw source identity");
}
void Run(unsigned type,const std::wstring& instance,HWND owner){
    Emit("api","info","Reading the loaded driver's capabilities.",5);const auto caps=Get_Capabilities();
    Emit("api","pass","API ABI "+std::to_string(caps.abi_version)+"; XUSB profiles="+std::to_string(caps.xusb_profiles)+", XInputHID="+std::to_string(caps.xinputhid_profiles)+", GIP="+std::to_string(caps.gip_profiles)+".",10);
    try{const auto trial=Get_Trial_Status();
        if(trial.state==IC_TRIAL_EXPIRED){Emit("trial_expired","unverified","The one-hour demo has expired: capture and modification are disabled until Windows restarts. Physical input remains functional.",100);return;}
        if(trial.state==IC_TRIAL_UNAVAILABLE){Emit("trial_state","error","Demo status unavailable; capture disabled, passthrough preserved. Windows="+std::to_string(trial.error)+".",100);return;}
        Emit("trial_state","info",trial.state==IC_TRIAL_NOT_STARTED?"The one-hour demo has not started. The first test that creates a capture context starts the shared timer for all device families.":"Demo active; remaining time: "+std::to_string(trial.remaining_100ns/10000000ull)+" seconds. Closing diagnostics does not reset the timer.",12);
    }catch(const std::system_error& error){if(error.code().value()!=ERROR_INVALID_FUNCTION&&error.code().value()!=ERROR_NOT_SUPPORTED)throw;}
    IC_DEVICE_QUERY_RESULT detail{};bool exactIdentity=true;
    try{detail=Get_Device_Details();Emit("identity","info","Exact identification by PnP instance; HID profiles="+std::to_string(detail.hid_profiles)+".",15);}
    catch(const std::system_error& error){if(error.code().value()!=ERROR_INVALID_FUNCTION&&error.code().value()!=ERROR_NOT_SUPPORTED)throw;exactIdentity=false;Emit("identity_api","warning","Older driver: token identity is unavailable. Fallback is allowed only when hardware ID and class identify one connected instance; duplicates are rejected.",15);}
    if(exactIdentity){
        unsigned found=0;
        for(unsigned ordinal=1;ordinal<=IC_MAX_DEVICES;++ordinal){
            IC_DEVICE_QUERY_RESULT candidate{};
            try{const auto availability=Get_Device_Availability(type,ordinal,true);candidate=availability.details;}
            catch(const std::system_error& error){if(error.code().value()==ERROR_DEVICE_NOT_CONNECTED)break;throw;}
            if((candidate.flags&IC_QUERY_IDENTITY_VALID)&&!_wcsicmp(reinterpret_cast<const wchar_t*>(candidate.instance),instance.c_str())){detail=candidate;++found;}
        }
        if(found==1){
            const char* availability=detail.reserved[0]==IC_DEVICE_READY?"ready":detail.reserved[0]==IC_DEVICE_PREPARING?"preparing":detail.reserved[0]==IC_DEVICE_UNSUPPORTED?"profile rejected":"unavailable";
            std::string reasons;
            const std::pair<uint32_t,const char*> names[]={{IC_DEVICE_WAIT_POWER,"power"},{IC_DEVICE_WAIT_READER,"reader"},{IC_DEVICE_WAIT_PROFILE,"profile validation"},{IC_DEVICE_WAIT_HELLO,"HELLO"},{IC_DEVICE_WAIT_METADATA,"metadata"},{IC_DEVICE_WAIT_FIRST_REPORT,"first valid report"},{IC_DEVICE_WAIT_MIRROR,"secondary HID view"},{IC_DEVICE_TRANSPARENT,"passthrough mode"},{IC_DEVICE_PROFILE_REJECTED,"profile rejected"},{IC_DEVICE_WAIT_CONNECTION,"Windows connection"}};
            for(const auto& reason:names)if(detail.reserved[1]&reason.first){if(!reasons.empty())reasons+=", ";reasons+=reason.second;}
            Emit("reader_state",detail.reserved[0]>=IC_DEVICE_UNSUPPORTED?"warning":"info",std::string("Availability before capture: ")+availability+"; reasons="+(reasons.empty()?"none":reasons)+"; source="+(detail.version==IC_DEVICE_STATE_VERSION?"driver v2":"estimated from legacy driver flags")+"; read="+Hex(detail.last_read_status)+", USB="+Hex(detail.last_usb_status)+". An idle device does not indicate a fault; its reader may start when a capture context opens.",18);
            if(type==6)Emit("gip_state","info","Exact GIP: HELLO="+std::to_string((detail.flags&128)!=0)+", complete metadata="+std::to_string((detail.flags&512)!=0)+", gamepad profile="+std::to_string((detail.flags&1024)!=0)+", metadata="+std::to_string(detail.metadata_bytes)+", declared input="+std::to_string(detail.metadata_input_bytes)+", report="+std::to_string(detail.report_bytes)+", physical reports="+std::to_string(detail.physical_reports)+". HELLO without complete gamepad metadata is insufficient to publish a source.",20);
            if(detail.reserved[0]==IC_DEVICE_UNSUPPORTED || (detail.reserved[1]&IC_DEVICE_TRANSPARENT)){
                Emit("profile_rejected","warning",detail.reserved[0]==IC_DEVICE_UNSUPPORTED?"The driver rejected this instance's profile. Compare metadata with physical reports.":"This collection is in passthrough mode. Its profile, preparation or a previous open may explain this; the flag alone does not prove an incompatible format.",95);
                Emit("capture_unavailable","unverified","Capture and modification were skipped: the instance is present but capture is unavailable. Cancelled reads do not prove disconnection.",100);return;
            }
        }else Emit("reader_state","warning",found?"Multiple readers share this instance; identity is ambiguous.":"No reader is registered for this instance. Check its filter association, Windows stack after restart and advertised profile.",20);
    }else if(type==6){
        Handle control(CreateFileW(L"\\\\.\\Interceptor",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
        IC_GET request{IC_VERSION,1};IC_GIP_DIAGNOSTICS state{};DWORD bytes=0;
        if(control.value!=INVALID_HANDLE_VALUE&&DeviceIoControl(control.value,IOCTL_IC_GIP_DIAGNOSTICS,&request,sizeof(request),&state,sizeof(state),&bytes,nullptr)&&bytes==sizeof(state)){
            unsigned physicalLength=0;for(unsigned i=0;i<std::min<unsigned>(state.trace_count,IC_GIP_TRACE_CAPACITY);++i){const auto& frame=state.trace[i];if(frame.origin==IC_GIP_ORIGIN_PHYSICAL&&frame.length>=4&&frame.bytes[0]==0x20)physicalLength=frame.length;}
            Emit("gip_legacy_state","info","First GIP reader from the older driver (identity cannot be attributed before capture): status="+Hex(state.flags)+", read="+Hex(state.last_read_status)+", USB="+Hex(state.last_usb_status)+", accepted reports="+std::to_string(state.physical_reports)+", latest physical format="+std::to_string(physicalLength)+" bytes. If this format is absent from the advertised profiles, update the driver; reconnecting does not change its decoder.",20);
        }
    }
    Observer observer;
    auto capture=Create_Context();
    Detail::Input_Relay relay(*capture);
    Evidence e;e.type=type;e.instance=instance;unsigned matches=0;
    for(const auto& d:Get_Devices(*capture)){
        if(d.type!=type)continue;
        if(exactIdentity){auto identity=Get_Device_Details(d.type,d.instance_token);if((identity.flags&IC_QUERY_IDENTITY_VALID)&&!_wcsicmp(reinterpret_cast<const wchar_t*>(identity.instance),instance.c_str())){e.id=d.id;++matches;detail=identity;}}
        else{const auto candidates=LegacyCandidates(d);if(candidates.size()==1&&!_wcsicmp(candidates[0].c_str(),instance.c_str())){e.id=d.id;++matches;detail.instance_token=d.instance_token;detail.report_bytes=d.input_bytes;}}
    }
    if(matches!=1){Destroy_Context(capture);Emit("identity","unverified",matches?"Multiple sources match this instance; the active test was rejected.":"No ready source exactly matches this instance. Check association, pending restarts and driver profile.",100);return;}
    Emit("identity",exactIdentity?"pass":"warning",std::string(exactIdentity?"Exact source: ":"Unique legacy driver match: ")+Utf8(instance)+"; capture ID="+std::to_string(e.id)+", token="+std::to_string(detail.instance_token)+"; status="+Hex(detail.flags)+"; read="+Hex(detail.last_read_status)+" ; USB="+Hex(detail.last_usb_status)+".",25);
    if(type==6)Emit("protocol","info","GIP: report="+std::to_string(detail.report_bytes)+", metadata="+std::to_string(detail.metadata_bytes)+", declared prefix="+std::to_string(detail.metadata_input_bytes)+" bytes; physical reports="+std::to_string(detail.physical_reports)+".",30);
    std::vector<BYTE> descriptor;if(type==3)descriptor=Get_Controller_Hid_Descriptor(*capture,e.id);else if(type==5)descriptor=Get_Controller_XinputHid_Descriptor(*capture,e.id);
    if(!descriptor.empty()){HIDP_CAPS hc{};if(HidP_GetCaps(reinterpret_cast<PHIDP_PREPARSED_DATA>(descriptor.data()),&hc)==HIDP_STATUS_SUCCESS)Emit("descriptor","pass","Descriptor: page="+std::to_string(hc.UsagePage)+", usage="+std::to_string(hc.Usage)+", input="+std::to_string(hc.InputReportByteLength)+", values="+std::to_string(hc.NumberInputValueCaps)+", button groups="+std::to_string(hc.NumberInputButtonCaps)+". Declared features are present; their individual operation has not been verified.",35);}
    Emit("capture_open","pass","Capture context opened. Other sources and device families are forwarded unchanged.",40);
    std::vector<Keyboard_Input> keys(256);std::vector<Mouse_Input> mouseInputs(256);std::vector<Controller_Hid_Input> hid(256);std::vector<Controller_Xinput_Input> xusb(256);std::vector<Controller_XinputHid_Input> xih(256);std::vector<Controller_GIP_Raw_Input> gipRaw(256);std::vector<Controller_GIP_Input> gip(256);
    const auto deadline=GetTickCount64()+700;unsigned lastProgress=40;bool probeDeferred=false;
    while(GetTickCount64()<deadline){
        Observe(observer,e);const auto ready=Wait_For_Input(*capture,0);
        relay.Forward(static_cast<std::uint8_t>(ready&~(1u<<(type-1))));bool permit=GetTickCount64()+100<deadline;
        if(ready&&!e.transformed){const bool clean=observer.BeginModification();if(!clean)probeDeferred=true;permit=permit&&clean;}
        if(type==1&&(ready&Keyboard))Drain(keys,[&](auto* p,uint32_t n){return Get_Keyboard_Input(*capture,p,n,n);},[&](const auto* p,size_t n){return Send_Input(*capture,p,n);},[&](auto& in){if(in.stroke.flags&KeyUp)return false;e.key=static_cast<USHORT>(MapVirtualKeyW(VK_F24,MAPVK_VK_TO_VSC));if(!e.key)return false;in.stroke.make_code=e.key;in.stroke.flags=0;return true;},e,owner,permit);
        if(type==2&&(ready&Mouse))Drain(mouseInputs,[&](auto* p,uint32_t n){return Get_Mouse_Input(*capture,p,n,n);},[&](const auto* p,size_t n){return Send_Input(*capture,p,n);},[&](auto& in){return ModifyMouse(in,e.x);},e,owner,permit);
        if(type==3&&(ready&Ready_Controller_Hid))Drain(hid,[&](auto* p,uint32_t n){return Get_Controller_Hid_Input(*capture,p,n,n);},[&](const auto* p,size_t n){return Send_Input(*capture,p,n);},[&](auto& in){if(!ModifyHid(in,descriptor))return false;e.report.assign(in.report,in.report+in.length);return true;},e,owner,permit);
        if(type==4&&(ready&Ready_Controller_Xinput))Drain(xusb,[&](auto* p,uint32_t n){return Get_Controller_Xinput_Input(*capture,p,n,n);},[&](const auto* p,size_t n){return Send_Input(*capture,p,n);},[&](auto& in){in.state.left_x=static_cast<short>(in.state.left_x==32767?32766:in.state.left_x+1);e.nativeX=in.state.left_x;return true;},e,owner,permit);
        if(type==5&&(ready&Ready_Controller_XinputHid))Drain(xih,[&](auto* p,uint32_t n){return Get_Controller_XinputHid_Input(*capture,p,n,n);},[&](const auto* p,size_t n){return Send_Input(*capture,p,n);},[&](auto& in){Controller_Hid_Input temporary{};temporary.length=in.length;memcpy(temporary.report,in.report,in.length);if(!ModifyHid(temporary,descriptor,true,&e.nativeX))return false;memcpy(in.report,temporary.report,in.length);e.report.assign(in.report,in.report+in.length);return true;},e,owner,permit);
        if(type==6&&(ready&Ready_Controller_GIP)){
            if(caps.gip_profiles&IC_PROFILE_GIP_USB_GAMEPAD_RAW)Drain(gipRaw,[&](auto* p,uint32_t n){return Get_Controller_GIP_Raw_Input(*capture,p,n,n);},[&](const auto* p,size_t n){return Send_Input(*capture,p,n);},[&](auto& in){in.input.state.left_x=static_cast<short>(in.input.state.left_x==32767?32766:in.input.state.left_x+1);e.nativeX=in.input.state.left_x;return true;},e,owner,permit);
            else Drain(gip,[&](auto* p,uint32_t n){return Get_Controller_GIP_Input(*capture,p,n,n);},[&](const auto* p,size_t n){return Send_Input(*capture,p,n);},[&](auto& in){in.state.left_x=static_cast<short>(in.state.left_x==32767?32766:in.state.left_x+1);e.nativeX=in.state.left_x;return true;},e,owner,permit);
        }
        const auto now=GetTickCount64();const unsigned progress=40+static_cast<unsigned>((700-std::min<ULONGLONG>(700,now<deadline?deadline-now:0))*45/700);if(progress>lastProgress+5&&progress<90){lastProgress=progress;Emit("sample","info","Automatically observing physical reports.",progress);}
        MsgWaitForMultipleObjectsEx(0,nullptr,1,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
    Observe(observer,e);Destroy_Context(capture);observer.Pump();
    if(probeDeferred&&!e.transformed)Emit("observer_boundary","unverified","The Raw Input queue still contained earlier observations. The probe was deferred to avoid false confirmation; original input continued to be forwarded.",89);
    Emit("physical_capture",e.physical?"pass":"unverified",e.physical?std::to_string(e.physical)+" physical report(s) intercepted from this instance.":"No physical input was received during the short observation. The device may be idle; capture has not been verified.",90);
    Emit("windows_modification",e.observed?"pass":"unverified",e.observed?"Windows Raw Input received the modification from this exact instance or its child view.":e.transformed?"Send accepted the modification, but the independent Windows consumer did not confirm it. Check the translated view, descriptor and open consumers.":"No modifiable report was received while the window was in the foreground; no modification was injected.",95);
    if(observer.error||observer.dropped)Emit("observer","warning","Observer: error="+Hex(observer.error)+", discarded events="+std::to_string(observer.dropped)+".",97);
    Emit("close","pass","Capture context closed and sources released; automatic driver recovery requested. This quick check does not verify every key/button, audio, force feedback or proprietary feature.",100);
}
}
int wmain(int argc,wchar_t** argv){setvbuf(stdout,nullptr,_IONBF,0);try{
    if(argc==2&&!wcscmp(argv[1],L"--self-test")){
        TestForwarding();
        TestObservationBoundary();
        if(RawInstance(L"\\\\?\\HID#VID_1234&PID_5678#x#{guid}")!=L"HID\\VID_1234&PID_5678\\x"||Escape("a\"\n\\")!="a\\\"\\n\\\\")throw std::runtime_error("identity/JSON regression");
        Mouse_Input in{};in.stroke.x=123;in.stroke.y=45;in.stroke.button_flags=0xffff;in.stroke.button_data=120;LONG expected=0;
        if(!ModifyMouse(in,expected)||in.stroke.x!=1||in.stroke.y||in.stroke.button_flags||in.stroke.button_data||expected!=1)throw std::runtime_error("relative mouse probe duplicates physical data");
        in.stroke.flags=MouseAbsolute;in.stroke.x=65535;if(!ModifyMouse(in,expected)||in.stroke.x!=65534)throw std::runtime_error("absolute mouse upper bound");
        in.stroke.x=0;if(!ModifyMouse(in,expected)||in.stroke.x!=1)throw std::runtime_error("absolute mouse lower bound");in.stroke.x=-1;if(ModifyMouse(in,expected))throw std::runtime_error("malformed absolute mouse");
        const auto message=ErrorText(std::system_error(ERROR_DEVICE_NOT_CONNECTED,std::system_category(),"probe"));
        if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,message.data(),static_cast<int>(message.size()),nullptr,0))throw std::runtime_error("Windows error text must be UTF-8");
        Emit("self_test","pass","JSON, PnP identity, mouse probes and 2,560,000 packets forwarded in batches (ordering, balanced insertion, partial sends, GIP extension); no driver opened.",100);return 0;
    }
    if(argc!=7||wcscmp(argv[1],L"--device")||wcscmp(argv[3],L"--type")||wcscmp(argv[5],L"--window"))throw std::invalid_argument("usage: --device INSTANCE --type 1..6 --window HWND");
    const unsigned type=wcstoul(argv[4],nullptr,10);if(type<1||type>6||wcslen(argv[2])>255)throw std::invalid_argument("invalid device selection");
    Run(type,argv[2],reinterpret_cast<HWND>(static_cast<ULONG_PTR>(_wcstoui64(argv[6],nullptr,10))));return 0;
}catch(const std::system_error& error){const auto code=static_cast<DWORD>(error.code().value());
    if(code==IC_ERROR_TRIAL_EXPIRED){Emit("trial_expired","unverified","The one-hour demo expired during the test. Captures were released; restart Windows to continue.",100);return 2;}
    if(code==ERROR_OPERATION_ABORTED){try{if(Get_Trial_Status().state==IC_TRIAL_EXPIRED){Emit("trial_expired","unverified","The demo expired during diagnostics. Remaining tests are unverified; restart Windows to continue.",100);return 2;}}catch(...){}}
    if(code==ERROR_NOT_READY || code==ERROR_TIMEOUT){Emit("capture_pending","unverified","The source is present but not yet ready to finish the test. This does not prove a disconnected or faulty controller. Review its status and preparation reasons above; Win32="+std::to_string(code)+".",100);return 2;}
    Emit("native_error",code==ERROR_INVALID_FUNCTION||code==ERROR_NOT_SUPPORTED||code==ERROR_DEVICE_NOT_CONNECTED||code==ERROR_SHARING_VIOLATION?"unverified":"error",ErrorText(error)+" ; Win32="+std::to_string(code)+" ("+Hex(code)+"). A complete test could not be verified; check driver version, permissions, availability and capture ownership.",100);return 2;
}catch(const std::exception& error){Emit("native_error","error",error.what(),100);return 1;}}
