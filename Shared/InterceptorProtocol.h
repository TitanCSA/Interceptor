#pragma once
#include <stdint.h>

// Versioned, pointer-free ABI. All reserved fields must be zero.
#define IC_VERSION 4u
#define IC_KEYBOARD 1u
#define IC_MOUSE 2u
#define IC_BOTH 3u
#define IC_CAPACITY 8192u
#define IC_MAX_SEND 256u
#define IC_MAX_DEVICES 256u
#define IC_TYPE_KEYBOARD 1u
#define IC_TYPE_MOUSE 2u
#define IC_TYPE_CONTROLLER_HID 3u
#define IC_TYPE_CONTROLLER_XUSB 4u
#define IC_TYPE_CONTROLLER_XINPUTHID 5u
#define IC_TYPE_CONTROLLER_GIP 6u
#define IC_INFO_HID_VALID 1u
#define IC_INFO_NAME_VALID 2u
#define IC_INFO_HARDWARE_ID_VALID 4u
typedef struct IC_DEVICE_INFO {
    uint32_t id,type,instance_token,flags;
    uint16_t vid,pid,revision,usage_page,usage,input_bytes,output_bytes,feature_bytes;
    uint16_t name[128],hardware_id[256]; // UTF-16; empty if unavailable.
} IC_DEVICE_INFO;
typedef struct IC_CREATE_RESULT { uint32_t version,count;uint64_t generation; } IC_CREATE_RESULT;
#define IC_DEVICE_TYPE 0x8349u
#define IC_IOCTL(n) CTL_CODE(IC_DEVICE_TYPE,n,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define IOCTL_IC_CREATE IC_IOCTL(0xA00)
#define IOCTL_IC_GET_KEYBOARD IC_IOCTL(0xA01)
#define IOCTL_IC_GET_MOUSE IC_IOCTL(0xA02)
#define IOCTL_IC_SEND_KEYBOARD IC_IOCTL(0xA03)
#define IOCTL_IC_SEND_MOUSE IC_IOCTL(0xA04)
#define IOCTL_IC_STATUS IC_IOCTL(0xA05)
#define IOCTL_IC_DESTROY IC_IOCTL(0xA06)
#define IOCTL_IC_SEND_OWNED_KEYBOARD IC_IOCTL(0xA07)
#define IOCTL_IC_SEND_OWNED_MOUSE IC_IOCTL(0xA08)
#define IOCTL_IC_CAPABILITIES IC_IOCTL(0xA26)
// Retired function numbers 0xA27 and 0xA28 are reserved; do not reuse them.
#define IC_CAPABILITIES_VERSION 1u
#define IC_PROFILE_XUSB_WIRED_028E 1u
#define IC_PROFILE_XUSB_WIRED_COMPATIBLE 2u
#define IC_PROFILE_XINPUTHID_GENERIC6 1u
#define IC_PROFILE_GIP_USB_GAMEPAD14 1u
#define IC_PROFILE_GIP_USB_GAMEPAD_RAW 2u
// Immutable capabilities. No input or capture is required. These profile bits
// identify supported stack/protocol shapes, not every product sharing a brand.
typedef struct IC_CAPABILITIES {
    uint32_t size,version,abi_version,input_mask;
    uint32_t controller_mask,flags,max_devices,queue_capacity;
    uint32_t max_send,hid_report_max,xusb_profiles,xinputhid_profiles;
    uint32_t gip_profiles,reserved[3];
} IC_CAPABILITIES;

typedef struct IC_KEYBOARD_STROKE {
    uint16_t unit_id, make_code, flags, reserved;
    uint32_t extra_information;
} IC_KEYBOARD_STROKE;
typedef struct IC_MOUSE_STROKE {
    uint16_t unit_id, flags, button_flags, button_data;
    uint32_t raw_buttons;
    int32_t x, y;
    uint32_t extra_information;
} IC_MOUSE_STROKE;
typedef struct IC_KEYBOARD_INPUT {
    uint64_t sequence, timestamp;
    uint32_t device, reserved;
    IC_KEYBOARD_STROKE stroke;
    uint32_t padding;
} IC_KEYBOARD_INPUT;
typedef struct IC_MOUSE_INPUT {
    uint64_t sequence, timestamp;
    uint32_t device, reserved;
    IC_MOUSE_STROKE stroke;
} IC_MOUSE_INPUT;
typedef struct IC_CREATE {
    uint32_t version;
    uint8_t mask;
    uint8_t controller_mask, reserved[2];
    uint64_t keyboard_event, mouse_event;
    uint64_t revoked_event, controller_hid_event;
} IC_CREATE;
typedef struct IC_GET { uint32_t version, count; } IC_GET;
typedef struct IC_SEND_RESULT { uint32_t accepted, status; } IC_SEND_RESULT;
typedef struct IC_STATUS {
    uint32_t version, active_mask, keyboard_count, mouse_count;
    uint32_t keyboard_devices, mouse_devices, last_reset_reason, capacity;
    uint64_t generation, overflows, timestamp_frequency;
    uint32_t controller_hid_count,controller_hid_devices;
} IC_STATUS;
#define IC_RESET_NONE 0u
#define IC_RESET_HOTKEY 1u
#define IC_RESET_OVERFLOW 2u
#define IC_RESET_CLOSE 3u
#define IC_RESET_DEVICE 4u
#define IC_RESET_TRIAL_EXPIRED 5u

#if defined(__cplusplus)
static_assert(sizeof(IC_DEVICE_INFO)==800 && sizeof(IC_CREATE_RESULT)==16,"Device snapshot ABI");
static_assert(sizeof(IC_KEYBOARD_INPUT)==40 && sizeof(IC_MOUSE_INPUT)==48,"ABI layout");
static_assert(sizeof(IC_CREATE)==40 && sizeof(IC_STATUS)==64,"ABI layout");
static_assert(sizeof(IC_CAPABILITIES)==64,"Capabilities ABI");
#else
typedef char IC_ABI_CHECK[(sizeof(IC_DEVICE_INFO)==800 && sizeof(IC_CREATE_RESULT)==16 && sizeof(IC_KEYBOARD_INPUT)==40 && sizeof(IC_MOUSE_INPUT)==48 && sizeof(IC_CREATE)==40 && sizeof(IC_STATUS)==64)?1:-1];
typedef char IC_CAPABILITIES_ABI_CHECK[(sizeof(IC_CAPABILITIES)==64)?1:-1];
#endif
