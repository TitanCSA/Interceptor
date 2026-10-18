#pragma once
#include "ControllerNativeProtocol.h"

#define IC_GIP_CAPACITY 256u
#define IC_READY_GIP 32u
#define IOCTL_IC_GET_GIP IC_IOCTL(0xA20)
#define IOCTL_IC_SEND_GIP IC_IOCTL(0xA21)
#define IOCTL_IC_SEND_OWNED_GIP IC_IOCTL(0xA22)
#define IOCTL_IC_GIP_DESCRIPTOR IC_IOCTL(0xA23)
#define IOCTL_IC_GIP_SOURCE IC_IOCTL(0xA24)
#define IOCTL_IC_GET_GIP_RAW IC_IOCTL(0xA29)
#define IOCTL_IC_SEND_GIP_RAW IC_IOCTL(0xA2A)
#define IOCTL_IC_SEND_OWNED_GIP_RAW IC_IOCTL(0xA2B)
#define IC_GIP_REPORT_MAX 64u
#define IC_GIP_INTERFACE_GUID {0x78310f48,0xccae,0x4a2d,{0x88,0x35,0xac,0xac,0xcf,0x26,0x6c,0xd2}}
// Native GIP state, not an XUSB envelope. state.buttons uses GIP bit positions;
// triggers are 0..1023, axes signed 16-bit, extra_buttons bit 0 is Guide.
// Input transport sequence/keep-alive remain owned by the USB transport.
typedef struct IC_CONTROLLER_GIP_INPUT {
    uint64_t sequence,timestamp;
    uint32_t device,reserved;
    IC_NATIVE_GAMEPAD_STATE state;
    uint32_t padding;
} IC_CONTROLLER_GIP_INPUT;
// Optional ABI4 extension. The legacy 48-byte state API remains unchanged.
// state controls the standard prefix; report retains the original envelope
// and opaque extension bytes. Send owns sequence/keep-alive, and accepts only
// the current device's validated gamepad shape. report_length=0 preserves the
// latest physical extension, as does the legacy state-only Send.
typedef struct IC_CONTROLLER_GIP_RAW_INPUT {
    IC_CONTROLLER_GIP_INPUT input;
    uint32_t report_length,report_reserved;
    uint8_t report[IC_GIP_REPORT_MAX];
} IC_CONTROLLER_GIP_RAW_INPUT;
#if defined(__cplusplus)
static_assert(sizeof(IC_CONTROLLER_GIP_INPUT)==48,"GIP state ABI");
static_assert(sizeof(IC_CONTROLLER_GIP_RAW_INPUT)==120,"GIP raw extension ABI");
#endif
