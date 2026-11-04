#pragma once
#include "Driver.h"
#include "../Shared/Trial.h"

void IcTrialInitialize(void);
void IcTrialUninitialize(void);
void IcTrialAttachTimer(WDFDEVICE device);
void IcTrialDetachTimer(void);
// All Locked functions require gGate. No clock reads in packet routing.
void IcTrialCheckLocked(void);
void IcTrialResumeLocked(void);
NTSTATUS IcTrialAccessLocked(void);
NTSTATUS IcTrialSendAccessLocked(void);
NTSTATUS IcTrialCommitLocked(void);
void IcTrialQueryLocked(IC_TRIAL_STATUS* result);
void IcTrialDeviceStateLocked(IC_DEVICE_QUERY_RESULT* result);
