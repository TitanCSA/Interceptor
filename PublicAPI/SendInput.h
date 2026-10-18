#pragma once
#include "Interceptor.h"
namespace Interceptor {
namespace Detail {
// Internal type dispatch. No family selection or size is needed by callers.
#define IC_INFER_SEND(Type) Send_Result SendAuto(Input_View<const Type> inputs);
IC_INFER_SEND(Keyboard_Input)
IC_INFER_SEND(Mouse_Input)
IC_INFER_SEND(Controller_Hid_Input)
IC_INFER_SEND(Controller_Xinput_Input)
IC_INFER_SEND(Controller_XinputHid_Input)
IC_INFER_SEND(Controller_GIP_Input)
IC_INFER_SEND(Controller_GIP_Raw_Input)
#undef IC_INFER_SEND
}
template<class Inputs> auto Send_Input(const Inputs& inputs)
    -> decltype(Detail::SendAuto(Detail::Input_View(inputs))) {
    return Detail::SendAuto(Detail::Input_View(inputs));
}
}
