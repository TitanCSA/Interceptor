#include "ControllerXusbUsb.h"
#include "ControllerMonitor.h"
#include "../Shared/ControllerXusbUsbProtocol.h"
#include <usb.h>
#include <usbioctl.h>

// Wired USB lower filter. Windows xusb22 retains its complete IOCTL validation,
// caller/focus policy and native HID translation. A private event-driven IN
// pump brokers only the validated gamepad USB interface. At zero capture every
// received packet retains its bytes and actual transfer length; URB scheduling
// is brokered, not claimed to be identical to an absent filter.
#define XUSB_USB_PACKET_BYTES 64u
#define XUSB_USB_OUTPUT_CAPACITY 128u
typedef struct XUSB_USB_PACKET {ULONG length;BOOLEAN input;UCHAR bytes[XUSB_USB_PACKET_BYTES];} XUSB_USB_PACKET;
typedef struct XUSB_USB_COMPLETION {
    LIST_ENTRY link;WDFREQUEST request;NTSTATUS status;ULONG_PTR information;
    PVOID monitorData;ULONG monitorBytes;
} XUSB_USB_COMPLETION;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XUSB_USB_COMPLETION,UsbCompletion)
typedef struct XUSB_USB_DEVICE {
    WDFDEVICE device;WDFWAITLOCK stateLock;WDFSPINLOCK completionLock;WDFDPC completionDpc;
    WDFQUEUE reads,dispatch;LIST_ENTRY link,completions;
    BOOLEAN registered,counted,powered,configured,running,valid,restorePending,retired,unsupported,recovering;
    BOOLEAN completing,completeAgain,barrier;
    KEVENT completionsIdle;
    IC_DEVICE_INFO info;USHORT instance[256];ULONG captureId;
    USBD_PIPE_HANDLE inPipe;UCHAR interfaceNumber;ULONG packetBytes;
    IC_NATIVE_GAMEPAD_STATE physical,delivered;ULONG physicalLength,physicalPacket;
    UCHAR physicalFrame[IC_XUSB_USB_MAX_BYTES];
    XUSB_USB_PACKET output[XUSB_USB_OUTPUT_CAPACITY];ULONG outputHead,outputCount;
    WDFREQUEST request;WDFMEMORY urbMemory;struct _URB_BULK_OR_INTERRUPT_TRANSFER urb;
    UCHAR buffer[XUSB_USB_PACKET_BYTES];BOOLEAN used;PETHREAD thread;
    KEVENT stop,started,done,maintenance;NTSTATUS readStatus;
    ULONGLONG physicalReports;ULONG lastReadStatus,lastUsbStatus;
} XUSB_USB_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XUSB_USB_DEVICE,XusbUsbDevice)
typedef struct XUSB_USB_DPC {XUSB_USB_DEVICE* device;} XUSB_USB_DPC;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XUSB_USB_DPC,UsbDpc)
static LIST_ENTRY devices;
static LONG nextId;
static IC_CONTROLLER_XUSB_INPUT* captured;
static XUSB_USB_DEVICE* targets[IC_MAX_DEVICES];
static ULONG targetCount;
static uint32_t head,count;
static PKEVENT readyEvent;
static VOID StopLocked(XUSB_USB_DEVICE* d,BOOLEAN cancelling);
static NTSTATUS StartLocked(XUSB_USB_DEVICE* d);
static VOID TryRestoreLocked(XUSB_USB_DEVICE* d);
static PURB RequestUrb(WDFREQUEST request) {
    return (PURB)IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(request))->Parameters.Others.Argument1;
}
static NTSTATUS EnsureCompletion(WDFREQUEST request) {
    WDF_OBJECT_ATTRIBUTES a;
    if(UsbCompletion(request)){UsbCompletion(request)->monitorData=NULL;UsbCompletion(request)->monitorBytes=0;return STATUS_SUCCESS;}
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,XUSB_USB_COMPLETION);
    return WdfObjectAllocateContext(request,&a,NULL);
}
static VOID QueueCompletion(XUSB_USB_DEVICE* d,WDFREQUEST request,NTSTATUS status,ULONG_PTR information) {
    XUSB_USB_COMPLETION* c=UsbCompletion(request);c->request=request;c->status=status;c->information=information;
    WdfSpinLockAcquire(d->completionLock);InsertTailList(&d->completions,&c->link);
    WdfSpinLockRelease(d->completionLock);WdfDpcEnqueue(d->completionDpc);
}
static VOID CompleteDpc(WDFDPC dpc) {
    XUSB_USB_DEVICE* d=UsbDpc(dpc)->device;ULONG work=0;BOOLEAN again;
    WdfSpinLockAcquire(d->completionLock);
    if(d->completing){d->completeAgain=TRUE;WdfSpinLockRelease(d->completionLock);return;}
    d->completing=TRUE;WdfSpinLockRelease(d->completionLock);
    while(work++<64) {
        XUSB_USB_COMPLETION* c;
        WdfSpinLockAcquire(d->completionLock);
        if(IsListEmpty(&d->completions)){WdfSpinLockRelease(d->completionLock);break;}
        c=CONTAINING_RECORD(RemoveHeadList(&d->completions),XUSB_USB_COMPLETION,link);
        WdfSpinLockRelease(d->completionLock);
        if(c->monitorData && NT_SUCCESS(c->status) && IcMonitorEnabled()) {
            IC_NATIVE_GAMEPAD_STATE state;
            if(IcXusbUsbDecode((const UCHAR*)c->monitorData,c->monitorBytes,&state))
                IcMonitorPublish(IC_TYPE_CONTROLLER_XUSB,d->info.instance_token,IC_MONITOR_OUT,
                    IC_MONITOR_NATIVE_VALID|IC_MONITOR_GUIDE_VALID,c->monitorData,c->monitorBytes,&state,STATUS_SUCCESS);
            else if(c->monitorBytes && ((const UCHAR*)c->monitorData)[0]==0)
                IcMonitorPublish(IC_TYPE_CONTROLLER_XUSB,d->info.instance_token,IC_MONITOR_OUT,
                    IC_MONITOR_RAW|IC_MONITOR_ERROR,c->monitorData,c->monitorBytes,NULL,STATUS_NOT_SUPPORTED);
        }
        // Upper completion may synchronously submit the next USB read.
        WdfRequestCompleteWithInformation(c->request,c->status,c->information);
    }
    WdfSpinLockAcquire(d->completionLock);
    again=d->completeAgain || !IsListEmpty(&d->completions);d->completeAgain=FALSE;d->completing=FALSE;
    if(d->barrier && !again){d->barrier=FALSE;KeSetEvent(&d->completionsIdle,IO_NO_INCREMENT,FALSE);}
    WdfSpinLockRelease(d->completionLock);if(again)WdfDpcEnqueue(dpc);
}
static VOID DrainCompletions(XUSB_USB_DEVICE* d) {
    KeClearEvent(&d->completionsIdle);WdfSpinLockAcquire(d->completionLock);d->barrier=TRUE;
    WdfSpinLockRelease(d->completionLock);WdfDpcEnqueue(d->completionDpc);
    KeWaitForSingleObject(&d->completionsIdle,Executive,KernelMode,FALSE,NULL);
}
static VOID CancelRead(WDFQUEUE queue,WDFREQUEST request) {
    XUSB_USB_DEVICE* d=XusbUsbDevice(WdfIoQueueGetDevice(queue));PURB urb=RequestUrb(request);
    if(urb){urb->UrbHeader.Status=USBD_STATUS_CANCELED;urb->UrbBulkOrInterruptTransfer.TransferBufferLength=0;}
    QueueCompletion(d,request,STATUS_CANCELLED,0);
}
static VOID DrainReadsLocked(XUSB_USB_DEVICE* d,NTSTATUS status,USBD_STATUS usbStatus) {
    WDFREQUEST request;
    while(NT_SUCCESS(WdfIoQueueRetrieveNextRequest(d->reads,&request))) {
        PURB urb=RequestUrb(request);if(urb){urb->UrbHeader.Status=usbStatus;urb->UrbBulkOrInterruptTransfer.TransferBufferLength=0;}
        QueueCompletion(d,request,status,0);
    }
}
static BOOLEAN CopyPacket(XUSB_USB_DEVICE* d,WDFREQUEST request,const XUSB_USB_PACKET* packet) {
    PURB urb=RequestUrb(request);PVOID buffer=NULL;NTSTATUS status=STATUS_SUCCESS;
    XUSB_USB_COMPLETION* completion=UsbCompletion(request);completion->monitorData=NULL;completion->monitorBytes=0;
    if(!urb || urb->UrbHeader.Length<sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER))status=STATUS_INVALID_PARAMETER;
    else if(urb->UrbBulkOrInterruptTransfer.TransferBufferLength<packet->length)status=STATUS_BUFFER_TOO_SMALL;
    else {
        if(urb->UrbBulkOrInterruptTransfer.TransferBufferMDL) {
            if(MmGetMdlByteCount(urb->UrbBulkOrInterruptTransfer.TransferBufferMDL)>=packet->length)
                buffer=MmGetSystemAddressForMdlSafe(urb->UrbBulkOrInterruptTransfer.TransferBufferMDL,NormalPagePriority|MdlMappingNoExecute);
        } else buffer=urb->UrbBulkOrInterruptTransfer.TransferBuffer;
        if(!buffer)status=STATUS_INSUFFICIENT_RESOURCES;else {
            RtlCopyMemory(buffer,packet->bytes,packet->length);
            completion->monitorData=buffer;completion->monitorBytes=packet->length;
        }
    }
    if(urb){urb->UrbHeader.Status=NT_SUCCESS(status)?USBD_STATUS_SUCCESS:USBD_STATUS_INVALID_PARAMETER;
        urb->UrbBulkOrInterruptTransfer.TransferBufferLength=NT_SUCCESS(status)?packet->length:0;}
    QueueCompletion(d,request,status,0);return NT_SUCCESS(status);
}
static VOID FlushOutputLocked(XUSB_USB_DEVICE* d) {
    WDFREQUEST request;
    while(d->outputCount && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(d->reads,&request))) {
        if(CopyPacket(d,request,&d->output[d->outputHead])) {
            d->outputHead=(d->outputHead+1)%XUSB_USB_OUTPUT_CAPACITY;--d->outputCount;
            KeSetEvent(&d->maintenance,IO_NO_INCREMENT,FALSE);
        }
    }
}
static BOOLEAN OutputLocked(XUSB_USB_DEVICE* d,const UCHAR* bytes,ULONG length,BOOLEAN input) {
    XUSB_USB_PACKET* packet;
    if(!length || length>XUSB_USB_PACKET_BYTES || d->outputCount==XUSB_USB_OUTPUT_CAPACITY)return FALSE;
    packet=&d->output[(d->outputHead+d->outputCount++)%XUSB_USB_OUTPUT_CAPACITY];
    packet->length=length;packet->input=input;RtlCopyMemory(packet->bytes,bytes,length);FlushOutputLocked(d);return TRUE;
}
static VOID ClearInputLocked(XUSB_USB_DEVICE* d) {
    ULONG i,kept=0;
    // Initialization/status packets keep their order through capture changes.
    for(i=0;i<d->outputCount;++i) {
        XUSB_USB_PACKET* packet=&d->output[(d->outputHead+i)%XUSB_USB_OUTPUT_CAPACITY];
        if(!packet->input){d->output[(d->outputHead+kept)%XUSB_USB_OUTPUT_CAPACITY]=*packet;++kept;}
    }
    d->outputCount=kept;KeSetEvent(&d->maintenance,IO_NO_INCREMENT,FALSE);
}
static BOOLEAN CaptureReady(const XUSB_USB_DEVICE* d) {
    return !d->retired && !d->recovering && d->running && d->powered && d->configured && d->valid;
}
static BOOLEAN DeliverStateLocked(XUSB_USB_DEVICE* d,const IC_NATIVE_GAMEPAD_STATE* state) {
    UCHAR bytes[IC_XUSB_USB_MAX_BYTES];
    if(!CaptureReady(d) || d->outputCount==XUSB_USB_OUTPUT_CAPACITY)return FALSE;
    RtlCopyMemory(bytes,d->physicalFrame,d->physicalLength);
    if(!IcXusbUsbReplace(bytes,d->physicalLength,state))return FALSE;
    if(!OutputLocked(d,bytes,d->physicalLength,TRUE))return FALSE;
    d->delivered=*state;return TRUE;
}
static VOID TryRestoreLocked(XUSB_USB_DEVICE* d) {
    if(d->restorePending && DeliverStateLocked(d,&d->physical))d->restorePending=FALSE;
}
void IcXusbInitialize(void){InitializeListHead(&devices);}
NTSTATUS IcXusbQueryLocked(ULONG token,IC_DEVICE_QUERY_RESULT* result,BOOLEAN ordinal) {
    PLIST_ENTRY p;ULONG position=0;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_USB_DEVICE* d=CONTAINING_RECORD(p,XUSB_USB_DEVICE,link);if(d->retired)continue;
        if((ordinal?++position:d->info.instance_token)!=token)continue;
        result->instance_token=d->info.instance_token;
        result->flags=(CaptureReady(d)?1u:0u)|(d->running?2u:0u)|(d->powered?4u:0u)|
            (d->valid?8u:0u)|(d->configured?16u:0u)|(d->restorePending?32u:0u)|(d->unsupported?64u:0u);
        if(!d->configured)result->reserved[1]|=IC_DEVICE_WAIT_PROFILE;
        if(!d->valid)result->reserved[1]|=IC_DEVICE_WAIT_FIRST_REPORT;
        if(d->unsupported)result->reserved[1]|=IC_DEVICE_PROFILE_REJECTED;
        result->last_read_status=d->lastReadStatus;result->last_usb_status=d->lastUsbStatus;
        result->physical_reports=d->physicalReports;result->report_bytes=IC_XUSB_STATE_BYTES;
        result->metadata_input_bytes=d->physicalLength;
        RtlCopyMemory(result->instance,d->instance,sizeof(result->instance));
        if(result->instance[0])result->flags|=IC_QUERY_IDENTITY_VALID;return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_NOT_CONNECTED;
}
NTSTATUS IcXusbMonitorDeviceLocked(ULONG token,IC_DEVICE_INFO* info) {
    PLIST_ENTRY p;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_USB_DEVICE* d=CONTAINING_RECORD(p,XUSB_USB_DEVICE,link);
        if(!d->retired && d->info.instance_token==token){*info=d->info;info->id=token;return STATUS_SUCCESS;}
    }
    return STATUS_DEVICE_NOT_CONNECTED;
}
ULONG IcXusbCountLocked(void) {
    ULONG n=0;PLIST_ENTRY p;for(p=devices.Flink;p!=&devices;p=p->Flink)if(CaptureReady(CONTAINING_RECORD(p,XUSB_USB_DEVICE,link)))++n;return n;
}
BOOLEAN IcXusbPhysicalLocked(PCUNICODE_STRING parent,IC_NATIVE_GAMEPAD_STATE* state) {
    PLIST_ENTRY p;XUSB_USB_DEVICE* found=NULL;
    if(!parent || !parent->Buffer || !parent->Length || !state)return FALSE;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_USB_DEVICE* d=CONTAINING_RECORD(p,XUSB_USB_DEVICE,link);UNICODE_STRING actual;
        if(!CaptureReady(d))continue;RtlInitUnicodeString(&actual,d->instance);
        if(RtlEqualUnicodeString(parent,&actual,TRUE)){if(found)return FALSE;found=d;}
    }
    if(!found)return FALSE;*state=found->physical;return TRUE;
}
NTSTATUS IcXusbCaptureLocked(PKEVENT event,IC_DEVICE_INFO* output,ULONG capacity,ULONG* written) {
    PLIST_ENTRY p;ULONG n=IcXusbCountLocked();if(!n)return STATUS_DEVICE_NOT_READY;
    if(n>capacity || n>IC_MAX_DEVICES)return STATUS_BUFFER_TOO_SMALL;if(captured)return STATUS_SHARING_VIOLATION;
    captured=ExAllocatePoolZero(NonPagedPoolNx,sizeof(*captured)*IC_XUSB_CAPACITY,FILTER_TAG);
    if(!captured)return STATUS_INSUFFICIENT_RESOURCES;n=0;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_USB_DEVICE* d=CONTAINING_RECORD(p,XUSB_USB_DEVICE,link);d->captureId=0;if(!CaptureReady(d))continue;
        d->captureId=++n;targets[n-1]=d;output[n-1]=d->info;output[n-1].id=n;
        ClearInputLocked(d);d->restorePending=FALSE;
    }
    targetCount=n;head=count=0;readyEvent=event;KeClearEvent(event);*written=n;return STATUS_SUCCESS;
}
NTSTATUS IcXusbArmLocked(PKEVENT event) {
    if(captured)return STATUS_SHARING_VIOLATION;
    captured=ExAllocatePoolZero(NonPagedPoolNx,sizeof(*captured)*IC_XUSB_CAPACITY,FILTER_TAG);
    if(!captured)return STATUS_INSUFFICIENT_RESOURCES;
    targetCount=head=count=0;readyEvent=event;KeClearEvent(event);return STATUS_SUCCESS;
}
void IcXusbRefreshCaptureLocked(void) {
    PLIST_ENTRY p;if(!captured || !IcAllCaptureLocked(IC_TYPE_CONTROLLER_XUSB))return;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_USB_DEVICE* d=CONTAINING_RECORD(p,XUSB_USB_DEVICE,link);if(d->captureId || !CaptureReady(d))continue;
        if(targetCount>=IC_MAX_DEVICES){IcRevokeAllLocked(IC_RESET_DEVICE);return;}
        if(!IcAllAttachLocked(IC_TYPE_CONTROLLER_XUSB))return;
        d->captureId=++targetCount;targets[targetCount-1]=d;ClearInputLocked(d);d->restorePending=FALSE;
    }
}
ULONG IcXusbSnapshotLocked(IC_DEVICE_INFO* output,ULONG capacity) {
    ULONG i,n=min(targetCount,capacity);for(i=0;i<n;++i){output[i]=targets[i]->info;output[i].id=i+1;}return n;
}
void IcXusbResetLocked(void) {
    PLIST_ENTRY p;if(!captured)return;
    ExFreePoolWithTag(captured,FILTER_TAG);captured=NULL;head=count=targetCount=0;readyEvent=NULL;RtlZeroMemory(targets,sizeof(targets));
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_USB_DEVICE* d=CONTAINING_RECORD(p,XUSB_USB_DEVICE,link);if(!d->captureId)continue;
        d->captureId=0;ClearInputLocked(d);d->restorePending=d->valid;TryRestoreLocked(d);
    }
}
NTSTATUS IcXusbGetLocked(ULONG requested,PVOID output,SIZE_T length,SIZE_T* written) {
    LONG n=IcInputReadSelected(&head,&count,captured,IC_XUSB_CAPACITY,sizeof(*captured),
        FIELD_OFFSET(IC_CONTROLLER_XUSB_INPUT,device),output,sizeof(*captured),
        (ULONG)min(length/sizeof(*captured),IC_XUSB_CAPACITY),requested,0,TRUE);
    if(n<0)return STATUS_BUFFER_TOO_SMALL;if(!count && readyEvent)KeClearEvent(readyEvent);
    *written=(SIZE_T)n*sizeof(*captured);return STATUS_SUCCESS;
}
NTSTATUS IcXusbGetSelectedLocked(const IC_GET_SELECT* request,PVOID output,SIZE_T length,SIZE_T* written) {
    const BOOLEAN consume=(request->flags&IC_GET_PEEK)==0;
    LONG n=IcInputReadSelected(&head,&count,captured,IC_XUSB_CAPACITY,sizeof(*captured),
        FIELD_OFFSET(IC_CONTROLLER_XUSB_INPUT,device),output,sizeof(*captured),
        (ULONG)min(length/sizeof(*captured),IC_XUSB_CAPACITY),request->base.count,request->device,consume);
    if(n<0)return STATUS_BUFFER_TOO_SMALL;if(consume && !count && readyEvent)KeClearEvent(readyEvent);
    *written=(SIZE_T)n*sizeof(*captured);return STATUS_SUCCESS;
}
static XUSB_USB_DEVICE* FindLocked(ULONG id,BOOLEAN owned) {
    PLIST_ENTRY p;ULONG ordinal=0;XUSB_USB_DEVICE* first=NULL;
    if(owned)return id && id<=targetCount?targets[id-1]:NULL;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_USB_DEVICE* d=CONTAINING_RECORD(p,XUSB_USB_DEVICE,link);if(!CaptureReady(d))continue;
        if(!first)first=d;if(++ordinal==id)return d;
    }
    return id?NULL:first;
}
NTSTATUS IcXusbSendLocked(PVOID input,SIZE_T length,IC_SEND_RESULT* result,BOOLEAN owned) {
    IC_CONTROLLER_XUSB_INPUT* in=(IC_CONTROLLER_XUSB_INPUT*)input;ULONG n=(ULONG)(length/sizeof(*in)),i;
    if(!n || n>IC_MAX_SEND || length%sizeof(*in))return STATUS_INVALID_PARAMETER;
    for(i=0;i<n;++i)if(in[i].reserved || in[i].padding || !IcXusbUsbValidState(&in[i].state))return STATUS_INVALID_PARAMETER;
    result->accepted=0;result->status=STATUS_SUCCESS;
    for(i=0;i<n;++i) {
        XUSB_USB_DEVICE* d=FindLocked(in[i].device,owned);
        if(!d || !CaptureReady(d) || (owned && !d->captureId)){result->status=(ULONG)STATUS_DEVICE_NOT_CONNECTED;break;}
        TryRestoreLocked(d);
        if(d->restorePending || !DeliverStateLocked(d,&in[i].state)){result->status=(ULONG)STATUS_DEVICE_BUSY;break;}
        ++result->accepted;
    }
    return STATUS_SUCCESS;
}
static VOID MonitorPhysicalLocked(XUSB_USB_DEVICE* d,const UCHAR* bytes,ULONG length) {
    IC_NATIVE_GAMEPAD_STATE state;
    if(!IcMonitorEnabled())return;
    if(IcXusbUsbDecode(bytes,length,&state)) {
        state.packet=d->physicalPacket+1;
        IcMonitorPublish(IC_TYPE_CONTROLLER_XUSB,d->info.instance_token,IC_MONITOR_IN,
            IC_MONITOR_NATIVE_VALID|IC_MONITOR_GUIDE_VALID,bytes,length,&state,STATUS_SUCCESS);
    } else if(length && bytes[0]==0)
        IcMonitorPublish(IC_TYPE_CONTROLLER_XUSB,d->info.instance_token,IC_MONITOR_IN,
            IC_MONITOR_RAW|IC_MONITOR_ERROR,bytes,length,NULL,STATUS_NOT_SUPPORTED);
}
static BOOLEAN ProcessPacketLocked(XUSB_USB_DEVICE* d,const UCHAR* bytes,ULONG length) {
    IC_NATIVE_GAMEPAD_STATE state;
    if(!IcXusbUsbDecode(bytes,length,&state)) {
        // An unknown ID0 shape can still be interpreted as physical controls by
        // xusb22. Revoke before forwarding it; never claim isolation for fields
        // this codec cannot capture. Non-state initialization/status stays live.
        if(length && bytes[0]==0) {
            if(captured && d->captureId)IcRevokeAllLocked(IC_RESET_DEVICE);
            d->valid=FALSE;d->unsupported=TRUE;
        }
        // Potential state remains input-origin even while unsupported, so a
        // later capture cannot replay this queued physical frame as protocol.
        return OutputLocked(d,bytes,length,length && bytes[0]==0);
    }
    d->unsupported=FALSE;
    state.packet=++d->physicalPacket;d->physical=state;d->physicalLength=length;
    RtlCopyMemory(d->physicalFrame,bytes,length);++d->physicalReports;
    if(!d->valid){d->valid=TRUE;IcAllReadersReadyLocked();}
    if(d->captureId && captured) {
        IC_CONTROLLER_XUSB_INPUT* out;
        if(count==IC_XUSB_CAPACITY){IcRevokeAllLocked(IC_RESET_OVERFLOW);return TRUE;}
        out=&captured[(head+count)%IC_XUSB_CAPACITY];RtlZeroMemory(out,sizeof(*out));
        out->sequence=IcNextSequenceLocked();out->timestamp=KeQueryPerformanceCounter(NULL).QuadPart;
        out->device=d->captureId;out->state=state;if(++count==1)KeSetEvent(readyEvent,IO_NO_INCREMENT,FALSE);return TRUE;
    }
    d->delivered=state;return OutputLocked(d,bytes,length,TRUE);
}
static VOID PhysicalDone(WDFREQUEST request,WDFIOTARGET target,PWDF_REQUEST_COMPLETION_PARAMS params,WDFCONTEXT context) {
    XUSB_USB_DEVICE* d=context;UNREFERENCED_PARAMETER(request);UNREFERENCED_PARAMETER(target);
    d->readStatus=params->IoStatus.Status;KeSetEvent(&d->done,IO_NO_INCREMENT,FALSE);
}
static NTSTATUS FormatTransfer(XUSB_USB_DEVICE* d) {
    WDF_REQUEST_REUSE_PARAMS reuse;NTSTATUS status;
    if(d->used){WDF_REQUEST_REUSE_PARAMS_INIT(&reuse,WDF_REQUEST_REUSE_NO_FLAGS,STATUS_SUCCESS);
        status=WdfRequestReuse(d->request,&reuse);if(!NT_SUCCESS(status))return status;}
    d->used=TRUE;RtlZeroMemory(&d->urb,sizeof(d->urb));
    d->urb.Hdr.Length=(USHORT)sizeof(d->urb);d->urb.Hdr.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    d->urb.PipeHandle=d->inPipe;d->urb.TransferFlags=USBD_TRANSFER_DIRECTION_IN|USBD_SHORT_TRANSFER_OK;
    d->urb.TransferBufferLength=d->packetBytes;d->urb.TransferBuffer=d->buffer;
    return WdfIoTargetFormatRequestForInternalIoctlOthers(WdfDeviceGetIoTarget(d->device),d->request,
        IOCTL_INTERNAL_USB_SUBMIT_URB,d->urbMemory,NULL,NULL,NULL,NULL,NULL);
}
static VOID Reader(PVOID context) {
    XUSB_USB_DEVICE* d=context;NTSTATUS status=STATUS_SUCCESS;PVOID waits[3]={&d->stop,&d->done,&d->maintenance};ULONG empty=0;
    KeWaitForSingleObject(&d->started,Executive,KernelMode,FALSE,NULL);
    while(!KeReadStateEvent(&d->stop)) {
        status=FormatTransfer(d);if(!NT_SUCCESS(status))break;
        KeClearEvent(&d->done);WdfRequestSetCompletionRoutine(d->request,PhysicalDone,d);
        if(!WdfRequestSend(d->request,WdfDeviceGetIoTarget(d->device),NULL)){status=WdfRequestGetStatus(d->request);break;}
        for(;;){status=KeWaitForMultipleObjects(3,waits,WaitAny,Executive,KernelMode,FALSE,NULL,NULL);
            if(status!=STATUS_WAIT_2)break;WdfWaitLockAcquire(gGate,NULL);TryRestoreLocked(d);WdfWaitLockRelease(gGate);}
        if(status==STATUS_WAIT_0){WdfRequestCancelSentRequest(d->request);KeWaitForSingleObject(&d->done,Executive,KernelMode,FALSE,NULL);break;}
        status=d->readStatus;
        if(!NT_SUCCESS(status))break;
        if(!USBD_SUCCESS(d->urb.Hdr.Status)){status=STATUS_IO_DEVICE_ERROR;break;}
        if(d->urb.TransferBufferLength>d->packetBytes){status=STATUS_INVALID_BUFFER_SIZE;break;}
        if(!d->urb.TransferBufferLength){if(++empty==16){status=STATUS_DEVICE_DATA_ERROR;break;}continue;}empty=0;
        if(IcMonitorEnabled()) {
            WdfWaitLockAcquire(gGate,NULL);
            if(!KeReadStateEvent(&d->stop))MonitorPhysicalLocked(d,d->buffer,d->urb.TransferBufferLength);
            WdfWaitLockRelease(gGate);
        }
        for(;;) {
            BOOLEAN processed=FALSE;PVOID space[2]={&d->stop,&d->maintenance};
            WdfWaitLockAcquire(gGate,NULL);TryRestoreLocked(d);
            if(!KeReadStateEvent(&d->stop) && d->outputCount<XUSB_USB_OUTPUT_CAPACITY)
                processed=ProcessPacketLocked(d,d->buffer,d->urb.TransferBufferLength);
            WdfWaitLockRelease(gGate);
            if(processed || KeReadStateEvent(&d->stop))break;
            KeWaitForMultipleObjects(2,space,WaitAny,Executive,KernelMode,FALSE,NULL,NULL);
        }
    }
    WdfWaitLockAcquire(gGate,NULL);d->running=FALSE;d->lastReadStatus=(ULONG)status;d->lastUsbStatus=d->urb.Hdr.Status;
    if(!KeReadStateEvent(&d->stop) && !NT_SUCCESS(status))IcMonitorPublish(IC_TYPE_CONTROLLER_XUSB,d->info.instance_token,
        IC_MONITOR_DEVICE,IC_MONITOR_ERROR,NULL,0,NULL,status);
    if(captured && d->captureId)IcRevokeAllLocked(IC_RESET_DEVICE);
    // Reset/abort owns a restart barrier. New upper reads submitted from an
    // earlier cancellation callback wait for that restart, not a fake unplug.
    if(!d->recovering)DrainReadsLocked(d,STATUS_DEVICE_NOT_CONNECTED,USBD_STATUS_DEVICE_GONE);
    WdfWaitLockRelease(gGate);PsTerminateSystemThread(STATUS_SUCCESS);
}
static NTSTATUS StartLocked(XUSB_USB_DEVICE* d) {
    HANDLE handle=NULL;OBJECT_ATTRIBUTES attributes;NTSTATUS status;LARGE_INTEGER zero;zero.QuadPart=0;
    if(d->thread && KeWaitForSingleObject(d->thread,Executive,KernelMode,FALSE,&zero)==STATUS_SUCCESS){ObDereferenceObject(d->thread);d->thread=NULL;}
    if(d->thread || d->retired || !d->powered || !d->configured)return STATUS_SUCCESS;
    KeClearEvent(&d->stop);KeClearEvent(&d->started);InitializeObjectAttributes(&attributes,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
    status=PsCreateSystemThread(&handle,THREAD_ALL_ACCESS,&attributes,NULL,NULL,Reader,d);
    if(NT_SUCCESS(status)){status=ObReferenceObjectByHandle(handle,SYNCHRONIZE,*PsThreadType,KernelMode,(PVOID*)&d->thread,NULL);
        if(!NT_SUCCESS(status))KeSetEvent(&d->stop,IO_NO_INCREMENT,FALSE);
        else {WdfWaitLockAcquire(gGate,NULL);d->running=TRUE;if(!d->recovering)IcAllReadersReadyLocked();WdfWaitLockRelease(gGate);}KeSetEvent(&d->started,IO_NO_INCREMENT,FALSE);}
    if(handle){if(!NT_SUCCESS(status))ZwWaitForSingleObject(handle,FALSE,NULL);ZwClose(handle);}return status;
}
static VOID StopLocked(XUSB_USB_DEVICE* d,BOOLEAN cancelling) {
    PETHREAD thread;
    WdfWaitLockAcquire(gGate,NULL);d->running=FALSE;d->recovering=cancelling;
    if(captured && d->captureId)IcRevokeAllLocked(IC_RESET_DEVICE);
    thread=d->thread;d->thread=NULL;KeSetEvent(&d->stop,IO_NO_INCREMENT,FALSE);
    DrainReadsLocked(d,cancelling?STATUS_CANCELLED:STATUS_DEVICE_NOT_CONNECTED,
        cancelling?USBD_STATUS_CANCELED:USBD_STATUS_DEVICE_GONE);
    d->outputHead=d->outputCount=0;d->restorePending=FALSE;WdfWaitLockRelease(gGate);
    if(thread){KeWaitForSingleObject(thread,Executive,KernelMode,FALSE,NULL);ObDereferenceObject(thread);}
}
static VOID Forward(WDFREQUEST request,WDFDEVICE device) {
    WDF_REQUEST_SEND_OPTIONS options;WdfRequestFormatRequestUsingCurrentType(request);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    if(!WdfRequestSend(request,WdfDeviceGetIoTarget(device),&options))WdfRequestComplete(request,WdfRequestGetStatus(request));
}
static VOID FindPipes(XUSB_USB_DEVICE* d,PURB urb) {
    ULONG offset,total;PUCHAR base=(PUCHAR)urb;USBD_PIPE_HANDLE pipe=NULL;ULONG bytes=0;UCHAR number=0;
    d->inPipe=NULL;d->configured=FALSE;
    if(urb->UrbHeader.Function==URB_FUNCTION_SELECT_CONFIGURATION)offset=FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION,Interface);
    else offset=FIELD_OFFSET(struct _URB_SELECT_INTERFACE,Interface);
    total=urb->UrbHeader.Length;
    while(offset+FIELD_OFFSET(USBD_INTERFACE_INFORMATION,Pipes)<=total) {
        PUSBD_INTERFACE_INFORMATION intf=(PUSBD_INTERFACE_INFORMATION)(base+offset);ULONG i;
        if(intf->Length<FIELD_OFFSET(USBD_INTERFACE_INFORMATION,Pipes) || intf->Length>total-offset ||
           intf->NumberOfPipes>(intf->Length-FIELD_OFFSET(USBD_INTERFACE_INFORMATION,Pipes))/sizeof(USBD_PIPE_INFORMATION))return;
        if(intf->AlternateSetting==0 && intf->Class==0xFF && intf->SubClass==0x5D && intf->Protocol==1) {
            for(i=0;i<intf->NumberOfPipes;++i) {
                PUSBD_PIPE_INFORMATION p=&intf->Pipes[i];
                if(p->PipeType!=UsbdPipeTypeInterrupt || !(p->EndpointAddress&0x80) ||
                   p->MaximumPacketSize<IC_XUSB_USB_NOMINAL_BYTES || p->MaximumPacketSize>XUSB_USB_PACKET_BYTES)continue;
                if(pipe)return;pipe=p->PipeHandle;bytes=p->MaximumPacketSize;number=intf->InterfaceNumber;
            }
        }
        offset+=intf->Length;
    }
    if(pipe){d->inPipe=pipe;d->packetBytes=bytes;d->interfaceNumber=number;d->configured=TRUE;}
}
static VOID InternalUsb(WDFQUEUE queue,WDFREQUEST request,size_t output,size_t input,ULONG code) {
    XUSB_USB_DEVICE* d=XusbUsbDevice(WdfIoQueueGetDevice(queue));PURB urb;NTSTATUS status;
    UNREFERENCED_PARAMETER(output);UNREFERENCED_PARAMETER(input);
    if(code!=IOCTL_INTERNAL_USB_SUBMIT_URB){Forward(request,d->device);return;}
    urb=RequestUrb(request);
    if(!urb || urb->UrbHeader.Length<sizeof(struct _URB_HEADER)){Forward(request,d->device);return;}
    if(urb->UrbHeader.Function==URB_FUNCTION_SELECT_INTERFACE &&
       urb->UrbHeader.Length>=FIELD_OFFSET(struct _URB_SELECT_INTERFACE,Interface)+FIELD_OFFSET(USBD_INTERFACE_INFORMATION,Pipes)) {
        PUSBD_INTERFACE_INFORMATION intf=&urb->UrbSelectInterface.Interface;
        BOOLEAN current=d->configured && intf->InterfaceNumber==d->interfaceNumber;
        BOOLEAN data=intf->AlternateSetting==0 && intf->Class==0xFF && intf->SubClass==0x5D && intf->Protocol==1;
        if(!current && !data){Forward(request,d->device);return;}
    }
    if(urb->UrbHeader.Function==URB_FUNCTION_SELECT_CONFIGURATION || urb->UrbHeader.Function==URB_FUNCTION_SELECT_INTERFACE) {
        WDF_REQUEST_SEND_OPTIONS options;status=EnsureCompletion(request);
        if(!NT_SUCCESS(status)){WdfRequestComplete(request,status);return;}
        WdfWaitLockAcquire(d->stateLock,NULL);StopLocked(d,TRUE);
        WdfWaitLockAcquire(gGate,NULL);d->configured=d->valid=d->unsupported=FALSE;d->physicalLength=0;WdfWaitLockRelease(gGate);
        WdfRequestFormatRequestUsingCurrentType(request);WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
        WdfRequestSend(request,WdfDeviceGetIoTarget(d->device),&options);status=WdfRequestGetStatus(request);
        if(NT_SUCCESS(status) && USBD_SUCCESS(urb->UrbHeader.Status)) {
            WdfWaitLockAcquire(gGate,NULL);FindPipes(d,urb);WdfWaitLockRelease(gGate);(void)StartLocked(d);
        }
        WdfWaitLockAcquire(gGate,NULL);d->recovering=FALSE;
        if(!d->running)DrainReadsLocked(d,STATUS_CANCELLED,USBD_STATUS_CANCELED);else IcAllReadersReadyLocked();WdfWaitLockRelease(gGate);
        WdfWaitLockRelease(d->stateLock);QueueCompletion(d,request,status,WdfRequestGetInformation(request));return;
    }
    if((urb->UrbHeader.Function==URB_FUNCTION_ABORT_PIPE || urb->UrbHeader.Function==URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL ||
        urb->UrbHeader.Function==URB_FUNCTION_SYNC_RESET_PIPE || urb->UrbHeader.Function==URB_FUNCTION_SYNC_CLEAR_STALL) &&
       urb->UrbHeader.Length>=sizeof(struct _URB_PIPE_REQUEST)) {
        BOOLEAN ours;WdfWaitLockAcquire(d->stateLock,NULL);WdfWaitLockAcquire(gGate,NULL);
        ours=d->configured && urb->UrbPipeRequest.PipeHandle==d->inPipe;WdfWaitLockRelease(gGate);
        if(ours){WDF_REQUEST_SEND_OPTIONS options;status=EnsureCompletion(request);
            if(!NT_SUCCESS(status)){WdfWaitLockRelease(d->stateLock);WdfRequestComplete(request,status);return;}
            StopLocked(d,TRUE);WdfRequestFormatRequestUsingCurrentType(request);
            WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
            WdfRequestSend(request,WdfDeviceGetIoTarget(d->device),&options);status=WdfRequestGetStatus(request);
            if(NT_SUCCESS(status) && USBD_SUCCESS(urb->UrbHeader.Status))(void)StartLocked(d);
            WdfWaitLockAcquire(gGate,NULL);d->recovering=FALSE;
            if(!d->running)DrainReadsLocked(d,STATUS_CANCELLED,USBD_STATUS_CANCELED);else IcAllReadersReadyLocked();WdfWaitLockRelease(gGate);
            WdfWaitLockRelease(d->stateLock);QueueCompletion(d,request,status,WdfRequestGetInformation(request));return;}
        WdfWaitLockRelease(d->stateLock);
    }
    WdfWaitLockAcquire(gGate,NULL);
    if(urb->UrbHeader.Function==URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER &&
       urb->UrbHeader.Length>=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER) && (d->configured || d->recovering) &&
       urb->UrbBulkOrInterruptTransfer.PipeHandle==d->inPipe &&
       (urb->UrbBulkOrInterruptTransfer.TransferFlags&USBD_TRANSFER_DIRECTION_IN)) {
        status=EnsureCompletion(request);
        if(NT_SUCCESS(status)){status=d->powered && (d->running || d->recovering) && !d->retired?WdfRequestForwardToIoQueue(request,d->reads):STATUS_DEVICE_NOT_READY;
            if(NT_SUCCESS(status) && d->running){FlushOutputLocked(d);TryRestoreLocked(d);}}
        WdfWaitLockRelease(gGate);
        if(!NT_SUCCESS(status)){urb->UrbHeader.Status=USBD_STATUS_DEVICE_GONE;urb->UrbBulkOrInterruptTransfer.TransferBufferLength=0;
            if(UsbCompletion(request))QueueCompletion(d,request,status,0);else WdfRequestComplete(request,status);}return;
    }
    WdfWaitLockRelease(gGate);Forward(request,d->device);
}
static VOID Default(WDFQUEUE queue,WDFREQUEST request){Forward(request,WdfIoQueueGetDevice(queue));}
static VOID Ioctl(WDFQUEUE queue,WDFREQUEST request,size_t output,size_t input,ULONG code) {
    XUSB_USB_DEVICE* d=XusbUsbDevice(WdfIoQueueGetDevice(queue));NTSTATUS status;
    UNREFERENCED_PARAMETER(output);UNREFERENCED_PARAMETER(input);
    if(code!=IOCTL_IC_XUSB_SOURCE){Forward(request,d->device);return;}
    WdfWaitLockAcquire(gGate,NULL);status=CaptureReady(d)?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
    WdfWaitLockRelease(gGate);WdfRequestComplete(request,status);
}
static NTSTATUS Resume(WDFDEVICE device) {
    XUSB_USB_DEVICE* d=XusbUsbDevice(device);
    WdfWaitLockAcquire(d->stateLock,NULL);WdfWaitLockAcquire(gGate,NULL);IcTrialResumeLocked();d->powered=TRUE;WdfWaitLockRelease(gGate);
    (void)StartLocked(d);WdfWaitLockRelease(d->stateLock);WdfIoQueueStart(d->reads);WdfIoQueueStart(d->dispatch);return STATUS_SUCCESS;
}
static VOID StopDevice(WDFDEVICE device) {
    XUSB_USB_DEVICE* d=XusbUsbDevice(device);WdfWaitLockAcquire(d->stateLock,NULL);WdfWaitLockAcquire(gGate,NULL);
    d->powered=FALSE;WdfWaitLockRelease(gGate);StopLocked(d,FALSE);WdfWaitLockRelease(d->stateLock);
}
static NTSTATUS Suspend(WDFDEVICE device) {
    XUSB_USB_DEVICE* d=XusbUsbDevice(device);StopDevice(device);
    // No upper callbacks while lifecycle/state locks are held.
    WdfIoQueuePurge(d->dispatch,NULL,NULL);WdfIoQueuePurge(d->reads,NULL,NULL);
    WdfIoQueuePurgeSynchronously(d->dispatch);WdfIoQueuePurgeSynchronously(d->reads);DrainCompletions(d);return STATUS_SUCCESS;
}
static VOID ManagedCleanup(WDFDEVICE device){
    XUSB_USB_DEVICE* d=XusbUsbDevice(device);(void)Suspend(device);
    WdfWaitLockAcquire(gGate,NULL);d->retired=TRUE;
    IcMonitorRemoved(IC_TYPE_CONTROLLER_XUSB,d->info.instance_token);WdfWaitLockRelease(gGate);
}
static NTSTATUS Pnp(WDFDEVICE device,PIRP irp) {
    XUSB_USB_DEVICE* d=XusbUsbDevice(device);WdfWaitLockAcquire(gGate,NULL);d->retired=TRUE;
    IcMonitorRemoved(IC_TYPE_CONTROLLER_XUSB,d->info.instance_token);WdfWaitLockRelease(gGate);
    StopDevice(device);IoSkipCurrentIrpStackLocation(irp);return WdfDeviceWdmDispatchPreprocessedIrp(device,irp);
}
static VOID Cleanup(WDFOBJECT object) {
    XUSB_USB_DEVICE* d=XusbUsbDevice(object);WdfWaitLockAcquire(gGate,NULL);
    if(d->registered){RemoveEntryList(&d->link);d->registered=FALSE;}WdfWaitLockRelease(gGate);if(d->counted)IcFdoRemoved();
}
static USHORT HardwareIdNumber(const USHORT* text,ULONG length,PCWSTR prefix) {
    ULONG i,j;USHORT value;
    for(i=0;i+8<=length && text[i];++i){for(j=0;j<4 && text[i+j]==prefix[j];++j){}if(j!=4)continue;value=0;
        for(j=4;j<8;++j){USHORT c=text[i+j],digit;if(c>='0' && c<='9')digit=c-'0';else if(c>='A' && c<='F')digit=c-'A'+10;
            else if(c>='a' && c<='f')digit=c-'a'+10;else break;value=(USHORT)((value<<4)|digit);}if(j==8)return value;}
    return 0;
}
NTSTATUS IcXusbUsbAdd(PWDFDEVICE_INIT init) {
    WDFDEVICE device;XUSB_USB_DEVICE* d;WDF_OBJECT_ATTRIBUTES a;WDF_PNPPOWER_EVENT_CALLBACKS p;
    WDF_IO_QUEUE_CONFIG q;WDF_DPC_CONFIG dpc;NTSTATUS status;ULONG bytes;UCHAR minor=IRP_MN_SURPRISE_REMOVAL;
    const GUID guid=IC_XUSB_INTERFACE_GUID;WdfFdoInitSetFilter(init);
    status=WdfDeviceInitAssignWdmIrpPreprocessCallback(init,Pnp,IRP_MJ_PNP,&minor,1);if(!NT_SUCCESS(status))return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);p.EvtDeviceSelfManagedIoInit=Resume;p.EvtDeviceSelfManagedIoRestart=Resume;
    p.EvtDeviceSelfManagedIoSuspend=Suspend;p.EvtDeviceSelfManagedIoCleanup=ManagedCleanup;WdfDeviceInitSetPnpPowerEventCallbacks(init,&p);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,XUSB_USB_COMPLETION);WdfDeviceInitSetRequestAttributes(init,&a);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,XUSB_USB_DEVICE);a.ExecutionLevel=WdfExecutionLevelPassive;
    a.SynchronizationScope=WdfSynchronizationScopeNone;a.EvtCleanupCallback=Cleanup;
    status=WdfDeviceCreate(&init,&a,&device);if(!NT_SUCCESS(status))return status;
    d=XusbUsbDevice(device);d->device=device;InitializeListHead(&d->link);InitializeListHead(&d->completions);IcCacheDeviceInstance(device,d->instance);
    KeInitializeEvent(&d->stop,NotificationEvent,FALSE);KeInitializeEvent(&d->started,NotificationEvent,FALSE);
    KeInitializeEvent(&d->done,SynchronizationEvent,FALSE);KeInitializeEvent(&d->maintenance,SynchronizationEvent,FALSE);
    KeInitializeEvent(&d->completionsIdle,NotificationEvent,FALSE);
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;status=WdfWaitLockCreate(&a,&d->stateLock);if(!NT_SUCCESS(status))return status;
    status=WdfSpinLockCreate(&a,&d->completionLock);if(!NT_SUCCESS(status))return status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,XUSB_USB_DPC);a.ParentObject=device;
    WDF_DPC_CONFIG_INIT(&dpc,CompleteDpc);dpc.AutomaticSerialization=FALSE;
    status=WdfDpcCreate(&dpc,&a,&d->completionDpc);if(!NT_SUCCESS(status))return status;UsbDpc(d->completionDpc)->device=d;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;status=WdfRequestCreate(&a,WdfDeviceGetIoTarget(device),&d->request);if(!NT_SUCCESS(status))return status;
    a.ParentObject=d->request;status=WdfMemoryCreatePreallocated(&a,&d->urb,sizeof(d->urb),&d->urbMemory);if(!NT_SUCCESS(status))return status;
    WDF_IO_QUEUE_CONFIG_INIT(&q,WdfIoQueueDispatchManual);q.PowerManaged=WdfFalse;q.EvtIoCanceledOnQueue=CancelRead;
    status=WdfIoQueueCreate(device,&q,WDF_NO_OBJECT_ATTRIBUTES,&d->reads);if(!NT_SUCCESS(status))return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q,WdfIoQueueDispatchParallel);q.PowerManaged=WdfFalse;
    q.EvtIoInternalDeviceControl=InternalUsb;q.EvtIoDeviceControl=Ioctl;q.EvtIoDefault=Default;
    status=WdfIoQueueCreate(device,&q,WDF_NO_OBJECT_ATTRIBUTES,&d->dispatch);if(!NT_SUCCESS(status))return status;
    d->info.type=IC_TYPE_CONTROLLER_XUSB;d->info.instance_token=(ULONG)InterlockedIncrement(&nextId);d->info.input_bytes=IC_XUSB_STATE_BYTES;
    if(NT_SUCCESS(WdfDeviceQueryProperty(device,DevicePropertyHardwareID,sizeof(d->info.hardware_id)-2,d->info.hardware_id,&bytes)))d->info.flags|=IC_INFO_HARDWARE_ID_VALID;
    d->info.vid=HardwareIdNumber(d->info.hardware_id,RTL_NUMBER_OF(d->info.hardware_id),L"VID_");
    d->info.pid=HardwareIdNumber(d->info.hardware_id,RTL_NUMBER_OF(d->info.hardware_id),L"PID_");
    if(NT_SUCCESS(WdfDeviceQueryProperty(device,DevicePropertyDeviceDescription,sizeof(d->info.name)-2,d->info.name,&bytes)))d->info.flags|=IC_INFO_NAME_VALID;
    status=IcFdoAdded();if(!NT_SUCCESS(status))return status;d->counted=TRUE;
    status=WdfDeviceCreateDeviceInterface(device,&guid,NULL);if(!NT_SUCCESS(status))return status;
    WdfWaitLockAcquire(gGate,NULL);InsertTailList(&devices,&d->link);d->registered=TRUE;
    IcMonitorPublish(IC_TYPE_CONTROLLER_XUSB,d->info.instance_token,IC_MONITOR_DEVICE,0,NULL,0,NULL,STATUS_SUCCESS);
    WdfWaitLockRelease(gGate);return STATUS_SUCCESS;
}
