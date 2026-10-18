#pragma once
#include "InputTypes.h"
#include "InputBatch.h"

namespace Interceptor {
// Uses installed packages. Elevated; no UI or automatic Windows restart.
Init_Result Init_AllDevices();
// One library-owned session, lazily opened on the first input operation.
// No installation, retry delay, timer or worker thread is started here.
Input_Batch Get_Input_With_Timeout(DWORD timeout=INFINITE,HANDLE stopEvent=nullptr);
// Independent snapshots; neither owns nor closes the internal capture.
Context_Info Get_Context();
std::vector<Device> Get_Devices();
// Optional explicit release. Finish the batch/reader before calling. A later
// input operation opens a new session; trial state is never reset.
void Destroy_Context();

#define IC_AUTOMATIC_INPUT(Name,Type) \
std::vector<Type> Get_##Name##_Input(std::uint32_t deviceId=0,bool consume=true)
IC_AUTOMATIC_INPUT(Keyboard,Keyboard_Input);
IC_AUTOMATIC_INPUT(Mouse,Mouse_Input);
IC_AUTOMATIC_INPUT(Controller_Hid,Controller_Hid_Input);
IC_AUTOMATIC_INPUT(Controller_Xinput,Controller_Xinput_Input);
IC_AUTOMATIC_INPUT(Controller_XinputHid,Controller_XinputHid_Input);
IC_AUTOMATIC_INPUT(Controller_GIP,Controller_GIP_Input);
IC_AUTOMATIC_INPUT(Controller_GIP_Raw,Controller_GIP_Raw_Input);
#undef IC_AUTOMATIC_INPUT
// Sending a batch again retries only its unaccepted, unblocked entries.
Send_Result Send_Input(Input_Batch& inputs);
Send_Result Send_Input(Input_Event& input);
// Resolve exact string tokens from Get_Context().devices / Get_Devices().
// HID data is Windows preparsed data, not a raw hardware report descriptor.
Descriptor_Result Get_descriptor(const std::string& token);
Report_Descriptor Get_Report_Descriptor(const std::string& token);

// Read-only queries do not create capture or activate the per-boot trial.
IC_STATUS Get_Status();
IC_CAPABILITIES Get_Capabilities();
IC_TRIAL_STATUS Get_Trial_Status();
// Exact token of an existing automatic capture. No capture is created here.
Device_Details Get_Device_Details(const std::string& token);
Device_Availability Get_Device_Availability(const std::string& token);
}
#include "SendInput.h"
