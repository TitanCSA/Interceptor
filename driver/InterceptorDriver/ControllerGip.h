#pragma once
#include "Driver.h"
#include "../Shared/ControllerGipProtocol.h"
#include "../Shared/ControllerGipDiagnostics.h"

// Integration contract for the experimental USB lower filter. Completion of
// upper URBs must occur outside gGate: XboxGIP can synchronously re-enter us.
NTSTATUS IcGipArmLocked(PKEVENT event);
void IcGipRefreshCaptureLocked(void);
ULONG IcGipSnapshotLocked(IC_DEVICE_INFO* output,ULONG capacity);
void IcGipInitialize(void);
NTSTATUS IcGipAdd(PWDFDEVICE_INIT init);
ULONG IcGipCountLocked(void);
NTSTATUS IcGipCaptureLocked(PKEVENT event,IC_DEVICE_INFO* devices,ULONG capacity,ULONG* count);
void IcGipResetLocked(void);
NTSTATUS IcGipGetLocked(ULONG requested,PVOID buffer,SIZE_T length,SIZE_T* written,BOOLEAN raw);
NTSTATUS IcGipGetSelectedLocked(const IC_GET_SELECT*,PVOID,SIZE_T,SIZE_T*,BOOLEAN);
NTSTATUS IcGipSendLocked(PVOID buffer,SIZE_T length,IC_SEND_RESULT* result,BOOLEAN owned,BOOLEAN raw);
NTSTATUS IcGipDescriptorLocked(ULONG id,PVOID buffer,SIZE_T length,SIZE_T* written);
ULONG IcGipQueuedLocked(void);
NTSTATUS IcGipQueryLocked(ULONG token,IC_DEVICE_QUERY_RESULT* result,BOOLEAN ordinal);
NTSTATUS IcGipDiagnosticsLocked(ULONG id,PVOID buffer,SIZE_T length,SIZE_T* written);
