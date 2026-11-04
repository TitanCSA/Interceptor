#pragma once
#include "ControllerHid.h"

// Capture storage must be zero-initialized and written only through this helper.
// Lengths are validated by the descriptor before the reader starts. Reusing a
// slot preserves its zero tail; when a shorter report replaces a longer one,
// erase only the bytes that used to contain payload. The full fixed-size ABI
// can then be copied to user mode without exposing a previous report's tail.
static __inline void IcHidWriteCapture(IC_CONTROLLER_HID_INPUT* slot,
    uint64_t sequence,uint64_t timestamp,uint32_t device,const uint8_t* report,uint32_t length) {
    const uint32_t previous=slot->length;
    if(previous>length) {
#ifdef _KERNEL_MODE
        RtlZeroMemory(slot->report+length,previous-length);
#else
        memset(slot->report+length,0,previous-length);
#endif
    }
    slot->sequence=sequence;slot->timestamp=timestamp;slot->device=device;slot->length=length;
#ifdef _KERNEL_MODE
    RtlCopyMemory(slot->report,report,length);
#else
    memcpy(slot->report,report,length);
#endif
}
