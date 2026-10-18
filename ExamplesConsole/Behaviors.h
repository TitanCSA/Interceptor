#pragma once
#include "Interceptor.h"
#include <map>
#include <set>
#include <string>
#include <cstdio>
#include <limits>
#include <algorithm>

namespace Examples {
using namespace Interceptor;
enum class Mode { SwapHJ, Hello, InvertY, InvertXAndHK, MonitorKeyboard, MonitorMouse };
constexpr std::uint16_t H=0x23,J=0x24,K=0x25;
constexpr std::uint16_t Side1Down=0x40,Side1Up=0x80,Side2Down=0x100,Side2Up=0x200;
struct Emission {
    std::uint8_t kind;
    union { Keyboard_Input key;Mouse_Input mouse; };
    explicit Emission(const Keyboard_Input& k):kind(Keyboard),key(k){}
    explicit Emission(const Mouse_Input& m):kind(Mouse),mouse(m){}
};
inline std::uint8_t Mask(Mode mode) {
    if(mode==Mode::InvertXAndHK)return Both;
    return mode==Mode::InvertY || mode==Mode::MonitorMouse?Mouse:Keyboard;
}
inline const char* Description(Mode mode) {
    switch(mode) {
    case Mode::SwapHJ:return "Swap H <-> J (presses and releases).";
    case Mode::Hello:return "Ctrl+H types hello once per press, then restores modifiers.";
    case Mode::InvertY:return "Invert the mouse Y axis.";
    case Mode::InvertXAndHK:return "Invert X; J or a side button holds H+K. Trigger inputs are blocked.";
    case Mode::MonitorKeyboard:return "KEYBOARD BLOCKED: display keys without forwarding them.";
    case Mode::MonitorMouse:return "MOUSE BLOCKED: display buttons and movement without forwarding them.";
    }
    return "";
}
inline std::int32_t InvertAxis(std::int32_t value,bool absolute) {
    if(absolute)return 65535-std::clamp(value,0,65535);
    return value==std::numeric_limits<std::int32_t>::min()?std::numeric_limits<std::int32_t>::max():-value;
}
inline bool Plain(const Keyboard_Input& k,std::uint16_t scan) { return k.stroke.make_code==scan && !(k.stroke.flags&(KeyE0|KeyE1)); }
inline std::uint64_t Token(const Keyboard_Input& k) {
    return (static_cast<std::uint64_t>(k.device)<<16)|(static_cast<std::uint64_t>(k.stroke.flags&(KeyE0|KeyE1))<<8)|k.stroke.make_code;
}

// Pure transformations, independent of context ownership and console lifetime.
// The runner sends emitted packets through the automatic public session.
class Behavior {
    Mode mode;
    bool caps,outputH=false,outputK=false;
    std::map<std::uint64_t,Keyboard_Input> held;
    std::set<std::uint64_t> consumedH,sideTriggers;
    static void EmitKey(std::vector<Emission>& out,const Keyboard_Input& source,std::uint16_t scan,bool up,bool defaultDevice=false) {
        auto k=source;k.stroke.make_code=scan;k.stroke.flags=up?KeyUp:0;
        if(defaultDevice) { k.device=0;k.stroke.unit_id=0; }
        out.emplace_back(k);
    }
    bool HasScan(std::uint16_t scan) const {
        for(const auto& pair:held)if(Plain(pair.second,scan))return true;
        return false;
    }
    bool HasControl() const {
        for(const auto& pair:held)if(pair.second.stroke.make_code==0x1D && !(pair.second.stroke.flags&KeyE1))return true;
        return false;
    }
    bool HasAltOrWin() const {
        for(const auto& pair:held) {
            const auto& k=pair.second;
            if(k.stroke.make_code==0x38 || ((k.stroke.flags&KeyE0) && (k.stroke.make_code==0x5B || k.stroke.make_code==0x5C)))return true;
        }
        return false;
    }
    void TypeHello(const Keyboard_Input& source,std::vector<Emission>& out) {
        // Ctrl must be released before typing, otherwise the letters are shortcuts.
        // Preserve left/right Ctrl and Shift independently, including device IDs.
        for(const auto& pair:held) {
            auto k=pair.second;
            if(k.stroke.make_code==0x1D || Plain(k,0x2A) || Plain(k,0x36)) { k.stroke.flags|=KeyUp;out.emplace_back(k); }
        }
        if(caps)EmitKey(out,source,0x2A,false);
        for(const std::uint16_t scan:{H,static_cast<std::uint16_t>(0x12),static_cast<std::uint16_t>(0x26),static_cast<std::uint16_t>(0x26),static_cast<std::uint16_t>(0x18)}) {
            EmitKey(out,source,scan,false);EmitKey(out,source,scan,true);
        }
        if(caps)EmitKey(out,source,0x2A,true);
        for(const auto& pair:held) {
            const auto& k=pair.second;
            if(k.stroke.make_code==0x1D || Plain(k,0x2A) || Plain(k,0x36))out.emplace_back(k);
        }
    }
    void UpdateHK(const Keyboard_Input& source,std::vector<Emission>& out) {
        const bool trigger=HasScan(J)||!sideTriggers.empty();
        const bool wantH=trigger||HasScan(H),wantK=trigger||HasScan(K);
        if(wantH!=outputH)EmitKey(out,source,H,!wantH,true);
        if(wantK!=outputK)EmitKey(out,source,K,!wantK,true);
        outputH=wantH;outputK=wantK;
    }
public:
    explicit Behavior(Mode selected,bool capsLock=false):mode(selected),caps(capsLock){}
    void KeyboardInput(Keyboard_Input input,std::vector<Emission>& out,std::string& log) {
        if(mode==Mode::MonitorKeyboard) {
            char text[256],name[96]{};
            LONG param=static_cast<LONG>(input.stroke.make_code)<<16;
            if(input.stroke.flags&KeyE0)param|=1L<<24;
            if(!GetKeyNameTextA(param,name,sizeof(name)))strcpy_s(name,"?");
            const int n=std::snprintf(text,sizeof(text),"KBD seq=%llu dev=%u %-4s scan=0x%02X flags=0x%02X key=%s\n",
                static_cast<unsigned long long>(input.sequence),input.device,(input.stroke.flags&KeyUp)?"UP":"DOWN",input.stroke.make_code,input.stroke.flags,name);
            if(n>0)log.append(text,static_cast<std::size_t>(std::min(n,static_cast<int>(sizeof(text)-1))));
            return; // Intentionally no Send, including no original packet.
        }
        if(mode==Mode::SwapHJ) {
            if(Plain(input,H))input.stroke.make_code=J;else if(Plain(input,J))input.stroke.make_code=H;
            out.emplace_back(input);return;
        }
        const auto token=Token(input);const bool up=(input.stroke.flags&KeyUp)!=0;
        const bool repeated=held.find(token)!=held.end();
        if(up)held.erase(token);else held[token]=input;
        if(mode==Mode::Hello) {
            if(Plain(input,0x3A) && !up && !repeated)caps=!caps;
            if(consumedH.count(token)) { if(up)consumedH.erase(token);return; }
            if(Plain(input,H) && !up && !repeated && HasControl() && !HasAltOrWin()) {
                consumedH.insert(token);TypeHello(input,out);return;
            }
        }
        if(mode==Mode::InvertXAndHK && (Plain(input,H)||Plain(input,J)||Plain(input,K))) {
            UpdateHK(input,out);
            if(repeated && !up) {
                if(Plain(input,H)||Plain(input,J))EmitKey(out,input,H,false,true);
                if(Plain(input,K)||Plain(input,J))EmitKey(out,input,K,false,true);
            }
            return;
        }
        out.emplace_back(input);
    }
    void MouseInput(Mouse_Input input,std::vector<Emission>& out,std::string& log) {
        if(mode==Mode::MonitorMouse) {
            char text[384];const auto f=input.stroke.button_flags;
            const int n=std::snprintf(text,sizeof(text),"MOUSE seq=%llu dev=%u x=%ld y=%ld %s flags=0x%03X wheel=%d %s%s%s%s%s%s%s%s%s%s\n",
                static_cast<unsigned long long>(input.sequence),input.device,static_cast<long>(input.stroke.x),static_cast<long>(input.stroke.y),
                (input.stroke.flags&MouseAbsolute)?"ABS":"REL",f,static_cast<short>(input.stroke.button_data),
                (f&1)?"L-DOWN ":"",(f&2)?"L-UP ":"",(f&4)?"R-DOWN ":"",(f&8)?"R-UP ":"",
                (f&16)?"MID-DOWN ":"",(f&32)?"MID-UP ":"",(f&Side1Down)?"X1-DOWN ":"",(f&Side1Up)?"X1-UP ":"",
                (f&Side2Down)?"X2-DOWN ":"",(f&Side2Up)?"X2-UP ":"");
            if(n>0)log.append(text,static_cast<std::size_t>(std::min(n,static_cast<int>(sizeof(text)-1))));
            return;
        }
        if(mode==Mode::InvertY)input.stroke.y=InvertAxis(input.stroke.y,(input.stroke.flags&MouseAbsolute)!=0);
        if(mode==Mode::InvertXAndHK) {
            input.stroke.x=InvertAxis(input.stroke.x,(input.stroke.flags&MouseAbsolute)!=0);
            const auto flags=input.stroke.button_flags;
            for(unsigned button=0;button<2;++button) {
                const auto token=(static_cast<std::uint64_t>(input.device)<<8)|button;
                if(flags&(Side1Down<<(button*2)))sideTriggers.insert(token);
                if(flags&(Side1Up<<(button*2)))sideTriggers.erase(token);
            }
            Keyboard_Input source{};source.sequence=input.sequence;source.timestamp=input.timestamp;
            UpdateHK(source,out);
            input.stroke.button_flags&=static_cast<std::uint16_t>(~(Side1Down|Side1Up|Side2Down|Side2Up));
            if(!input.stroke.button_flags && !input.stroke.x && !input.stroke.y && !(input.stroke.flags&MouseAbsolute))return;
        }
        out.emplace_back(input);
    }
};
int Run(Mode mode,int argc,wchar_t** argv);
}
