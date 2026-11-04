#pragma once
#include "ControllerNativeProtocol.h"
// MS-XUSBI gamepad usages, with exact scaling observed from Windows 10/11
// xusb22 -> HidUsb collections (20 Win11 +36 Win10 real pairs, 2026-10-14).
// This helper produces semantic HID values; it does not assume report offsets.
typedef struct IC_XUSB_MIRROR_VALUES {
    uint32_t axes[6]; // X, Y, Rx, Ry, Z, hat
    uint16_t buttons[10];
    uint32_t button_count;
} IC_XUSB_MIRROR_VALUES;
static __inline int IcXusbMirrorMakeValues(const IC_NATIVE_GAMEPAD_STATE* state,IC_XUSB_MIRROR_VALUES* output) {
    IC_XUSB_MIRROR_VALUES value={0};unsigned i;
    static const uint16_t buttonMasks[10]={0x1000,0x2000,0x4000,0x8000,0x0100,0x0200,0x0020,0x0010,0x0040,0x0080};
    // Includes contradictory synthetic D-pad combinations. These values were
    // read from Windows with a unique X-axis marker for every combination;
    // assuming opposite directions simply cancel would be incorrect.
    static const uint8_t hats[16]={0,1,5,0,7,8,6,1,3,2,4,3,6,1,5,0};
    if(!state || !output || state->reserved || state->left_trigger>255 || state->right_trigger>255)return 0;
    value.axes[5]=hats[state->buttons&15];
    value.axes[0]=(uint32_t)(32768+(int32_t)state->left_x);
    value.axes[1]=(uint32_t)(32767-(int32_t)state->left_y);
    value.axes[2]=(uint32_t)(32768+(int32_t)state->right_x);
    value.axes[3]=(uint32_t)(32767-(int32_t)state->right_y);
    value.axes[4]=(uint32_t)(32768+128*((int32_t)state->left_trigger-(int32_t)state->right_trigger));
    for(i=0;i<10;++i)if(state->buttons&buttonMasks[i])value.buttons[value.button_count++]=(uint16_t)(i+1);
    *output=value;return 1;
}
