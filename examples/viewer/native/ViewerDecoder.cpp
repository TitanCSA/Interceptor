#include "ViewerDecoder.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#pragma comment(lib,"hid.lib")

namespace Viewer {
namespace {
DWORD DecodeFailure(VIEWER_DEVICE& s,DWORD error,bool unsupported=false) noexcept {
    s.decodeError=error;s.stateFlags|=unsupported?ViewerDecodeUnsupported:ViewerDecodeError;
    return error;
}
float Axis(int64_t value,int64_t minimum,int64_t maximum) noexcept {
    return static_cast<float>((2.0*static_cast<double>(value-minimum)/static_cast<double>(maximum-minimum))-1.0);
}
int64_t SignedValue(ULONG raw,USHORT bits,LONG minimum) noexcept {
    // HidP_GetUsageValue does not sign-extend: use the declared BitSize/minimum.
    // https://learn.microsoft.com/windows-hardware/drivers/ddi/hidpi/nf-hidpi-hidp_getusagevalue
    const uint64_t modulus=uint64_t{1}<<bits;
    const uint64_t masked=static_cast<uint64_t>(raw)&(modulus-1);
    return minimum<0 && (masked&(modulus>>1))?static_cast<int64_t>(masked)-static_cast<int64_t>(modulus):static_cast<int64_t>(masked);
}
int Hat(bool up,bool down,bool left,bool right) noexcept {
    if((up&&down)||(left&&right))return -1;
    if(up)return left?7:right?1:0;
    if(down)return left?5:right?3:4;
    return left?6:right?2:-1;
}
bool SupportedValue(const HIDP_VALUE_CAPS& v) noexcept {
    return v.UsagePage==1 && !v.IsAlias && v.IsAbsolute && v.BitSize && v.BitSize<=32 &&
        v.LogicalMax>v.LogicalMin && (v.IsRange || v.ReportCount<=1);
}
void ValueOwners(const std::vector<HIDP_VALUE_CAPS>& values,std::array<int,9>& owners,bool& partial) noexcept {
    owners.fill(-1);partial=false;
    for(size_t n=0;n<values.size();++n){const auto& v=values[n];
        if(!SupportedValue(v)){partial=true;continue;}
        const unsigned minimum=v.IsRange?v.Range.UsageMin:v.NotRange.Usage;
        const unsigned maximum=v.IsRange?v.Range.UsageMax:v.NotRange.Usage;
        if(minimum<0x30 || maximum>0x39)partial=true;
        for(unsigned usage=std::max(0x30u,minimum);usage<=std::min(0x39u,maximum);++usage){
            if(usage==0x38){partial=true;continue;}
            const unsigned slot=usage==0x39?8:usage-0x30;
            if(slot>=owners.size()){partial=true;continue;}
            // A single UI slot cannot distinguish identical usages from two
            // reports/links. Select the first declared source consistently and
            // mark the omitted controls; input arrival order must not choose it.
            if(owners[slot]>=0){partial=true;continue;}
            owners[slot]=static_cast<int>(n);
        }
    }
}
DWORD HatValue(int64_t value,int64_t minimum,int64_t maximum,bool hasNull,int32_t& hat) noexcept {
    const int64_t range=maximum-minimum+1;
    if(range!=4 && range!=8)return ERROR_NOT_SUPPORTED;
    if(value<minimum || value>maximum){if(!hasNull)return ERROR_INVALID_DATA;hat=-1;return ERROR_SUCCESS;}
    hat=static_cast<int32_t>((value-minimum)*(range==4?2:1));return ERROR_SUCCESS;
}
}
void Metadata(uint64_t sequence,uint64_t timestamp,VIEWER_DEVICE& s) noexcept {
    s.sequence=sequence;s.timestamp=timestamp;++s.inputCount;s.stateFlags|=ViewerHasInput;
}
DWORD DecodeKeyboard(const Interceptor::Keyboard_Input& in,VIEWER_DEVICE& s) noexcept {
    const auto code=in.stroke.make_code,flags=in.stroke.flags;
    if(code>=255 || ((flags&Interceptor::KeyE0)&&(flags&Interceptor::KeyE1)))
        return DecodeFailure(s,ERROR_INVALID_DATA);
    const unsigned index=code+((flags&Interceptor::KeyE1)?512u:(flags&Interceptor::KeyE0)?256u:0u);
    const auto bit=static_cast<uint8_t>(1u<<(index%8));
    if(flags&Interceptor::KeyUp)s.keys[index/8]&=static_cast<uint8_t>(~bit);
    else s.keys[index/8]|=bit;
    return ERROR_SUCCESS;
}
DWORD DecodeMouse(const Interceptor::Mouse_Input& in,VIEWER_DEVICE& s) noexcept {
    const auto& m=in.stroke;
    for(unsigned i=0;i<5;++i){
        if(m.button_flags&(1u<<(i*2)))s.mouseButtons|=1u<<i;
        if(m.button_flags&(2u<<(i*2)))s.mouseButtons&=~(1u<<i);
    }
    s.mouseX=m.x;s.mouseY=m.y;s.mouseFlags=m.flags;
    const int delta=m.button_data<0x8000u?m.button_data:static_cast<int>(m.button_data)-65536;
    s.wheel=(m.button_flags&Interceptor::Wheel)?delta:0;
    s.horizontalWheel=(m.button_flags&Interceptor::HorizontalWheel)?delta:0;
    return ERROR_SUCCESS;
}
DWORD DecodeNative(const IC_NATIVE_GAMEPAD_STATE& in,bool gip,VIEWER_DEVICE& s) noexcept {
    const unsigned maxTrigger=gip?1023u:255u;
    if(in.left_trigger>maxTrigger || in.right_trigger>maxTrigger || in.reserved)
        return DecodeFailure(s,ERROR_INVALID_DATA);
    s.buttons=in.buttons|(static_cast<uint64_t>(in.extra_buttons&1u)<<16);
    s.buttonMask=(gip?0xFFFCull:0xF3FFull)|0x10000ull;
    s.stateFlags|=ViewerNativeButtons|ViewerHasTriggers|ViewerHasHat;
    s.leftTrigger=static_cast<float>(in.left_trigger)/static_cast<float>(maxTrigger);
    s.rightTrigger=static_cast<float>(in.right_trigger)/static_cast<float>(maxTrigger);
    const short values[]={in.left_x,in.left_y,in.right_x,in.right_y};
    constexpr unsigned indices[]={0,1,3,4};
    for(unsigned n=0;n<4;++n){const auto i=indices[n];const short value=values[n];
        s.axes[i]=static_cast<float>(value)/(value<0?32768.0f:32767.0f);
        s.rawAxes[i]=value;s.axisUsages[i]=static_cast<uint16_t>(0x30u+i);s.axisMask|=1u<<i;
    }
    const unsigned dpad=gip?(in.buttons>>8)&15u:in.buttons&15u;
    s.hat=Hat((dpad&1u)!=0,(dpad&2u)!=0,(dpad&4u)!=0,(dpad&8u)!=0);
    return ERROR_SUCCESS;
}
HidDecoder::HidDecoder(std::vector<uint8_t> descriptor):descriptor_(std::move(descriptor)) {
    // Descriptor bytes originate in the SDK/Windows. Arbitrary untrusted blobs
    // are not accepted from UI, and are not a substitute for HID preparsed data.
    if(descriptor_.empty() || descriptor_.size()>65536)return;
    auto* pp=reinterpret_cast<PHIDP_PREPARSED_DATA>(descriptor_.data());HIDP_CAPS caps{};
    const auto status=HidP_GetCaps(pp,&caps);
    if(status!=HIDP_STATUS_SUCCESS){error_=static_cast<DWORD>(status);return;}
    if(caps.InputReportByteLength==0 || caps.InputReportByteLength>IC_HID_REPORT_MAX ||
       caps.NumberInputButtonCaps>256 || caps.NumberInputValueCaps>256)return;
    inputBytes_=caps.InputReportByteLength;
    buttons_.resize(caps.NumberInputButtonCaps);values_.resize(caps.NumberInputValueCaps);
    if(!buttons_.empty()){
        USHORT count=caps.NumberInputButtonCaps;const auto result=HidP_GetButtonCaps(HidP_Input,buttons_.data(),&count,pp);
        if(result!=HIDP_STATUS_SUCCESS){error_=static_cast<DWORD>(result);return;}buttons_.resize(count);
    }
    if(!values_.empty()){
        USHORT count=caps.NumberInputValueCaps;const auto result=HidP_GetValueCaps(HidP_Input,values_.data(),&count,pp);
        if(result!=HIDP_STATUS_SUCCESS){error_=static_cast<DWORD>(result);return;}values_.resize(count);
    }
    ValueOwners(values_,valueOwners_,layoutPartial_);
    std::array<bool,256> seen{};
    for(const auto& b:buttons_)if(b.UsagePage==9 && !b.IsAlias && !seen[b.ReportID]){
        seen[b.ReportID]=true;buttonReportIds_.push_back(b.ReportID);
    }
    error_=ERROR_SUCCESS;
}
DWORD HidDecoder::Decode(const uint8_t* report,uint32_t bytes,VIEWER_DEVICE& s) noexcept {
    if(error_)return DecodeFailure(s,error_,true);
    if(!report || bytes!=inputBytes_)return DecodeFailure(s,ERROR_INVALID_DATA);
    auto* pp=reinterpret_cast<PHIDP_PREPARSED_DATA>(descriptor_.data());
    auto* data=reinterpret_cast<PCHAR>(const_cast<uint8_t*>(report));
    if(layoutPartial_)s.stateFlags|=ViewerPartialDecode;
    const auto id=report[0];uint64_t reportBits=0;bool recognized=false;DWORD firstError=ERROR_SUCCESS;
    for(const auto& b:buttons_){
        if(b.ReportID!=id)continue;
        if(b.UsagePage!=9 || b.IsAlias){s.stateFlags|=ViewerPartialDecode;continue;}
        const unsigned minimum=b.IsRange?b.Range.UsageMin:b.NotRange.Usage;
        const unsigned maximum=b.IsRange?b.Range.UsageMax:b.NotRange.Usage;
        if(minimum<1 || maximum>64){s.stateFlags|=ViewerPartialDecode;}
        USAGE active[256]{};ULONG count=256;
        const auto status=HidP_GetUsages(HidP_Input,9,b.LinkCollection,active,&count,pp,data,bytes);
        if(status!=HIDP_STATUS_SUCCESS){if(!firstError)firstError=static_cast<DWORD>(status);continue;}
        recognized=true;
        for(unsigned usage=std::max(1u,minimum);usage<=std::min(64u,maximum);++usage)s.buttonMask|=uint64_t{1}<<(usage-1);
        for(ULONG n=0;n<count;++n)if(active[n]>=minimum && active[n]<=maximum && active[n]>=1 && active[n]<=64)
            reportBits|=uint64_t{1}<<(active[n]-1);
    }
    reportButtons_[id]=reportBits;s.buttons=0;
    for(const auto buttonId:buttonReportIds_)s.buttons|=reportButtons_[buttonId];
    for(size_t capIndex=0;capIndex<values_.size();++capIndex){const auto& v=values_[capIndex];
        if(v.ReportID!=id)continue;
        if(!SupportedValue(v)){
            s.stateFlags|=ViewerPartialDecode;continue;
        }
        const unsigned minimum=v.IsRange?v.Range.UsageMin:v.NotRange.Usage;
        const unsigned maximum=v.IsRange?v.Range.UsageMax:v.NotRange.Usage;
        // Only declared Generic Desktop axes and hats. Vendor bytes, trigger
        // assumptions, duplicate-link axes, and value arrays are not guessed.
        if(minimum>0x39 || maximum<0x30){s.stateFlags|=ViewerPartialDecode;continue;}
        for(unsigned usage=std::max(0x30u,minimum);usage<=std::min(0x39u,maximum);++usage){
            if(usage==0x38){s.stateFlags|=ViewerPartialDecode;continue;}
            const unsigned slot=usage==0x39?8:usage-0x30;
            if(slot>=valueOwners_.size() || valueOwners_[slot]!=static_cast<int>(capIndex)){
                s.stateFlags|=ViewerPartialDecode;continue;
            }
            ULONG raw=0;const auto result=HidP_GetUsageValue(HidP_Input,1,v.LinkCollection,static_cast<USAGE>(usage),&raw,pp,data,bytes);
            if(result!=HIDP_STATUS_SUCCESS){if(!firstError)firstError=static_cast<DWORD>(result);continue;}
            const int64_t value=SignedValue(raw,v.BitSize,v.LogicalMin);
            if(usage==0x39){
                const auto hatStatus=HatValue(value,v.LogicalMin,v.LogicalMax,v.HasNull!=FALSE,s.hat);
                if(hatStatus==ERROR_NOT_SUPPORTED){s.stateFlags|=ViewerPartialDecode;continue;}
                if(hatStatus){if(!firstError)firstError=hatStatus;continue;}
                s.stateFlags|=ViewerHasHat;recognized=true;continue;
            }
            const auto index=usage-0x30;
            if(index>=8){s.stateFlags|=ViewerPartialDecode;continue;}
            if(value<v.LogicalMin || value>v.LogicalMax || value>std::numeric_limits<int32_t>::max()){
                s.axisMask&=~(1u<<index);s.stateFlags|=ViewerPartialDecode;continue;
            }
            s.rawAxes[index]=static_cast<int32_t>(value);s.axes[index]=Axis(value,v.LogicalMin,v.LogicalMax);
            s.axisUsages[index]=static_cast<uint16_t>(usage);s.axisMask|=1u<<index;recognized=true;
        }
    }
    if(firstError)return DecodeFailure(s,firstError);
    if(!recognized)return DecodeFailure(s,ERROR_NOT_SUPPORTED,true);
    return ERROR_SUCCESS;
}
DWORD TestHidRules(uint32_t& assertions) {
    assertions=0;
    const auto check=[&](bool ok){++assertions;return ok;};
    HIDP_VALUE_CAPS first{};first.UsagePage=1;first.IsAbsolute=TRUE;first.BitSize=16;
    first.LogicalMin=-32768;first.LogicalMax=32767;first.ReportID=1;first.ReportCount=1;first.NotRange.Usage=0x30;
    auto otherReport=first;otherReport.ReportID=2;
    auto otherLink=first;otherLink.LinkCollection=3;
    auto y=first;y.NotRange.Usage=0x31;
    std::array<int,9> owners{};bool partial=false;
    ValueOwners({first,y},owners,partial);
    if(!check(!partial&&owners[0]==0&&owners[1]==1&&owners[8]==-1))return ERROR_INVALID_DATA;
    ValueOwners({first,otherReport,y},owners,partial);
    if(!check(partial&&owners[0]==0&&owners[1]==2))return ERROR_INVALID_DATA;
    ValueOwners({first,otherLink},owners,partial);
    if(!check(partial&&owners[0]==0))return ERROR_INVALID_DATA;
    ValueOwners({otherReport,first},owners,partial);
    if(!check(partial&&owners[0]==0))return ERROR_INVALID_DATA;
    auto hat=first;hat.NotRange.Usage=0x39;hat.LogicalMin=1;hat.LogicalMax=8;
    auto secondHat=hat;secondHat.ReportID=7;
    ValueOwners({hat,secondHat},owners,partial);
    if(!check(partial&&owners[8]==0))return ERROR_INVALID_DATA;
    int32_t direction=6;
    if(!check(HatValue(0,1,8,false,direction)==ERROR_INVALID_DATA&&direction==6))return ERROR_INVALID_DATA;
    if(!check(HatValue(9,1,8,false,direction)==ERROR_INVALID_DATA&&direction==6))return ERROR_INVALID_DATA;
    if(!check(HatValue(0,1,8,true,direction)==ERROR_SUCCESS&&direction==-1))return ERROR_INVALID_DATA;
    if(!check(HatValue(9,1,8,true,direction)==ERROR_SUCCESS&&direction==-1))return ERROR_INVALID_DATA;
    for(int32_t n=1;n<=8;++n)if(!check(HatValue(n,1,8,false,direction)==ERROR_SUCCESS&&direction==n-1))return ERROR_INVALID_DATA;
    for(int32_t n=0;n<4;++n)if(!check(HatValue(n,0,3,false,direction)==ERROR_SUCCESS&&direction==n*2))return ERROR_INVALID_DATA;
    if(!check(HatValue(2,0,9,true,direction)==ERROR_NOT_SUPPORTED))return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}
}

DWORD __cdecl Viewer_TestHidRules(uint32_t* assertions) noexcept {
    if(!assertions)return ERROR_INVALID_PARAMETER;
    try{return Viewer::TestHidRules(*assertions);}catch(const std::bad_alloc&){return ERROR_NOT_ENOUGH_MEMORY;}catch(...){return ERROR_GEN_FAILURE;}
}

DWORD __cdecl Viewer_DecodeInput(uint32_t type,const void* input,uint32_t inputBytes,
    const void* preparsed,uint32_t preparsedBytes,VIEWER_DEVICE* state) noexcept {
    if(!input || !state)return ERROR_INVALID_PARAMETER;
    try {
        DWORD result=ERROR_INVALID_PARAMETER;
        if(type==IC_TYPE_KEYBOARD && inputBytes==sizeof(IC_KEYBOARD_INPUT)){
            const auto& in=*static_cast<const IC_KEYBOARD_INPUT*>(input);result=Viewer::DecodeKeyboard(in,*state);Viewer::Metadata(in.sequence,in.timestamp,*state);
        }else if(type==IC_TYPE_MOUSE && inputBytes==sizeof(IC_MOUSE_INPUT)){
            const auto& in=*static_cast<const IC_MOUSE_INPUT*>(input);result=Viewer::DecodeMouse(in,*state);Viewer::Metadata(in.sequence,in.timestamp,*state);
        }else if((type==IC_TYPE_CONTROLLER_XUSB || type==IC_TYPE_CONTROLLER_GIP) && inputBytes==sizeof(IC_CONTROLLER_XUSB_INPUT)){
            // These two native API structures deliberately have identical ABI.
            IC_CONTROLLER_XUSB_INPUT in{};memcpy(&in,input,sizeof(in));result=Viewer::DecodeNative(in.state,type==IC_TYPE_CONTROLLER_GIP,*state);Viewer::Metadata(in.sequence,in.timestamp,*state);
        }else if((type==IC_TYPE_CONTROLLER_HID || type==IC_TYPE_CONTROLLER_XINPUTHID) && inputBytes==sizeof(IC_CONTROLLER_HID_INPUT)){
            if(!preparsed || preparsedBytes==0 || preparsedBytes>65536)return ERROR_INVALID_PARAMETER;
            IC_CONTROLLER_HID_INPUT in{};memcpy(&in,input,sizeof(in));
            const auto* p=static_cast<const uint8_t*>(preparsed);Viewer::HidDecoder decoder(std::vector<uint8_t>(p,p+preparsedBytes));
            result=decoder.Decode(in.report,in.length,*state);Viewer::Metadata(in.sequence,in.timestamp,*state);
        }
        return result;
    }catch(const std::bad_alloc&){return ERROR_NOT_ENOUGH_MEMORY;}catch(...){return ERROR_GEN_FAILURE;}
}
