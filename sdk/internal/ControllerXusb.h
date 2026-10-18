#pragma once
#include "ControllerNativeProtocol.h"
#define IC_XUSB_CAPACITY 256u
#define IC_READY_XUSB 8u
#define IOCTL_IC_CREATE_XUSB IC_IOCTL(0xA10)
#define IOCTL_IC_GET_XUSB IC_IOCTL(0xA11)
#define IOCTL_IC_SEND_XUSB IC_IOCTL(0xA12)
#define IOCTL_IC_SEND_OWNED_XUSB IC_IOCTL(0xA13)
#define IOCTL_IC_XUSB_SOURCE IC_IOCTL(0xA14)
#define IC_XUSB_INTERFACE_GUID {0xf631b58e,0x571b,0x44ba,{0xa3,0x43,0x47,0xe2,0x25,0x7a,0x91,0xb3}}
// Additive ABI: the original 40-byte IC_CREATE is unchanged.
typedef struct IC_CREATE_XUSB { IC_CREATE base;uint64_t xusb_event; } IC_CREATE_XUSB;
typedef struct IC_CONTROLLER_XUSB_INPUT {
    uint64_t sequence,timestamp;
    uint32_t device,reserved;
    IC_NATIVE_GAMEPAD_STATE state;
    uint32_t padding;
} IC_CONTROLLER_XUSB_INPUT;
#if defined(__cplusplus)
static_assert(sizeof(IC_CREATE_XUSB)==48,"Native create ABI");
static_assert(sizeof(IC_CONTROLLER_XUSB_INPUT)==48,"XUSB state ABI");
#endif
