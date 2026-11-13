#ifndef INTERCEPTOR_BRIDGE_H
#define INTERCEPTOR_BRIDGE_H
#include <stdint.h>
#ifdef IC_BRIDGE_EXPORTS
#define INTERCEPTOR_API __declspec(dllexport)
#else
#define INTERCEPTOR_API __declspec(dllimport)
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Windows x64, cdecl, natural alignment. Never free borrowed pointers.
   Receive views remain valid until the next receive or close. Copy data before
   another thread calls either. Every function catches C++ exceptions. */
enum {
    INTERCEPTOR_KEYBOARD = 1,
    INTERCEPTOR_MOUSE = 2,
    INTERCEPTOR_HID = 3,
    INTERCEPTOR_XINPUT = 4,
    INTERCEPTOR_XINPUTHID = 5,
    INTERCEPTOR_GIP = 6
};
enum {
    INTERCEPTOR_STATE = 0,
    INTERCEPTOR_AXIS = 1,
    INTERCEPTOR_TRIGGER = 2,
    INTERCEPTOR_HAT = 3,
    INTERCEPTOR_VALUE = 4,
    INTERCEPTOR_BUTTON = 5,
    INTERCEPTOR_BINARY = 6,
    INTERCEPTOR_REPORT = 7
};
enum {
    INTERCEPTOR_NORMALIZED = 1,
    INTERCEPTOR_RELATIVE = 2,
    INTERCEPTOR_HAS_NULL = 4,
    INTERCEPTOR_IS_NULL = 8,
    INTERCEPTOR_IS_TRIGGER = 16,
    INTERCEPTOR_READ_ONLY = 32
};
#pragma pack(push, 8)
typedef struct interceptor_error {
    uint32_t code, accepted;
    char message[768];
} interceptor_error;
typedef struct interceptor_field {
    const char *name;
    int64_t value, minimum, maximum, physical_minimum, physical_maximum;
    double normalized;
    uint32_t group, index, usage_page, usage, collection, report_id, data_index, element;
    uint32_t flags, units, units_exponent, bit_size;
    uint8_t *data;
    uint32_t length, reserved;
} interceptor_field;
typedef struct interceptor_event {
    uint32_t kind, blocked;
    uint64_t sequence, timestamp;
    const char *token;
    interceptor_field *fields;
    uint32_t field_count, decode_error, decode_complete, parser_status, device, reserved;
} interceptor_event;
typedef struct interceptor_batch {
    uint64_t id;
    uint32_t count, reserved;
    interceptor_event *events;
} interceptor_batch;
typedef struct interceptor_device {
    uint32_t kind, id;
    uint16_t vid, pid, usage_page, usage;
    const char *token;
    const char *name;
    const char *hardware_id;
} interceptor_device;
typedef struct interceptor_devices {
    uint32_t count, reserved;
    const interceptor_device *items;
} interceptor_devices;
typedef struct interceptor_blob {
    uint32_t applicable, format, length, reserved;
    const uint8_t *data;
} interceptor_blob;
typedef struct interceptor_init {
    uint32_t ready, reboot_required, associated, already_associated, skipped, reserved;
} interceptor_init;
typedef struct interceptor_info {
    uint32_t installed, active, trial_state, input_mask, controller_mask, device_count, queued,
        last_reset;
    uint64_t remaining_ms, timestamp_frequency, generation;
} interceptor_info;
#pragma pack(pop)
INTERCEPTOR_API const char* interceptor_product_name(void);
INTERCEPTOR_API uint32_t interceptor_input_mask(void);
INTERCEPTOR_API uint32_t interceptor_controller_mask(void);
INTERCEPTOR_API uint32_t interceptor_version(void);
/* sizeof: 1 field, 2 event, 3 batch, 4 device, 5 error, 6 info. */
INTERCEPTOR_API uint32_t interceptor_sizeof(uint32_t type);
/* Open is lazy: no driver capture, installation, timer or worker thread. */
INTERCEPTOR_API uint32_t interceptor_open(uint64_t *session, interceptor_error *error);
INTERCEPTOR_API uint32_t interceptor_close(uint64_t session, interceptor_error *error);
INTERCEPTOR_API uint32_t interceptor_initialize(uint64_t session, interceptor_init *result, interceptor_error *error);
INTERCEPTOR_API uint32_t interceptor_info_get(uint64_t session, interceptor_info *result, interceptor_error *error);
INTERCEPTOR_API uint32_t interceptor_devices_get(uint64_t session, const interceptor_devices **result, interceptor_error *error);
/* timeout in milliseconds, UINT32_MAX means infinite. Cancel returns empty. */
INTERCEPTOR_API uint32_t interceptor_receive(uint64_t session, uint32_t timeout, const interceptor_batch **result,
                             interceptor_error *error);
/* Counts inferred from ticket. Retry forwards only the unaccepted remainder. */
INTERCEPTOR_API uint32_t interceptor_send(uint64_t session, uint64_t ticket, interceptor_error *error);
INTERCEPTOR_API uint32_t interceptor_discard(uint64_t session, uint64_t ticket, interceptor_error *error);
INTERCEPTOR_API uint32_t interceptor_cancel(uint64_t session, interceptor_error *error);
/* Native HID report: report=1; common descriptor: report=0. */
INTERCEPTOR_API uint32_t interceptor_descriptor(uint64_t session, const char *token, uint32_t report,
                                interceptor_blob *result, interceptor_error *error);
/* Synthetic input: kind and capture token select an existing source. Named
   STATE fields plus REPORT bytes use the same representation as receive.
   Input and field counts belong only to this low-level FFI; wrappers infer them. */
INTERCEPTOR_API uint32_t interceptor_inject(uint64_t session, const interceptor_event *inputs, uint32_t count,
                            interceptor_error *error);
#ifdef __cplusplus
}
static_assert(sizeof(interceptor_field) == 120 && sizeof(interceptor_event) == 64 && sizeof(interceptor_batch) == 24,
              "C ABI layout");
static_assert(sizeof(interceptor_device) == 40 && sizeof(interceptor_error) == 776 && sizeof(interceptor_info) == 56,
              "C ABI layout");
#endif
#endif
