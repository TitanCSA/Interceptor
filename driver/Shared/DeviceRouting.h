#pragma once
#include "InterceptorProtocol.h"
// One-based ID within a category. Zero return means no available destination.
static __inline uint32_t IcResolveDeviceId(uint32_t requested,uint32_t count) {
    return !count?0:requested && requested<=count?requested:1;
}
