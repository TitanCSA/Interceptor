#pragma once
#include "InterceptorProtocol.h"

// Optional ABI4 extension. Queries never start, pause or reset the trial.
#define IOCTL_IC_TRIAL_STATUS IC_IOCTL(0xA2D)
#define IC_TRIAL_VERSION 1u
#define IC_TRIAL_NOT_STARTED 0u
#define IC_TRIAL_ACTIVE 1u
#define IC_TRIAL_EXPIRED 2u
#define IC_TRIAL_UNAVAILABLE 3u
#define IC_TRIAL_DURATION_100NS (7200ull*10000000ull)
// STATUS_EVALUATION_EXPIRATION -> ERROR_EVALUATION_EXPIRATION.
#define IC_ERROR_TRIAL_EXPIRED 622u
typedef struct IC_TRIAL_STATUS {
    uint32_t size,version,state,error;
    uint64_t duration_100ns,first_context_100ns,elapsed_100ns,remaining_100ns;
    uint32_t active_mask,reserved[3];
} IC_TRIAL_STATUS;
#if defined(__cplusplus)
static_assert(sizeof(IC_TRIAL_STATUS)==64,"Trial status extension ABI");
#endif
