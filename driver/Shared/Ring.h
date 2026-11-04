#pragma once
#include "InterceptorProtocol.h"
#ifndef _KERNEL_MODE
#include <string.h>
#endif
// Caller provides synchronization. No allocation, overwrite or reordering.
typedef struct IC_RING_INDEX { uint32_t head, count; } IC_RING_INDEX;
static __inline uint32_t IcRingTail(const IC_RING_INDEX* r) { return (r->head+r->count)&(IC_CAPACITY-1u); }
static __inline uint32_t IcRingTakeCount(const IC_RING_INDEX* r,uint32_t requested) {
    return !requested || requested>r->count ? r->count : requested;
}
static __inline void IcRingConsume(IC_RING_INDEX* r,uint32_t n) {
    r->head=(r->head+n)&(IC_CAPACITY-1u);r->count-=n;
}
// Return -1 without consuming anything when the entire requested snapshot does
// not fit. Caller holds the same lock used by the producer throughout this call.
static __inline int32_t IcRingRead(IC_RING_INDEX* r,const void* storage,uint32_t stride,
                                 void* out,uint32_t capacity,uint32_t requested) {
    uint32_t n=IcRingTakeCount(r,requested),first;
    if(n>capacity)return -1;
    first=n<IC_CAPACITY-r->head?n:IC_CAPACITY-r->head;
    if(n) {
#ifdef _KERNEL_MODE
        RtlCopyMemory(out,(const unsigned char*)storage+(size_t)r->head*stride,(size_t)first*stride);
        RtlCopyMemory((unsigned char*)out+(size_t)first*stride,storage,(size_t)(n-first)*stride);
#else
        memcpy(out,(const unsigned char*)storage+(size_t)r->head*stride,(size_t)first*stride);
        memcpy((unsigned char*)out+(size_t)first*stride,storage,(size_t)(n-first)*stride);
#endif
    }
    IcRingConsume(r,n);return (int32_t)n;
}
