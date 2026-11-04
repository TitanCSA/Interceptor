#pragma once
#include "InterceptorProtocol.h"
#ifndef _KERNEL_MODE
#include <string.h>
#endif
// Additive request for the existing GET IOCTLs. The 8-byte IC_GET and every
// input record retain their ABI. Flags=0 consumes; PEEK never alters the FIFO.
#define IC_GET_PEEK 1u
typedef struct IC_GET_SELECT {IC_GET base;uint32_t device,flags;} IC_GET_SELECT;
static __inline int IcSelectedGetValid(const IC_GET_SELECT* request) {
    return request && request->base.version==IC_VERSION && !(request->flags&~IC_GET_PEEK);
}
static __inline void IcInputReadCopy(void* out,const void* in,size_t bytes) {
    if(!bytes)return;
#ifdef _KERNEL_MODE
    RtlCopyMemory(out,in,bytes);
#else
    memcpy(out,in,bytes);
#endif
}
static __inline uint32_t IcInputReadDevice(const unsigned char* input,uint32_t offset) {
    uint32_t id;IcInputReadCopy(&id,input+offset,sizeof(id));return id;
}
// Caller holds the producer's lock. No allocation; -1 leaves queue/output
// unchanged if the complete selected snapshot does not fit. Output may be a
// prefix projection (standard GIP) while retained records keep their full tail.
static __inline int32_t IcInputReadSelected(uint32_t* head,uint32_t* count,
    void* storage,uint32_t ringCapacity,uint32_t stride,uint32_t deviceOffset,
    void* output,uint32_t outputStride,uint32_t capacity,uint32_t requested,
    uint32_t device,int consume) {
    unsigned char* data=(unsigned char*)storage;
    unsigned char* out=(unsigned char*)output;
    uint32_t original=*count,n=0,i,taken=0,read=*head,write=*head;
    if(!device) {
        uint32_t first;
        n=!requested || requested>original?original:requested;
        if(n>capacity)return -1;
        if(n==1)IcInputReadCopy(out,data+(size_t)*head*stride,outputStride);
        else if(outputStride==stride) {
            first=n<ringCapacity-*head?n:ringCapacity-*head;
            if(first)IcInputReadCopy(out,data+(size_t)*head*stride,(size_t)first*stride);
            if(first<n)IcInputReadCopy(out+(size_t)first*stride,data,(size_t)(n-first)*stride);
        } else for(i=0;i<n;++i)
            IcInputReadCopy(out+(size_t)i*outputStride,data+(size_t)((*head+i)%ringCapacity)*stride,outputStride);
        if(consume){*head=(*head+n)%ringCapacity;*count=original-n;}
        return (int32_t)n;
    }
    for(i=0;i<original;++i) {
        if(IcInputReadDevice(data+(size_t)read*stride,deviceOffset)==device) {
            ++n;if(requested && n==requested)break;
        }
        if(++read==ringCapacity)read=0;
    }
    if(n>capacity)return -1;
    if(!n)return 0;
    read=*head;
    for(i=0;i<original;++i) {
        unsigned char* source=data+(size_t)read*stride;
        if(taken<n && IcInputReadDevice(source,deviceOffset)==device) {
            IcInputReadCopy(out+(size_t)taken*outputStride,source,outputStride);++taken;
            // Peeking cannot compact the queue. Once the last selected record
            // is copied, the rest of the ring requires no reads or writes.
            if(!consume && taken==n)break;
        } else {
            if(consume && write!=read)IcInputReadCopy(data+(size_t)write*stride,source,stride);
            if(++write==ringCapacity)write=0;
        }
        if(++read==ringCapacity)read=0;
    }
    if(consume)*count=original-n;
    return (int32_t)n;
}
#if defined(__cplusplus)
static_assert(sizeof(IC_GET_SELECT)==16,"Selected GET request ABI");
#endif
