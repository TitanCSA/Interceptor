#pragma once
#include "InterceptorProtocol.h"
// User-mode native gamepad wire state. Transport and queue implementations
// remain private to the binary driver and are not part of this SDK.
typedef struct IC_NATIVE_GAMEPAD_STATE {
    uint32_t packet;
    uint16_t buttons,left_trigger,right_trigger;
    short left_x,left_y,right_x,right_y;
    uint8_t extra_buttons,reserved;
} IC_NATIVE_GAMEPAD_STATE;
#define IC_GIP_INPUT_BYTES 18u
// The raw USB descriptor query in the SDK reads USB little-endian fields.
static __inline uint16_t IcNativeRead16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8));
}
