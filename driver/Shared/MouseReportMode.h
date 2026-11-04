#pragma once
#include "InterceptorProtocol.h"

#define IC_MOUSE_REPORT_X 1u
#define IC_MOUSE_REPORT_Y 2u
#define IC_MOUSE_REPORT_ABSOLUTE 4u
#define IC_MOUSE_REPORT_MIXED 8u

// MOUSE_INPUT_DATA has one coordinate mode for both axes. A mixed mode within
// one report cannot be represented faithfully; retain the existing absolute
// packet behavior for that case, but identify it separately from supported
// homogeneous reports. Different report IDs may use different modes safely.
static __inline uint8_t IcMouseReportMode(int hasX,int absoluteX,int hasY,int absoluteY) {
    uint8_t mode=(uint8_t)((hasX?IC_MOUSE_REPORT_X:0u)|(hasY?IC_MOUSE_REPORT_Y:0u));
    if((hasX && absoluteX) || (hasY && absoluteY))mode|=IC_MOUSE_REPORT_ABSOLUTE;
    if(hasX && hasY && !!absoluteX!=!!absoluteY)mode|=IC_MOUSE_REPORT_MIXED;
    return mode;
}
