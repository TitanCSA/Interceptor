#include "ControllerGip.h"
#include "ControllerHid.h"
#include "ControllerMonitor.h"
#include "../Shared/ControllerGipSequence.h"
#include "../Shared/ControllerGipOut.h"
#include <usb.h>
#include <usbioctl.h>

// Experimental MS-GIPUSB data-interface lower filter. A single private IN URB
// feeds a bounded output FIFO and the public capture FIFO. Other USB interfaces
// and protocol messages retain their normal Windows path. No private XboxGIP
// IOCTL layout is assumed here.
#define GIP_PACKET_BYTES 64u
#define GIP_OUTPUT_CAPACITY 128u
#define GIP_CACHE_CAPACITY 16u
typedef struct GIP_METADATA_CACHE { BOOLEAN valid; UCHAR hello[28]; IC_GIP_METADATA metadata; } GIP_METADATA_CACHE;
typedef struct GIP_PACKET { ULONG length; UCHAR origin; UCHAR bytes[GIP_PACKET_BYTES]; } GIP_PACKET;
typedef struct GIP_COMPLETION {
    LIST_ENTRY link; WDFREQUEST request; NTSTATUS status; ULONG_PTR information;
    PVOID originalBuffer; PMDL originalMdl; ULONG originalLength,forwardedLength; UCHAR outgoing[GIP_PACKET_BYTES];
    PUCHAR outgoingLarge;
    IC_GIP_ACK_TICKET ackTicket;
    BOOLEAN ackSucceeded;
    BOOLEAN traced;IC_GIP_TRACE trace;
} GIP_COMPLETION;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GIP_COMPLETION,GipCompletion)
typedef struct GIP_DEVICE {
    WDFDEVICE device;
    WDFWAITLOCK stateLock;
    WDFSPINLOCK completionLock;
    WDFDPC completionDpc;
    WDFWORKITEM outWork;
    WDFQUEUE reads,dispatch;
    LIST_ENTRY link,completions,outCompletions;
    BOOLEAN outScheduled;
    BOOLEAN registered,counted,powered,ready,configured,running,valid,restorePending,helloValid,unsupported,translated;
    BOOLEAN monitorRetired;
    BOOLEAN completing,completeAgain,barrier;
    KEVENT completionsIdle;
    IC_DEVICE_INFO info;
    USHORT instance[256];
    ULONG captureId;
    USBD_PIPE_HANDLE inPipe,outPipe;
    IC_GIP_METADATA metadata;
    IC_GIP_SEQUENCE_STATE sequences;
    UCHAR hello[28];
    IC_NATIVE_GAMEPAD_STATE physical,delivered;
    UCHAR physicalFrame[IC_GIP_REPORT_MAX],inputSequence;
    ULONG physicalLength;
    GIP_PACKET output[GIP_OUTPUT_CAPACITY];
    ULONG outputHead,outputCount;
    WDFREQUEST request,ackRequest;
    WDFMEMORY urbMemory,ackMemory;
    struct _URB_BULK_OR_INTERRUPT_TRANSFER urb,ackUrb;
    UCHAR buffer[GIP_PACKET_BYTES],ack[13];
    BOOLEAN used,ackUsed;
    PETHREAD thread;
    KEVENT stop,started,done,ackDone,maintenance;
    NTSTATUS readStatus,ackStatus;
    IC_GIP_TRACE trace[IC_GIP_TRACE_CAPACITY];ULONG traceHead,traceCount;ULONGLONG traceOrdinal;
    ULONGLONG physicalReports,sendAttempts,resetCalls,restoreAttempts,restoreQueued;
    ULONG lastReadStatus,lastUsbStatus;
} GIP_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GIP_DEVICE,GipDevice)
typedef struct GIP_DPC { GIP_DEVICE* device; } GIP_DPC;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GIP_DPC,GipDpc)
typedef struct GIP_WORK { GIP_DEVICE* device; } GIP_WORK;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GIP_WORK,GipWork)
static LIST_ENTRY devices;
static GIP_METADATA_CACHE metadataCache[GIP_CACHE_CAPACITY];
static ULONG cacheNext;
static LONG nextId;
static IC_CONTROLLER_GIP_RAW_INPUT* captured;
static GIP_DEVICE* targets[IC_MAX_DEVICES];
static ULONG targetCount,head,count;
static PKEVENT readyEvent;

static VOID StopLocked(GIP_DEVICE* d);
static NTSTATUS StartLocked(GIP_DEVICE* d);
static VOID FlushOutputLocked(GIP_DEVICE* d);
static VOID TryRestoreLocked(GIP_DEVICE* d);

static UCHAR NextSequence(UCHAR* sequence) { if(!++*sequence)++*sequence;return *sequence; }
static PURB RequestUrb(WDFREQUEST request) {
    PIO_STACK_LOCATION stack=IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(request));
    return (PURB)stack->Parameters.Others.Argument1;
}
static NTSTATUS EnsureCompletion(WDFREQUEST request) {
    WDF_OBJECT_ATTRIBUTES a;
    if(GipCompletion(request)){GipCompletion(request)->traced=FALSE;return STATUS_SUCCESS;}
    // Forwarded WDFREQUESTs can have been allocated by another driver.
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,GIP_COMPLETION);
    return WdfObjectAllocateContext(request,&a,NULL);
}
static VOID QueueCompletion(GIP_DEVICE* d,WDFREQUEST request,NTSTATUS status,ULONG_PTR information) {
    GIP_COMPLETION* completion=GipCompletion(request);
    completion->request=request;completion->status=status;completion->information=information;
    WdfSpinLockAcquire(d->completionLock);
    InsertTailList(&d->completions,&completion->link);
    WdfSpinLockRelease(d->completionLock);WdfDpcEnqueue(d->completionDpc);
}
static VOID CommitOutLocked(GIP_DEVICE* d,GIP_COMPLETION* completion) {
    IcGipSequenceSetTime(&d->sequences,KeQueryInterruptTime()/10000);
    IcGipCompleteOut(&d->sequences,&completion->ackTicket,completion->ackSucceeded);
    KeSetEvent(&d->maintenance,IO_NO_INCREMENT,FALSE);
}
static VOID CompleteOutWork(WDFWORKITEM work) {
    GIP_DEVICE* d=GipWork(work)->device;
    for(;;) {
        GIP_COMPLETION* completion;
        WdfSpinLockAcquire(d->completionLock);
        if(IsListEmpty(&d->outCompletions)) {
            d->outScheduled=FALSE;WdfSpinLockRelease(d->completionLock);return;
        }
        completion=CONTAINING_RECORD(RemoveHeadList(&d->outCompletions),GIP_COMPLETION,link);
        WdfSpinLockRelease(d->completionLock);
        // gGate is a passive wait lock. OUT can complete at DISPATCH_LEVEL,
        // so its ACK transaction is committed here before the existing DPC
        // delivers the upper completion. Never invoke a callback under gGate.
        WdfWaitLockAcquire(gGate,NULL);CommitOutLocked(d,completion);WdfWaitLockRelease(gGate);
        QueueCompletion(d,completion->request,completion->status,completion->information);
    }
}
static VOID QueueOutCompletion(GIP_DEVICE* d,WDFREQUEST request,NTSTATUS status,ULONG_PTR information,BOOLEAN succeeded) {
    GIP_COMPLETION* completion=GipCompletion(request);
    if(!IcGipOutHasPins(&completion->ackTicket)){QueueCompletion(d,request,status,information);return;}
    completion->request=request;completion->status=status;completion->information=information;completion->ackSucceeded=succeeded;
    WdfSpinLockAcquire(d->completionLock);
    InsertTailList(&d->outCompletions,&completion->link);
    if(!d->outScheduled){d->outScheduled=TRUE;WdfWorkItemEnqueue(d->outWork);}
    WdfSpinLockRelease(d->completionLock);
}
static VOID CompleteDpc(WDFDPC dpc) {
    GIP_DEVICE* d=GipDpc(dpc)->device;ULONG work=0;BOOLEAN again;
    WdfSpinLockAcquire(d->completionLock);
    if(d->completing){d->completeAgain=TRUE;WdfSpinLockRelease(d->completionLock);return;}
    d->completing=TRUE;WdfSpinLockRelease(d->completionLock);
    while(work++<64) {
        GIP_COMPLETION* completion;BOOLEAN traced;
        WdfSpinLockAcquire(d->completionLock);
        if(IsListEmpty(&d->completions)){WdfSpinLockRelease(d->completionLock);break;}
        completion=CONTAINING_RECORD(RemoveHeadList(&d->completions),GIP_COMPLETION,link);
        traced=completion->traced;
        if(completion->traced) {
            ULONG slot=(d->traceHead+d->traceCount)%IC_GIP_TRACE_CAPACITY;
            if(d->traceCount==IC_GIP_TRACE_CAPACITY){slot=d->traceHead;d->traceHead=(d->traceHead+1)%IC_GIP_TRACE_CAPACITY;}
            else ++d->traceCount;
            completion->trace.ordinal=++d->traceOrdinal;completion->trace.timestamp=KeQueryPerformanceCounter(NULL).QuadPart;
            d->trace[slot]=completion->trace;completion->traced=FALSE;
        }
        WdfSpinLockRelease(d->completionLock);
        if(traced && NT_SUCCESS(completion->status) && IcMonitorEnabled()) {
            IC_NATIVE_GAMEPAD_STATE state;const UCHAR* bytes=completion->trace.bytes;ULONG length=completion->trace.length;
            if(IcGipDecodeGamepadReport(bytes,length,14,&state))
                IcMonitorPublish(IC_TYPE_CONTROLLER_GIP,d->info.instance_token,IC_MONITOR_OUT,
                    IC_MONITOR_NATIVE_VALID,bytes,length,&state,STATUS_SUCCESS);
            else if(length==6 && bytes[0]==7 && (bytes[1]==0x20 || bytes[1]==0x30) &&
                    bytes[3]==2 && bytes[4]<=1 && bytes[5]==0x5B) {
                RtlZeroMemory(&state,sizeof(state));state.extra_buttons=bytes[4];
                IcMonitorPublish(IC_TYPE_CONTROLLER_GIP,d->info.instance_token,IC_MONITOR_OUT,
                    IC_MONITOR_GUIDE_VALID,bytes,length,&state,STATUS_SUCCESS);
            }
            else if(length && bytes[0]==0x20)
                IcMonitorPublish(IC_TYPE_CONTROLLER_GIP,d->info.instance_token,IC_MONITOR_OUT,
                    IC_MONITOR_RAW|IC_MONITOR_ERROR,bytes,length,NULL,STATUS_NOT_SUPPORTED);
        }
        // Xbox's USB client can immediately resubmit from this call. No gate,
        // lifecycle lock or spin lock may be held across its completion.
        WdfRequestCompleteWithInformation(completion->request,completion->status,completion->information);
    }
    WdfSpinLockAcquire(d->completionLock);
    again=d->completeAgain || !IsListEmpty(&d->completions);
    d->completeAgain=FALSE;d->completing=FALSE;
    if(d->barrier && !again){d->barrier=FALSE;KeSetEvent(&d->completionsIdle,IO_NO_INCREMENT,FALSE);}
    WdfSpinLockRelease(d->completionLock);
    if(again)WdfDpcEnqueue(dpc);
}
static VOID DrainCompletions(GIP_DEVICE* d) {
    KeClearEvent(&d->completionsIdle);
    WdfSpinLockAcquire(d->completionLock);d->barrier=TRUE;WdfSpinLockRelease(d->completionLock);
    WdfDpcEnqueue(d->completionDpc);
    KeWaitForSingleObject(&d->completionsIdle,Executive,KernelMode,FALSE,NULL);
}
static VOID CancelRead(WDFQUEUE queue,WDFREQUEST request) {
    GIP_DEVICE* d=GipDevice(WdfIoQueueGetDevice(queue));PURB urb=RequestUrb(request);
    if(urb){urb->UrbHeader.Status=USBD_STATUS_CANCELED;urb->UrbBulkOrInterruptTransfer.TransferBufferLength=0;}
    QueueCompletion(d,request,STATUS_CANCELLED,0);
}
static BOOLEAN CopyPacket(GIP_DEVICE* d,WDFREQUEST request,const GIP_PACKET* packet) {
    PURB urb=RequestUrb(request);PVOID buffer=NULL;NTSTATUS status=STATUS_SUCCESS;
    if(!urb || urb->UrbHeader.Length<sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER))status=STATUS_INVALID_PARAMETER;
    else if(urb->UrbBulkOrInterruptTransfer.TransferBufferLength<packet->length)status=STATUS_BUFFER_TOO_SMALL;
    else {
        if(urb->UrbBulkOrInterruptTransfer.TransferBufferMDL) {
            if(MmGetMdlByteCount(urb->UrbBulkOrInterruptTransfer.TransferBufferMDL)>=packet->length)
                buffer=MmGetSystemAddressForMdlSafe(urb->UrbBulkOrInterruptTransfer.TransferBufferMDL,NormalPagePriority|MdlMappingNoExecute);
        } else buffer=urb->UrbBulkOrInterruptTransfer.TransferBuffer;
        if(!buffer)status=STATUS_INSUFFICIENT_RESOURCES;
        else {
            GIP_COMPLETION* c=GipCompletion(request);RtlCopyMemory(buffer,packet->bytes,packet->length);
            RtlZeroMemory(&c->trace,sizeof(c->trace));c->trace.origin=packet->origin;c->trace.length=packet->length;
            RtlCopyMemory(c->trace.bytes,packet->bytes,packet->length);c->traced=TRUE;
        }
    }
    if(urb){urb->UrbHeader.Status=NT_SUCCESS(status)?USBD_STATUS_SUCCESS:USBD_STATUS_INVALID_PARAMETER;
        urb->UrbBulkOrInterruptTransfer.TransferBufferLength=NT_SUCCESS(status)?packet->length:0;}
    QueueCompletion(d,request,status,0);return NT_SUCCESS(status);
}
static VOID FlushOutputLocked(GIP_DEVICE* d) {
    WDFREQUEST request;
    while(d->outputCount && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(d->reads,&request))) {
        if(CopyPacket(d,request,&d->output[d->outputHead])) {
            d->outputHead=(d->outputHead+1)%GIP_OUTPUT_CAPACITY;--d->outputCount;
            KeSetEvent(&d->maintenance,IO_NO_INCREMENT,FALSE);
        }
    }
}
static BOOLEAN OutputLocked(GIP_DEVICE* d,const UCHAR* bytes,ULONG length,UCHAR origin) {
    GIP_PACKET* packet;
    if(!length || length>GIP_PACKET_BYTES || d->outputCount==GIP_OUTPUT_CAPACITY)return FALSE;
    packet=&d->output[(d->outputHead+d->outputCount++)%GIP_OUTPUT_CAPACITY];
    packet->length=length;packet->origin=origin;RtlCopyMemory(packet->bytes,bytes,length);
    FlushOutputLocked(d);return TRUE;
}
static VOID ClearInputLocked(GIP_DEVICE* d) {
    ULONG i,kept=0;
    // Retain protocol, metadata and audio traffic in its original FIFO order.
    for(i=0;i<d->outputCount;++i) {
        GIP_PACKET* packet=&d->output[(d->outputHead+i)%GIP_OUTPUT_CAPACITY];
        if(!packet->origin){d->output[(d->outputHead+kept)%GIP_OUTPUT_CAPACITY]=*packet;++kept;}
    }
    d->outputCount=kept;KeSetEvent(&d->maintenance,IO_NO_INCREMENT,FALSE);
}
static BOOLEAN DeliverStateLocked(GIP_DEVICE* d,const IC_NATIVE_GAMEPAD_STATE* state,const UCHAR* report,ULONG length,UCHAR origin) {
    UCHAR bytes[IC_GIP_REPORT_MAX],guide[6]={7,0x20,0,2,0,0x5B};
    IC_NATIVE_GAMEPAD_STATE ordinary=*state;BOOLEAN guideChanged=state->extra_buttons!=d->delivered.extra_buttons;
    if(GIP_OUTPUT_CAPACITY-d->outputCount<(guideChanged?2u:1u))return FALSE;
    if(!length){length=d->physicalLength;report=d->physicalFrame;}
    if(length!=d->physicalLength || length>sizeof(bytes))return FALSE;
    RtlCopyMemory(bytes,report,length);ordinary.extra_buttons=0;
    // Keep-alive comes from the device even when an application supplies raw
    // extension bytes. It must never become an application-owned control.
    bytes[4]=(UCHAR)((bytes[4]&~2u)|(d->physicalFrame[4]&2u));
    if(!IcGipReplaceGamepadReport(bytes,length,d->metadata.input_length,&ordinary))return FALSE;
    IcGipSequenceSetTime(&d->sequences,KeQueryInterruptTime()/10000);
    if(guideChanged && !IcGipSequenceGuide(&d->sequences,&guide[2]))return FALSE;
    bytes[2]=NextSequence(&d->inputSequence);
    if(guideChanged){guide[4]=state->extra_buttons&1;OutputLocked(d,guide,sizeof(guide),origin);}
    OutputLocked(d,bytes,length,origin);d->delivered=*state;return TRUE;
}
static VOID TryRestoreLocked(GIP_DEVICE* d) {
    if(d->restorePending && d->valid && d->powered) {
        ++d->restoreAttempts;
        if(DeliverStateLocked(d,&d->physical,NULL,0,IC_GIP_ORIGIN_RESTORE)){d->restorePending=FALSE;++d->restoreQueued;}
    }
}
void IcGipInitialize(void){InitializeListHead(&devices);}
NTSTATUS IcGipMonitorDeviceLocked(ULONG token,IC_DEVICE_INFO* info) {
    PLIST_ENTRY p;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        GIP_DEVICE* d=CONTAINING_RECORD(p,GIP_DEVICE,link);
        if(!d->monitorRetired && d->info.instance_token==token){*info=d->info;info->id=token;return STATUS_SUCCESS;}
    }
    return STATUS_DEVICE_NOT_CONNECTED;
}
static BOOLEAN CaptureReady(const GIP_DEVICE* d) {
    return d->ready && d->running && d->powered && d->configured && d->valid &&
        d->metadata.complete && d->metadata.gamepad && !d->unsupported;
}
NTSTATUS IcGipQueryLocked(ULONG token,IC_DEVICE_QUERY_RESULT* result,BOOLEAN ordinal) {
    PLIST_ENTRY p;ULONG position=0;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        GIP_DEVICE* d=CONTAINING_RECORD(p,GIP_DEVICE,link);
        if(d->monitorRetired)continue;
        if((ordinal?++position:d->info.instance_token)!=token)continue;
        result->instance_token=d->info.instance_token;
        result->flags=(CaptureReady(d)?1u:0u)|(d->running?2u:0u)|(d->powered?4u:0u)|(d->valid?8u:0u)|
            (d->configured?16u:0u)|(d->restorePending?32u:0u)|(d->unsupported?64u:0u)|(d->helloValid?128u:0u)|
            (d->metadata.active?256u:0u)|(d->metadata.complete?512u:0u)|(d->metadata.gamepad?1024u:0u);
        result->last_read_status=d->lastReadStatus;result->last_usb_status=d->lastUsbStatus;
        result->physical_reports=d->physicalReports;result->report_bytes=d->physicalLength;
        result->metadata_bytes=d->metadata.total;result->metadata_input_bytes=d->metadata.input_length;
        RtlCopyMemory(result->instance,d->instance,sizeof(result->instance));
        if(result->instance[0])result->flags|=IC_QUERY_IDENTITY_VALID;
        return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_NOT_CONNECTED;
}
ULONG IcGipCountLocked(void) {
    ULONG n=0;PLIST_ENTRY p;for(p=devices.Flink;p!=&devices;p=p->Flink)if(CaptureReady(CONTAINING_RECORD(p,GIP_DEVICE,link)))++n;return n;
}
NTSTATUS IcGipCaptureLocked(PKEVENT event,IC_DEVICE_INFO* output,ULONG capacity,ULONG* written) {
    ULONG n=IcGipCountLocked();PLIST_ENTRY p;
    if(!n)return STATUS_DEVICE_NOT_READY;if(n>capacity || n>IC_MAX_DEVICES)return STATUS_BUFFER_TOO_SMALL;
    if(captured)return STATUS_SHARING_VIOLATION;
    captured=ExAllocatePoolZero(NonPagedPoolNx,sizeof(*captured)*IC_GIP_CAPACITY,FILTER_TAG);
    if(!captured)return STATUS_INSUFFICIENT_RESOURCES;
    n=0;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        GIP_DEVICE* d=CONTAINING_RECORD(p,GIP_DEVICE,link);d->captureId=0;if(!CaptureReady(d))continue;
        d->captureId=++n;d->translated=TRUE;targets[n-1]=d;output[n-1]=d->info;output[n-1].id=n;
        ClearInputLocked(d);d->restorePending=FALSE;
    }
    targetCount=n;head=count=0;readyEvent=event;KeClearEvent(event);*written=n;return STATUS_SUCCESS;
}
NTSTATUS IcGipArmLocked(PKEVENT event) {
    if(captured)return STATUS_SHARING_VIOLATION;
    captured=ExAllocatePoolZero(NonPagedPoolNx,sizeof(*captured)*IC_GIP_CAPACITY,FILTER_TAG);
    if(!captured)return STATUS_INSUFFICIENT_RESOURCES;
    targetCount=head=count=0;readyEvent=event;KeClearEvent(event);return STATUS_SUCCESS;
}
void IcGipRefreshCaptureLocked(void) {
    PLIST_ENTRY p;
    if(!captured || !IcAllCaptureLocked(IC_TYPE_CONTROLLER_GIP))return;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        GIP_DEVICE* d=CONTAINING_RECORD(p,GIP_DEVICE,link);
        if(d->captureId || !CaptureReady(d))continue;
        if(!IcAllAttachLocked(IC_TYPE_CONTROLLER_GIP))return;
        d->captureId=++targetCount;d->translated=TRUE;targets[targetCount-1]=d;
        ClearInputLocked(d);d->restorePending=FALSE;
    }
}
ULONG IcGipSnapshotLocked(IC_DEVICE_INFO* output,ULONG capacity) {
    ULONG i,n=min(targetCount,capacity);
    for(i=0;i<n;++i){output[i]=targets[i]->info;output[i].id=i+1;}
    return n;
}
void IcGipResetLocked(void) {
    PLIST_ENTRY p;if(!captured)return;
    ExFreePoolWithTag(captured,FILTER_TAG);captured=NULL;head=count=targetCount=0;readyEvent=NULL;
    RtlZeroMemory(targets,sizeof(targets));
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        GIP_DEVICE* d=CONTAINING_RECORD(p,GIP_DEVICE,link);
        // An all-family context can close while a source is still preparing.
        // It never owned this device's native queue or delivered state.
        if(!d->captureId)continue;
        d->captureId=0;++d->resetCalls;ClearInputLocked(d);
        d->restorePending=d->valid;TryRestoreLocked(d);
    }
}
ULONG IcGipQueuedLocked(void){return count;}
NTSTATUS IcGipGetLocked(ULONG requested,PVOID output,SIZE_T length,SIZE_T* written,BOOLEAN raw) {
    const ULONG item=raw?sizeof(*captured):sizeof(IC_CONTROLLER_GIP_INPUT);
    ULONG n=requested?min(requested,count):count;
    if(length/item<n)return STATUS_BUFFER_TOO_SMALL;
    if(raw) {
        uint32_t readHead=head,readCount=count;
        LONG copied=IcInputReadSelected(&readHead,&readCount,captured,IC_GIP_CAPACITY,sizeof(*captured),
            FIELD_OFFSET(IC_CONTROLLER_GIP_INPUT,device),output,sizeof(*captured),
            (ULONG)min(length/item,IC_GIP_CAPACITY),requested,0,TRUE);
        if(copied<0)return STATUS_BUFFER_TOO_SMALL;
        head=readHead;count=readCount;
    } else {
        ULONG i;
        // Keep the fixed-size typed projection: unlike full raw reports,
        // these prefixes are not contiguous in the capture ring.
        for(i=0;i<n;++i)((IC_CONTROLLER_GIP_INPUT*)output)[i]=captured[(head+i)%IC_GIP_CAPACITY].input;
        head=(head+n)%IC_GIP_CAPACITY;count-=n;
    }
    *written=(SIZE_T)n*item;
    if(!count && readyEvent)KeClearEvent(readyEvent);return STATUS_SUCCESS;
}
NTSTATUS IcGipGetSelectedLocked(const IC_GET_SELECT* request,PVOID output,SIZE_T length,SIZE_T* written,BOOLEAN raw) {
    const BOOLEAN consume=(request->flags&IC_GET_PEEK)==0;
    const ULONG item=raw?sizeof(*captured):sizeof(IC_CONTROLLER_GIP_INPUT);
    uint32_t readHead=head,readCount=count;
    LONG n=IcInputReadSelected(&readHead,&readCount,captured,IC_GIP_CAPACITY,sizeof(*captured),
        FIELD_OFFSET(IC_CONTROLLER_GIP_INPUT,device),output,item,
        (ULONG)min(length/item,IC_GIP_CAPACITY),request->base.count,request->device,consume);
    if(n<0)return STATUS_BUFFER_TOO_SMALL;
    if(consume){head=readHead;count=readCount;}
    if(consume && !count && readyEvent)KeClearEvent(readyEvent);
    *written=(SIZE_T)n*item;return STATUS_SUCCESS;
}
static GIP_DEVICE* FindLocked(ULONG id,BOOLEAN owned) {
    PLIST_ENTRY p;ULONG ordinal=0;GIP_DEVICE* first=NULL;
    if(owned)return targetCount?targets[id && id<=targetCount?id-1:0]:NULL;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        GIP_DEVICE* d=CONTAINING_RECORD(p,GIP_DEVICE,link);if(!d->ready)continue;
        if(!first)first=d;if(++ordinal==id)return d;
    }
    return first;
}
NTSTATUS IcGipSendLocked(PVOID input,SIZE_T length,IC_SEND_RESULT* result,BOOLEAN owned,BOOLEAN raw) {
    SIZE_T item=raw?sizeof(IC_CONTROLLER_GIP_RAW_INPUT):sizeof(IC_CONTROLLER_GIP_INPUT);
    ULONG n=(ULONG)(length/item),i;
    if(!n || n>IC_MAX_SEND || length%item)return STATUS_INVALID_PARAMETER;
    for(i=0;i<n;++i) {
        IC_CONTROLLER_GIP_INPUT* in=(IC_CONTROLLER_GIP_INPUT*)((PUCHAR)input+i*item);
        if(in->reserved || in->padding || in->state.reserved || in->state.extra_buttons>1 ||
           in->state.left_trigger>1023 || in->state.right_trigger>1023 || (in->state.buttons&1))return STATUS_INVALID_PARAMETER;
        if(raw) {
            IC_CONTROLLER_GIP_RAW_INPUT* r=(IC_CONTROLLER_GIP_RAW_INPUT*)in;
            IC_NATIVE_GAMEPAD_STATE state;
            if(r->report_reserved || r->report_length>IC_GIP_REPORT_MAX ||
               (r->report_length && !IcGipDecodeGamepadReport(r->report,r->report_length,14,&state)))return STATUS_INVALID_PARAMETER;
        }
    }
    result->accepted=0;result->status=STATUS_SUCCESS;
    for(i=0;i<n;++i) {
        IC_CONTROLLER_GIP_INPUT* in=(IC_CONTROLLER_GIP_INPUT*)((PUCHAR)input+i*item);
        IC_CONTROLLER_GIP_RAW_INPUT* r=raw?(IC_CONTROLLER_GIP_RAW_INPUT*)in:NULL;
        GIP_DEVICE* d=FindLocked(in->device,owned);
        if(!d || !d->ready){result->status=(ULONG)STATUS_DEVICE_NOT_CONNECTED;break;}
        if(r && r->report_length && r->report_length!=d->physicalLength){result->status=(ULONG)STATUS_INVALID_PARAMETER;break;}
        TryRestoreLocked(d);
        ++d->sendAttempts;
        if(d->restorePending || !DeliverStateLocked(d,&in->state,r?r->report:NULL,r?r->report_length:0,IC_GIP_ORIGIN_SEND)){result->status=(ULONG)STATUS_DEVICE_BUSY;break;}
        ++result->accepted;
    }
    return STATUS_SUCCESS;
}
NTSTATUS IcGipDescriptorLocked(ULONG id,PVOID output,SIZE_T length,SIZE_T* written) {
    GIP_DEVICE* d=FindLocked(id,TRUE);
    if(!d || id!=d->captureId || !d->metadata.complete)return STATUS_DEVICE_NOT_CONNECTED;
    if(length<d->metadata.total)return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(output,d->metadata.bytes,d->metadata.total);*written=d->metadata.total;return STATUS_SUCCESS;
}
NTSTATUS IcGipDiagnosticsLocked(ULONG id,PVOID output,SIZE_T length,SIZE_T* written) {
    PLIST_ENTRY p;ULONG ordinal=0,i;GIP_DEVICE* d=NULL;IC_GIP_DIAGNOSTICS* result=output;
    if(length<sizeof(*result))return STATUS_BUFFER_TOO_SMALL;
    if(!id)id=1;
    for(p=devices.Flink;p!=&devices;p=p->Flink)if(++ordinal==id){d=CONTAINING_RECORD(p,GIP_DEVICE,link);break;}
    if(!d)return STATUS_DEVICE_NOT_CONNECTED;
    RtlZeroMemory(result,sizeof(*result));result->version=IC_GIP_DIAGNOSTIC_VERSION;
    result->flags=(d->ready?1u:0u)|(d->running?2u:0u)|(d->powered?4u:0u)|(d->valid?8u:0u)|
        (d->configured?16u:0u)|(d->restorePending?32u:0u)|(d->unsupported?64u:0u)|(d->helloValid?128u:0u)|
        (d->metadata.active?256u:0u)|(d->metadata.complete?512u:0u)|(d->metadata.gamepad?1024u:0u);
    result->capture_id=d->captureId;result->output_count=d->outputCount;
    result->physical_reports=d->physicalReports;result->send_attempts=d->sendAttempts;result->reset_calls=d->resetCalls;
    result->restore_attempts=d->restoreAttempts;result->restore_queued=d->restoreQueued;
    result->last_read_status=d->lastReadStatus;result->last_usb_status=d->lastUsbStatus;
    result->physical=d->physical;result->scheduled=d->delivered;result->last_input_sequence=d->inputSequence;
    WdfSpinLockAcquire(d->completionLock);result->trace_count=d->traceCount;
    for(i=0;i<d->traceCount;++i)result->trace[i]=d->trace[(d->traceHead+i)%IC_GIP_TRACE_CAPACITY];
    WdfSpinLockRelease(d->completionLock);*written=sizeof(*result);return STATUS_SUCCESS;
}
static VOID PhysicalDone(WDFREQUEST request,WDFIOTARGET target,PWDF_REQUEST_COMPLETION_PARAMS params,WDFCONTEXT context) {
    GIP_DEVICE* d=context;UNREFERENCED_PARAMETER(request);UNREFERENCED_PARAMETER(target);
    d->readStatus=params->IoStatus.Status;KeSetEvent(&d->done,IO_NO_INCREMENT,FALSE);
}
static VOID AckDone(WDFREQUEST request,WDFIOTARGET target,PWDF_REQUEST_COMPLETION_PARAMS params,WDFCONTEXT context) {
    GIP_DEVICE* d=context;UNREFERENCED_PARAMETER(request);UNREFERENCED_PARAMETER(target);
    d->ackStatus=params->IoStatus.Status;KeSetEvent(&d->ackDone,IO_NO_INCREMENT,FALSE);
}
static NTSTATUS FormatTransfer(GIP_DEVICE* d,BOOLEAN ack) {
    WDFREQUEST request=ack?d->ackRequest:d->request;WDF_REQUEST_REUSE_PARAMS reuse;NTSTATUS status;
    struct _URB_BULK_OR_INTERRUPT_TRANSFER* urb=ack?&d->ackUrb:&d->urb;
    if(ack?d->ackUsed:d->used) {
        WDF_REQUEST_REUSE_PARAMS_INIT(&reuse,WDF_REQUEST_REUSE_NO_FLAGS,STATUS_SUCCESS);
        status=WdfRequestReuse(request,&reuse);if(!NT_SUCCESS(status))return status;
    }
    if(ack)d->ackUsed=TRUE;else d->used=TRUE;
    RtlZeroMemory(urb,sizeof(*urb));urb->Hdr.Length=(USHORT)sizeof(*urb);urb->Hdr.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    urb->PipeHandle=ack?d->outPipe:d->inPipe;urb->TransferFlags=ack?0:USBD_TRANSFER_DIRECTION_IN|USBD_SHORT_TRANSFER_OK;
    urb->TransferBufferLength=ack?sizeof(d->ack):sizeof(d->buffer);urb->TransferBuffer=ack?d->ack:d->buffer;
    return WdfIoTargetFormatRequestForInternalIoctlOthers(WdfDeviceGetIoTarget(d->device),request,
        IOCTL_INTERNAL_USB_SUBMIT_URB,ack?d->ackMemory:d->urbMemory,NULL,NULL,NULL,NULL,NULL);
}
static NTSTATUS SendGuideAck(GIP_DEVICE* d,UCHAR sequence) {
    NTSTATUS status;PVOID waits[2]={&d->stop,&d->ackDone};LARGE_INTEGER timeout;
    // Only suppressed reliable Guide messages are ACKed here. Metadata/security
    // ACKs continue to be generated by the real Windows protocol driver.
    RtlZeroMemory(d->ack,sizeof(d->ack));d->ack[0]=1;d->ack[1]=0x20;d->ack[2]=sequence;
    d->ack[3]=9;d->ack[5]=7;d->ack[6]=0x20;d->ack[7]=2;
    status=FormatTransfer(d,TRUE);if(!NT_SUCCESS(status))return status;
    KeClearEvent(&d->ackDone);WdfRequestSetCompletionRoutine(d->ackRequest,AckDone,d);
    if(!WdfRequestSend(d->ackRequest,WdfDeviceGetIoTarget(d->device),NULL))return WdfRequestGetStatus(d->ackRequest);
    timeout.QuadPart=-2LL*1000*1000*10;
    status=KeWaitForMultipleObjects(2,waits,WaitAny,Executive,KernelMode,FALSE,&timeout,NULL);
    if(status!=STATUS_WAIT_1){WdfRequestCancelSentRequest(d->ackRequest);KeWaitForSingleObject(&d->ackDone,Executive,KernelMode,FALSE,NULL);return STATUS_CANCELLED;}
    return NT_SUCCESS(d->ackStatus) && !USBD_SUCCESS(d->ackUrb.Hdr.Status)?STATUS_IO_DEVICE_ERROR:d->ackStatus;
}
static VOID CaptureStateLocked(GIP_DEVICE* d) {
    IC_CONTROLLER_GIP_RAW_INPUT* input;
    if(captured && (!d->captureId || count==IC_GIP_CAPACITY))IcRevokeAllLocked(count==IC_GIP_CAPACITY?IC_RESET_OVERFLOW:IC_RESET_DEVICE);
    if(!captured)return;
    input=&captured[(head+count)%IC_GIP_CAPACITY];RtlZeroMemory(input,sizeof(*input));
    input->input.sequence=IcNextSequenceLocked();input->input.timestamp=KeQueryPerformanceCounter(NULL).QuadPart;
    input->input.device=d->captureId;input->input.state=d->physical;
    input->report_length=d->physicalLength;RtlCopyMemory(input->report,d->physicalFrame,d->physicalLength);
    if(++count==1)KeSetEvent(readyEvent,IO_NO_INCREMENT,FALSE);
}
// Work out one frame's extent before parsing, so coalesced protocol packets are
// supported without treating an arbitrary 0x20 payload as a known gamepad.
static ULONG FrameLength(const UCHAR* bytes,ULONG remaining) {
    return IcGipFrameLength(bytes,remaining);
}
static VOID ObserveHelloLocked(GIP_DEVICE* d,const UCHAR* bytes,ULONG length) {
    ULONG i;
    d->ready=d->valid=d->helloValid=d->unsupported=FALSE;
    d->physicalLength=0;
    // Keep sequence mappings for protocol frames already queued or delivered:
    // a retransmitted HELLO must not alias their later ACKs to a new transfer.
    // Configuration changes stop/drain the pipe before resetting this table.
    RtlZeroMemory(&d->metadata,sizeof(d->metadata));
    RtlZeroMemory(&d->physical,sizeof(d->physical));RtlZeroMemory(&d->delivered,sizeof(d->delivered));
    // MS-GIPUSB HELLO: physical ID, USB VID/PID, firmware/hardware versions and
    // RF/security/GIP protocol versions. Never infer the schema from VID alone.
    if(length!=32 || bytes[3]!=28 || IcNativeRead16(bytes+12)!=d->info.vid ||
       IcNativeRead16(bytes+14)!=d->info.pid || bytes[30]!=1 || bytes[31]!=0)return;
    RtlCopyMemory(d->hello,bytes+4,sizeof(d->hello));d->helloValid=TRUE;
    for(i=0;i<GIP_CACHE_CAPACITY;++i)if(metadataCache[i].valid &&
        IcGipMetadataIdentityEqual(metadataCache[i].hello,d->hello)) {
        d->metadata=metadataCache[i].metadata;break;
    }
}
static VOID CacheMetadataLocked(GIP_DEVICE* d) {
    ULONG i,index=GIP_CACHE_CAPACITY;
    if(!d->helloValid || !d->metadata.complete || !d->metadata.gamepad)return;
    for(i=0;i<GIP_CACHE_CAPACITY;++i)if(metadataCache[i].valid &&
        IcGipMetadataIdentityEqual(metadataCache[i].hello,d->hello)){index=i;break;}
    if(index==GIP_CACHE_CAPACITY){index=cacheNext;cacheNext=(cacheNext+1)%GIP_CACHE_CAPACITY;}
    metadataCache[index].valid=TRUE;RtlCopyMemory(metadataCache[index].hello,d->hello,sizeof(d->hello));
    metadataCache[index].metadata=d->metadata;
}
static VOID MonitorPhysicalLocked(GIP_DEVICE* d,const UCHAR* bytes,ULONG length) {
    IC_NATIVE_GAMEPAD_STATE state;
    if(!IcMonitorEnabled())return;
    // Passive observation validates the standard wire shape, exactly like
    // OUT telemetry. Capture still requires the independently verified
    // metadata below; observing a report never claims capture readiness.
    if(IcGipDecodeGamepadReport(bytes,length,14,&state))
        IcMonitorPublish(IC_TYPE_CONTROLLER_GIP,d->info.instance_token,IC_MONITOR_IN,
            IC_MONITOR_NATIVE_VALID,bytes,length,&state,STATUS_SUCCESS);
    else if(length==6 && bytes[0]==7 &&
            (bytes[1]==0x20 || bytes[1]==0x30) && bytes[3]==2 && bytes[4]<=1 && bytes[5]==0x5B) {
        RtlZeroMemory(&state,sizeof(state));state.extra_buttons=bytes[4];
        IcMonitorPublish(IC_TYPE_CONTROLLER_GIP,d->info.instance_token,IC_MONITOR_IN,
            IC_MONITOR_GUIDE_VALID,bytes,length,&state,STATUS_SUCCESS);
    } else if(length && bytes[0]==0x20)
        IcMonitorPublish(IC_TYPE_CONTROLLER_GIP,d->info.instance_token,IC_MONITOR_IN,
            IC_MONITOR_RAW|IC_MONITOR_ERROR,bytes,length,NULL,STATUS_NOT_SUPPORTED);
}
static BOOLEAN ProcessFrameLocked(GIP_DEVICE* d,UCHAR* bytes,ULONG length,UCHAR* ack) {
    IC_NATIVE_GAMEPAD_STATE state;IC_GIP_FRAME frame;BOOLEAN guide=FALSE,ordinary=FALSE;
    if(!IcGipParseFrame(bytes,length,&frame)) {
        if(captured && (d->captureId || !IcAllCaptureLocked(IC_TYPE_CONTROLLER_GIP)))IcRevokeAllLocked(IC_RESET_DEVICE);d->ready=FALSE;d->unsupported=TRUE;
        OutputLocked(d,bytes,length,FALSE);return TRUE;
    }
    if(frame.command==2 && frame.flags==0x20) {
        if(d->ready && captured)IcRevokeAllLocked(IC_RESET_DEVICE);
        ObserveHelloLocked(d,bytes,length);
    }
    guide=d->metadata.complete && d->metadata.gamepad && length==6 && bytes[0]==7 &&
        (bytes[1]==0x20 || bytes[1]==0x30) && bytes[3]==2 && bytes[4]<=1 && bytes[5]==0x5B;
    if(guide) {
        ++d->physicalReports;
    }
    if(guide && d->ready && captured) {
        IcGipSequenceTrack(&d->sequences,bytes[2]);
        d->physical.extra_buttons=bytes[4];CaptureStateLocked(d);
        if(captured){if(bytes[1]&0x10)*ack=bytes[2];return TRUE;}
    }
    IcGipSequenceSetTime(&d->sequences,KeQueryInterruptTime()/10000);
    // Installation alone must not renumber Windows' native GIP traffic. Once
    // capture has introduced synthetic traffic, retain translation until the
    // USB configuration resets, including after the last context closes.
    if(!(d->translated?IcGipSequenceObserve(&d->sequences,bytes,length):
        IcGipSequenceObservePhysical(&d->sequences,bytes,length)))return FALSE;
    if(frame.command==4 && (frame.flags&0x20) && !(frame.flags&7)) {
        if((frame.flags&0xC0)==0xC0) {
            if(d->ready && captured)IcRevokeAllLocked(IC_RESET_DEVICE);d->ready=d->valid=FALSE;
        }
        int result=IcGipObserveMetadata(&d->metadata,bytes,length);
        if(result<0){d->metadata.complete=d->metadata.gamepad=0;if(d->ready && captured)IcRevokeAllLocked(IC_RESET_DEVICE);d->ready=FALSE;}
        else if(result>0)CacheMetadataLocked(d);
    }
    ordinary=!d->unsupported && d->metadata.complete && d->metadata.gamepad &&
        IcGipDecodeGamepadReport(bytes,length,d->metadata.input_length,&state)!=0;
    if(ordinary) {
        // Shape changes invalidate the immutable context snapshot. Forward the
        // new physical report, then let a fresh context enumerate its length.
        if(d->ready && length!=d->physicalLength && captured)IcRevokeAllLocked(IC_RESET_DEVICE);
        ++d->physicalReports;
        state.extra_buttons=d->physical.extra_buttons;d->physical=state;
        RtlZeroMemory(d->physicalFrame,sizeof(d->physicalFrame));RtlCopyMemory(d->physicalFrame,bytes,length);
        d->physicalLength=length;d->info.input_bytes=(USHORT)length;d->valid=TRUE;
        if(!d->ready) {
            if(captured && !IcAllCaptureLocked(IC_TYPE_CONTROLLER_GIP))IcRevokeAllLocked(IC_RESET_DEVICE);
            d->ready=TRUE;d->inputSequence=(UCHAR)(bytes[2]-1);
            IcAllReadersReadyLocked();
        }
    } else if(guide) {
        d->physical.extra_buttons=bytes[4];
    } else if(frame.command==0x20 && !(frame.flags&0x27) && d->metadata.complete && d->metadata.gamepad) {
        // Malformed standard prefix or out-of-bounds extension: passthrough.
        if(captured && (d->captureId || !IcAllCaptureLocked(IC_TYPE_CONTROLLER_GIP)))IcRevokeAllLocked(IC_RESET_DEVICE);d->ready=FALSE;d->unsupported=TRUE;
    }
    if((ordinary || guide) && d->ready && captured) {
        CaptureStateLocked(d);
        if(captured)return TRUE;
    }
    if(ordinary && d->ready) {
        if(d->translated)bytes[2]=NextSequence(&d->inputSequence);
        else d->inputSequence=bytes[2];
        d->delivered=d->physical;
    }
    if(guide)d->delivered.extra_buttons=d->physical.extra_buttons;
    OutputLocked(d,bytes,length,ordinary || guide);return TRUE;
}
static VOID Reader(PVOID context) {
    GIP_DEVICE* d=context;NTSTATUS status=STATUS_SUCCESS;PVOID waits[3]={&d->stop,&d->done,&d->maintenance};
    ULONG emptyReads=0;
    KeWaitForSingleObject(&d->started,Executive,KernelMode,FALSE,NULL);
    while(!KeReadStateEvent(&d->stop)) {
        status=FormatTransfer(d,FALSE);if(!NT_SUCCESS(status))break;
        KeClearEvent(&d->done);WdfRequestSetCompletionRoutine(d->request,PhysicalDone,d);
        if(!WdfRequestSend(d->request,WdfDeviceGetIoTarget(d->device),NULL)){status=WdfRequestGetStatus(d->request);break;}
        for(;;) {
            status=KeWaitForMultipleObjects(3,waits,WaitAny,Executive,KernelMode,FALSE,NULL,NULL);
            if(status==STATUS_WAIT_2){WdfWaitLockAcquire(gGate,NULL);TryRestoreLocked(d);WdfWaitLockRelease(gGate);continue;}
            break;
        }
        if(status==STATUS_WAIT_0){WdfRequestCancelSentRequest(d->request);KeWaitForSingleObject(&d->done,Executive,KernelMode,FALSE,NULL);break;}
        status=d->readStatus;
        if(!NT_SUCCESS(status) || !USBD_SUCCESS(d->urb.Hdr.Status) || d->urb.TransferBufferLength>sizeof(d->buffer))break;
        if(!d->urb.TransferBufferLength){if(++emptyReads>=16){status=STATUS_DEVICE_DATA_ERROR;break;}continue;}
        emptyReads=0;
        {
            ULONG offset=0,bytes=d->urb.TransferBufferLength;
            while(offset<bytes && !KeReadStateEvent(&d->stop)) {
                ULONG length=FrameLength(d->buffer+offset,bytes-offset);UCHAR ack=0;
                ULONGLONG sequenceDeadline=0;
                if(!length)length=bytes-offset;
                // Observe each physical frame once, before any output/ACK
                // backpressure can retry its protocol processing.
                if(IcMonitorEnabled()) {
                    WdfWaitLockAcquire(gGate,NULL);
                    if(!KeReadStateEvent(&d->stop))MonitorPhysicalLocked(d,d->buffer+offset,length);
                    WdfWaitLockRelease(gGate);
                }
                // Bounded protocol FIFO: pause physical reads until Windows has
                // room instead of dropping security/metadata/ACK traffic.
                for(;;) {
                    ULONG delay=0;BOOLEAN processed=FALSE;
                    WdfWaitLockAcquire(gGate,NULL);TryRestoreLocked(d);
                    // A protocol-triggered revocation may enqueue the restored
                    // Guide and gamepad state before this protocol frame.
                    if(KeReadStateEvent(&d->stop)){WdfWaitLockRelease(gGate);break;}
                    if(GIP_OUTPUT_CAPACITY-d->outputCount>=3) {
                        processed=ProcessFrameLocked(d,d->buffer+offset,length,&ack);
                        if(!processed) {
                            delay=IcGipSequenceWaitMilliseconds(&d->sequences);
                            if(!delay) {
                                ULONGLONG now=KeQueryInterruptTime()/10000;
                                if(!sequenceDeadline)sequenceDeadline=now+2000;
                                if(now>=sequenceDeadline)status=STATUS_IO_TIMEOUT;
                                else delay=(ULONG)(sequenceDeadline-now);
                            }
                        }
                    }
                    WdfWaitLockRelease(gGate);
                    if(processed || !NT_SUCCESS(status))break;
                    {PVOID space[2]={&d->stop,&d->maintenance};LARGE_INTEGER timeout;
                        timeout.QuadPart=-(LONGLONG)delay*10000;
                        KeWaitForMultipleObjects(2,space,WaitAny,Executive,KernelMode,FALSE,delay?&timeout:NULL,NULL);}
                }
                offset+=length;
                if(!NT_SUCCESS(status))break;
                if(ack && !NT_SUCCESS(SendGuideAck(d,ack))) {status=STATUS_IO_DEVICE_ERROR;break;}
            }
        }
        if(!NT_SUCCESS(status))break;
    }
    WdfWaitLockAcquire(gGate,NULL);d->ready=d->running=FALSE;d->lastReadStatus=(ULONG)status;d->lastUsbStatus=d->urb.Hdr.Status;
    if(!KeReadStateEvent(&d->stop)) {
        NTSTATUS observed=!NT_SUCCESS(status)?status:!USBD_SUCCESS(d->urb.Hdr.Status)?STATUS_IO_DEVICE_ERROR:
            d->urb.TransferBufferLength>sizeof(d->buffer)?STATUS_INVALID_BUFFER_SIZE:STATUS_UNSUCCESSFUL;
        IcMonitorPublish(IC_TYPE_CONTROLLER_GIP,d->info.instance_token,IC_MONITOR_DEVICE,
            IC_MONITOR_ERROR,NULL,0,NULL,observed);
    }
    // A retiring device can finish after a replacement has been captured.
    // Only revoke an owner whose snapshot still contains this device.
    if(captured && d->captureId)IcRevokeAllLocked(IC_RESET_DEVICE);
    // Failed physical reads must not leave Windows' upper READs pending with no
    // producer. A later USB pipe reset or power restart can restart the pump.
    {WDFREQUEST pending;while(NT_SUCCESS(WdfIoQueueRetrieveNextRequest(d->reads,&pending))) {
        PURB urb=RequestUrb(pending);if(urb){urb->UrbHeader.Status=USBD_STATUS_DEVICE_GONE;urb->UrbBulkOrInterruptTransfer.TransferBufferLength=0;}
        QueueCompletion(d,pending,STATUS_DEVICE_NOT_CONNECTED,0);
    }}
    WdfWaitLockRelease(gGate);PsTerminateSystemThread(STATUS_SUCCESS);
}
static NTSTATUS StartLocked(GIP_DEVICE* d) {
    HANDLE handle=NULL;OBJECT_ATTRIBUTES attributes;NTSTATUS status;LARGE_INTEGER zero;
    zero.QuadPart=0;
    if(d->thread && KeWaitForSingleObject(d->thread,Executive,KernelMode,FALSE,&zero)==STATUS_SUCCESS){ObDereferenceObject(d->thread);d->thread=NULL;}
    if(d->thread || !d->powered || !d->configured)return STATUS_SUCCESS;
    KeClearEvent(&d->stop);KeClearEvent(&d->started);
    InitializeObjectAttributes(&attributes,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
    status=PsCreateSystemThread(&handle,THREAD_ALL_ACCESS,&attributes,NULL,NULL,Reader,d);
    if(NT_SUCCESS(status)) {
        status=ObReferenceObjectByHandle(handle,SYNCHRONIZE,*PsThreadType,KernelMode,(PVOID*)&d->thread,NULL);
        if(!NT_SUCCESS(status))KeSetEvent(&d->stop,IO_NO_INCREMENT,FALSE);
        else {WdfWaitLockAcquire(gGate,NULL);d->running=TRUE;WdfWaitLockRelease(gGate);}
        KeSetEvent(&d->started,IO_NO_INCREMENT,FALSE);
    }
    if(handle){if(!NT_SUCCESS(status))ZwWaitForSingleObject(handle,FALSE,NULL);ZwClose(handle);}
    if(!NT_SUCCESS(status)){WdfWaitLockAcquire(gGate,NULL);d->running=FALSE;WdfWaitLockRelease(gGate);}return status;
}
static VOID StopLocked(GIP_DEVICE* d) {
    PETHREAD thread;WDFREQUEST request;
    WdfWaitLockAcquire(gGate,NULL);d->ready=d->running=FALSE;
    // Surprise removal, suspend and cleanup may call Stop more than once.
    // A later call must not revoke a new owner's unrelated device snapshot.
    if(captured && d->captureId)IcRevokeAllLocked(IC_RESET_DEVICE);
    thread=d->thread;d->thread=NULL;KeSetEvent(&d->stop,IO_NO_INCREMENT,FALSE);
    while(NT_SUCCESS(WdfIoQueueRetrieveNextRequest(d->reads,&request))) {
        PURB urb=RequestUrb(request);if(urb){urb->UrbHeader.Status=USBD_STATUS_DEVICE_GONE;urb->UrbBulkOrInterruptTransfer.TransferBufferLength=0;}
        QueueCompletion(d,request,STATUS_DEVICE_NOT_CONNECTED,0);
    }
    d->outputHead=d->outputCount=0;d->restorePending=FALSE;WdfWaitLockRelease(gGate);
    if(thread){KeWaitForSingleObject(thread,Executive,KernelMode,FALSE,NULL);ObDereferenceObject(thread);}
}
static VOID Forward(WDFREQUEST request,WDFDEVICE device) {
    WDF_REQUEST_SEND_OPTIONS options;WdfRequestFormatRequestUsingCurrentType(request);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    if(!WdfRequestSend(request,WdfDeviceGetIoTarget(device),&options))WdfRequestComplete(request,WdfRequestGetStatus(request));
}
static VOID OutDone(WDFREQUEST request,WDFIOTARGET target,PWDF_REQUEST_COMPLETION_PARAMS params,WDFCONTEXT context) {
    GIP_DEVICE* d=context;GIP_COMPLETION* c=GipCompletion(request);PURB urb=RequestUrb(request);NTSTATUS status=params->IoStatus.Status;
    BOOLEAN succeeded=FALSE;
    UNREFERENCED_PARAMETER(target);
    urb->UrbBulkOrInterruptTransfer.TransferBuffer=c->originalBuffer;
    urb->UrbBulkOrInterruptTransfer.TransferBufferMDL=c->originalMdl;
    if(c->outgoingLarge){ExFreePoolWithTag(c->outgoingLarge,FILTER_TAG);c->outgoingLarge=NULL;}
    if(NT_SUCCESS(status) && USBD_SUCCESS(urb->UrbHeader.Status)) {
        if(urb->UrbBulkOrInterruptTransfer.TransferBufferLength==c->forwardedLength) {
            succeeded=TRUE;
            urb->UrbBulkOrInterruptTransfer.TransferBufferLength=c->originalLength;
        }
        else {status=STATUS_IO_DEVICE_ERROR;urb->UrbHeader.Status=USBD_STATUS_XACT_ERROR;}
    }
    QueueOutCompletion(d,request,status,params->IoStatus.Information,succeeded);
}
// The caller owns gGate. Copy protocol ACKs before translating their sequence;
// the upper driver's buffer/MDL is never edited, and is restored at completion.
static NTSTATUS PrepareOutLocked(GIP_DEVICE* d,WDFREQUEST request,PURB urb,BOOLEAN* handled) {
    GIP_COMPLETION* c;PUCHAR buffer,copy;ULONG length=urb->UrbBulkOrInterruptTransfer.TransferBufferLength;uint32_t kept=0;
    NTSTATUS status;int prepared;*handled=FALSE;
    if(!length)return STATUS_SUCCESS;
    // Never forward an untranslated ACK just because its URB spans several USB
    // packets. Oversized transfers fail explicitly instead of corrupting mapping.
    if(length>IC_GIP_OUT_MAX)return STATUS_INVALID_BUFFER_SIZE;
    buffer=urb->UrbBulkOrInterruptTransfer.TransferBuffer;
    if(urb->UrbBulkOrInterruptTransfer.TransferBufferMDL) {
        if(MmGetMdlByteCount(urb->UrbBulkOrInterruptTransfer.TransferBufferMDL)<length)return STATUS_INVALID_PARAMETER;
        buffer=MmGetSystemAddressForMdlSafe(urb->UrbBulkOrInterruptTransfer.TransferBufferMDL,NormalPagePriority|MdlMappingNoExecute);
    }
    if(!buffer)return STATUS_INSUFFICIENT_RESOURCES;
    status=EnsureCompletion(request);if(!NT_SUCCESS(status))return status;c=GipCompletion(request);
    copy=c->outgoing;
    if(length>sizeof(c->outgoing)) {
        c->outgoingLarge=ExAllocatePoolZero(NonPagedPoolNx,length,FILTER_TAG);
        if(!c->outgoingLarge)return STATUS_INSUFFICIENT_RESOURCES;
        copy=c->outgoingLarge;
    }
    RtlCopyMemory(copy,buffer,length);IcGipSequenceSetTime(&d->sequences,KeQueryInterruptTime()/10000);
    prepared=IcGipPrepareOut(&d->sequences,copy,length,&c->ackTicket,&kept);
    if(prepared!=1) {
        if(c->outgoingLarge){ExFreePoolWithTag(c->outgoingLarge,FILTER_TAG);c->outgoingLarge=NULL;}
        return prepared<0?STATUS_DEVICE_BUSY:STATUS_INVALID_PARAMETER;
    }
    KeSetEvent(&d->maintenance,IO_NO_INCREMENT,FALSE);
    if(!IcGipOutHasPins(&c->ackTicket) && kept==length && RtlCompareMemory(copy,buffer,length)==length) {
        if(c->outgoingLarge){ExFreePoolWithTag(c->outgoingLarge,FILTER_TAG);c->outgoingLarge=NULL;}
        return STATUS_SUCCESS;
    }
    *handled=TRUE;c->originalBuffer=urb->UrbBulkOrInterruptTransfer.TransferBuffer;
    c->originalMdl=urb->UrbBulkOrInterruptTransfer.TransferBufferMDL;c->originalLength=length;c->forwardedLength=kept;
    if(!kept) {
        if(c->outgoingLarge){ExFreePoolWithTag(c->outgoingLarge,FILTER_TAG);c->outgoingLarge=NULL;}
        return STATUS_SUCCESS;
    }
    urb->UrbBulkOrInterruptTransfer.TransferBuffer=copy;
    urb->UrbBulkOrInterruptTransfer.TransferBufferMDL=NULL;urb->UrbBulkOrInterruptTransfer.TransferBufferLength=kept;
    return STATUS_SUCCESS;
}
// No lock is held here: lower completion may run inline in WdfRequestSend.
static VOID SendPreparedOut(GIP_DEVICE* d,WDFREQUEST request,PURB urb,NTSTATUS status,BOOLEAN handled) {
    WDF_REQUEST_SEND_OPTIONS options;
    if(!NT_SUCCESS(status)){urb->UrbHeader.Status=USBD_STATUS_INVALID_PARAMETER;WdfRequestComplete(request,status);return;}
    if(!handled){Forward(request,d->device);return;}
    if(!GipCompletion(request)->forwardedLength){urb->UrbHeader.Status=USBD_STATUS_SUCCESS;QueueCompletion(d,request,STATUS_SUCCESS,0);return;}
    WdfRequestFormatRequestUsingCurrentType(request);WdfRequestSetCompletionRoutine(request,OutDone,d);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,WDF_REL_TIMEOUT_IN_SEC(2));
    if(!WdfRequestSend(request,WdfDeviceGetIoTarget(d->device),&options)) {
        GIP_COMPLETION* c=GipCompletion(request);status=WdfRequestGetStatus(request);
        urb->UrbBulkOrInterruptTransfer.TransferBuffer=c->originalBuffer;urb->UrbBulkOrInterruptTransfer.TransferBufferMDL=c->originalMdl;
        if(c->outgoingLarge){ExFreePoolWithTag(c->outgoingLarge,FILTER_TAG);c->outgoingLarge=NULL;}
        urb->UrbBulkOrInterruptTransfer.TransferBufferLength=0;QueueOutCompletion(d,request,status,0,FALSE);
    }
}
static VOID FindPipes(GIP_DEVICE* d,PURB urb) {
    ULONG offset=urb->UrbHeader.Function==URB_FUNCTION_SELECT_CONFIGURATION?
        FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION,Interface):FIELD_OFFSET(struct _URB_SELECT_INTERFACE,Interface);
    ULONG length=urb->UrbHeader.Length;
    d->inPipe=d->outPipe=NULL;d->configured=FALSE;
    while(offset<=length && length-offset>=(ULONG)FIELD_OFFSET(USBD_INTERFACE_INFORMATION,Pipes)) {
        PUSBD_INTERFACE_INFORMATION intf=(PUSBD_INTERFACE_INFORMATION)((PUCHAR)urb+offset);ULONG i;
        if(intf->Length<FIELD_OFFSET(USBD_INTERFACE_INFORMATION,Pipes) || intf->Length>length-offset ||
           intf->NumberOfPipes>(intf->Length-FIELD_OFFSET(USBD_INTERFACE_INFORMATION,Pipes))/sizeof(USBD_PIPE_INFORMATION))return;
        if(!intf->InterfaceNumber && !intf->AlternateSetting && intf->Class==0xFF && intf->SubClass==0x47 && intf->Protocol==0xD0) {
            for(i=0;i<intf->NumberOfPipes;++i) {
                PUSBD_PIPE_INFORMATION pipe=&intf->Pipes[i];
                if(pipe->PipeType!=UsbdPipeTypeInterrupt || pipe->MaximumPacketSize!=64)continue;
                if(pipe->EndpointAddress&0x80)d->inPipe=pipe->PipeHandle;else d->outPipe=pipe->PipeHandle;
            }
        }
        offset+=intf->Length;
    }
    d->configured=d->inPipe && d->outPipe;
}
static VOID InternalUsb(WDFQUEUE queue,WDFREQUEST request,size_t output,size_t input,ULONG code) {
    GIP_DEVICE* d=GipDevice(WdfIoQueueGetDevice(queue));PURB urb;NTSTATUS status;
    UNREFERENCED_PARAMETER(output);UNREFERENCED_PARAMETER(input);
    if(code!=IOCTL_INTERNAL_USB_SUBMIT_URB){Forward(request,d->device);return;}
    urb=RequestUrb(request);
    if(!urb || urb->UrbHeader.Length<sizeof(struct _URB_HEADER)){WdfRequestComplete(request,STATUS_INVALID_PARAMETER);return;}
    if(urb->UrbHeader.Function==URB_FUNCTION_SELECT_INTERFACE &&
       urb->UrbHeader.Length>=FIELD_OFFSET(struct _URB_SELECT_INTERFACE,Interface)+FIELD_OFFSET(USBD_INTERFACE_INFORMATION,Pipes) &&
       urb->UrbSelectInterface.Interface.InterfaceNumber!=0){Forward(request,d->device);return;}
    if(urb->UrbHeader.Function==URB_FUNCTION_SELECT_CONFIGURATION || urb->UrbHeader.Function==URB_FUNCTION_SELECT_INTERFACE) {
        WDF_REQUEST_SEND_OPTIONS options;
        status=EnsureCompletion(request);if(!NT_SUCCESS(status)){WdfRequestComplete(request,status);return;}
        WdfWaitLockAcquire(d->stateLock,NULL);StopLocked(d);
        WdfWaitLockAcquire(gGate,NULL);d->configured=FALSE;d->valid=d->helloValid=d->unsupported=FALSE;
        RtlZeroMemory(&d->metadata,sizeof(d->metadata));IcGipSequenceReset(&d->sequences);d->translated=FALSE;WdfWaitLockRelease(gGate);
        WdfRequestFormatRequestUsingCurrentType(request);WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
        WdfRequestSend(request,WdfDeviceGetIoTarget(d->device),&options);status=WdfRequestGetStatus(request);
        if(NT_SUCCESS(status) && USBD_SUCCESS(urb->UrbHeader.Status)) {
            WdfWaitLockAcquire(gGate,NULL);FindPipes(d,urb);WdfWaitLockRelease(gGate);(void)StartLocked(d);
        }
        WdfWaitLockRelease(d->stateLock);QueueCompletion(d,request,status,WdfRequestGetInformation(request));return;
    }
    if((urb->UrbHeader.Function==URB_FUNCTION_ABORT_PIPE || urb->UrbHeader.Function==URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL ||
        urb->UrbHeader.Function==URB_FUNCTION_SYNC_RESET_PIPE || urb->UrbHeader.Function==URB_FUNCTION_SYNC_CLEAR_STALL) &&
       urb->UrbHeader.Length>=sizeof(struct _URB_PIPE_REQUEST)) {
        BOOLEAN ours;WdfWaitLockAcquire(d->stateLock,NULL);
        WdfWaitLockAcquire(gGate,NULL);ours=d->configured && urb->UrbPipeRequest.PipeHandle==d->inPipe;WdfWaitLockRelease(gGate);
        if(ours) {
            WDF_REQUEST_SEND_OPTIONS options;
            status=EnsureCompletion(request);if(!NT_SUCCESS(status)){WdfWaitLockRelease(d->stateLock);WdfRequestComplete(request,status);return;}
            StopLocked(d);WdfRequestFormatRequestUsingCurrentType(request);
            WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
            WdfRequestSend(request,WdfDeviceGetIoTarget(d->device),&options);status=WdfRequestGetStatus(request);
            if(NT_SUCCESS(status) && USBD_SUCCESS(urb->UrbHeader.Status))(void)StartLocked(d);
            WdfWaitLockRelease(d->stateLock);QueueCompletion(d,request,status,WdfRequestGetInformation(request));return;
        }
        WdfWaitLockRelease(d->stateLock);
    }
    WdfWaitLockAcquire(gGate,NULL);
    if(urb->UrbHeader.Function==URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER &&
       urb->UrbHeader.Length>=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER) &&
       d->configured && urb->UrbBulkOrInterruptTransfer.PipeHandle==d->outPipe &&
       !(urb->UrbBulkOrInterruptTransfer.TransferFlags&USBD_TRANSFER_DIRECTION_IN)) {
        BOOLEAN handled;
        status=PrepareOutLocked(d,request,urb,&handled);WdfWaitLockRelease(gGate);
        SendPreparedOut(d,request,urb,status,handled);
        return;
    }
    if(urb->UrbHeader.Function==URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER &&
       urb->UrbHeader.Length>=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER) &&
       d->configured && urb->UrbBulkOrInterruptTransfer.PipeHandle==d->inPipe &&
       (urb->UrbBulkOrInterruptTransfer.TransferFlags&USBD_TRANSFER_DIRECTION_IN)) {
        status=EnsureCompletion(request);
        if(NT_SUCCESS(status)) {
            if(!d->powered || !d->running)status=STATUS_DEVICE_NOT_READY;
            else status=WdfRequestForwardToIoQueue(request,d->reads);
            if(NT_SUCCESS(status)){FlushOutputLocked(d);TryRestoreLocked(d);}
        }
        WdfWaitLockRelease(gGate);
        if(!NT_SUCCESS(status)) {
            urb->UrbHeader.Status=USBD_STATUS_DEVICE_GONE;urb->UrbBulkOrInterruptTransfer.TransferBufferLength=0;
            if(GipCompletion(request))QueueCompletion(d,request,status,0);else WdfRequestComplete(request,status);
        }
        return;
    }
    WdfWaitLockRelease(gGate);Forward(request,d->device);
}
static VOID Default(WDFQUEUE queue,WDFREQUEST request){Forward(request,WdfIoQueueGetDevice(queue));}
static VOID Ioctl(WDFQUEUE queue,WDFREQUEST request,size_t output,size_t input,ULONG code) {
    GIP_DEVICE* d=GipDevice(WdfIoQueueGetDevice(queue));NTSTATUS status;
    UNREFERENCED_PARAMETER(output);UNREFERENCED_PARAMETER(input);
    if(code!=IOCTL_IC_GIP_SOURCE){Forward(request,d->device);return;}
    WdfWaitLockAcquire(gGate,NULL);status=d->ready?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;WdfWaitLockRelease(gGate);WdfRequestComplete(request,status);
}
static NTSTATUS Resume(WDFDEVICE device) {
    GIP_DEVICE* d=GipDevice(device);NTSTATUS status;
    WdfWaitLockAcquire(d->stateLock,NULL);WdfWaitLockAcquire(gGate,NULL);IcTrialResumeLocked();d->powered=TRUE;WdfWaitLockRelease(gGate);
    status=StartLocked(d);WdfWaitLockRelease(d->stateLock);
    WdfIoQueueStart(d->reads);WdfIoQueueStart(d->dispatch);return status;
}
static VOID StopDevice(WDFDEVICE device) {
    GIP_DEVICE* d=GipDevice(device);
    WdfWaitLockAcquire(d->stateLock,NULL);WdfWaitLockAcquire(gGate,NULL);d->powered=FALSE;WdfWaitLockRelease(gGate);
    StopLocked(d);WdfWaitLockRelease(d->stateLock);
}
static NTSTATUS Suspend(WDFDEVICE device) {
    GIP_DEVICE* d=GipDevice(device);StopDevice(device);
    // End dispatch/cancellation/translated-OUT producers before the DPC drain.
    // Purge waits run outside both locks: an upper completion can resubmit or
    // close synchronously. Start the purge on both queues before either wait.
    WdfIoQueuePurge(d->dispatch,NULL,NULL);WdfIoQueuePurge(d->reads,NULL,NULL);
    WdfIoQueuePurgeSynchronously(d->dispatch);WdfIoQueuePurgeSynchronously(d->reads);
    WdfWorkItemFlush(d->outWork);DrainCompletions(d);return STATUS_SUCCESS;
}
static VOID ManagedCleanup(WDFDEVICE device){
    GIP_DEVICE* d=GipDevice(device);(void)Suspend(device);
    WdfWaitLockAcquire(gGate,NULL);d->monitorRetired=TRUE;
    IcMonitorRemoved(IC_TYPE_CONTROLLER_GIP,d->info.instance_token);WdfWaitLockRelease(gGate);
}
static NTSTATUS Pnp(WDFDEVICE device,PIRP irp) {
    GIP_DEVICE* d=GipDevice(device);WdfWaitLockAcquire(gGate,NULL);d->monitorRetired=TRUE;
    IcMonitorRemoved(IC_TYPE_CONTROLLER_GIP,d->info.instance_token);WdfWaitLockRelease(gGate);
    StopDevice(device);IoSkipCurrentIrpStackLocation(irp);return WdfDeviceWdmDispatchPreprocessedIrp(device,irp);
}
static VOID Cleanup(WDFOBJECT object) {
    GIP_DEVICE* d=GipDevice(object);WdfWaitLockAcquire(gGate,NULL);
    if(d->registered){RemoveEntryList(&d->link);d->registered=FALSE;}WdfWaitLockRelease(gGate);
    if(d->counted)IcFdoRemoved();
}
static USHORT HardwareIdNumber(const USHORT* text,ULONG length,PCWSTR prefix) {
    ULONG i,j;USHORT value;
    for(i=0;i+8<=length && text[i];++i) {
        for(j=0;j<4 && text[i+j]==prefix[j];++j){}
        if(j!=4)continue;
        value=0;
        for(j=4;j<8;++j) {
            USHORT c=text[i+j],digit;
            if(c>='0' && c<='9')digit=c-'0';else if(c>='A' && c<='F')digit=c-'A'+10;
            else if(c>='a' && c<='f')digit=c-'a'+10;else break;
            value=(USHORT)((value<<4)|digit);
        }
        if(j==8)return value;
    }
    return 0;
}
NTSTATUS IcGipAdd(PWDFDEVICE_INIT init) {
    WDFDEVICE device;GIP_DEVICE* d;WDF_OBJECT_ATTRIBUTES a;WDF_PNPPOWER_EVENT_CALLBACKS p;
    WDF_IO_QUEUE_CONFIG q;WDF_DPC_CONFIG dpc;WDF_WORKITEM_CONFIG work;NTSTATUS status;ULONG bytes;UCHAR minor=IRP_MN_SURPRISE_REMOVAL;
    const GUID guid=IC_GIP_INTERFACE_GUID;
    WdfFdoInitSetFilter(init);
    status=WdfDeviceInitAssignWdmIrpPreprocessCallback(init,Pnp,IRP_MJ_PNP,&minor,1);if(!NT_SUCCESS(status))return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);p.EvtDeviceSelfManagedIoInit=Resume;p.EvtDeviceSelfManagedIoRestart=Resume;
    p.EvtDeviceSelfManagedIoSuspend=Suspend;p.EvtDeviceSelfManagedIoCleanup=ManagedCleanup;WdfDeviceInitSetPnpPowerEventCallbacks(init,&p);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,GIP_COMPLETION);WdfDeviceInitSetRequestAttributes(init,&a);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,GIP_DEVICE);a.ExecutionLevel=WdfExecutionLevelPassive;
    a.SynchronizationScope=WdfSynchronizationScopeNone;a.EvtCleanupCallback=Cleanup;
    status=WdfDeviceCreate(&init,&a,&device);if(!NT_SUCCESS(status))return status;
    d=GipDevice(device);d->device=device;InitializeListHead(&d->link);InitializeListHead(&d->completions);InitializeListHead(&d->outCompletions);
    IcCacheDeviceInstance(device,d->instance);
    KeInitializeEvent(&d->stop,NotificationEvent,FALSE);KeInitializeEvent(&d->started,NotificationEvent,FALSE);
    KeInitializeEvent(&d->done,SynchronizationEvent,FALSE);
    KeInitializeEvent(&d->ackDone,SynchronizationEvent,FALSE);KeInitializeEvent(&d->maintenance,SynchronizationEvent,FALSE);
    KeInitializeEvent(&d->completionsIdle,NotificationEvent,FALSE);
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;
    status=WdfWaitLockCreate(&a,&d->stateLock);if(!NT_SUCCESS(status))return status;
    status=WdfSpinLockCreate(&a,&d->completionLock);if(!NT_SUCCESS(status))return status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,GIP_DPC);a.ParentObject=device;
    WDF_DPC_CONFIG_INIT(&dpc,CompleteDpc);dpc.AutomaticSerialization=FALSE;
    status=WdfDpcCreate(&dpc,&a,&d->completionDpc);if(!NT_SUCCESS(status))return status;
    GipDpc(d->completionDpc)->device=d;WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;
    WDF_WORKITEM_CONFIG_INIT(&work,CompleteOutWork);work.AutomaticSerialization=FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,GIP_WORK);a.ParentObject=device;
    status=WdfWorkItemCreate(&work,&a,&d->outWork);if(!NT_SUCCESS(status))return status;
    GipWork(d->outWork)->device=d;WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;
    status=WdfRequestCreate(&a,WdfDeviceGetIoTarget(device),&d->request);if(!NT_SUCCESS(status))return status;
    status=WdfRequestCreate(&a,WdfDeviceGetIoTarget(device),&d->ackRequest);if(!NT_SUCCESS(status))return status;
    a.ParentObject=d->request;status=WdfMemoryCreatePreallocated(&a,&d->urb,sizeof(d->urb),&d->urbMemory);if(!NT_SUCCESS(status))return status;
    a.ParentObject=d->ackRequest;status=WdfMemoryCreatePreallocated(&a,&d->ackUrb,sizeof(d->ackUrb),&d->ackMemory);if(!NT_SUCCESS(status))return status;
    WDF_IO_QUEUE_CONFIG_INIT(&q,WdfIoQueueDispatchManual);q.PowerManaged=WdfFalse;q.EvtIoCanceledOnQueue=CancelRead;
    status=WdfIoQueueCreate(device,&q,WDF_NO_OBJECT_ATTRIBUTES,&d->reads);if(!NT_SUCCESS(status))return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q,WdfIoQueueDispatchParallel);q.PowerManaged=WdfFalse;
    q.EvtIoInternalDeviceControl=InternalUsb;q.EvtIoDeviceControl=Ioctl;q.EvtIoDefault=Default;
    status=WdfIoQueueCreate(device,&q,WDF_NO_OBJECT_ATTRIBUTES,&d->dispatch);if(!NT_SUCCESS(status))return status;
    d->info.type=IC_TYPE_CONTROLLER_GIP;d->info.instance_token=(ULONG)InterlockedIncrement(&nextId);d->info.input_bytes=IC_GIP_INPUT_BYTES;
    if(NT_SUCCESS(WdfDeviceQueryProperty(device,DevicePropertyHardwareID,sizeof(d->info.hardware_id)-2,d->info.hardware_id,&bytes)))d->info.flags|=IC_INFO_HARDWARE_ID_VALID;
    d->info.vid=HardwareIdNumber(d->info.hardware_id,RTL_NUMBER_OF(d->info.hardware_id),L"VID_");
    d->info.pid=HardwareIdNumber(d->info.hardware_id,RTL_NUMBER_OF(d->info.hardware_id),L"PID_");
    if(NT_SUCCESS(WdfDeviceQueryProperty(device,DevicePropertyDeviceDescription,sizeof(d->info.name)-2,d->info.name,&bytes)))d->info.flags|=IC_INFO_NAME_VALID;
    status=IcFdoAdded();if(!NT_SUCCESS(status))return status;d->counted=TRUE;
    status=WdfDeviceCreateDeviceInterface(device,&guid,NULL);if(!NT_SUCCESS(status))return status;
    WdfWaitLockAcquire(gGate,NULL);InsertTailList(&devices,&d->link);d->registered=TRUE;
    IcMonitorPublish(IC_TYPE_CONTROLLER_GIP,d->info.instance_token,IC_MONITOR_DEVICE,0,NULL,0,NULL,STATUS_SUCCESS);
    WdfWaitLockRelease(gGate);return STATUS_SUCCESS;
}
