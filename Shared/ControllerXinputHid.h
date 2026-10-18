#pragma once
#include "ControllerHid.h"

#define IC_READY_XINPUTHID 16u
#define IOCTL_IC_GET_XINPUTHID IC_IOCTL(0xA17)
#define IOCTL_IC_SEND_XINPUTHID IC_IOCTL(0xA18)
#define IOCTL_IC_SEND_OWNED_XINPUTHID IC_IOCTL(0xA19)
#define IOCTL_IC_XINPUTHID_DESCRIPTOR IC_IOCTL(0xA1A)
#define IOCTL_IC_XINPUTHID_SOURCE IC_IOCTL(0xA1B)
// Source HID report BEFORE xinputhid translation, including the report ID.
// Descriptor and length belong to the physical collection: they are not the
// synthetic 16-byte DirectInput report exposed above the Windows translator.
typedef struct IC_CONTROLLER_XINPUTHID_INPUT {
    uint64_t sequence,timestamp;
    uint32_t device,length;
    uint32_t reserved[2];
    uint8_t report[IC_HID_REPORT_MAX];
} IC_CONTROLLER_XINPUTHID_INPUT;
#define IC_XINPUTHID_INTERFACE_GUID {0x2788ce3e,0x51bd,0x43eb,{0xa6,0x6d,0x13,0x74,0xb8,0x48,0xc5,0x8c}}
#if defined(__cplusplus)
static_assert(sizeof(IC_CONTROLLER_XINPUTHID_INPUT)==sizeof(IC_CONTROLLER_HID_INPUT),"XInputHID wire layout");
#endif
