#pragma once
#include "ControllerMonitor.h"
// Pure bounded cursor arithmetic shared by production and portable tests.
static __inline uint32_t IcMonitorTake(uint64_t written,uint64_t* cursor,uint64_t* dropped,uint32_t maximum) {
    uint64_t oldest=written>IC_MONITOR_CAPACITY?written-IC_MONITOR_CAPACITY:0;
    uint64_t available;
    if(*cursor<oldest){*dropped+=oldest-*cursor;*cursor=oldest;}
    available=written-*cursor;
    return (uint32_t)(available<maximum?available:maximum);
}
