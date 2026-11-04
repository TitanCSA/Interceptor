#pragma once
#include "Driver.h"
#include "../Shared/ControllerXinputHid.h"
NTSTATUS IcXinputHidArmLocked(PKEVENT event);
void IcXinputHidRefreshCaptureLocked(void);
ULONG IcXinputHidSnapshotLocked(IC_DEVICE_INFO* output,ULONG capacity);
void IcXinputHidInitialize(void);
NTSTATUS IcXinputHidAdd(PWDFDEVICE_INIT init);
ULONG IcXinputHidCountLocked(void);
NTSTATUS IcXinputHidCaptureLocked(PKEVENT event,IC_DEVICE_INFO* devices,ULONG capacity,ULONG* count);
void IcXinputHidResetLocked(void);
NTSTATUS IcXinputHidGetLocked(ULONG requested,PVOID buffer,SIZE_T length,SIZE_T* written);
NTSTATUS IcXinputHidGetSelectedLocked(const IC_GET_SELECT*,PVOID,SIZE_T,SIZE_T*);
NTSTATUS IcXinputHidSendLocked(PVOID buffer,SIZE_T length,IC_SEND_RESULT* result,BOOLEAN owned);
NTSTATUS IcXinputHidDescriptorLocked(ULONG id,PVOID buffer,SIZE_T length,SIZE_T* written);
ULONG IcXinputHidQueuedLocked(void);
NTSTATUS IcXinputHidQueryLocked(ULONG token,IC_DEVICE_QUERY_RESULT* result,BOOLEAN ordinal);
void IcXinputHidMonitorChangedLocked(void);
NTSTATUS IcXinputHidMonitorDeviceLocked(ULONG token,IC_DEVICE_INFO* info,PVOID descriptor,SIZE_T capacity,ULONG* bytes,BOOLEAN preparsed);
