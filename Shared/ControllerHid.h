#pragma once
#include "InterceptorProtocol.h"
#define IC_CONTROLLER_HID 1u
#define IC_CONTROLLER_XUSB 2u
#define IC_CONTROLLER_XINPUTHID 4u
#define IC_CONTROLLER_GIP 8u
#define IC_CONTROLLER_ALL 15u
#define IC_READY_HID 4u
#define IC_STREAM_COUNT 6u
#define IC_ALL_STREAMS 63u
#define IC_HID_REPORT_MAX 1024u
#define IC_HID_CAPACITY 256u
#define IC_HID_FILE_CAPACITY 16u
#define IOCTL_IC_GET_HID IC_IOCTL(0xA09)
#define IOCTL_IC_SEND_HID IC_IOCTL(0xA0A)
#define IOCTL_IC_SEND_OWNED_HID IC_IOCTL(0xA0B)
#define IOCTL_IC_HID_DESCRIPTOR IC_IOCTL(0xA0C)
#define IOCTL_IC_HID_SOURCE IC_IOCTL(0xA0D)
// Collection-level reports, including byte zero (Report ID, zero if absent).
typedef struct IC_CONTROLLER_HID_INPUT {
    uint64_t sequence,timestamp;
    uint32_t device,length;
    uint32_t reserved[2];
    uint8_t report[IC_HID_REPORT_MAX];
} IC_CONTROLLER_HID_INPUT;
// {9CE34440-4716-4F39-922B-995C28A52C87}
#define IC_HID_INTERFACE_GUID {0x9ce34440,0x4716,0x4f39,{0x92,0x2b,0x99,0x5c,0x28,0xa5,0x2c,0x87}}
#if defined(__cplusplus)
static_assert(sizeof(IC_CONTROLLER_HID_INPUT)==1056,"HID report ABI");
#endif
