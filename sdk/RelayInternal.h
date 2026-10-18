#pragma once
#include "ContextAPI.h"
#include <system_error>

// Shared by the shipped examples/diagnostic UI. Not an additional driver API.
namespace Interceptor { namespace Detail {
class Input_Relay {
    Context& context_;
    bool raw_;
    std::vector<Keyboard_Input> keys_;
    std::vector<Mouse_Input> mice_;
    std::vector<Controller_Hid_Input> hid_;
    std::vector<Controller_Xinput_Input> xusb_;
    std::vector<Controller_XinputHid_Input> xih_;
    std::vector<Controller_GIP_Input> gip_;
    std::vector<Controller_GIP_Raw_Input> gipRaw_;
    template<class T,class Get,class Send> void Transfer(std::vector<T>& storage,Get get,Send send) {
        // Unused families occupy no heap storage. Reserve before reading so
        // an allocation failure cannot consume a physical report.
        if(storage.empty())storage.resize(256);
        const auto count=get(storage.data(),256);
        if(!count)return;
        const auto result=send(storage.data(),count);
        if(result.error || result.accepted!=count)
            throw std::system_error(result.error?result.error:ERROR_WRITE_FAULT,std::system_category(),"Forward untouched input families");
    }
public:
    explicit Input_Relay(Context& context):context_(context),raw_(Raw_Gip_Supported(context)){}
    void Forward(std::uint8_t ready) {
#define IC_RELAY(bit,buffer,Name) if(ready&(bit))Transfer(buffer, \
        [&](auto* p,std::uint32_t n){return Get_##Name##_Input(context_,p,n,n);}, \
        [&](const auto* p,std::size_t n){return Send_Input(context_,p,n);})
        IC_RELAY(Keyboard,keys_,Keyboard);
        IC_RELAY(Mouse,mice_,Mouse);
        IC_RELAY(Ready_Controller_Hid,hid_,Controller_Hid);
        IC_RELAY(Ready_Controller_Xinput,xusb_,Controller_Xinput);
        IC_RELAY(Ready_Controller_XinputHid,xih_,Controller_XinputHid);
        if(raw_){IC_RELAY(Ready_Controller_GIP,gipRaw_,Controller_GIP_Raw);}
        else {IC_RELAY(Ready_Controller_GIP,gip_,Controller_GIP);}
#undef IC_RELAY
    }
};
}}
