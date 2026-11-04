#pragma once
// Private lab ABI. No trace storage, IOCTL extension or recording is compiled
// into a normal build. VM fixture builds may retain exact GET_STATE responses.
#if defined(IC_COMPAT_TRACE)
#if !defined(_KERNEL_MODE)
#include <stdint.h>
#endif
#define IC_XUSB_COMPAT_TRACE_SCHEMA 2u
#define IC_XUSB_COMPAT_TRACE_CAPACITY 64u
#define IC_XUSB_COMPAT_ERROR_CAPACITY 8u
enum IC_XUSB_COMPAT_STAGE {
    IC_XUSB_TRACE_CALLER=1, IC_XUSB_TRACE_DISPATCH=2,
    IC_XUSB_TRACE_COMPLETE=3, IC_XUSB_TRACE_FORWARD=4
};
enum IC_XUSB_COMPAT_ROUTE {
    IC_XUSB_TRACE_SEEN=0, IC_XUSB_TRACE_IRQL=1,
    IC_XUSB_TRACE_ENQUEUE_FAILED=2, IC_XUSB_TRACE_ACCESS=3,
    IC_XUSB_TRACE_FORMAT=4, IC_XUSB_TRACE_NOT_READY=5,
    IC_XUSB_TRACE_CACHE=6, IC_XUSB_TRACE_PENDING=7,
    IC_XUSB_TRACE_NATIVE=8, IC_XUSB_TRACE_BROADCAST=9,
    IC_XUSB_TRACE_DRAIN=10, IC_XUSB_TRACE_NATIVE_GET=11
};
typedef struct IC_XUSB_COMPAT_RECORD {
    uint64_t sequence,qpc,request_id,requestor_pid,current_pid,current_thread;
    uint32_t code,input_bytes,output_bytes;
    int32_t status;
    uint64_t information;
    uint32_t capture_id;
    uint8_t input[8],stage,route,requestor_mode,system_access;
    uint8_t output[29],output_copied,padding[2];
} IC_XUSB_COMPAT_RECORD;
typedef struct IC_XUSB_COMPAT_TRACE {
    uint32_t schema,record_bytes,count,capacity;
    uint64_t next_sequence,qpc_frequency;
    uint32_t error_count,error_capacity;
    IC_XUSB_COMPAT_RECORD records[IC_XUSB_COMPAT_TRACE_CAPACITY];
    IC_XUSB_COMPAT_RECORD first_errors[IC_XUSB_COMPAT_ERROR_CAPACITY];
} IC_XUSB_COMPAT_TRACE;
#if defined(__cplusplus)
static_assert(sizeof(IC_XUSB_COMPAT_RECORD)==120,"Private XUSB trace record ABI");
static_assert(sizeof(IC_XUSB_COMPAT_TRACE)==8680,"Private XUSB trace snapshot ABI");
#endif
#endif
