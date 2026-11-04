#pragma once
#include "Driver.h"
#include "../Shared/ControllerHid.h"
NTSTATUS IcHidArmLocked(PKEVENT event);
void IcHidRefreshCaptureLocked(void);
ULONG IcHidSnapshotLocked(IC_DEVICE_INFO* output,ULONG capacity);
void IcHidInitialize(void);
NTSTATUS IcHidAdd(PWDFDEVICE_INIT init);
ULONG IcHidCountLocked(void);
NTSTATUS IcHidCaptureLocked(PKEVENT event,IC_DEVICE_INFO* devices,ULONG capacity,ULONG* count);
void IcHidResetLocked(void);
NTSTATUS IcHidGetLocked(ULONG requested,PVOID buffer,SIZE_T length,SIZE_T* written);
NTSTATUS IcHidGetSelectedLocked(const IC_GET_SELECT*,PVOID,SIZE_T,SIZE_T*);
NTSTATUS IcHidSendLocked(PVOID buffer,SIZE_T length,IC_SEND_RESULT* result,BOOLEAN owned);
NTSTATUS IcHidDescriptorLocked(ULONG id,PVOID buffer,SIZE_T length,SIZE_T* written);
ULONG IcHidQueuedLocked(void);
NTSTATUS IcHidQueryLocked(ULONG token,IC_DEVICE_QUERY_RESULT* result,BOOLEAN ordinal);
void IcHidMonitorChangedLocked(void);
NTSTATUS IcHidMonitorDeviceLocked(ULONG token,IC_DEVICE_INFO* info,PVOID descriptor,SIZE_T capacity,ULONG* bytes,BOOLEAN preparsed);
void IcRevokeAllLocked(ULONG reason);
ULONG IcActiveMaskLocked(void);
void IcTrialRevokeLocked(void);
ULONGLONG IcNextSequenceLocked(void);
