#pragma once
#include "InterceptorProtocol.h"
#include "HidGameProfile.h"
// Optional ABI4 extension. Old binaries and the original 64-byte capabilities
// remain unchanged. type=token=0 queries the supported HID profiles only.
// by_ordinal=1 selects a registered reader by 1-based ordinal, even when it is
// not ready. Result.instance_token is always the real token. No capture starts.
#define IOCTL_IC_DEVICE_QUERY IC_IOCTL(0xA2C)
#define IC_DEVICE_QUERY_VERSION 1u
#define IC_DEVICE_STATE_VERSION 2u
#define IC_QUERY_IDENTITY_VALID 0x10000u
// Version 2 preserves the version-1 layout/flags and defines reserved[0] as
// availability and reserved[1] as reasons. Version-1 reserved fields stay zero.
#define IC_DEVICE_ABSENT 0u // Enumeration ended; never returned for a live token.
#define IC_DEVICE_PREPARING 1u
#define IC_DEVICE_READY 2u // Capture can start; input activity is not required.
#define IC_DEVICE_UNSUPPORTED 3u
#define IC_DEVICE_UNAVAILABLE 4u
#define IC_DEVICE_WAIT_POWER 0x1u
#define IC_DEVICE_WAIT_READER 0x2u
#define IC_DEVICE_WAIT_PROFILE 0x4u
#define IC_DEVICE_WAIT_HELLO 0x8u
#define IC_DEVICE_WAIT_METADATA 0x10u
#define IC_DEVICE_WAIT_FIRST_REPORT 0x20u
#define IC_DEVICE_WAIT_MIRROR 0x40u
#define IC_DEVICE_TRANSPARENT 0x80u
#define IC_DEVICE_PROFILE_REJECTED 0x100u
#define IC_DEVICE_WAIT_CONNECTION 0x200u
#define IC_DEVICE_TRIAL_EXPIRED 0x400u
#define IC_DEVICE_TRIAL_UNAVAILABLE 0x800u
typedef struct IC_DEVICE_QUERY {uint32_t version,type,instance_token,by_ordinal;} IC_DEVICE_QUERY;
typedef struct IC_DEVICE_QUERY_RESULT {
    uint32_t size,version,type,instance_token,hid_profiles,flags,last_read_status,last_usb_status;
    uint64_t physical_reports;
    uint32_t report_bytes,metadata_bytes,metadata_input_bytes,reserved[3];
    uint16_t instance[256];
} IC_DEVICE_QUERY_RESULT;
#if defined(__cplusplus)
static_assert(sizeof(IC_DEVICE_QUERY)==16 && sizeof(IC_DEVICE_QUERY_RESULT)==576,"Device query extension ABI");
#endif
