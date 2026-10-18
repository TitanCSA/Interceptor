#pragma once
#include "InputTypes.h"
#include <type_traits>
namespace Interceptor::Detail {
struct Hid_Input_Access {
    static void Clear(Hid_Decoded_Input& output) {
        output.token.clear();output.state.axes.clear();output.state.triggers.clear();output.state.hats.clear();output.state.values.clear();output.state.buttons.clear();output.state.binary_values.clear();
        output.decode_error=ERROR_SUCCESS;output.parser_status=0;output.decode_complete=false;
        output.codec_.reset();output.report_id_=0;output.source_id_=0;
    }
};
template<class T> struct Wire_Input {using type=T;};
template<> struct Wire_Input<Controller_Hid_Input>{using type=IC_CONTROLLER_HID_INPUT;};
template<> struct Wire_Input<Controller_XinputHid_Input>{using type=IC_CONTROLLER_XINPUTHID_INPUT;};
template<class T> using Wire=typename Wire_Input<T>::type;
template<class T> inline constexpr bool DecodedHid=!std::is_same_v<T,Wire<T>>;
template<class T> void AssignWire(T& output,const Wire<T>& input) {
    if constexpr(DecodedHid<T>){static_cast<Wire<T>&>(output)=input;Hid_Input_Access::Clear(output);}
    else output=input;
}
template<class T> void AssignWire(std::vector<T>& output,const Wire<T>* input,std::size_t count) {
    output.resize(count);for(std::size_t i=0;i<count;++i)AssignWire(output[i],input[i]);
}
}
