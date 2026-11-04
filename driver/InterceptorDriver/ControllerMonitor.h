#pragma once
#include "Driver.h"
#include "../Shared/ControllerMonitor.h"
typedef struct IC_MONITOR_OBSERVER {
    LIST_ENTRY link;
    PKEVENT event;
    ULONGLONG cursor,dropped;
    BOOLEAN linked;
} IC_MONITOR_OBSERVER;
void IcMonitorInitialize(void);
void IcMonitorUninitialize(void);
BOOLEAN IcMonitorEnabled(void);
void IcMonitorPublish(ULONG type,ULONG token,ULONG direction,ULONG flags,const VOID* report,ULONG length,
    const IC_NATIVE_GAMEPAD_STATE* state,NTSTATUS status);
void IcMonitorRemoved(ULONG type,ULONG token);
NTSTATUS IcMonitorQuery(PVOID output,SIZE_T length,SIZE_T* written);
NTSTATUS IcMonitorSubscribe(IC_MONITOR_OBSERVER* observer,const IC_MONITOR_SUBSCRIBE* request);
NTSTATUS IcMonitorRead(IC_MONITOR_OBSERVER* observer,const IC_MONITOR_READ* request,PVOID output,SIZE_T length,SIZE_T* written);
void IcMonitorClose(IC_MONITOR_OBSERVER* observer);
void IcMonitorRevokeAllLocked(void);
void IcHidMonitorChangedLocked(void);
void IcXinputHidMonitorChangedLocked(void);
NTSTATUS IcHidMonitorDeviceLocked(ULONG token,IC_DEVICE_INFO* info,PVOID descriptor,SIZE_T capacity,ULONG* bytes,BOOLEAN preparsed);
NTSTATUS IcXinputHidMonitorDeviceLocked(ULONG token,IC_DEVICE_INFO* info,PVOID descriptor,SIZE_T capacity,ULONG* bytes,BOOLEAN preparsed);
NTSTATUS IcXusbMonitorDeviceLocked(ULONG token,IC_DEVICE_INFO* info);
NTSTATUS IcGipMonitorDeviceLocked(ULONG token,IC_DEVICE_INFO* info);
