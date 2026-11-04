#pragma once
#include "ControllerGip.h"

// Experimental laboratory diagnostics through the administrator-only control
// device. IC_GET.count is a 1-based registered GIP device ordinal (0 = first).
#define IOCTL_IC_GIP_DIAGNOSTICS IC_IOCTL(0xA25)
#define IC_GIP_DIAGNOSTIC_VERSION 1u
#define IC_GIP_TRACE_CAPACITY 32u
#define IC_GIP_ORIGIN_PROTOCOL 0u
#define IC_GIP_ORIGIN_PHYSICAL 1u
#define IC_GIP_ORIGIN_SEND 2u
#define IC_GIP_ORIGIN_RESTORE 3u
typedef struct IC_GIP_TRACE {
    uint64_t ordinal,timestamp;
    uint32_t origin,length;
    uint8_t bytes[64];
} IC_GIP_TRACE;
typedef struct IC_GIP_DIAGNOSTICS {
    uint32_t version,flags,capture_id,output_count;
    uint64_t physical_reports,send_attempts,reset_calls,restore_attempts,restore_queued;
    uint32_t last_read_status,last_usb_status,trace_count,reserved;
    IC_NATIVE_GAMEPAD_STATE physical,scheduled;
    uint8_t last_input_sequence,reserved_bytes[7];
    IC_GIP_TRACE trace[IC_GIP_TRACE_CAPACITY];
} IC_GIP_DIAGNOSTICS;
// flags: ready=1, running=2, powered=4, valid=8, configured=16,
// restorePending=32, unsupported=64, helloValidated=128.
// metadataReceiving=256, metadataComplete=512, metadataGamepad14=1024.
#if defined(__cplusplus)
static_assert(sizeof(IC_GIP_TRACE)==88,"GIP trace ABI");
static_assert(sizeof(IC_GIP_DIAGNOSTICS)==2936,"GIP diagnostics ABI");
#endif
