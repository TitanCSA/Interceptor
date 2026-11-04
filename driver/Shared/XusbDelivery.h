#pragma once
#include "ControllerNativeProtocol.h"

// A published stream has its own counter: mixing physical and injected packet
// numbers can assign the same number to different gamepad states. Call under
// the device's publication lock, for already validated 29-byte envelopes.
// Preserve the first physical number, then count changes to gamepad fields only.
// Transport headers, reserved bytes and source/caller packet numbers do not
// represent a gamepad change. Unsigned overflow is the normal DWORD wrap.
static __inline void IcXusbPublishState(uint8_t delivered[IC_XUSB_STATE_BYTES],int valid,
                                      const uint8_t input[IC_XUSB_STATE_BYTES]) {
    uint32_t packet=IcNativeRead32(input+5),i;
    if(valid) {
        int changed=delivered[28]!=input[28];
        for(i=11;i<23;++i)if(delivered[i]!=input[i]){changed=1;break;}
        packet=IcNativeRead32(delivered+5)+(uint32_t)changed;
    }
    for(i=0;i<IC_XUSB_STATE_BYTES;++i)delivered[i]=input[i];
    IcNativeWrite32(delivered+5,packet);
}
