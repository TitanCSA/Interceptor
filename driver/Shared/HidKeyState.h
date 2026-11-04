#pragma once
#include "InterceptorProtocol.h"

// Merge one HID keyboard report into the union of all report IDs. A usage can
// be present in several IDs, so releasing it in one report must not release it
// globally while another still owns it. All arrays start at zero. The input
// bitmap contains only keyboard usages 4..255 (rollover is handled by caller).
// Returns whether the merged state changed, not merely the report's ownership.
static __inline int IcHidMergeKeyReport(uint8_t previous[32],const uint8_t report[32],
    uint16_t owners[256],uint8_t current[256]) {
    unsigned byte;int changed=0;
    for(byte=0;byte<32;++byte) {
        const unsigned delta=previous[byte]^report[byte];unsigned bit;
        if(!delta)continue;
        previous[byte]=report[byte];
        for(bit=0;bit<8;++bit)if(delta&(1u<<bit)) {
            const unsigned usage=byte*8+bit;
            const uint8_t before=current[usage];
            if(report[byte]&(1u<<bit))++owners[usage];else --owners[usage];
            current[usage]=(uint8_t)(owners[usage]!=0);
            changed|=(before!=current[usage]);
        }
    }
    return changed;
}
