#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "InputView.h"
#include "DecodedHid.h"
#include "internal/ControllerHid.h"
#include "internal/ControllerXusb.h"
#include "internal/ControllerNative.h"
#include "internal/ControllerGip.h"
#include "internal/DeviceQuery.h"
#include "internal/Trial.h"

namespace Interceptor {
inline constexpr std::uint8_t Keyboard=IC_KEYBOARD, Mouse=IC_MOUSE, Both=IC_BOTH;
inline constexpr std::uint8_t Controller_Hid=IC_CONTROLLER_HID, Controller_Xinput=IC_CONTROLLER_XUSB,
    Controller_XinputHid=IC_CONTROLLER_XINPUTHID, Controller_GIP=IC_CONTROLLER_GIP, Ready_Controller_Hid=IC_READY_HID;
inline constexpr std::uint8_t Ready_Controller_Xinput=IC_READY_XUSB,
    Ready_Controller_XinputHid=IC_READY_XINPUTHID, Ready_Controller_GIP=IC_READY_GIP;
using Keyboard_Input=IC_KEYBOARD_INPUT;
using Mouse_Input=IC_MOUSE_INPUT;
struct Controller_Hid_Input : IC_CONTROLLER_HID_INPUT,Hid_Decoded_Input {
    Controller_Hid_Input():IC_CONTROLLER_HID_INPUT{}{}
};
using Controller_Xinput_Input=IC_CONTROLLER_XUSB_INPUT;
struct Controller_XinputHid_Input : IC_CONTROLLER_XINPUTHID_INPUT,Hid_Decoded_Input {
    Controller_XinputHid_Input():IC_CONTROLLER_XINPUTHID_INPUT{}{}
};
using Controller_GIP_Input=IC_CONTROLLER_GIP_INPUT;
using Controller_GIP_Raw_Input=IC_CONTROLLER_GIP_RAW_INPUT;
// Application identity contains only the capture-scoped string token.
// Kernel wire records and their numeric source identity stay private.
struct Device {
    std::uint32_t id=0,type=0,flags=0;
    std::uint16_t vid=0,pid=0,revision=0,usage_page=0,usage=0;
    std::uint16_t input_bytes=0,output_bytes=0,feature_bytes=0;
    std::uint16_t name[128]{},hardware_id[256]{};
    std::string token;
};
struct Device_Details {
    std::string token;
    // Size/version describe the validated driver response, not this C++ object.
    std::uint32_t size=0,version=0,type=0,hid_profiles=0,flags=0;
    std::uint32_t last_read_status=0,last_usb_status=0;
    std::uint64_t physical_reports=0;
    std::uint32_t report_bytes=0,metadata_bytes=0,metadata_input_bytes=0,reserved[3]{};
    std::uint16_t instance[256]{};
};
struct Context_Info {
    std::string token;
    std::uint64_t generation=0;
    bool active=false;
    std::vector<Device> devices;
    IC_STATUS status{};
    IC_CAPABILITIES capabilities{};
    IC_TRIAL_STATUS trial{};
    DWORD status_error=ERROR_SUCCESS,capabilities_error=ERROR_SUCCESS,trial_error=ERROR_SUCCESS;
};
enum class Descriptor_Format { Identity, Hid_Preparsed, Gip_Metadata, Hid_Report };
struct Descriptor_Result {
    Device device;
    Descriptor_Format format=Descriptor_Format::Identity;
    std::vector<std::uint8_t> data;
    Device_Details details;
    DWORD error=ERROR_SUCCESS,details_error=ERROR_SUCCESS;
    bool applicable=true;
    explicit operator bool() const noexcept{return applicable&&error==ERROR_SUCCESS;}
};
using Report_Descriptor=Descriptor_Result;
enum class Device_Type : std::uint32_t {
    Keyboard=IC_TYPE_KEYBOARD,Mouse=IC_TYPE_MOUSE,Controller_Hid=IC_TYPE_CONTROLLER_HID,
    Controller_Xinput=IC_TYPE_CONTROLLER_XUSB,Controller_XinputHid=IC_TYPE_CONTROLLER_XINPUTHID,
    Controller_GIP=IC_TYPE_CONTROLLER_GIP
};
inline constexpr std::uint16_t KeyUp=1, KeyE0=2, KeyE1=4;
inline constexpr std::uint16_t MouseAbsolute=1, MouseVirtualDesktop=2;
inline constexpr std::uint16_t LeftDown=1, LeftUp=2, RightDown=4, RightUp=8;
inline constexpr std::uint16_t MiddleDown=16, MiddleUp=32, Wheel=0x400, HorizontalWheel=0x800;
struct Init_Skip_Diagnostic {std::wstring instance,reason;DWORD error=ERROR_SUCCESS;};
struct Init_Result {
    bool ready=false,reboot_required=false;
    std::uint32_t associated=0,already_associated=0,skipped=0;
    DWORD error=ERROR_SUCCESS;
    std::vector<Init_Skip_Diagnostic> skipped_devices;
};
struct Send_Result {
    std::size_t accepted=0;DWORD error=ERROR_SUCCESS;
    explicit operator bool() const noexcept{return error==ERROR_SUCCESS;}
};
struct Device_Availability {
    Device_Details details;
    std::uint32_t state=IC_DEVICE_ABSENT,reasons=0;
    bool reported_by_driver=false;
};
}
