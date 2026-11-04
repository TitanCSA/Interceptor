#pragma once
#include "Trial.h"

// Pure arithmetic shared by the kernel and boundary tests. The caller owns
// synchronization and a separate started bit (zero is a valid start time).
static __inline uint64_t IcTrialElapsed(uint64_t first,uint64_t now) {
    return now>=first?now-first:0;
}
static __inline uint64_t IcTrialRemaining(uint64_t first,uint64_t now,uint64_t duration) {
    const uint64_t elapsed=IcTrialElapsed(first,now);
    return elapsed>=duration?0:duration-elapsed;
}
static __inline int IcTrialPastDeadline(uint64_t first,uint64_t now,uint64_t duration) {
    // A monotonic-clock rollback indicates corrupt state, never a new budget.
    return now<first || IcTrialElapsed(first,now)>=duration;
}
