#include <ntddk.h>
#include <wdmsec.h>
#include "Protocol.h"

// This small non-PnP clock deliberately has no unload routine. It owns no
// input stacks, timers, threads or user handles. Only reboot renews a budget;
// removing/reinstalling a product never unloads or changes these four slots.
static FAST_MUTEX gate;
static struct {BOOLEAN started;ULONGLONG first;} budgets[TC_PRODUCTS];
DRIVER_INITIALIZE DriverEntry;
DRIVER_DISPATCH ClockDispatch;

NTSTATUS ClockDispatch(PDEVICE_OBJECT device,PIRP irp) {
    PIO_STACK_LOCATION stack=IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status=STATUS_ACCESS_DENIED;ULONG_PTR information=0;
    UNREFERENCED_PARAMETER(device);
    if(stack->MajorFunction==IRP_MJ_CREATE) {
        if(irp->RequestorMode==KernelMode)status=STATUS_SUCCESS;
    } else if(stack->MajorFunction==IRP_MJ_CLOSE || stack->MajorFunction==IRP_MJ_CLEANUP)status=STATUS_SUCCESS;
    else if(stack->MajorFunction==IRP_MJ_INTERNAL_DEVICE_CONTROL && irp->RequestorMode==KernelMode &&
        stack->Parameters.DeviceIoControl.IoControlCode==TC_IOCTL) {
        TC_REQUEST request;TC_RESULT result;ULONGLONG qpc,now;ULONG slot;
        if(!irp->AssociatedIrp.SystemBuffer || stack->Parameters.DeviceIoControl.InputBufferLength!=sizeof(request) ||
           stack->Parameters.DeviceIoControl.OutputBufferLength<sizeof(result))status=STATUS_INVALID_BUFFER_SIZE;
        else {
            RtlCopyMemory(&request,irp->AssociatedIrp.SystemBuffer,sizeof(request));
            if(request.size!=sizeof(request) || request.version!=TC_VERSION || request.product>=TC_PRODUCTS || request.operation>TC_COMMIT)
                status=STATUS_INVALID_PARAMETER;
            else {
                slot=request.product;ExAcquireFastMutex(&gate);now=KeQueryInterruptTimePrecise(&qpc);
                if(request.operation==TC_COMMIT && !budgets[slot].started){budgets[slot].first=now;budgets[slot].started=TRUE;}
                RtlZeroMemory(&result,sizeof(result));result.size=sizeof(result);result.version=TC_VERSION;
                result.started=budgets[slot].started;result.first=budgets[slot].first;result.duration=TC_DURATION;
                result.expired=result.started && (now<result.first || now-result.first>=TC_DURATION);
                ExReleaseFastMutex(&gate);
                RtlCopyMemory(irp->AssociatedIrp.SystemBuffer,&result,sizeof(result));information=sizeof(result);status=STATUS_SUCCESS;
            }
        }
    }
    irp->IoStatus.Status=status;irp->IoStatus.Information=information;IoCompleteRequest(irp,IO_NO_INCREMENT);return status;
}
NTSTATUS DriverEntry(PDRIVER_OBJECT driver,PUNICODE_STRING registry) {
    PDEVICE_OBJECT device=NULL;NTSTATUS status;ULONG i;TC_IDENTITY* identity=NULL;
    const GUID securityClass={0x4a5cf9ab,0x667e,0x40bb,{0xb2,0x92,0xa1,0xb4,0x4b,0x8c,0x13,0x1f}};
    DECLARE_CONST_UNICODE_STRING(name,TC_NAME);
    DECLARE_CONST_UNICODE_STRING(security,L"D:P(A;;GA;;;SY)");
    UNREFERENCED_PARAMETER(registry);ExInitializeFastMutex(&gate);
    for(i=0;i<=IRP_MJ_MAXIMUM_FUNCTION;++i)driver->MajorFunction[i]=ClockDispatch;
    driver->DriverUnload=NULL;
    status=IoAllocateDriverObjectExtension(driver,TC_EXTENSION_TAG,sizeof(*identity),(PVOID*)&identity);
    if(!NT_SUCCESS(status))return status;
    status=IoCreateDeviceSecure(driver,0,(PUNICODE_STRING)&name,0x834A,FILE_DEVICE_SECURE_OPEN,FALSE,(PUNICODE_STRING)&security,&securityClass,&device);
    if(!NT_SUCCESS(status))return status;
    identity->size=sizeof(*identity);identity->version=TC_VERSION;identity->owner=driver;identity->device=device;
    device->Flags|=DO_BUFFERED_IO;device->Flags&=~DO_DEVICE_INITIALIZING;return STATUS_SUCCESS;
}
