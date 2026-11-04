#pragma once
#include "ControllerXusb.h"
// Private source for the legacy HID child of a native XUSB gamepad. It does not
// expose a second capture queue: its lifetime and delivery follow XUSB ownership.
#define IC_XUSB_MIRROR_INTERFACE_GUID IC_PRODUCT_XUSB_MIRROR_GUID
#define IOCTL_IC_XUSB_MIRROR_SOURCE IC_IOCTL(0xA15)
#define IC_XUSB_MIRROR_HID_PARENT_VALUE IC_PRODUCT_XUSB_HID_PARENT_VALUE
#define IC_XUSB_MIRROR_PARENT_VALUE IC_PRODUCT_XUSB_PARENT_VALUE
#define IC_XUSB_MIRROR_INSTANCE_CHARS 200u
