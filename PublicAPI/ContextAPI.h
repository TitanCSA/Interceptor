#pragma once
// Internal compatibility interface for installer/viewer workers and archived
// regressions. New application code includes only Interceptor.h.
#include "Interceptor.h"
#include "WireInputs.h"
#include <memory>
#include <stdexcept>
namespace Interceptor {
using Device_Info=IC_DEVICE_INFO;
namespace Detail {
struct Native_Device_Availability {
    IC_DEVICE_QUERY_RESULT details{};
    std::uint32_t state=IC_DEVICE_ABSENT,reasons=0;
    bool reported_by_driver=false;
};
}
class Context;
struct Context_Deleter {void operator()(Context*) const noexcept;};
using Context_Ptr=std::unique_ptr<Context,Context_Deleter>;
struct Context_Result {
    Context_Ptr context;
    std::vector<Device_Info> devices;
    std::uint64_t generation=0;
    Context& Get() const {if(!context)throw std::invalid_argument("Capture context is closed");return *context;}
    operator Context&() const {return Get();}
    Context& operator*() const {return Get();}
    Context* operator->() const noexcept{return context.get();}
    explicit operator bool() const noexcept{return static_cast<bool>(context);}
};
Context_Result Create_Context();
namespace Detail {bool Raw_Gip_Supported(const Context&) noexcept;}
std::vector<Device_Info> Get_Devices(Context&);
void Destroy_Context(Context_Ptr&) noexcept;
void Destroy_Context(Context_Result&) noexcept;
#define IC_EXPLICIT_INPUT(Name,Type) \
std::vector<Type> Get_##Name##_Input(Context&,std::uint32_t count=0); \
void Get_##Name##_Input(Context&,std::vector<Type>&,std::uint32_t count=0); \
std::uint32_t Get_##Name##_Input(Context&,Type*,std::uint32_t capacity,std::uint32_t count=0); \
Send_Result Send_Input(const Type*,std::size_t); \
Send_Result Send_Input(Context&,const Type*,std::size_t)
IC_EXPLICIT_INPUT(Keyboard,Keyboard_Input);
IC_EXPLICIT_INPUT(Mouse,Mouse_Input);
IC_EXPLICIT_INPUT(Controller_Hid,Controller_Hid_Input);
IC_EXPLICIT_INPUT(Controller_Xinput,Controller_Xinput_Input);
IC_EXPLICIT_INPUT(Controller_XinputHid,Controller_XinputHid_Input);
IC_EXPLICIT_INPUT(Controller_GIP,Controller_GIP_Input);
IC_EXPLICIT_INPUT(Controller_GIP_Raw,Controller_GIP_Raw_Input);
#undef IC_EXPLICIT_INPUT
std::vector<std::uint8_t> Get_Controller_Hid_Descriptor(Context&,std::uint32_t);
std::vector<std::uint8_t> Get_Controller_XinputHid_Descriptor(Context&,std::uint32_t);
std::uint8_t Wait_For_Input(Context&,DWORD timeout=INFINITE);
std::uint8_t Wait_For_Input(Context&,DWORD,HANDLE);
IC_STATUS Get_Status(Context*);
// Numeric query is private to installers, diagnostics and historical workers.
IC_DEVICE_QUERY_RESULT Get_Device_Details(std::uint32_t type=0,std::uint32_t instance_token=0,bool by_ordinal=false);
Detail::Native_Device_Availability Get_Device_Availability(std::uint32_t type,std::uint32_t instance_token,bool by_ordinal=false);
}
#include "ContextHelpers.h"
