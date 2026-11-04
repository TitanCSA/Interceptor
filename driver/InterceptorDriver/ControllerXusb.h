#pragma once
#include "ControllerHid.h"
#include "../Shared/ControllerXusb.h"
NTSTATUS IcXusbArmLocked(PKEVENT event);
void IcXusbRefreshCaptureLocked(void);
ULONG IcXusbSnapshotLocked(IC_DEVICE_INFO* output,ULONG capacity);
void IcXusbInitialize(void);
NTSTATUS IcXusbAdd(PWDFDEVICE_INIT init);
ULONG IcXusbCountLocked(void);
BOOLEAN IcXusbPhysicalLocked(PCUNICODE_STRING parent,IC_NATIVE_GAMEPAD_STATE* state);
NTSTATUS IcXusbCaptureLocked(PKEVENT event,IC_DEVICE_INFO* devices,ULONG capacity,ULONG* count);
void IcXusbResetLocked(void);
NTSTATUS IcXusbGetLocked(ULONG requested,PVOID buffer,SIZE_T length,SIZE_T* written);
NTSTATUS IcXusbGetSelectedLocked(const IC_GET_SELECT*,PVOID,SIZE_T,SIZE_T*);
NTSTATUS IcXusbQueryLocked(ULONG token,IC_DEVICE_QUERY_RESULT* result,BOOLEAN ordinal);
NTSTATUS IcXusbSendLocked(PVOID buffer,SIZE_T length,IC_SEND_RESULT* result,BOOLEAN owned);
