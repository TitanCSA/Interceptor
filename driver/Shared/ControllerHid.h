#pragma once
#include "InterceptorProtocol.h"
#ifndef _KERNEL_MODE
#include <string.h>
#endif
#define IC_CONTROLLER_HID 1u
#define IC_CONTROLLER_XUSB 2u
#define IC_CONTROLLER_XINPUTHID 4u
#define IC_CONTROLLER_GIP 8u
#define IC_CONTROLLER_ALL 15u
#define IC_READY_HID 4u
#define IC_STREAM_COUNT 6u
#define IC_ALL_STREAMS 63u
#define IC_HID_REPORT_MAX 1024u
#define IC_HID_CAPACITY 256u
#define IC_HID_FILE_CAPACITY 16u
#define IOCTL_IC_GET_HID IC_IOCTL(0xA09)
#define IOCTL_IC_SEND_HID IC_IOCTL(0xA0A)
#define IOCTL_IC_SEND_OWNED_HID IC_IOCTL(0xA0B)
#define IOCTL_IC_HID_DESCRIPTOR IC_IOCTL(0xA0C)
#define IOCTL_IC_HID_SOURCE IC_IOCTL(0xA0D)
// Collection-level reports, including byte zero (Report ID, zero if absent).
typedef struct IC_CONTROLLER_HID_INPUT {
    uint64_t sequence,timestamp;
    uint32_t device,length;
    uint32_t reserved[2];
    uint8_t report[IC_HID_REPORT_MAX];
} IC_CONTROLLER_HID_INPUT;
// {9CE34440-4716-4F39-922B-995C28A52C87}
#define IC_HID_INTERFACE_GUID IC_PRODUCT_HID_GUID
static __inline uint32_t IcHidTake(uint32_t available,uint32_t requested) {
    return !requested || requested>available?available:requested;
}
static __inline int IcHidValidReport(const IC_CONTROLLER_HID_INPUT* in) {
    return in && in->length && in->length<=IC_HID_REPORT_MAX && !in->reserved[0] && !in->reserved[1];
}
static __inline int32_t IcHidRead(uint32_t* head,uint32_t* count,const IC_CONTROLLER_HID_INPUT* storage,
                                IC_CONTROLLER_HID_INPUT* output,uint32_t capacity,uint32_t requested) {
    uint32_t n=IcHidTake(*count,requested),first;
    if(n>capacity)return -1;
    first=n<IC_HID_CAPACITY-*head?n:IC_HID_CAPACITY-*head;
    if(n) {
#ifdef _KERNEL_MODE
        RtlCopyMemory(output,storage+*head,(size_t)first*sizeof(*storage));
        if(first<n)RtlCopyMemory(output+first,storage,(size_t)(n-first)*sizeof(*storage));
#else
        memcpy(output,storage+*head,(size_t)first*sizeof(*storage));
        if(first<n)memcpy(output+first,storage,(size_t)(n-first)*sizeof(*storage));
#endif
    }
    *head=(*head+n)%IC_HID_CAPACITY;*count-=n;return (int32_t)n;
}
static __inline int IcHidInvertAxis(uint32_t raw,uint16_t bits,int32_t minimum,int32_t maximum,uint32_t* flipped) {
    __int64 value=raw;
    if(!bits || bits>32 || maximum<=minimum || !flipped)return 0;
    if(bits<32) {raw&=(1u<<bits)-1u;value=raw;}
    if(minimum<0 && (raw&(1u<<(bits-1))))value-=((__int64)1<<bits);
    if(value<minimum || value>maximum)return 0;
    *flipped=(uint32_t)((__int64)minimum+maximum-value);
    return 1;
}
#if defined(__cplusplus)
static_assert(sizeof(IC_CONTROLLER_HID_INPUT)==1056,"HID report ABI");
#endif
