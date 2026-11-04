#pragma once
#include "ControllerGipSequence.h"

// Bound one URB independently of the interrupt endpoint's packet size. USB can
// carry several GIP frames in one transfer; every ACK still requires translation.
// No allocation occurs here. The caller owns the mutable copy and serializes
// sequence state for both passes. Rejected transfers leave bytes/state unchanged.
#define IC_GIP_OUT_MAX 65536u
typedef struct IC_GIP_ACK_TICKET {
    uint64_t generation;
    uint8_t pins[32],retire[32];
} IC_GIP_ACK_TICKET;
static __inline int IcGipOutHasPins(const IC_GIP_ACK_TICKET* ticket) {
    uint32_t i;for(i=0;i<32;++i)if(ticket->pins[i])return 1;return 0;
}

static __inline uint32_t IcGipFrameLength(const uint8_t* bytes,uint32_t remaining) {
    uint32_t cursor=3,payload,ignored;
    if(!bytes || remaining<4 || !IcGipVarint(bytes,remaining,&cursor,&payload))return 0;
    if((bytes[1]&0x80) && !IcGipVarint(bytes,remaining,&cursor,&ignored))return 0;
    if(payload>remaining-cursor)return 0;
    return cursor+payload;
}
// 1 = prepared, 0 = invalid, -1 = pin counter busy. One pin per unique sequence
// per URB, even for duplicate ACK frames. A pin prevents reuse during USB I/O.
static __inline int IcGipPrepareOut(IC_GIP_SEQUENCE_STATE* state,uint8_t* bytes,uint32_t length,IC_GIP_ACK_TICKET* ticket,uint32_t* written) {
    uint32_t offset=0,kept=0,i;IC_GIP_ACK_TICKET proposed={0};
    if(!state || !bytes || !ticket || !written || !length || length>IC_GIP_OUT_MAX)return 0;
    while(offset<length) {
        const uint32_t frame=IcGipFrameLength(bytes+offset,length-offset);
        const int action=frame?IcGipSequenceAckAction(state,bytes+offset,frame):IC_GIP_ACK_REJECT;
        if(action==IC_GIP_ACK_REJECT)return 0;
        if(action==IC_GIP_ACK_FORWARD && bytes[offset]==1 && bytes[offset+4]==0) {
            const uint8_t sequence=bytes[offset+2];const IC_GIP_SEQUENCE_ENTRY* entry=&state->entries[sequence];
            proposed.pins[sequence/8]|=(uint8_t)(1u<<(sequence%8));
            if(!entry->fragmented && IcNativeRead16(bytes+offset+7)>=entry->total && !IcNativeRead16(bytes+offset+11))
                proposed.retire[sequence/8]|=(uint8_t)(1u<<(sequence%8));
        }
        offset+=frame;
    }
    for(i=1;i<256;++i)if((proposed.pins[i/8]&(1u<<(i%8))) && state->ackPins[i]==0xffffffffu)return -1;
    for(i=1;i<256;++i)if(proposed.pins[i/8]&(1u<<(i%8)))++state->ackPins[i];
    proposed.generation=state->ackGeneration;
    offset=0;
    while(offset<length) {
        const uint32_t frame=IcGipFrameLength(bytes+offset,length-offset);
        const int action=IcGipSequenceAckAction(state,bytes+offset,frame);
        if(action==IC_GIP_ACK_FORWARD) {
            if(bytes[offset]==1 && bytes[offset+4]==0)bytes[offset+2]=state->entries[bytes[offset+2]].source;
            // memmove permits compaction after an ACK terminated locally.
            memmove(bytes+kept,bytes+offset,frame);kept+=frame;
        }
        offset+=frame;
    }
    *ticket=proposed;*written=kept;return 1;
}
// Called under the same serialization as preparation, only after the lower USB
// result is known. Retired tables never receive an old completion's decrements.
static __inline void IcGipCompleteOut(IC_GIP_SEQUENCE_STATE* state,IC_GIP_ACK_TICKET* ticket,int succeeded) {
    uint32_t i;
    if(state->ackGeneration==ticket->generation)for(i=1;i<256;++i) {
        if(!(ticket->pins[i/8]&(1u<<(i%8))))continue;
        if(state->ackPins[i])--state->ackPins[i];
        if(succeeded && (ticket->retire[i/8]&(1u<<(i%8)))) {
            state->entries[i].active=0;state->entries[i].until=IcGipSequenceExpiry(state);
        }
    }
    memset(ticket,0,sizeof(*ticket));
}
// Synchronous convenience for pure codec callers. Kernel OUT uses the two
// phases above and commits only on a complete successful USB transfer.
static __inline int IcGipTranslateOut(IC_GIP_SEQUENCE_STATE* state,uint8_t* bytes,uint32_t length,uint32_t* written) {
    IC_GIP_ACK_TICKET ticket;
    if(IcGipPrepareOut(state,bytes,length,&ticket,written)!=1)return 0;
    IcGipCompleteOut(state,&ticket,1);return 1;
}
