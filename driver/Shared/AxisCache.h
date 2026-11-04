#pragma once
#include "InterceptorProtocol.h"
// Store the first matching descriptor, as the original linear search did.
// Slots contain descriptor index + 1; zero means no matching value capability.
static __inline void IcCacheAxes(uint16_t slots[4],uint16_t index,uint16_t page,
                                uint16_t first,uint16_t last) {
    static const uint16_t pages[4]={1,1,1,0x0C},usages[4]={0x30,0x31,0x38,0x238};
    unsigned axis;
    for(axis=0;axis<4;++axis)
        if(!slots[axis] && page==pages[axis] && first<=usages[axis] && usages[axis]<=last)
            slots[axis]=(uint16_t)(index+1);
}
