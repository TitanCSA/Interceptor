#pragma once
#include "ControllerXusb.h"
#include "ControllerXinputHid.h"
#define IOCTL_IC_CREATE_NATIVE IC_IOCTL(0xA16)
// Additive request: neither IC_CREATE nor IC_CREATE_XUSB changes layout.
typedef struct IC_CREATE_NATIVE {
    IC_CREATE base;
    uint64_t xusb_event,xinputhid_event,gip_event;
} IC_CREATE_NATIVE;
#if defined(__cplusplus)
static_assert(sizeof(IC_CREATE_NATIVE)==64,"Extended native create ABI");
#endif
