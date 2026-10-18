#pragma once
#include "InterceptorProtocol.h"
// Selected input-read request. Queue implementation is private to the driver.
#define IC_GET_PEEK 1u
typedef struct IC_GET_SELECT {IC_GET base;uint32_t device,flags;} IC_GET_SELECT;
#if defined(__cplusplus)
static_assert(sizeof(IC_GET_SELECT)==16,"Selected GET request ABI");
#endif
