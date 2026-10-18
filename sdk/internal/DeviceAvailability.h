#pragma once
#include "DeviceQuery.h"

// One interpretation for all six backends. Ready describes capture startup,
// never recent activity. A GIP report is required to validate its wire shape,
// not to prove the user moved a stick. An idle established source stays ready.
static __inline void IcDeviceAvailability(IC_DEVICE_QUERY_RESULT* result,uint32_t extra) {
    const uint32_t f=result->flags;uint32_t reasons=extra,state;
    if(!(f&1u) || extra) {
        if(result->type<=IC_TYPE_MOUSE) {
            if(!(f&16u))reasons|=IC_DEVICE_WAIT_CONNECTION;
            if(!(f&2u) || !(f&1u))reasons|=IC_DEVICE_WAIT_READER;
        } else {
            if(!(f&4u))reasons|=IC_DEVICE_WAIT_POWER;
            if(!(f&2u))reasons|=IC_DEVICE_WAIT_READER;
            if(result->type==IC_TYPE_CONTROLLER_GIP) {
                if(!(f&128u))reasons|=IC_DEVICE_WAIT_HELLO;
                if(!(f&512u))reasons|=IC_DEVICE_WAIT_METADATA;
                if(!(f&8u))reasons|=IC_DEVICE_WAIT_FIRST_REPORT;
                if((f&64u) || ((f&512u) && !(f&1024u)))reasons|=IC_DEVICE_PROFILE_REJECTED;
            } else if(result->type==IC_TYPE_CONTROLLER_HID || result->type==IC_TYPE_CONTROLLER_XINPUTHID) {
                if(!(f&16u))reasons|=IC_DEVICE_WAIT_PROFILE;
                if(f&64u)reasons|=IC_DEVICE_TRANSPARENT;
            } else if(!(f&1u) && !extra)reasons|=IC_DEVICE_WAIT_READER;
        }
    }
    if((f&1u) && !reasons)state=IC_DEVICE_READY;
    else if(reasons&IC_DEVICE_PROFILE_REJECTED)state=IC_DEVICE_UNSUPPORTED;
    else if(reasons&(IC_DEVICE_WAIT_POWER|IC_DEVICE_TRANSPARENT))state=IC_DEVICE_UNAVAILABLE;
    else state=IC_DEVICE_PREPARING;
    result->reserved[0]=state;result->reserved[1]=reasons;
    // In both query versions, bit 0 must agree with actual capture readiness.
    if(state!=IC_DEVICE_READY)result->flags&=~1u;
}
