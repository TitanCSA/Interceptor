#pragma once
#include "ControllerXusb.h"
// Private source for the legacy HID child of a native XUSB gamepad. It does not
// expose a second capture queue: its lifetime and delivery follow XUSB ownership.
#define IC_XUSB_MIRROR_INTERFACE_GUID {0x4bdfd98d,0x5b54,0x4ba0,{0x9e,0x75,0x0b,0xc7,0x37,0xa8,0xbd,0x51}}
#define IOCTL_IC_XUSB_MIRROR_SOURCE IC_IOCTL(0xA15)
#define IC_XUSB_MIRROR_HID_PARENT_VALUE L"InterceptorXusbHidParent"
#define IC_XUSB_MIRROR_PARENT_VALUE L"InterceptorXusbParent"
#define IC_XUSB_MIRROR_INSTANCE_CHARS 200u
