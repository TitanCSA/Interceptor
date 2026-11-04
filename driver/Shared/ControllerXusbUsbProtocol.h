#pragma once
#include "ControllerNativeProtocol.h"

// Wired XUSB has a twelve-byte state after Id/Size. Physical devices normally
// transfer twenty bytes; ViGEm also transfers the fourteen-byte prefix with
// nominal Size=20. Never infer accessible memory from that nominal size byte.
#define IC_XUSB_USB_MIN_BYTES 14u
#define IC_XUSB_USB_MAX_BYTES 32u
#define IC_XUSB_USB_NOMINAL_BYTES 20u
#define IC_XUSB_USB_GUIDE 0x0400u
static __inline int IcXusbUsbDecode(const uint8_t* bytes,uint32_t size,IC_NATIVE_GAMEPAD_STATE* output) {
    IC_NATIVE_GAMEPAD_STATE state={0};uint16_t buttons;
    if(!bytes || !output || size<IC_XUSB_USB_MIN_BYTES || size>IC_XUSB_USB_MAX_BYTES ||
       bytes[0]!=0 || bytes[1]!=IC_XUSB_USB_NOMINAL_BYTES)return 0;
    buttons=IcNativeRead16(bytes+2);state.buttons=(uint16_t)(buttons&~IC_XUSB_USB_GUIDE);
    state.extra_buttons=(uint8_t)((buttons&IC_XUSB_USB_GUIDE)!=0);
    state.left_trigger=bytes[4];state.right_trigger=bytes[5];
    state.left_x=IcNativeSigned16(bytes+6);state.left_y=IcNativeSigned16(bytes+8);
    state.right_x=IcNativeSigned16(bytes+10);state.right_y=IcNativeSigned16(bytes+12);
    *output=state;return 1;
}
static __inline int IcXusbUsbValidState(const IC_NATIVE_GAMEPAD_STATE* state) {
    return state && !state->reserved && state->extra_buttons<=1 &&
        state->left_trigger<=255 && state->right_trigger<=255 && !(state->buttons&IC_XUSB_USB_GUIDE);
}
static __inline int IcXusbUsbReplace(uint8_t* bytes,uint32_t size,const IC_NATIVE_GAMEPAD_STATE* state) {
    IC_NATIVE_GAMEPAD_STATE original;
    if(!IcXusbUsbValidState(state) || !IcXusbUsbDecode(bytes,size,&original))return 0;
    IcNativeWrite16(bytes+2,(uint16_t)(state->buttons|(state->extra_buttons?IC_XUSB_USB_GUIDE:0)));
    bytes[4]=(uint8_t)state->left_trigger;bytes[5]=(uint8_t)state->right_trigger;
    IcNativeWrite16(bytes+6,(uint16_t)state->left_x);IcNativeWrite16(bytes+8,(uint16_t)state->left_y);
    IcNativeWrite16(bytes+10,(uint16_t)state->right_x);IcNativeWrite16(bytes+12,(uint16_t)state->right_y);
    // Id, nominal Size, original transfer extent and every opaque suffix byte
    // remain owned by the physical transport. packet is SDK metadata only.
    return 1;
}
