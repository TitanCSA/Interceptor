#include "Trial.h"
#include "ControllerHid.h"
#include "../Shared/TrialState.h"
#ifdef INTERCEPTOR_TRIAL_LAB_SECONDS
#define TITAN_TRIAL_LAB_SECONDS INTERCEPTOR_TRIAL_LAB_SECONDS
#endif
#include "../TrialClock/Client.h"

// The per-boot start is owned by a kernel-only resident clock. The public
// product driver may unload/reload; registry edits never renew its budget.
static TC_CLIENT trialClock;
static WDFTIMER trialTimer;
static NTSTATUS storageStatus=STATUS_DEVICE_NOT_READY,timerStatus=STATUS_DEVICE_NOT_READY;
static ULONGLONG firstContext;
static BOOLEAN started,expired;
#ifdef INTERCEPTOR_TRIAL_LAB_SECONDS
#if INTERCEPTOR_TRIAL_LAB_SECONDS < 1 || INTERCEPTOR_TRIAL_LAB_SECONDS > 120
#error Accelerated laboratory trial must be between 1 and 120 seconds.
#endif
static const ULONGLONG duration=(ULONGLONG)INTERCEPTOR_TRIAL_LAB_SECONDS*10000000ull;
#else
static const ULONGLONG duration=IC_TRIAL_DURATION_100NS;
#endif
EVT_WDF_TIMER IcTrialTimer;

static ULONGLONG Now(void) {ULONGLONG qpc;return KeQueryInterruptTimePrecise(&qpc);}
static void ArmLocked(ULONGLONG now) {
    if(trialTimer && started && !expired) {
        const ULONGLONG left=IcTrialRemaining(firstContext,now,duration);
        WdfTimerStart(trialTimer,-(LONGLONG)max(left,1ull));
    }
}
static void CheckAtLocked(ULONGLONG now) {
    if(started && !expired && IcTrialPastDeadline(firstContext,now,duration)) {
        expired=TRUE;
        // Existing reset paths release all six FIFO/snapshot backends and
        // restore physical state. Revocation wakes even an infinite SDK wait.
        IcTrialRevokeLocked();
    }
}
void IcTrialCheckLocked(void) {if(started && !expired)CheckAtLocked(Now());}
void IcTrialResumeLocked(void) {
    IcTrialCheckLocked();
    if(started && !expired)ArmLocked(Now());
}
void IcTrialTimer(WDFTIMER timer) {
    WdfWaitLockAcquire(gGate,NULL);
    if(timer==trialTimer) {
        IcTrialCheckLocked();
        // A normal timer may run just before the precise deadline. Rearm
        // only the remainder; never shorten or renew the one-hour budget.
        if(!expired)ArmLocked(Now());
    }
    WdfWaitLockRelease(gGate);
}
void IcTrialInitialize(void) {
    storageStatus=STATUS_DEVICE_NOT_READY;
}
void IcTrialUninitialize(void) {
    IcTrialDetachTimer();
    TcClose(&trialClock);
}
void IcTrialAttachTimer(WDFDEVICE device) {
    WDF_TIMER_CONFIG config;WDF_OBJECT_ATTRIBUTES attributes;WDFTIMER timer=NULL;NTSTATUS status;
    WDF_TIMER_CONFIG_INIT(&config,IcTrialTimer);config.AutomaticSerialization=FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);attributes.ParentObject=device;
    attributes.ExecutionLevel=WdfExecutionLevelPassive;
    status=WdfTimerCreate(&config,&attributes,&timer);
    WdfWaitLockAcquire(gGate,NULL);timerStatus=status;trialTimer=timer;
    IcTrialCheckLocked();if(NT_SUCCESS(status) && started && !expired)ArmLocked(Now());
    WdfWaitLockRelease(gGate);
    // Timer/storage allocation errors disable capture, never device passthrough.
}
void IcTrialDetachTimer(void) {
    WDFTIMER timer;
    if(!gGate)return;
    WdfWaitLockAcquire(gGate,NULL);timer=trialTimer;trialTimer=NULL;timerStatus=STATUS_DEVICE_NOT_READY;WdfWaitLockRelease(gGate);
    if(timer){WdfTimerStop(timer,TRUE);WdfObjectDelete(timer);}
}
NTSTATUS IcTrialAccessLocked(void) {
    if(!NT_SUCCESS(storageStatus)) {
        TC_RESULT state;
        storageStatus=TcRead(&trialClock,IC_PRODUCT_TRIAL_SLOT,TC_QUERY,&state);
        if(NT_SUCCESS(storageStatus)) {
            if(state.duration!=duration)storageStatus=STATUS_REVISION_MISMATCH;
            else {firstContext=state.first;started=(BOOLEAN)state.started;expired=(BOOLEAN)state.expired;}
            if(NT_SUCCESS(storageStatus) && started && !expired)ArmLocked(Now());
        }
    }
    if(expired)return STATUS_EVALUATION_EXPIRATION;
    if(!NT_SUCCESS(storageStatus))return storageStatus;
    return timerStatus;
}
NTSTATUS IcTrialSendAccessLocked(void) {
    const NTSTATUS status=IcTrialAccessLocked();
    // Keep the requested first-successful-CREATE anchor. Injection-only
    // clients must activate once instead of obtaining an unlimited pre-trial.
    return NT_SUCCESS(status) && !started?STATUS_DEVICE_NOT_READY:status;
}
NTSTATUS IcTrialCommitLocked(void) {
    NTSTATUS status;TC_RESULT state;
    IcTrialCheckLocked();status=IcTrialAccessLocked();if(!NT_SUCCESS(status))return status;
    if(started)return STATUS_SUCCESS;
    status=TcRead(&trialClock,IC_PRODUCT_TRIAL_SLOT,TC_COMMIT,&state);
    if(!NT_SUCCESS(status)){storageStatus=status;return status;}
    firstContext=state.first;started=(BOOLEAN)state.started;expired=(BOOLEAN)state.expired;
    if(expired)return STATUS_EVALUATION_EXPIRATION;
    if(!started || state.duration!=duration){storageStatus=STATUS_DATA_ERROR;return storageStatus;}
    ArmLocked(Now());return STATUS_SUCCESS;
}
void IcTrialQueryLocked(IC_TRIAL_STATUS* result) {
    NTSTATUS status;ULONGLONG now;
    now=Now();CheckAtLocked(now);status=IcTrialAccessLocked();
    RtlZeroMemory(result,sizeof(*result));result->size=sizeof(*result);result->version=IC_TRIAL_VERSION;
    result->state=expired?IC_TRIAL_EXPIRED:!NT_SUCCESS(status)?IC_TRIAL_UNAVAILABLE:started?IC_TRIAL_ACTIVE:IC_TRIAL_NOT_STARTED;
    result->error=NT_SUCCESS(status)?0:RtlNtStatusToDosError(status);result->duration_100ns=duration;
    result->first_context_100ns=started?firstContext:0;
    result->elapsed_100ns=started?IcTrialElapsed(firstContext,now):0;
    result->remaining_100ns=expired?0:started?IcTrialRemaining(firstContext,now,duration):duration;
    result->active_mask=IcActiveMaskLocked();
}
void IcTrialDeviceStateLocked(IC_DEVICE_QUERY_RESULT* result) {
    if(!NT_SUCCESS(IcTrialAccessLocked())) {
        result->flags&=~1u;result->reserved[0]=IC_DEVICE_UNAVAILABLE;
        result->reserved[1]|=expired?IC_DEVICE_TRIAL_EXPIRED:IC_DEVICE_TRIAL_UNAVAILABLE;
    }
}
