#pragma once
#include "ControllerHid.h"
#include "ControllerNativeProtocol.h"

// Read-only, administrator-only telemetry. It never creates capture, associates
// a device or starts the trial. All handles and buffers are pointer-free ABI.
#define IC_MONITOR_VERSION 1u
#define IC_MONITOR_CAPACITY 256u
#define IC_MONITOR_MAX_READ 64u
#define IC_MONITOR_MAX_OBSERVERS 4u
#define IC_MONITOR_IOCTL(n) CTL_CODE(IC_DEVICE_TYPE,n,METHOD_BUFFERED,FILE_READ_DATA)
#define IOCTL_IC_MONITOR_QUERY IC_MONITOR_IOCTL(0xA30)
#define IOCTL_IC_MONITOR_SUBSCRIBE IC_MONITOR_IOCTL(0xA31)
#define IOCTL_IC_MONITOR_READ IC_MONITOR_IOCTL(0xA32)
#define IOCTL_IC_MONITOR_DEVICE IC_MONITOR_IOCTL(0xA33)
#define IC_MONITOR_DEVICE 0u
#define IC_MONITOR_IN 1u
#define IC_MONITOR_OUT 2u
#define IC_MONITOR_REMOVED 1u
#define IC_MONITOR_ERROR 2u
#define IC_MONITOR_NATIVE_VALID 4u
#define IC_MONITOR_GUIDE_VALID 8u
#define IC_MONITOR_RAW 16u
#define IC_MONITOR_BY_ORDINAL 1u
#define IC_MONITOR_PREPARSED 2u
#define IC_MONITOR_SOURCE_READY 0x40000u
typedef struct IC_MONITOR_CAPABILITIES {
    uint32_t size,version,frame_bytes,capacity;
    uint64_t timestamp_frequency,session;
} IC_MONITOR_CAPABILITIES;
typedef struct IC_MONITOR_SUBSCRIBE {uint32_t version,flags;uint64_t event_handle;} IC_MONITOR_SUBSCRIBE;
typedef struct IC_MONITOR_READ {uint32_t version,max_frames;} IC_MONITOR_READ;
typedef struct IC_MONITOR_READ_RESULT {
    uint32_t version,frame_bytes,count,capacity;
    uint64_t session,next_sequence,dropped;
} IC_MONITOR_READ_RESULT;
typedef struct IC_MONITOR_FRAME {
    uint64_t sequence,timestamp;
    uint32_t type,instance_token,direction,flags,length,status;
    IC_NATIVE_GAMEPAD_STATE state;
    uint32_t reserved;
    uint8_t report[IC_HID_REPORT_MAX];
} IC_MONITOR_FRAME;
// type + instance_token identifies a device FDO lifetime, never a capture ID.
// IN means a successful physical report. OUT means successful copying into a
// Windows READ/USB IN completion, not an accepted SEND or queued intention.
// It does not assert that every application consumed or displayed that state.
// HID/XInputHID carry exact raw reports, including byte-zero report ID.
// XUSB has NATIVE_VALID|GUIDE_VALID. Ordinary GIP has NATIVE_VALID; GIP Guide
// has GUIDE_VALID only. Merge those independently, retaining other fields.
// Unknown fields remain in report; no console semantics are inferred for HID.
// A read consumes only its subscriber cursor. dropped is cumulative overwrite
// loss for that subscription. session changes when the shared ring is recreated.
// REMOVED is terminal for this FDO identity; Release/Prepare and power restarts
// never remove a device. Ignore late completions from a removed identity.
// Use a manual-reset event with EVENT_MODIFY_STATE; event_handle=0 unsubscribes.
typedef struct IC_MONITOR_DEVICE_REQUEST {uint32_t version,type,instance_token,flags;} IC_MONITOR_DEVICE_REQUEST;
typedef struct IC_MONITOR_DEVICE_RESULT {
    uint32_t size,version,descriptor_bytes,flags;
    IC_DEVICE_INFO info;
    uint16_t instance[256];
} IC_MONITOR_DEVICE_RESULT;
// DEVICE flags: BY_ORDINAL enumerates the family's live devices (one-based).
// PREPARSED appends the Windows HID preparsed data for HID/XInputHID only;
// info alone is available for every family. Result.flags are DEVICE_QUERY flags.
#if defined(__cplusplus)
static_assert(sizeof(IC_MONITOR_CAPABILITIES)==32 && sizeof(IC_MONITOR_SUBSCRIBE)==16,"Monitor setup ABI");
static_assert(sizeof(IC_MONITOR_READ)==8 && sizeof(IC_MONITOR_READ_RESULT)==40,"Monitor read ABI");
static_assert(sizeof(IC_MONITOR_FRAME)==1088,"Monitor frame ABI");
static_assert(sizeof(IC_MONITOR_DEVICE_REQUEST)==16 && sizeof(IC_MONITOR_DEVICE_RESULT)==1328,"Monitor metadata ABI");
#endif
