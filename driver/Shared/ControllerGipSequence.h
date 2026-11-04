#pragma once
#include "ControllerGipProtocol.h"

// MS-GIPUSB 3.1.5.1: reliable-message timeout is 1000ms. Command sequence
// numbers are shared by Guide and the other Command messages; Protocol Control
// instead echoes the sequence of the message acknowledged in the other direction.
// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/5a4be035-9e20-4350-b81a-a08627a985ca
// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/b52ea191-fce4-45bc-bc92-6bc43e94f010
// Caller serializes access and supplies monotonic time before each operation.
// No timer or allocation is needed. Full windows apply backpressure, rather than
// aliasing an in-flight ACK to a different physical message after 8-bit wrap.
#define IC_GIP_SEQUENCE_WINDOW_MS 1000u
#define IC_GIP_ACK_REJECT 0
#define IC_GIP_ACK_FORWARD 1
#define IC_GIP_ACK_CONSUME 2
typedef struct IC_GIP_SEQUENCE_ENTRY {
    uint64_t until;
    uint32_t total,epoch;
    uint8_t source,command,address,valid;
    uint8_t active,fragmented,synthetic,reserved;
} IC_GIP_SEQUENCE_ENTRY;
typedef struct IC_GIP_SEQUENCE_STATE {
    uint64_t now;
    uint64_t ackGeneration;
    uint32_t sourceEpoch;
    uint8_t last,lastSource,reserved[2];
    uint32_t ackPins[256];
    IC_GIP_SEQUENCE_ENTRY entries[256];
} IC_GIP_SEQUENCE_STATE;

static __inline void IcGipSequenceReset(IC_GIP_SEQUENCE_STATE* state) {
    const uint64_t generation=state->ackGeneration+1;
    memset(state,0,sizeof(*state));state->ackGeneration=generation;
}

// USB supplies Command messages in order. A retransmission may refer to a
// recently preceding source number; use serial-number arithmetic to distinguish
// it from the next wrap. Call Track for physically suppressed Guide messages too,
// so the source counter remains observed independently of delivered messages.
static __inline uint32_t IcGipSequenceSourceEpoch(const IC_GIP_SEQUENCE_STATE* state,uint8_t source,int* advances) {
    uint32_t distance=(source+255u-state->lastSource)%255u;
    *advances=!state->lastSource || (distance && distance<=127);
    if(*advances)return state->sourceEpoch+(state->lastSource && source<state->lastSource?1u:0u);
    return state->sourceEpoch-(source>state->lastSource && state->sourceEpoch?1u:0u);
}
static __inline void IcGipSequenceTrack(IC_GIP_SEQUENCE_STATE* state,uint8_t source) {
    int advances;uint32_t epoch;
    if(!state || !source)return;
    epoch=IcGipSequenceSourceEpoch(state,source,&advances);
    if(advances){state->lastSource=source;state->sourceEpoch=epoch;}
}

static __inline void IcGipSequenceSetTime(IC_GIP_SEQUENCE_STATE* state,uint64_t milliseconds) {
    if(state && milliseconds>state->now)state->now=milliseconds;
}
static __inline uint64_t IcGipSequenceExpiry(const IC_GIP_SEQUENCE_STATE* state) {
    const uint64_t maximum=~(uint64_t)0;
    return state->now>maximum-IC_GIP_SEQUENCE_WINDOW_MS?maximum:state->now+IC_GIP_SEQUENCE_WINDOW_MS;
}
static __inline uint8_t IcGipSequenceAllocate(IC_GIP_SEQUENCE_STATE* state) {
    uint32_t n;uint8_t sequence=state->last;
    for(n=0;n<255;++n) {
        IC_GIP_SEQUENCE_ENTRY* entry;
        if(!++sequence)++sequence;
        entry=&state->entries[sequence];
        if(!state->ackPins[sequence] && (!entry->valid || (!entry->active && entry->until<=state->now))) {
            state->last=sequence;return sequence;
        }
    }
    return 0;
}
// Zero means only an ACK/protocol event can make a slot available. A positive
// delay is an exact expiry wakeup, to combine with the caller's maintenance event.
static __inline uint32_t IcGipSequenceWaitMilliseconds(const IC_GIP_SEQUENCE_STATE* state) {
    uint64_t earliest=~(uint64_t)0;uint32_t i;
    if(!state)return 0;
    for(i=1;i<256;++i) {
        const IC_GIP_SEQUENCE_ENTRY* entry=&state->entries[i];
        if(state->ackPins[i])continue;
        if(!entry->valid || (!entry->active && entry->until<=state->now))return 1;
        if(!entry->active && entry->until<earliest)earliest=entry->until;
    }
    if(earliest==~(uint64_t)0)return 0;
    return (uint32_t)(earliest-state->now);
}
// Returns 0 without changing the packet/state if the bounded window is full.
// Invalid framing passes unchanged for the caller's normal protocol validation.
// A fragmented transfer retains its number through ACKs and retransmissions,
// until its zero-payload completion. Completed mappings remain for the timeout
// window, including duplicate completions and unsolicited/late ACKs.
static __inline int IcGipSequenceObserveMode(IC_GIP_SEQUENCE_STATE* state,uint8_t* bytes,uint32_t size,int physical) {
    IC_GIP_FRAME frame;IC_GIP_SEQUENCE_ENTRY* entry=NULL;uint32_t i,epoch;uint8_t sequence=0,address;int advances;
    if(!state || !bytes)return 0;
    if(!IcGipParseFrame(bytes,size,&frame) || frame.command>=0x20 || frame.command==1)return 1;
    address=(uint8_t)(frame.flags&0x27);
    epoch=IcGipSequenceSourceEpoch(state,frame.sequence,&advances);
    for(i=1;i<256;++i) {
        IC_GIP_SEQUENCE_ENTRY* candidate=&state->entries[i];
        if(candidate->valid && !candidate->synthetic && candidate->source==frame.sequence &&
           candidate->command==frame.command && candidate->address==address &&
           (candidate->active || candidate->epoch==epoch || (candidate->fragmented && (frame.flags&0xC0)==0x80)) &&
           (candidate->active || candidate->until>state->now)) {
            entry=candidate;sequence=(uint8_t)i;
            if(candidate->active || candidate->epoch!=epoch)advances=0; // reserved/retired fragment retransmission
            break;
        }
    }
    if(!entry) {
        // Before this device has ever been captured, retain its exact native
        // Command counter. Identity mappings still record outstanding ACKs,
        // so the first capture can safely introduce synthetic Guide messages.
        if(physical) {
            sequence=frame.sequence;
            if(state->ackPins[sequence])return 0;
            state->last=sequence;
        } else {sequence=IcGipSequenceAllocate(state);if(!sequence)return 0;}
        entry=&state->entries[sequence];
        entry->source=frame.sequence;entry->command=frame.command;entry->address=address;
        entry->valid=1;entry->synthetic=0;entry->reserved=0;
        entry->fragmented=(uint8_t)((frame.flags&0x80)!=0);
        entry->active=(uint8_t)(entry->fragmented || (frame.flags&0x10)!=0);
        entry->total=entry->fragmented?frame.tlo:frame.payload;entry->epoch=epoch;
    }
    if((frame.flags&0xC0)==0xC0)entry->total=frame.tlo;
    // An OUT ACK may have failed after the caller prepared it. A physical retry
    // makes the transfer outstanding again, even if a previous ACK retired it.
    if(frame.flags&0x90)entry->active=1;
    if((frame.flags&0xC0)==0x80 && !frame.payload)entry->active=0;
    if(advances){state->lastSource=frame.sequence;state->sourceEpoch=epoch;}
    entry->until=IcGipSequenceExpiry(state);bytes[2]=sequence;return 1;
}
static __inline int IcGipSequenceObserve(IC_GIP_SEQUENCE_STATE* state,uint8_t* bytes,uint32_t size) {
    return IcGipSequenceObserveMode(state,bytes,size,0);
}
static __inline int IcGipSequenceObservePhysical(IC_GIP_SEQUENCE_STATE* state,uint8_t* bytes,uint32_t size) {
    return IcGipSequenceObserveMode(state,bytes,size,1);
}
static __inline int IcGipSequenceGuide(IC_GIP_SEQUENCE_STATE* state,uint8_t* output) {
    uint8_t sequence;IC_GIP_SEQUENCE_ENTRY* entry;
    if(!state || !output)return 0;
    sequence=IcGipSequenceAllocate(state);if(!sequence)return 0;
    entry=&state->entries[sequence];entry->until=IcGipSequenceExpiry(state);entry->total=2;
    entry->source=0;entry->command=7;entry->address=0x20;entry->valid=1;
    entry->active=0;entry->fragmented=0;entry->synthetic=1;entry->reserved=0;entry->epoch=0;
    *output=sequence;return 1;
}
// Only Protocol Control ACK (control code zero) is translated. Other control
// codes remain untouched. ACKs for synthetic Guide messages terminate locally.
// A rejected ACK must not be forwarded with an unrelated physical sequence.
static __inline int IcGipSequenceAckAction(const IC_GIP_SEQUENCE_STATE* state,const uint8_t* bytes,uint32_t size) {
    IC_GIP_FRAME frame;const IC_GIP_SEQUENCE_ENTRY* entry;
    if(!state || !bytes)return IC_GIP_ACK_REJECT;
    if(!size || bytes[0]!=1)return IC_GIP_ACK_FORWARD;
    if(!IcGipParseFrame(bytes,size,&frame) || size!=13 || frame.header!=4 || frame.payload!=9 ||
       (frame.flags&0xD8))return IC_GIP_ACK_REJECT;
    if(bytes[4]!=0)return IC_GIP_ACK_FORWARD;
    entry=&state->entries[frame.sequence];
    if(!entry->valid || entry->command!=bytes[5] || entry->address!=(bytes[6]&0x27) ||
       (frame.flags&7)!=(entry->address&7))return IC_GIP_ACK_REJECT;
    if(entry->synthetic)return IC_GIP_ACK_CONSUME;
    return IC_GIP_ACK_FORWARD;
}
static __inline int IcGipSequenceAck(IC_GIP_SEQUENCE_STATE* state,uint8_t* bytes,uint32_t size) {
    IC_GIP_SEQUENCE_ENTRY* entry;uint32_t received;
    const int action=IcGipSequenceAckAction(state,bytes,size);
    if(action!=IC_GIP_ACK_FORWARD || !size || bytes[0]!=1 || bytes[4]!=0)return action;
    entry=&state->entries[bytes[2]];
    received=IcNativeRead16(bytes+7);
    if(!entry->fragmented && received>=entry->total && !IcNativeRead16(bytes+11)) {
        entry->active=0;entry->until=IcGipSequenceExpiry(state);
    }
    bytes[2]=entry->source;return IC_GIP_ACK_FORWARD;
}
