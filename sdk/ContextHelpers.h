#pragma once
#include "ContextAPI.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>

namespace Interceptor {
// All seven send families accept a single input, vector, std::array, C array
// or Detail::Input_View. No allocation/copy and no separate count in the simple API.
#define IC_SIZED_INPUT(Name,Type,Limit) \
inline Send_Result Send_Input(Context& context,Detail::Input_View<const Type> inputs) {return Send_Input(context,inputs.data(),inputs.size());} \
inline Detail::Input_View<Type> Get_##Name##_Input(Context& context,Detail::Input_View<Type> storage) { \
    if(storage.empty())return storage; \
    const auto capacity=static_cast<std::uint32_t>(std::min<std::size_t>(storage.size(),Limit)); \
    return storage.first(Get_##Name##_Input(context,storage.data(),capacity,capacity)); \
}
IC_SIZED_INPUT(Keyboard,Keyboard_Input,IC_CAPACITY)
IC_SIZED_INPUT(Mouse,Mouse_Input,IC_CAPACITY)
IC_SIZED_INPUT(Controller_Hid,Controller_Hid_Input,IC_HID_CAPACITY)
IC_SIZED_INPUT(Controller_Xinput,Controller_Xinput_Input,IC_XUSB_CAPACITY)
IC_SIZED_INPUT(Controller_XinputHid,Controller_XinputHid_Input,IC_HID_CAPACITY)
IC_SIZED_INPUT(Controller_GIP,Controller_GIP_Input,IC_GIP_CAPACITY)
IC_SIZED_INPUT(Controller_GIP_Raw,Controller_GIP_Raw_Input,IC_GIP_CAPACITY)
#undef IC_SIZED_INPUT

inline std::vector<Controller_GIP_Raw_Input> Get_Controller_GIP_Raw_Input(Context& context,std::uint32_t count) {
    std::vector<Controller_GIP_Raw_Input> inputs;Get_Controller_GIP_Raw_Input(context,inputs,count);return inputs;
}

namespace Detail {
#define IC_AUTO_SEND(Name,Type) \
inline Send_Result SendAuto(Context& context,Detail::Input_View<const Type> inputs){return Send_Input(context,inputs);}
IC_AUTO_SEND(Keyboard,Keyboard_Input)
IC_AUTO_SEND(Mouse,Mouse_Input)
IC_AUTO_SEND(Controller_Hid,Controller_Hid_Input)
IC_AUTO_SEND(Controller_Xinput,Controller_Xinput_Input)
IC_AUTO_SEND(Controller_XinputHid,Controller_XinputHid_Input)
IC_AUTO_SEND(Controller_GIP,Controller_GIP_Input)
IC_AUTO_SEND(Controller_GIP_Raw,Controller_GIP_Raw_Input)
#undef IC_AUTO_SEND
}
// Input element type chooses the family; .device keeps the original target.

template<class Inputs> Send_Result Send_Input(Context& context,const Inputs& inputs) {return Detail::SendAuto(context,Detail::Input_View(inputs));}

inline IC_STATUS Get_Status(Context& context){return Get_Status(&context);}
inline IC_DEVICE_QUERY_RESULT Get_Device_Details(const Device_Info& d){return Get_Device_Details(d.type,d.instance_token);}
inline Detail::Native_Device_Availability Get_Device_Availability(const Device_Info& d){return Get_Device_Availability(d.type,d.instance_token);}
inline std::vector<std::uint8_t> Get_Controller_Descriptor(Context& context,const Device_Info& device) {
    if(device.type==IC_TYPE_CONTROLLER_HID)return Get_Controller_Hid_Descriptor(context,device.id);
    if(device.type==IC_TYPE_CONTROLLER_XINPUTHID)return Get_Controller_XinputHid_Descriptor(context,device.id);
    throw std::invalid_argument("This device has no HID descriptor API");
}


namespace Detail {
struct Input_Callbacks {
    void* user=nullptr;
    bool (*keyboard)(void*,Keyboard_Input&)=nullptr;
    bool (*mouse)(void*,Mouse_Input&)=nullptr;
    bool (*hid)(void*,Controller_Hid_Input&)=nullptr;
    bool (*xusb)(void*,Controller_Xinput_Input&)=nullptr;
    bool (*xinputhid)(void*,Controller_XinputHid_Input&)=nullptr;
    bool (*gip)(void*,Controller_GIP_Input&)=nullptr;
    bool (*gipRaw)(void*,Controller_GIP_Raw_Input&)=nullptr;
};
std::size_t ProcessInputs(Context& context,const Input_Callbacks& callbacks,DWORD timeout,HANDLE stop);
template<class Handler,class Input> bool HandleInput(Handler& handler,Input& input) {
    if constexpr(std::is_invocable_v<Handler&,Input&>) {
        using Result=std::invoke_result_t<Handler&,Input&>;
        static_assert(std::is_void_v<Result> || std::is_same_v<Result,bool>,"Input handler must return void (forward) or bool (false blocks)");
        if constexpr(std::is_void_v<Result>)return (std::invoke(handler,input),true);
        else return std::invoke(handler,input);
    } else if constexpr(std::is_same_v<Input,Controller_GIP_Raw_Input>) {
        // Standard GIP edits retain the complete physical extension bytes.
        return HandleInput(handler,input.input);
    } else return true;
}
template<class H,class T> bool InputThunk(void* user,T& input){return (*static_cast<H*>(user))(input);}
}
// One wait + batch: invoke the handler for compatible types, then forward all
// retained inputs. Unhandled families pass unchanged. false blocks one input;
// void/true forwards it. Processing failure closes the control handle before
// propagating the error; the owning Context_Result can then be destroyed.
// Storage is reused; FIFO order is preserved within each family.
template<class Handler,std::enable_if_t<!std::is_integral_v<std::remove_reference_t<Handler>>,int> =0>
std::size_t Process_Captured_Input(Context_Result& capture,Handler&& handler,DWORD timeout=INFINITE,HANDLE stop=nullptr) {
    using H=std::remove_reference_t<Handler>;
    static_assert(std::is_invocable_v<H&,Keyboard_Input&> || std::is_invocable_v<H&,Mouse_Input&> ||
        std::is_invocable_v<H&,Controller_Hid_Input&> || std::is_invocable_v<H&,Controller_Xinput_Input&> ||
        std::is_invocable_v<H&,Controller_XinputHid_Input&> || std::is_invocable_v<H&,Controller_GIP_Input&> ||
        std::is_invocable_v<H&,Controller_GIP_Raw_Input&>,"Handler must accept an Interceptor input by reference");
    auto invoke=[&handler](auto& input){return Detail::HandleInput(handler,input);};
    using F=decltype(invoke);
    const Detail::Input_Callbacks callbacks{&invoke,
        Detail::InputThunk<F,Keyboard_Input>,Detail::InputThunk<F,Mouse_Input>,
        Detail::InputThunk<F,Controller_Hid_Input>,Detail::InputThunk<F,Controller_Xinput_Input>,
        Detail::InputThunk<F,Controller_XinputHid_Input>,Detail::InputThunk<F,Controller_GIP_Input>,
        Detail::InputThunk<F,Controller_GIP_Raw_Input>};
    return Detail::ProcessInputs(capture.Get(),callbacks,timeout,stop);
}
inline std::size_t Process_Captured_Input(Context_Result& capture,DWORD timeout=INFINITE,HANDLE stop=nullptr) {
    return Process_Captured_Input(capture,[](auto&){},timeout,stop);
}
}
