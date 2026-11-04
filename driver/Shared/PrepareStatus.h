#pragma once
// WDF PrepareHardware forbids STATUS_NOT_SUPPORTED even when a lower query
// supplies it. Preserve every other success/failure code without broad fallback.
static __inline NTSTATUS IcPrepareStatus(NTSTATUS status) {
    return status==STATUS_NOT_SUPPORTED?STATUS_DEVICE_CONFIGURATION_ERROR:status;
}
