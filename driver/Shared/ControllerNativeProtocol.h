#pragma once
#include "InterceptorProtocol.h"

// Internal protocol building blocks, not a claim that a native filter is loaded.
// Windows XUSB IOCTL replies and USB GIP messages are DIFFERENT envelopes.
#define IC_XUSB_GET_INFORMATION 0x80006000u
#define IC_XUSB_GET_STATE 0x8000E00Cu
#define IC_XUSB_WAIT_INPUT 0x8000E3ACu
#define IC_XUSB_WAIT_UNFILTERED 0x8000E388u
#define IC_XUSB_UNFILTERED_BYTES 24u
#define IC_XUSB_STATE_BYTES 29u
#define IC_XUSB_STATE_REQUEST_VERSION 0x0101u
#define IC_XUSB_WAIT_REQUEST_VERSION 0x0102u

typedef struct IC_NATIVE_GAMEPAD_STATE {
    uint32_t packet;
    uint16_t buttons,left_trigger,right_trigger;
    short left_x,left_y,right_x,right_y;
    uint8_t extra_buttons,reserved;
} IC_NATIVE_GAMEPAD_STATE;

static __inline uint16_t IcNativeRead16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8));
}
static __inline uint32_t IcNativeRead32(const uint8_t* p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static __inline short IcNativeSigned16(const uint8_t* p) {
    uint16_t raw=IcNativeRead16(p);
    return (short)(raw<0x8000u?(int32_t)raw:(int32_t)raw-65536);
}
static __inline void IcNativeWrite16(uint8_t* p,uint16_t value) {
    p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);
}
static __inline void IcNativeWrite32(uint8_t* p,uint32_t value) {
    p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);p[2]=(uint8_t)(value>>16);p[3]=(uint8_t)(value>>24);
}
// Returns the exact input length. WAIT includes a fourth byte; passing the
// three-byte GET_STATE request to WAIT is rejected by xusb22 with error 87.
static __inline uint32_t IcXusbBuildRequest(uint8_t slot,int wait,uint8_t output[4]) {
    IcNativeWrite16(output,(uint16_t)(wait?IC_XUSB_WAIT_REQUEST_VERSION:IC_XUSB_STATE_REQUEST_VERSION));
    output[2]=slot;output[3]=0;return wait?4u:3u;
}
// Native system-management stream observed in xusb22 on Windows 10/11.
// It requires a LocalSystem issuing thread, and is not the foreground WAIT.
static __inline uint32_t IcXusbBuildUnfilteredRequest(uint8_t slot,uint8_t output[4]) {
    IcNativeWrite16(output,0x0103);output[2]=slot;output[3]=0;return 4;
}
static __inline int IcXusbDecodeUnfiltered(const uint8_t* data,uint32_t size,IC_NATIVE_GAMEPAD_STATE* out) {
    IC_NATIVE_GAMEPAD_STATE value={0};
    if(!data || !out || size!=IC_XUSB_UNFILTERED_BYTES)return 0;
    value.packet=IcNativeRead32(data);value.buttons=IcNativeRead16(data+6);
    value.left_trigger=data[8];value.right_trigger=data[9];
    value.left_x=IcNativeSigned16(data+10);value.left_y=IcNativeSigned16(data+12);
    value.right_x=IcNativeSigned16(data+14);value.right_y=IcNativeSigned16(data+16);
    value.extra_buttons=data[23];*out=value;return 1;
}
// Known 29-byte state envelope. Unknown versions/statuses are NOT guessed.
// status 1: connected snapshot; status 3: resumed asynchronous input.
static __inline int IcXusbDecodeState(const uint8_t* data,uint32_t size,IC_NATIVE_GAMEPAD_STATE* out) {
    IC_NATIVE_GAMEPAD_STATE value={0};uint16_t version;
    if(!data || !out || size!=IC_XUSB_STATE_BYTES)return 0;
    version=IcNativeRead16(data);
    if((version!=0x0101 && version!=0x0102 && version!=0x0103) || (data[2]!=1 && data[2]!=3))return 0;
    value.packet=IcNativeRead32(data+5);value.buttons=IcNativeRead16(data+11);
    value.left_trigger=data[13];value.right_trigger=data[14];
    value.left_x=IcNativeSigned16(data+15);value.left_y=IcNativeSigned16(data+17);
    value.right_x=IcNativeSigned16(data+19);value.right_y=IcNativeSigned16(data+21);
    value.extra_buttons=data[28];*out=value;return 1;
}
// Replace only the state fields; preserve the original protocol envelope.
static __inline int IcXusbReplaceState(uint8_t* data,uint32_t size,const IC_NATIVE_GAMEPAD_STATE* value) {
    IC_NATIVE_GAMEPAD_STATE original;
    if(!value || value->reserved || value->left_trigger>255 || value->right_trigger>255 ||
       !IcXusbDecodeState(data,size,&original))return 0;
    IcNativeWrite32(data+5,value->packet);IcNativeWrite16(data+11,value->buttons);
    data[13]=(uint8_t)value->left_trigger;data[14]=(uint8_t)value->right_trigger;
    IcNativeWrite16(data+15,(uint16_t)value->left_x);IcNativeWrite16(data+17,(uint16_t)value->left_y);
    IcNativeWrite16(data+19,(uint16_t)value->right_x);IcNativeWrite16(data+21,(uint16_t)value->right_y);
    data[28]=value->extra_buttons;return 1;
}

// MS-GIPUSB 3.1.5.6.1.1: ordinary gamepad input on the primary device.
// This intentionally does not parse \Device\XboxGIP, whose framing is different,
// nor classify all message 0x20 payloads as gamepads (wheels use it too).
#define IC_GIP_INPUT_BYTES 18u
static __inline int IcGipDecodeGamepad(const uint8_t* data,uint32_t size,int gamepadDescriptorConfirmed,
                                     IC_NATIVE_GAMEPAD_STATE* out) {
    IC_NATIVE_GAMEPAD_STATE value={0};
    if(!data || !out || !gamepadDescriptorConfirmed || size!=IC_GIP_INPUT_BYTES ||
       data[0]!=0x20 || data[1]!=0 || data[2]==0 || data[3]!=14)return 0;
    value.packet=data[2];value.buttons=IcNativeRead16(data+4);
    value.left_trigger=IcNativeRead16(data+6);value.right_trigger=IcNativeRead16(data+8);
    if(value.left_trigger>1023 || value.right_trigger>1023 || (value.buttons&1u))return 0;
    value.left_x=IcNativeSigned16(data+10);value.left_y=IcNativeSigned16(data+12);
    value.right_x=IcNativeSigned16(data+14);value.right_y=IcNativeSigned16(data+16);
    *out=value;return 1;
}
static __inline int IcGipReplaceGamepad(uint8_t* data,uint32_t size,int gamepadDescriptorConfirmed,
                                      const IC_NATIVE_GAMEPAD_STATE* value) {
    IC_NATIVE_GAMEPAD_STATE original;
    if(!value || value->reserved || value->extra_buttons || value->left_trigger>1023 || value->right_trigger>1023 ||
       (value->buttons&1u) || !IcGipDecodeGamepad(data,size,gamepadDescriptorConfirmed,&original))return 0;
    // Keep-alive is transport state, not an application-controlled button.
    IcNativeWrite16(data+4,(uint16_t)((value->buttons&~2u)|(original.buttons&2u)));
    IcNativeWrite16(data+6,value->left_trigger);IcNativeWrite16(data+8,value->right_trigger);
    IcNativeWrite16(data+10,(uint16_t)value->left_x);IcNativeWrite16(data+12,(uint16_t)value->left_y);
    IcNativeWrite16(data+14,(uint16_t)value->right_x);IcNativeWrite16(data+16,(uint16_t)value->right_y);
    return 1; // Sequence, flags, type and length remain owned by the transport.
}
