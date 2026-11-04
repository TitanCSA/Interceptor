#pragma once
#include "ControllerGip.h"
#ifndef _KERNEL_MODE
#include <string.h>
#endif

// Bounded MS-GIPUSB framing. These helpers do not prove a native filter exists.
// Metadata v1 layout is checked structurally, including the class list and
// upstream message descriptor. Other schemas remain unsupported/passthrough.
#define IC_GIP_METADATA_MAX 4096u
typedef struct IC_GIP_FRAME {
    uint32_t header,payload,tlo;
    uint8_t command,flags,sequence,reserved;
} IC_GIP_FRAME;
typedef struct IC_GIP_METADATA {
    uint32_t total,received;
    uint8_t sequence,active,complete,gamepad;
    uint32_t input_length;
    uint8_t bytes[IC_GIP_METADATA_MAX];
} IC_GIP_METADATA;

// MS-GIPUSB 2.2.2: the Windows metadata cache key consists of VID, PID,
// firmware major/minor. The physical DeviceID, build and hardware revision
// are not part of that key. Otherwise Windows can reuse its cache while our
// filter waits forever for metadata that will not be transmitted again.
// Arguments are the 28-byte HELLO payload, after full HELLO validation.
static __inline int IcGipMetadataIdentityEqual(const uint8_t* a,const uint8_t* b) {
    uint32_t i;
    if(!a || !b)return 0;
    for(i=8;i<16;++i)if(a[i]!=b[i])return 0;
    return 1;
}

static __inline int IcGipVarint(const uint8_t* bytes,uint32_t size,uint32_t* cursor,uint32_t* value) {
    uint32_t result=0,shift;
    for(shift=0;shift<28;shift+=7) {
        uint8_t byte;
        if(*cursor>=size)return 0;
        byte=bytes[(*cursor)++];result|=(uint32_t)(byte&127u)<<shift;
        if(!(byte&128u)){*value=result;return 1;}
    }
    return 0;
}
static __inline int IcGipParseFrame(const uint8_t* bytes,uint32_t size,IC_GIP_FRAME* frame) {
    IC_GIP_FRAME result={0};uint32_t cursor=3;
    if(!bytes || !frame || size<4 || !bytes[2] || (bytes[1]&8u))return 0;
    result.command=bytes[0];result.flags=bytes[1];result.sequence=bytes[2];
    if(!IcGipVarint(bytes,size,&cursor,&result.payload))return 0;
    if(result.flags&0x80u) {
        // Even the zero-payload completion carries its cumulative offset.
        if(!IcGipVarint(bytes,size,&cursor,&result.tlo))return 0;
    }else if(result.flags&0x40u)return 0;
    if(result.payload!=size-cursor)return 0;
    result.header=cursor;*frame=result;return 1;
}
static __inline uint32_t IcGipMetadataGamepadLength(const uint8_t* bytes,uint32_t size) {
    static const char name[]="Windows.Xbox.Input.Gamepad";
    static const uint8_t gamepadGuid[16]={0x2C,0x40,0x2E,0x08,0xDF,0x07,0xE1,0x45,0xA5,0xAB,0xA3,0x12,0x7A,0xF1,0x97,0xB5};
    uint32_t offset,n,i,length,commandOffset,interfaceOffset,input=0;int gamepad=0;
    if(!bytes || size<32 || size>IC_GIP_METADATA_MAX || IcNativeRead16(bytes)!=16 ||
       bytes[2]!=1 || bytes[3]!=0 || IcNativeRead16(bytes+14)!=size)return 0;
    offset=16u+IcNativeRead16(bytes+26);commandOffset=16u+IcNativeRead16(bytes+16);
    interfaceOffset=16u+IcNativeRead16(bytes+28);
    if(offset<32 || offset>=size || commandOffset<32 || commandOffset>=size || interfaceOffset<32 || interfaceOffset>=size)return 0;
    n=bytes[offset++];
    for(i=0;i<n;++i) {
        if(size-offset<2)return 0;
        length=IcNativeRead16(bytes+offset);offset+=2;
        if(!length || length>size-offset)return 0;
        if(length==sizeof(name)-1) {
            uint32_t j;
            for(j=0;j<length && bytes[offset+j]==(uint8_t)name[j];++j){}
            if(j==length)gamepad=1;
        }
        offset+=length;
    }
    offset=interfaceOffset;n=bytes[offset++];
    if(n>(size-offset)/16u)return 0;
    for(i=0;i<n;++i,offset+=16) {
        uint32_t j;
        for(j=0;j<16 && bytes[offset+j]==gamepadGuid[j];++j){}
        if(j==16)gamepad=1;
    }
    offset=commandOffset;n=bytes[offset++];
    if(!n)return 0;
    for(i=0;i<n;++i) {
        if(size-offset<2)return 0;
        length=IcNativeRead16(bytes+offset);
        if(length<8 || length>size-offset)return 0;
        if(bytes[offset+2]==0x20) {
            uint32_t payload=IcNativeRead16(bytes+offset+3);
            if(input || payload<14 || payload>IC_GIP_REPORT_MAX || IcNativeRead16(bytes+offset+5)!=1 ||
               !(bytes[offset+7]&0x10) || (bytes[offset+7]&3))return 0;
            input=payload;
        }
        offset+=length;
    }
    return gamepad?input:0;
}
static __inline int IcGipMetadataGamepad(const uint8_t* bytes,uint32_t size) {
    return IcGipMetadataGamepadLength(bytes,size)!=0;
}
// A confirmed IGamepad/preferred gamepad type establishes the 14-byte standard
// prefix. Firmware can append bytes beyond the declared payload, or declare
// the complete frame length instead of its payload (observed: declared 50,
// actual 50-byte frame / 46-byte payload). Accept that exact header-inclusive
// convention too, without treating arbitrary short payloads as complete.
// This is gated by structurally validated gamepad metadata at the caller;
// extensions remain opaque and intact, independent of vendor/product IDs.
static __inline int IcGipDecodeGamepadReport(const uint8_t* bytes,uint32_t size,uint32_t declared,
                                            IC_NATIVE_GAMEPAD_STATE* out) {
    uint8_t base[IC_GIP_INPUT_BYTES];uint32_t i;
    if(!bytes || !out || declared<14 || declared>IC_GIP_REPORT_MAX || size<IC_GIP_INPUT_BYTES ||
       size>IC_GIP_REPORT_MAX || bytes[3]!=size-4 || (size-4<declared && size!=declared))return 0;
    for(i=0;i<IC_GIP_INPUT_BYTES;++i)base[i]=bytes[i];
    base[3]=14;return IcGipDecodeGamepad(base,sizeof(base),1,out);
}
static __inline int IcGipReplaceGamepadReport(uint8_t* bytes,uint32_t size,uint32_t declared,
                                             const IC_NATIVE_GAMEPAD_STATE* state) {
    uint8_t base[IC_GIP_INPUT_BYTES];uint32_t i;IC_NATIVE_GAMEPAD_STATE original;
    if(!IcGipDecodeGamepadReport(bytes,size,declared,&original))return 0;
    for(i=0;i<IC_GIP_INPUT_BYTES;++i)base[i]=bytes[i];
    base[3]=14;
    if(!IcGipReplaceGamepad(base,sizeof(base),1,state))return 0;
    for(i=4;i<IC_GIP_INPUT_BYTES;++i)bytes[i]=base[i];
    return 1;
}
// Returns -1 for invalid metadata, 0 for incomplete/non-metadata, 1 on complete.
// Duplicate contiguous fragments are accepted only when their bytes match.
// Caller forwards every original protocol packet irrespective of this result.
static __inline int IcGipObserveMetadata(IC_GIP_METADATA* state,const uint8_t* bytes,uint32_t size) {
    IC_GIP_FRAME frame;uint32_t offset,i;
    if(!state || !IcGipParseFrame(bytes,size,&frame))return -1;
    if(frame.command!=4 || !(frame.flags&0x20) || (frame.flags&7))return 0;
    if(!(frame.flags&0x80)) {
        if(!frame.payload)return 0; // downstream metadata request
        // The protocol permits a complete response without chunking too.
        state->active=state->complete=state->gamepad=0;
        if(frame.payload>IC_GIP_METADATA_MAX)return -1;
        state->total=state->received=frame.payload;state->sequence=frame.sequence;
        for(i=0;i<frame.payload;++i)state->bytes[i]=bytes[frame.header+i];
        state->complete=1;state->input_length=IcGipMetadataGamepadLength(state->bytes,state->total);
        state->gamepad=(uint8_t)(state->input_length!=0);return 1;
    }
    if(frame.flags&0x40) {
        state->active=state->complete=state->gamepad=0;
        if(!frame.tlo || frame.tlo>IC_GIP_METADATA_MAX || !frame.payload || frame.payload>frame.tlo)return -1;
        state->total=frame.tlo;state->received=0;state->sequence=frame.sequence;state->active=1;offset=0;
    }else{
        if(!state->active || frame.sequence!=state->sequence)return -1;
        if(!frame.payload) {
            if(state->received!=state->total || frame.tlo!=state->total)return -1;
            state->active=0;state->complete=1;
            state->input_length=IcGipMetadataGamepadLength(state->bytes,state->total);
            state->gamepad=(uint8_t)(state->input_length!=0);return 1;
        }
        offset=frame.tlo;
    }
    if(offset>state->received || offset>state->total || frame.payload>state->total-offset)return -1;
    if(offset<state->received) {
        if(frame.payload>state->received-offset)return -1;
        for(i=0;i<frame.payload;++i)if(state->bytes[offset+i]!=bytes[frame.header+i])return -1;
        return 0;
    }
    for(i=0;i<frame.payload;++i)state->bytes[offset+i]=bytes[frame.header+i];
    state->received+=frame.payload;return 0;
}
