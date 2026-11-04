#include "ControllerXusb.h"
#include "../Shared/XusbMirrorIdentity.h"
#include "ControllerXusbMirror.h"
#include "../Shared/XusbDelivery.h"
#include "../Shared/ControllerXusbTrace.h"
#include <devpkey.h>
#if defined(IC_COMPAT_NATIVE_GET) && !defined(IC_COMPAT_TRACE)
#error IC_COMPAT_NATIVE_GET is restricted to instrumented private laboratory builds.
#endif
#if defined(IC_COMPAT_NATIVE_XUSB) && !defined(IC_COMPAT_TRACE)
#error IC_COMPAT_NATIVE_XUSB is restricted to instrumented private laboratory builds.
#endif
// Exported by ntoskrnl on supported Windows versions; not declared by the WDK
// ntifs header. Match xusb22's effective-token membership check exactly.
NTSYSAPI NTSTATUS NTAPI RtlCheckTokenMembershipEx(
    _In_opt_ HANDLE TokenHandle,_In_ PSID SidToCheck,_In_ ULONG Flags,_Out_ PBOOLEAN IsMember);

// Experimental single-slot XUSB upper filter. Build-gated at FilterAdd.
// Uses the LocalSystem unfiltered stream, not the foreground-only WAIT stream.
// Experimental: HID mirror isolation and platform coverage remain mandatory.
// All WAIT_INPUT requests are kept in WDF cancel-safe queues. Only the private
// reader sends WAIT to xusb22; it is canceled/drained BEFORE surprise removal.
// Lock order is stateLock -> gGate; never join the reader while holding gGate.
typedef struct XUSB_FILE {
    LIST_ENTRY link;
    WDFFILEOBJECT file;
    WDFQUEUE waits;
    BOOLEAN linked;
} XUSB_FILE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XUSB_FILE,XusbFile)
typedef struct XUSB_REQUEST {
    BOOLEAN systemAccess;
    BOOLEAN forward;
    LIST_ENTRY completionLink;
    WDFREQUEST request;
    NTSTATUS status;
    ULONG bytes;
#if defined(IC_COMPAT_TRACE)
    ULONGLONG traceId;
#endif
} XUSB_REQUEST;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XUSB_REQUEST,XusbRequest)
typedef struct XUSB_DEVICE {
    WDFDEVICE device;
    WDFWAITLOCK stateLock;
    LIST_ENTRY link,files,completions;
    WDFWORKITEM completionWork;
    BOOLEAN completionScheduled;
    IC_DEVICE_INFO info;
    WCHAR instanceBuffer[IC_XUSB_MIRROR_INSTANCE_CHARS];
    UNICODE_STRING instance;
    ULONG captureId;
    BOOLEAN registered,counted,powered,ready,used,valid,bypass,bypassing;
    XUSB_FILE* source;
    WDFREQUEST request;
    WDFMEMORY inputMemory,outputMemory;
    UCHAR input[4],raw[IC_XUSB_STATE_BYTES],physical[IC_XUSB_STATE_BYTES],delivered[IC_XUSB_STATE_BYTES];
    PETHREAD thread;
    KEVENT stop,done,started;
    NTSTATUS status,startStatus;
    BOOLEAN starting;
    ULONG_PTR bytes;
#if defined(IC_COMPAT_TRACE)
    KSPIN_LOCK traceLock;
    ULONGLONG traceSequence,traceRequests;
    IC_XUSB_COMPAT_RECORD trace[IC_XUSB_COMPAT_TRACE_CAPACITY];
    ULONG traceErrors;
    IC_XUSB_COMPAT_RECORD traceError[IC_XUSB_COMPAT_ERROR_CAPACITY];
#endif
} XUSB_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XUSB_DEVICE,XusbDevice)
typedef struct XUSB_WORK {XUSB_DEVICE* device;} XUSB_WORK;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XUSB_WORK,XusbWork)
static LIST_ENTRY devices;
static IC_CONTROLLER_XUSB_INPUT* captured;
static XUSB_DEVICE* targets[IC_MAX_DEVICES];
static ULONG targetCount,head,count;
static PKEVENT readyEvent;
static LONG nextId;
static NTSTATUS StartLocked(XUSB_DEVICE* d);
static void StopLocked(XUSB_DEVICE* d);
static void Forward(WDFREQUEST r,WDFDEVICE d);

#if defined(IC_COMPAT_TRACE)
static void Trace(XUSB_DEVICE* d,WDFREQUEST r,UCHAR stage,UCHAR route,NTSTATUS status,ULONG_PTR information) {
    WDF_REQUEST_PARAMETERS p;IC_XUSB_COMPAT_RECORD record;KIRQL irql;
    PVOID data=NULL;size_t length=0;LARGE_INTEGER now;
    WDF_REQUEST_PARAMETERS_INIT(&p);WdfRequestGetParameters(r,&p);
    if(p.Type!=WdfRequestTypeDeviceControl)return;
    // Include forwarded Windows management/enumeration requests as well as
    // the three virtualized state requests. Exclude private capture IOCTLs.
    if((p.Parameters.DeviceIoControl.IoControlCode&0xffff0000u)!=0x80000000u)return;
    RtlZeroMemory(&record,sizeof(record));now=KeQueryPerformanceCounter(NULL);record.qpc=now.QuadPart;
    record.requestor_pid=(ULONGLONG)(ULONG_PTR)IoGetRequestorProcessId(WdfRequestWdmGetIrp(r));
    record.current_pid=(ULONGLONG)(ULONG_PTR)PsGetCurrentProcessId();
    record.current_thread=(ULONGLONG)(ULONG_PTR)PsGetCurrentThreadId();
    record.code=p.Parameters.DeviceIoControl.IoControlCode;
    record.input_bytes=(ULONG)p.Parameters.DeviceIoControl.InputBufferLength;
    record.output_bytes=(ULONG)p.Parameters.DeviceIoControl.OutputBufferLength;
    record.status=status;record.information=information;record.capture_id=d->captureId;
    record.stage=stage;record.route=route;record.requestor_mode=(UCHAR)WdfRequestGetRequestorMode(r);
    record.system_access=XusbRequest(r)->systemAccess;
    if(NT_SUCCESS(WdfRequestRetrieveInputBuffer(r,1,&data,&length)))
        RtlCopyMemory(record.input,data,min(length,sizeof(record.input)));
    // Dedicated lab fixture evidence only. METHOD_BUFFERED has overwritten the
    // original input header by completion; output is explicitly distinguished.
    if(stage==IC_XUSB_TRACE_COMPLETE && NT_SUCCESS(status) &&
       record.code==IC_XUSB_GET_STATE && information==IC_XUSB_STATE_BYTES &&
       NT_SUCCESS(WdfRequestRetrieveOutputBuffer(r,IC_XUSB_STATE_BYTES,&data,NULL))) {
        RtlCopyMemory(record.output,data,IC_XUSB_STATE_BYTES);record.output_copied=IC_XUSB_STATE_BYTES;
    }
    KeAcquireSpinLock(&d->traceLock,&irql);
    if(stage==IC_XUSB_TRACE_CALLER)XusbRequest(r)->traceId=++d->traceRequests;
    record.request_id=XusbRequest(r)->traceId;record.sequence=++d->traceSequence;
    d->trace[(record.sequence-1)%IC_XUSB_COMPAT_TRACE_CAPACITY]=record;
    if(!NT_SUCCESS(status) && d->traceErrors<IC_XUSB_COMPAT_ERROR_CAPACITY) {
        BOOLEAN seen=FALSE;
        for(ULONG i=0;i<d->traceErrors;++i)if(d->traceError[i].request_id==record.request_id)seen=TRUE;
        if(!seen)d->traceError[d->traceErrors++]=record;
    }
    KeReleaseSpinLock(&d->traceLock,irql);
}
static void TraceSnapshot(XUSB_DEVICE* d,WDFREQUEST r) {
    IC_XUSB_COMPAT_TRACE* output;KIRQL irql;LARGE_INTEGER frequency;
    NTSTATUS status=WdfRequestRetrieveOutputBuffer(r,sizeof(*output),(PVOID*)&output,NULL);
    if(!NT_SUCCESS(status)){WdfRequestComplete(r,status);return;}
    RtlZeroMemory(output,sizeof(*output));(void)KeQueryPerformanceCounter(&frequency);
    output->schema=IC_XUSB_COMPAT_TRACE_SCHEMA;output->record_bytes=sizeof(IC_XUSB_COMPAT_RECORD);
    output->capacity=IC_XUSB_COMPAT_TRACE_CAPACITY;output->qpc_frequency=frequency.QuadPart;
    output->error_capacity=IC_XUSB_COMPAT_ERROR_CAPACITY;
    KeAcquireSpinLock(&d->traceLock,&irql);
    output->count=(ULONG)min(d->traceSequence,(ULONGLONG)IC_XUSB_COMPAT_TRACE_CAPACITY);
    output->next_sequence=d->traceSequence+1;
    output->error_count=d->traceErrors;
    RtlCopyMemory(output->first_errors,d->traceError,sizeof(d->traceError));
    // Oldest to newest, including after the fixed-size ring wraps.
    for(ULONG i=0;i<output->count;++i) {
        ULONGLONG sequence=d->traceSequence-output->count+1+i;
        output->records[i]=d->trace[(sequence-1)%IC_XUSB_COMPAT_TRACE_CAPACITY];
    }
    KeReleaseSpinLock(&d->traceLock,irql);
    WdfRequestCompleteWithInformation(r,STATUS_SUCCESS,sizeof(*output));
}
#define XUSB_TRACE(d,r,stage,route,status,bytes) Trace(d,r,stage,route,status,bytes)
#else
#define XUSB_TRACE(d,r,stage,route,status,bytes) ((void)0)
#endif

void IcXusbInitialize(void){InitializeListHead(&devices);}
static BOOLEAN CaptureReady(XUSB_DEVICE* d) {
    return !d->bypass && !d->bypassing && d->ready && d->powered && d->thread && !KeReadStateEvent(&d->stop) && IcXusbMirrorReadyLocked(&d->instance);
}
NTSTATUS IcXusbQueryLocked(ULONG token,IC_DEVICE_QUERY_RESULT* result,BOOLEAN ordinal) {
    PLIST_ENTRY p;ULONG position=0;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_DEVICE* d=CONTAINING_RECORD(p,XUSB_DEVICE,link);
        if((ordinal?++position:d->info.instance_token)!=token)continue;
        result->instance_token=d->info.instance_token;
        const BOOLEAN mirror=IcXusbMirrorReadyLocked(&d->instance);
        result->flags=(CaptureReady(d)?1u:0u)|(d->thread && !KeReadStateEvent(&d->stop)?2u:0u)|(d->powered?4u:0u)|(d->valid?8u:0u)|(d->bypass?64u:0u);
        if(d->ready && !mirror)result->reserved[1]=IC_DEVICE_WAIT_MIRROR;
        result->last_read_status=(ULONG)d->status;result->report_bytes=d->info.input_bytes;
        RtlCopyMemory(result->instance,d->instanceBuffer,sizeof(d->instanceBuffer));
        if(result->instance[0])result->flags|=IC_QUERY_IDENTITY_VALID;
        return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_NOT_CONNECTED;
}
ULONG IcXusbCountLocked(void) {
    PLIST_ENTRY p;ULONG n=0;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_DEVICE* d=CONTAINING_RECORD(p,XUSB_DEVICE,link);
        if(CaptureReady(d))++n;
    }
    return n;
}
BOOLEAN IcXusbPhysicalLocked(PCUNICODE_STRING parent,IC_NATIVE_GAMEPAD_STATE* state) {
    PLIST_ENTRY p;XUSB_DEVICE* found=NULL;
    if(!parent || !parent->Buffer || !parent->Length || !state)return FALSE;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_DEVICE* d=CONTAINING_RECORD(p,XUSB_DEVICE,link);
        if(RtlEqualUnicodeString(parent,&d->instance,TRUE) && d->ready && d->thread && !KeReadStateEvent(&d->stop)) {
            // An old removed FDO can remain alive through an application file.
            // Ignore it; two active pumps for one instance are ambiguous.
            if(found)return FALSE;found=d;
        }
    }
    return found && IcXusbDecodeState(found->physical,sizeof(found->physical),state);
}
static void CompleteWaits(WDFWORKITEM work) {
    XUSB_DEVICE* d=XusbWork(work)->device;
    for(;;) {
        XUSB_REQUEST* pending;
        WdfWaitLockAcquire(gGate,NULL);
        if(IsListEmpty(&d->completions)) {
            d->completionScheduled=FALSE;WdfWaitLockRelease(gGate);return;
        }
        pending=CONTAINING_RECORD(RemoveHeadList(&d->completions),XUSB_REQUEST,completionLink);
        WdfWaitLockRelease(gGate);
        // Kernel clients may resubmit synchronously from completion. The
        // global state lock must not be held when calling their callbacks.
        if(pending->forward)Forward(pending->request,d->device);
        else {
#if defined(IC_COMPAT_TRACE)
            XUSB_TRACE(d,pending->request,IC_XUSB_TRACE_COMPLETE,
                NT_SUCCESS(pending->status)?IC_XUSB_TRACE_BROADCAST:IC_XUSB_TRACE_DRAIN,pending->status,pending->bytes);
#endif
            WdfRequestCompleteWithInformation(pending->request,pending->status,pending->bytes);
        }
    }
}
static void QueueCompletion(XUSB_DEVICE* d,WDFREQUEST r,NTSTATUS status,ULONG bytes) {
    XUSB_REQUEST* pending=XusbRequest(r);
    pending->request=r;pending->status=status;pending->bytes=bytes;pending->forward=FALSE;
    InsertTailList(&d->completions,&pending->completionLink);
    if(!d->completionScheduled) {d->completionScheduled=TRUE;WdfWorkItemEnqueue(d->completionWork);}
}
static void QueueForward(XUSB_DEVICE* d,WDFREQUEST r) {
    XUSB_REQUEST* pending=XusbRequest(r);
    pending->request=r;pending->forward=TRUE;pending->status=STATUS_SUCCESS;pending->bytes=0;
    InsertTailList(&d->completions,&pending->completionLink);
    if(!d->completionScheduled) {d->completionScheduled=TRUE;WdfWorkItemEnqueue(d->completionWork);}
}
static NTSTATUS CopyState(WDFREQUEST r,const UCHAR* state,ULONG* written) {
    WDF_REQUEST_PARAMETERS p;PVOID output;ULONG bytes;NTSTATUS s;
    WDF_REQUEST_PARAMETERS_INIT(&p);WdfRequestGetParameters(r,&p);
    bytes=p.Parameters.DeviceIoControl.IoControlCode==IC_XUSB_WAIT_UNFILTERED?IC_XUSB_UNFILTERED_BYTES:IC_XUSB_STATE_BYTES;
    s=WdfRequestRetrieveOutputBuffer(r,bytes,&output,NULL);
    if(NT_SUCCESS(s))RtlCopyMemory(output,state+(bytes==IC_XUSB_UNFILTERED_BYTES?5:0),bytes);
    *written=NT_SUCCESS(s)?bytes:0;return s;
}
static BOOLEAN NativeManagementAccess(void) {
    BOOLEAN member=FALSE;
    NTSTATUS status=RtlCheckTokenMembershipEx(NULL,SeExports->SeLocalSystemSid,0,&member);
    return NT_SUCCESS(status) && member;
}
static void CallerContext(WDFDEVICE device,WDFREQUEST request) {
    WDF_REQUEST_PARAMETERS p;NTSTATUS s;
    WDF_REQUEST_PARAMETERS_INIT(&p);WdfRequestGetParameters(request,&p);
#if defined(IC_COMPAT_TRACE)
    XusbRequest(request)->systemAccess=FALSE;
#endif
#if defined(IC_COMPAT_NATIVE_XUSB)
    if(p.Type==WdfRequestTypeDeviceControl &&
       (p.Parameters.DeviceIoControl.IoControlCode&0xffff0000u)==0x80000000u) {
        // A permanent noncapturable FDO is the diagnostic control: preserve
        // native validation, security, caller identity and pending semantics.
        XUSB_TRACE(XusbDevice(device),request,IC_XUSB_TRACE_CALLER,IC_XUSB_TRACE_SEEN,STATUS_PENDING,0);
        Forward(request,device);return;
    }
#endif
    if(p.Type==WdfRequestTypeDeviceControl && p.Parameters.DeviceIoControl.IoControlCode==IC_XUSB_WAIT_UNFILTERED) {
        if(KeGetCurrentIrql()!=PASSIVE_LEVEL){
#if defined(IC_COMPAT_TRACE)
            XUSB_TRACE(XusbDevice(device),request,IC_XUSB_TRACE_CALLER,IC_XUSB_TRACE_IRQL,STATUS_INVALID_DEVICE_STATE,0);
#endif
            WdfRequestComplete(request,STATUS_INVALID_DEVICE_STATE);return;
        }
        // Native policy checks enabled membership, including special Windows
        // identities whose user SID differs from an enabled LocalSystem SID.
        // Keep the check in the issuing thread before KMDF can move execution.
        XusbRequest(request)->systemAccess=NativeManagementAccess();
    }
#if defined(IC_COMPAT_TRACE)
    XUSB_TRACE(XusbDevice(device),request,IC_XUSB_TRACE_CALLER,IC_XUSB_TRACE_SEEN,STATUS_PENDING,0);
#endif
    s=WdfDeviceEnqueueRequest(device,request);if(!NT_SUCCESS(s)){
#if defined(IC_COMPAT_TRACE)
        XUSB_TRACE(XusbDevice(device),request,IC_XUSB_TRACE_COMPLETE,IC_XUSB_TRACE_ENQUEUE_FAILED,s,0);
#endif
        WdfRequestComplete(request,s);
    }
}
static void Broadcast(XUSB_DEVICE* d,const UCHAR* state) {
    PLIST_ENTRY p;IcXusbPublishState(d->delivered,d->valid,state);d->valid=TRUE;
    for(p=d->files.Flink;p!=&d->files;p=p->Flink) {
        XUSB_FILE* f=CONTAINING_RECORD(p,XUSB_FILE,link);WDFREQUEST r;
        while(NT_SUCCESS(WdfIoQueueRetrieveNextRequest(f->waits,&r))) {
            ULONG bytes;NTSTATUS s=CopyState(r,d->delivered,&bytes);QueueCompletion(d,r,s,bytes);
        }
    }
}
NTSTATUS IcXusbCaptureLocked(PKEVENT event,IC_DEVICE_INFO* output,ULONG capacity,ULONG* written) {
    ULONG n=IcXusbCountLocked();PLIST_ENTRY p;NTSTATUS status;
    if(!n)return STATUS_DEVICE_NOT_READY;
    if(n>capacity)return STATUS_BUFFER_TOO_SMALL;
    if(captured)return STATUS_SHARING_VIOLATION;
    // Only complete native + legacy-HID pairs are capturable, exactly as
    // reported by DEVICE_QUERY and CountLocked. A pending pair is not half-owned.
    captured=ExAllocatePoolZero(NonPagedPoolNx,sizeof(*captured)*IC_XUSB_CAPACITY,FILTER_TAG);
    if(!captured)return STATUS_INSUFFICIENT_RESOURCES;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_DEVICE* d=CONTAINING_RECORD(p,XUSB_DEVICE,link);
        if(!CaptureReady(d))continue;
        status=IcXusbMirrorCaptureLocked(&d->instance);
        if(!NT_SUCCESS(status)) {
            IcXusbResetLocked();return status;
        }
    }
    n=0;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_DEVICE* d=CONTAINING_RECORD(p,XUSB_DEVICE,link);d->captureId=0;
        if(!CaptureReady(d))continue;
        d->captureId=++n;targets[n-1]=d;output[n-1]=d->info;output[n-1].id=n;
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
    PLIST_ENTRY p;
    if(!captured || !IcAllCaptureLocked(IC_TYPE_CONTROLLER_XUSB))return;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_DEVICE* d=CONTAINING_RECORD(p,XUSB_DEVICE,link);NTSTATUS status;
        if(d->captureId) {if(!CaptureReady(d)) {IcRevokeAllLocked(IC_RESET_DEVICE);return;}continue;}
        if(!CaptureReady(d))continue;
        status=IcXusbMirrorCaptureLocked(&d->instance);
        if(!NT_SUCCESS(status)){IcRevokeAllLocked(IC_RESET_DEVICE);return;}
        if(!IcAllAttachLocked(IC_TYPE_CONTROLLER_XUSB))return;
        d->captureId=++targetCount;targets[targetCount-1]=d;
    }
}
ULONG IcXusbSnapshotLocked(IC_DEVICE_INFO* output,ULONG capacity) {
    ULONG i,n=min(targetCount,capacity);
    for(i=0;i<n;++i){output[i]=targets[i]->info;output[i].id=i+1;}
    return n;
}
void IcXusbResetLocked(void) {
    PLIST_ENTRY p;
    if(!captured)return;
    ExFreePoolWithTag(captured,FILTER_TAG);captured=NULL;head=count=targetCount=0;readyEvent=NULL;
    IcXusbMirrorResetLocked();
    RtlZeroMemory(targets,sizeof(targets));
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_DEVICE* d=CONTAINING_RECORD(p,XUSB_DEVICE,link);BOOLEAN wasCaptured=d->captureId!=0;d->captureId=0;
        if(d->ready || wasCaptured) {
            IC_NATIVE_GAMEPAD_STATE physical;
            // The HID reader may trail the native reader. Restore the latest
            // native physical state in both views when that mirror is available.
            if(IcXusbDecodeState(d->physical,sizeof(d->physical),&physical))
                (void)IcXusbMirrorSendLocked(&d->instance,&physical,FALSE);
            if(d->ready)Broadcast(d,d->physical);
        }
    }
}
NTSTATUS IcXusbGetLocked(ULONG requested,PVOID output,SIZE_T length,SIZE_T* written) {
    uint32_t readHead=head,readCount=count;
    LONG n=IcInputReadSelected(&readHead,&readCount,captured,IC_XUSB_CAPACITY,sizeof(*captured),
        FIELD_OFFSET(IC_CONTROLLER_XUSB_INPUT,device),output,sizeof(*captured),
        (ULONG)min(length/sizeof(*captured),IC_XUSB_CAPACITY),requested,0,TRUE);
    if(n<0)return STATUS_BUFFER_TOO_SMALL;
    head=readHead;count=readCount;*written=(SIZE_T)n*sizeof(*captured);
    if(!count && readyEvent)KeClearEvent(readyEvent);return STATUS_SUCCESS;
}
NTSTATUS IcXusbGetSelectedLocked(const IC_GET_SELECT* request,PVOID output,SIZE_T length,SIZE_T* written) {
    const BOOLEAN consume=(request->flags&IC_GET_PEEK)==0;
    uint32_t readHead=head,readCount=count;
    LONG n=IcInputReadSelected(&readHead,&readCount,captured,IC_XUSB_CAPACITY,sizeof(*captured),
        FIELD_OFFSET(IC_CONTROLLER_XUSB_INPUT,device),output,sizeof(*captured),
        (ULONG)min(length/sizeof(*captured),IC_XUSB_CAPACITY),request->base.count,request->device,consume);
    if(n<0)return STATUS_BUFFER_TOO_SMALL;
    if(consume){head=readHead;count=readCount;}
    if(consume && !count && readyEvent)KeClearEvent(readyEvent);
    *written=(SIZE_T)n*sizeof(*captured);return STATUS_SUCCESS;
}
static XUSB_DEVICE* Find(ULONG id,BOOLEAN owned) {
    PLIST_ENTRY p;ULONG n=0;XUSB_DEVICE* first=NULL;
    if(owned)return targetCount?targets[id && id<=targetCount?id-1:0]:NULL;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        XUSB_DEVICE* d=CONTAINING_RECORD(p,XUSB_DEVICE,link);
        if(!d->ready)continue;if(!first)first=d;if(++n==id)return d;
    }
    return first;
}
NTSTATUS IcXusbSendLocked(PVOID input,SIZE_T length,IC_SEND_RESULT* result,BOOLEAN owned) {
    IC_CONTROLLER_XUSB_INPUT* in=input;ULONG i,n=(ULONG)(length/sizeof(*in));
    if(!n || n>IC_MAX_SEND || length%sizeof(*in))return STATUS_INVALID_PARAMETER;
    for(i=0;i<n;++i)if(in[i].reserved || in[i].padding || in[i].state.reserved || in[i].state.left_trigger>255 || in[i].state.right_trigger>255)return STATUS_INVALID_PARAMETER;
    result->accepted=0;result->status=STATUS_SUCCESS;
    for(i=0;i<n;++i) {
        UCHAR data[IC_XUSB_STATE_BYTES];IC_NATIVE_GAMEPAD_STATE state=in[i].state;
        XUSB_DEVICE* d=Find(in[i].device,owned);
        if(!d || !d->ready){result->status=(ULONG)STATUS_DEVICE_NOT_CONNECTED;break;}
        RtlCopyMemory(data,d->physical,sizeof(data));
        // Broadcast assigns the same publication counter policy to injection,
        // physical input, restart and context restoration. Caller packet values
        // never determine the native stream's packet number.
        if(!IcXusbReplaceState(data,sizeof(data),&state)){result->status=(ULONG)STATUS_NOT_SUPPORTED;break;}
        {
            NTSTATUS mirrorStatus=IcXusbMirrorSendLocked(&d->instance,&state,owned);
            if(!NT_SUCCESS(mirrorStatus)){result->status=(ULONG)mirrorStatus;break;}
        }
        Broadcast(d,data);++result->accepted;
    }
    return STATUS_SUCCESS;
}
static void Done(WDFREQUEST r,WDFIOTARGET t,PWDF_REQUEST_COMPLETION_PARAMS p,WDFCONTEXT context) {
    XUSB_DEVICE* d=context;UNREFERENCED_PARAMETER(r);UNREFERENCED_PARAMETER(t);
    d->status=p->IoStatus.Status;d->bytes=p->IoStatus.Information;KeSetEvent(&d->done,IO_NO_INCREMENT,FALSE);
}
static void ReaderStarted(XUSB_DEVICE* d,NTSTATUS status) {
    if(!d->starting)return;
    WdfWaitLockAcquire(gGate,NULL);
    if(NT_SUCCESS(status) && KeReadStateEvent(&d->stop))status=STATUS_CANCELLED;
    d->startStatus=status;d->ready=NT_SUCCESS(status);
    d->starting=FALSE;
    IcAllReadersReadyLocked();
    WdfWaitLockRelease(gGate);
    KeSetEvent(&d->started,IO_NO_INCREMENT,FALSE);
}
static NTSTATUS PhysicalRequest(XUSB_DEVICE* d,BOOLEAN wait,BOOLEAN snapshot,ULONGLONG deadline) {
    WDFIOTARGET target=WdfDeviceGetIoTarget(d->device);WDFMEMORY_OFFSET offset;
    WDF_REQUEST_REUSE_PARAMS reuse;NTSTATUS s;PVOID events[2]={&d->stop,&d->done};LARGE_INTEGER timeout;
    if(KeReadStateEvent(&d->stop))return STATUS_CANCELLED;
    if(d->used){WDF_REQUEST_REUSE_PARAMS_INIT(&reuse,WDF_REQUEST_REUSE_NO_FLAGS,STATUS_SUCCESS);s=WdfRequestReuse(d->request,&reuse);if(!NT_SUCCESS(s))return s;}
    d->used=TRUE;KeClearEvent(&d->done);
    RtlZeroMemory(&offset,sizeof(offset));offset.BufferLength=wait?IcXusbBuildUnfilteredRequest(0,d->input):IcXusbBuildRequest(0,FALSE,d->input);
    if(snapshot)d->input[3]=1; // Complete this unfiltered request from the cached physical state.
    s=WdfIoTargetFormatRequestForIoctl(target,d->request,wait?IC_XUSB_WAIT_UNFILTERED:IC_XUSB_GET_STATE,d->inputMemory,&offset,d->outputMemory,NULL);
    if(!NT_SUCCESS(s))return s;
    IoGetNextIrpStackLocation(WdfRequestWdmGetIrp(d->request))->FileObject=WdfFileObjectWdmGetFileObject(d->source->file);
    // xusb22 inspects IoGetRequestorProcess even for kernel-originated WAITs.
    // A fresh WDF-owned IRP has no associated thread. This issuing thread stays
    // alive and waits for completion/cancellation before reuse or termination.
    WdfRequestWdmGetIrp(d->request)->Tail.Overlay.Thread=PsGetCurrentThread();
    WdfRequestSetCompletionRoutine(d->request,Done,d);
    if(!WdfRequestSend(d->request,target,NULL))return WdfRequestGetStatus(d->request);
    // CREATE cannot advertise a source until the event-driven reader is armed.
    // This also prevents an immediate application report from racing startup.
    if(wait && !snapshot) {
        // Submission can complete synchronously with a lower-stack failure.
        // Such a request never establishes a usable event-driven source.
        if(d->starting && KeReadStateEvent(&d->done)) {
            IC_NATIVE_GAMEPAD_STATE state;
            if(!NT_SUCCESS(d->status))return d->status;
            if(d->bytes!=IC_XUSB_UNFILTERED_BYTES || !IcXusbDecodeUnfiltered(d->raw,(ULONG)d->bytes,&state))
                return d->status=STATUS_NOT_SUPPORTED;
        }
        ReaderStarted(d,STATUS_SUCCESS);
    }
    timeout.QuadPart=-2LL*1000*1000*10;
    if(deadline){ULONGLONG now=KeQueryInterruptTime();timeout.QuadPart=now>=deadline?0:-(LONGLONG)(deadline-now);}
    if(deadline && timeout.QuadPart>=0)s=STATUS_TIMEOUT;
    else s=KeWaitForMultipleObjects(2,events,WaitAny,Executive,KernelMode,FALSE,wait && !snapshot?NULL:&timeout,NULL);
    if(s==STATUS_WAIT_0 || s==STATUS_TIMEOUT){WdfRequestCancelSentRequest(d->request);KeWaitForSingleObject(&d->done,Executive,KernelMode,FALSE,NULL);return s==STATUS_TIMEOUT?STATUS_IO_TIMEOUT:STATUS_CANCELLED;}
    return d->status;
}
static NTSTATUS Physical(XUSB_DEVICE* d,BOOLEAN wait) {return PhysicalRequest(d,wait,FALSE,0);}
static void Reader(PVOID context) {
    XUSB_DEVICE* d=context;BOOLEAN wait=TRUE;
    while(!KeReadStateEvent(&d->stop)) {
        IC_NATIVE_GAMEPAD_STATE decoded;NTSTATUS s=Physical(d,wait);
        ReaderStarted(d,s); // Publish submission failures as well as success.
        if(KeReadStateEvent(&d->stop))break;
        WdfWaitLockAcquire(gGate,NULL);
        // StopLocked may have signalled stop while we were acquiring gGate.
        // A stopped reader must never resurrect ready or publish another input.
        if(KeReadStateEvent(&d->stop)){WdfWaitLockRelease(gGate);break;}
        if(!NT_SUCCESS(s) || !IcXusbDecodeUnfiltered(d->raw,(ULONG)d->bytes,&decoded)) {
            if(NT_SUCCESS(s))d->status=STATUS_NOT_SUPPORTED;
            d->ready=FALSE;if(captured && d->captureId)IcRevokeAllLocked(IC_RESET_DEVICE);
            WdfWaitLockRelease(gGate);break; // unknown formats fail closed, no polling
        }
        // The unfiltered payload is the complete suffix of the 29-byte state.
        // Preserve opaque fields too, rather than rebuilding only known axes.
        RtlMoveMemory(d->raw+5,d->raw,IC_XUSB_UNFILTERED_BYTES);
        RtlCopyMemory(d->raw,d->physical,5);
        if(!d->ready && captured && !IcAllCaptureLocked(IC_TYPE_CONTROLLER_XUSB))IcRevokeAllLocked(IC_RESET_DEVICE);
        d->ready=TRUE;RtlCopyMemory(d->physical,d->raw,sizeof(d->raw));
        if(captured && !d->captureId)IcAllReadersReadyLocked();
        // CREATE_ALL can arm XUSB before its required HID mirror is ready.
        // Until this parent is attached, its physical reports stay transparent
        // and cannot overflow another captured parent's FIFO. Fixed snapshots
        // still revoke when an input source falls outside their membership.
        if(captured && ((!d->captureId && !IcAllCaptureLocked(IC_TYPE_CONTROLLER_XUSB)) ||
                        (d->captureId && count==IC_XUSB_CAPACITY)))
            IcRevokeAllLocked(count==IC_XUSB_CAPACITY?IC_RESET_OVERFLOW:IC_RESET_DEVICE);
        if(captured && d->captureId) {
            IC_CONTROLLER_XUSB_INPUT* out=&captured[(head+count)%IC_XUSB_CAPACITY];RtlZeroMemory(out,sizeof(*out));
            out->sequence=IcNextSequenceLocked();out->timestamp=KeQueryPerformanceCounter(NULL).QuadPart;
            out->device=d->captureId;out->state=decoded;if(++count==1)KeSetEvent(readyEvent,IO_NO_INCREMENT,FALSE);
        }else {
            // One source orders both views. The mirror's independent HID read
            // must not later replay an older physical packet after restoration.
            IcXusbMirrorPhysicalLocked(&d->instance,&decoded);Broadcast(d,d->raw);
        }
        WdfWaitLockRelease(gGate);wait=TRUE;
    }
    ReaderStarted(d,STATUS_CANCELLED); // Also release CREATE if stopped before its first WAIT.
    PsTerminateSystemThread(STATUS_SUCCESS);
}
static void Bootstrap(PVOID context) {
    XUSB_DEVICE* d=context;IC_NATIVE_GAMEPAD_STATE state;UCHAR envelope[5];
    const ULONGLONG deadline=KeQueryInterruptTime()+2ULL*1000*1000*10;
    // Both native queries belong to this persistent LocalSystem thread. The
    // foreground GET supplies verified metadata; it is not the physical input
    // oracle. The targeted unfiltered snapshot supplies the exact cached state
    // even when no button or axis changes during application startup.
    NTSTATUS s=PhysicalRequest(d,FALSE,FALSE,deadline);
    if(NT_SUCCESS(s)) {
        if(d->bytes!=IC_XUSB_STATE_BYTES || !IcXusbDecodeState(d->raw,(ULONG)d->bytes,&state))s=STATUS_NOT_SUPPORTED;
        else {
            RtlCopyMemory(envelope,d->raw,sizeof(envelope));
            s=PhysicalRequest(d,TRUE,TRUE,deadline);
            if(NT_SUCCESS(s) && (d->bytes!=IC_XUSB_UNFILTERED_BYTES || !IcXusbDecodeUnfiltered(d->raw,(ULONG)d->bytes,&state)))s=STATUS_NOT_SUPPORTED;
        }
    }
    WdfWaitLockAcquire(gGate,NULL);
    if(KeReadStateEvent(&d->stop))s=STATUS_CANCELLED;
    if(NT_SUCCESS(s)) {
        RtlMoveMemory(d->raw+sizeof(envelope),d->raw,IC_XUSB_UNFILTERED_BYTES);
        RtlCopyMemory(d->raw,envelope,sizeof(envelope));
        if(captured && !IcAllCaptureLocked(IC_TYPE_CONTROLLER_XUSB))IcRevokeAllLocked(IC_RESET_DEVICE);
        RtlCopyMemory(d->physical,d->raw,sizeof(d->raw));Broadcast(d,d->raw);
    }else d->status=s;
    WdfWaitLockRelease(gGate);
    if(!NT_SUCCESS(s)){ReaderStarted(d,s);PsTerminateSystemThread(STATUS_SUCCESS);return;}
    Reader(d); // Only its ordinary pending WAIT is allowed to publish ready.
}
static NTSTATUS StartLocked(XUSB_DEVICE* d) {
    HANDLE handle=NULL;NTSTATUS s;OBJECT_ATTRIBUTES a;LARGE_INTEGER noWait;
    noWait.QuadPart=0;
    if(d->bypass)return STATUS_SUCCESS;
    if(d->thread && KeWaitForSingleObject(d->thread,Executive,KernelMode,FALSE,&noWait)==STATUS_SUCCESS) {
        ObDereferenceObject(d->thread);d->thread=NULL;
    }
    if(d->thread || !d->powered || IsListEmpty(&d->files))return STATUS_SUCCESS;
    d->source=CONTAINING_RECORD(d->files.Flink,XUSB_FILE,link);KeClearEvent(&d->stop);
    InitializeObjectAttributes(&a,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
    WdfWaitLockAcquire(gGate,NULL);
    d->ready=FALSE;
    d->starting=TRUE;d->startStatus=STATUS_PENDING;KeClearEvent(&d->started);
    s=PsCreateSystemThread(&handle,THREAD_ALL_ACCESS,&a,NULL,NULL,Bootstrap,d);
    if(NT_SUCCESS(s)){s=ObReferenceObjectByHandle(handle,SYNCHRONIZE,*PsThreadType,KernelMode,(PVOID*)&d->thread,NULL);if(!NT_SUCCESS(s))KeSetEvent(&d->stop,IO_NO_INCREMENT,FALSE);}
    if(!NT_SUCCESS(s)){d->ready=FALSE;d->starting=FALSE;d->startStatus=d->status=s;}
    WdfWaitLockRelease(gGate);
    if(handle){if(!NT_SUCCESS(s))ZwWaitForSingleObject(handle,FALSE,NULL);ZwClose(handle);}
    if(NT_SUCCESS(s)){KeWaitForSingleObject(&d->started,Executive,KernelMode,FALSE,NULL);s=d->startStatus;}
    return s;
}
static void StopLocked(XUSB_DEVICE* d) {
    PETHREAD thread;PLIST_ENTRY p;
    WdfWaitLockAcquire(gGate,NULL);thread=d->thread;d->thread=NULL;d->ready=FALSE;
    if(thread && captured && d->captureId)IcRevokeAllLocked(IC_RESET_DEVICE);
    // These queues intentionally are not power-managed. Drain them explicitly
    // before lower-stack removal, even while applications retain open handles.
    KeSetEvent(&d->stop,IO_NO_INCREMENT,FALSE);WdfWaitLockRelease(gGate);
    if(thread){KeWaitForSingleObject(thread,Executive,KernelMode,FALSE,NULL);ObDereferenceObject(thread);}
    WdfWaitLockAcquire(gGate,NULL);d->ready=FALSE;
    for(p=d->files.Flink;p!=&d->files;p=p->Flink) {
        XUSB_FILE* f=CONTAINING_RECORD(p,XUSB_FILE,link);WDFREQUEST request;
        while(NT_SUCCESS(WdfIoQueueRetrieveNextRequest(f->waits,&request))) {
            if(d->bypass || d->bypassing)QueueForward(d,request);
            else QueueCompletion(d,request,STATUS_DEVICE_NOT_CONNECTED,0);
        }
    }
    WdfWaitLockRelease(gGate);d->source=NULL;
}
static BOOLEAN FailedStartBypassLocked(XUSB_DEVICE* d) {
    BOOLEAN bypass,entered=FALSE;
    // stateLock serializes this transition with CREATE, cleanup and D0. A
    // completed failed bootstrap cannot replace a working Windows controller
    // with permanent NOT_READY responses. A warming or owned source never
    // enters this mode. Once lower WAITs are admitted, keep this FDO transparent
    // so a later capture cannot inherit pending physical lower requests.
    WdfWaitLockAcquire(gGate,NULL);
    if(!d->bypass && !d->captureId && d->powered && !d->starting && !d->ready && !NT_SUCCESS(d->startStatus))d->bypassing=entered=TRUE;
    bypass=d->bypass;WdfWaitLockRelease(gGate);
    if(entered) {
        StopLocked(d);
        WdfWaitLockAcquire(gGate,NULL);d->bypass=TRUE;d->bypassing=FALSE;WdfWaitLockRelease(gGate);bypass=TRUE;
    }
    return bypass;
}
static void Forward(WDFREQUEST r,WDFDEVICE d) {
#if defined(IC_COMPAT_TRACE)
    XUSB_TRACE(XusbDevice(d),r,IC_XUSB_TRACE_FORWARD,IC_XUSB_TRACE_NATIVE,STATUS_PENDING,0);
#endif
    WDF_REQUEST_SEND_OPTIONS o;WdfRequestFormatRequestUsingCurrentType(r);WDF_REQUEST_SEND_OPTIONS_INIT(&o,WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    if(!WdfRequestSend(r,WdfDeviceGetIoTarget(d),&o))WdfRequestComplete(r,WdfRequestGetStatus(r));
}
static NTSTATUS SourceStatusLocked(XUSB_DEVICE* d) {
    if(d->bypass)return STATUS_NOT_SUPPORTED;
    if(!d->powered || KeReadStateEvent(&d->stop))return STATUS_DEVICE_NOT_READY;
    if(d->ready)return STATUS_SUCCESS;
    if(!NT_SUCCESS(d->startStatus))return d->startStatus;
    if(!NT_SUCCESS(d->status))return d->status;
    return STATUS_DEVICE_NOT_READY;
}
#if defined(IC_COMPAT_NATIVE_GET)
// Causal probe only: preserve Windows' exact GET contract at zero capture.
// Never substitute an unfiltered cache for Windows' caller-specific policy.
// A concurrent capture refuses the result; it cannot expose physical data.
static void NativeGet(XUSB_DEVICE* d,WDFREQUEST r) {
    WDF_REQUEST_SEND_OPTIONS options;NTSTATUS status;ULONG_PTR information;
    WdfWaitLockAcquire(gGate,NULL);
    if(d->captureId) {
        WdfWaitLockRelease(gGate);
        XUSB_TRACE(d,r,IC_XUSB_TRACE_COMPLETE,IC_XUSB_TRACE_NATIVE_GET,STATUS_DEVICE_BUSY,0);
        WdfRequestCompleteWithInformation(r,STATUS_DEVICE_BUSY,0);return;
    }
    WdfWaitLockRelease(gGate);
    WdfRequestFormatRequestUsingCurrentType(r);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,WDF_REL_TIMEOUT_IN_SEC(2));
    (void)WdfRequestSend(r,WdfDeviceGetIoTarget(d->device),&options);
    status=WdfRequestGetStatus(r);information=WdfRequestGetInformation(r);
    WdfWaitLockAcquire(gGate,NULL);
    if(d->captureId){status=STATUS_DEVICE_BUSY;information=0;}
    WdfWaitLockRelease(gGate);
    XUSB_TRACE(d,r,IC_XUSB_TRACE_COMPLETE,IC_XUSB_TRACE_NATIVE_GET,status,information);
    WdfRequestCompleteWithInformation(r,status,information);
}
#endif
static void Ioctl(WDFQUEUE queue,WDFREQUEST r,size_t outSize,size_t inSize,ULONG code) {
    XUSB_DEVICE* d=XusbDevice(WdfIoQueueGetDevice(queue));NTSTATUS s;PUCHAR in;BOOLEAN unfiltered=code==IC_XUSB_WAIT_UNFILTERED;
    WDFFILEOBJECT file=WdfRequestGetFileObject(r);XUSB_FILE* f=file?XusbFile(file):NULL;
#if defined(IC_COMPAT_TRACE)
    if(code==IOCTL_IC_XUSB_SOURCE && outSize==sizeof(IC_XUSB_COMPAT_TRACE)) {TraceSnapshot(d,r);return;}
    XUSB_TRACE(d,r,IC_XUSB_TRACE_DISPATCH,IC_XUSB_TRACE_SEEN,STATUS_PENDING,0);
#endif
#if defined(IC_COMPAT_NATIVE_GET)
    if(code==IC_XUSB_GET_STATE){NativeGet(d,r);return;}
#endif
    if(code==IOCTL_IC_XUSB_SOURCE){
        // CREATE owns stateLock through the bootstrap/arm handshake. A source
        // query must observe its final result rather than discard a live file
        // because the System reader has not finished starting yet.
        WdfWaitLockAcquire(d->stateLock,NULL);(void)FailedStartBypassLocked(d);
        WdfWaitLockAcquire(gGate,NULL);s=SourceStatusLocked(d);
        WdfWaitLockRelease(gGate);WdfWaitLockRelease(d->stateLock);WdfRequestComplete(r,s);return;
    }
    if(code!=IC_XUSB_GET_STATE && code!=IC_XUSB_WAIT_INPUT && !unfiltered){Forward(r,d->device);return;}
    if(unfiltered && !XusbRequest(r)->systemAccess){
#if defined(IC_COMPAT_TRACE)
        XUSB_TRACE(d,r,IC_XUSB_TRACE_COMPLETE,IC_XUSB_TRACE_ACCESS,STATUS_ACCESS_DENIED,0);
#endif
        WdfRequestComplete(r,STATUS_ACCESS_DENIED);return;
    }
    s=WdfRequestRetrieveInputBuffer(r,code==IC_XUSB_GET_STATE?3:4,(PVOID*)&in,NULL);
    if(NT_SUCCESS(s) && (outSize<(unfiltered?IC_XUSB_UNFILTERED_BYTES:IC_XUSB_STATE_BYTES) || inSize!=(code==IC_XUSB_GET_STATE?3u:4u)))s=STATUS_INVALID_PARAMETER;
    if(NT_SUCCESS(s) && (in[2]!=0 || (unfiltered?(IcNativeRead16(in)!=0x0103 || in[3]>1):
        (code==IC_XUSB_WAIT_INPUT?(IcNativeRead16(in)!=0x0102 || in[3]!=0):(IcNativeRead16(in)!=0x0101 && IcNativeRead16(in)!=0x0103)))))s=STATUS_NOT_SUPPORTED;
    WdfWaitLockAcquire(gGate,NULL);
    if(d->bypassing || (!d->bypass && !d->ready && !d->starting && !NT_SUCCESS(d->startStatus))) {
        BOOLEAN bypass;
        WdfWaitLockRelease(gGate);WdfWaitLockAcquire(d->stateLock,NULL);
        bypass=FailedStartBypassLocked(d);WdfWaitLockRelease(d->stateLock);
        if(bypass){Forward(r,d->device);return;}
        WdfWaitLockAcquire(gGate,NULL);
    }
    if(d->bypass){WdfWaitLockRelease(gGate);Forward(r,d->device);return;}
    if(!NT_SUCCESS(s)){
#if defined(IC_COMPAT_TRACE)
        XUSB_TRACE(d,r,IC_XUSB_TRACE_COMPLETE,IC_XUSB_TRACE_FORMAT,s,0);
#endif
        WdfWaitLockRelease(gGate);WdfRequestComplete(r,s);return;
    }
    if(!d->ready || !f || !f->linked)s=STATUS_DEVICE_NOT_READY;
    else if(code==IC_XUSB_GET_STATE || (unfiltered && in[3]==1)) {
        ULONG bytes;s=CopyState(r,d->delivered,&bytes);WdfWaitLockRelease(gGate);
#if defined(IC_COMPAT_TRACE)
        XUSB_TRACE(d,r,IC_XUSB_TRACE_COMPLETE,IC_XUSB_TRACE_CACHE,s,bytes);
#endif
        WdfRequestCompleteWithInformation(r,s,bytes);return;
    }
    else {
#if defined(IC_COMPAT_TRACE)
        XUSB_TRACE(d,r,IC_XUSB_TRACE_DISPATCH,IC_XUSB_TRACE_PENDING,STATUS_PENDING,0);
#endif
        s=WdfRequestForwardToIoQueue(r,f->waits);
    }
#if defined(IC_COMPAT_TRACE)
    if(!NT_SUCCESS(s))XUSB_TRACE(d,r,IC_XUSB_TRACE_COMPLETE,IC_XUSB_TRACE_NOT_READY,s,0);
#endif
    WdfWaitLockRelease(gGate);if(!NT_SUCCESS(s))WdfRequestComplete(r,s);
}
static void Default(WDFQUEUE q,WDFREQUEST r){Forward(r,WdfIoQueueGetDevice(q));}
static void Create(WDFQUEUE queue,WDFREQUEST r) {
    WDFDEVICE device=WdfIoQueueGetDevice(queue);XUSB_DEVICE* d=XusbDevice(device);
    XUSB_FILE* f=XusbFile(WdfRequestGetFileObject(r));WDF_IO_QUEUE_CONFIG q;WDF_OBJECT_ATTRIBUTES a;WDF_REQUEST_SEND_OPTIONS o;NTSTATUS s;
    f->file=WdfRequestGetFileObject(r);WdfWaitLockAcquire(d->stateLock,NULL);
    WDF_IO_QUEUE_CONFIG_INIT(&q,WdfIoQueueDispatchManual);q.PowerManaged=WdfFalse;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;s=WdfIoQueueCreate(device,&q,&a,&f->waits);
    if(NT_SUCCESS(s)) {
        WdfRequestFormatRequestUsingCurrentType(r);WDF_REQUEST_SEND_OPTIONS_INIT(&o,WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
        WdfRequestSend(r,WdfDeviceGetIoTarget(device),&o);s=WdfRequestGetStatus(r);
        if(NT_SUCCESS(s)){WdfWaitLockAcquire(gGate,NULL);InsertTailList(&d->files,&f->link);f->linked=TRUE;WdfWaitLockRelease(gGate);(void)StartLocked(d);(void)FailedStartBypassLocked(d);}
    }
    WdfWaitLockRelease(d->stateLock);WdfRequestComplete(r,s);
}
static void FileCleanup(WDFFILEOBJECT file) {
    XUSB_DEVICE* d=XusbDevice(WdfFileObjectGetDevice(file));XUSB_FILE* f=XusbFile(file);BOOLEAN source;WDFQUEUE waits;
    WdfWaitLockAcquire(d->stateLock,NULL);source=d->source==f;if(source)StopLocked(d);
    WdfWaitLockAcquire(gGate,NULL);if(f->linked){RemoveEntryList(&f->link);f->linked=FALSE;}waits=f->waits;f->waits=NULL;WdfWaitLockRelease(gGate);
    if(source){(void)StartLocked(d);(void)FailedStartBypassLocked(d);}WdfWaitLockRelease(d->stateLock);
    // An upper completion callback may close its own file. A synchronous purge
    // would wait for that callback to finish, potentially waiting on itself.
    // KMDF retains the queue until its already-dequeued requests complete.
    if(waits){WdfIoQueuePurge(waits,NULL,NULL);WdfObjectDelete(waits);}
}
static void FileDestroy(WDFOBJECT file){XUSB_FILE* f=XusbFile(file);if(f->waits)WdfObjectDelete(f->waits);}
static NTSTATUS Resume(WDFDEVICE device){XUSB_DEVICE* d=XusbDevice(device);NTSTATUS s;WdfWaitLockAcquire(d->stateLock,NULL);WdfWaitLockAcquire(gGate,NULL);IcTrialResumeLocked();d->powered=TRUE;WdfWaitLockRelease(gGate);s=StartLocked(d);if(FailedStartBypassLocked(d))s=STATUS_SUCCESS;WdfWaitLockRelease(d->stateLock);return s;}
static NTSTATUS Suspend(WDFDEVICE device){XUSB_DEVICE* d=XusbDevice(device);WdfWaitLockAcquire(d->stateLock,NULL);d->powered=FALSE;StopLocked(d);WdfWaitLockRelease(d->stateLock);WdfWorkItemFlush(d->completionWork);return STATUS_SUCCESS;}
static void ManagedCleanup(WDFDEVICE device){(void)Suspend(device);}
static NTSTATUS Pnp(WDFDEVICE device,PIRP irp) {
    // Stop private xusb22 WAIT before the lower stack observes surprise removal.
    (void)Suspend(device);IoSkipCurrentIrpStackLocation(irp);return WdfDeviceWdmDispatchPreprocessedIrp(device,irp);
}
static void Cleanup(WDFOBJECT object) {
    XUSB_DEVICE* d=XusbDevice(object);WdfWaitLockAcquire(gGate,NULL);
    if(d->registered){RemoveEntryList(&d->link);d->registered=FALSE;}WdfWaitLockRelease(gGate);
    if(d->counted)IcFdoRemoved();
}
NTSTATUS IcXusbAdd(PWDFDEVICE_INIT init) {
    WDFDEVICE device;WDF_OBJECT_ATTRIBUTES a;WDF_PNPPOWER_EVENT_CALLBACKS p;WDF_FILEOBJECT_CONFIG f;WDF_IO_QUEUE_CONFIG q;
    WDFQUEUE creates;WDF_WORKITEM_CONFIG work;XUSB_DEVICE* d;NTSTATUS s;UCHAR minor=IRP_MN_SURPRISE_REMOVAL;ULONG bytes;
    WDF_DEVICE_PROPERTY_DATA property;DEVPROPTYPE propertyType;
    const GUID guid=IC_XUSB_INTERFACE_GUID;
    WdfFdoInitSetFilter(init);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,XUSB_REQUEST);WdfDeviceInitSetRequestAttributes(init,&a);
    WdfDeviceInitSetIoInCallerContextCallback(init,CallerContext);
    s=WdfDeviceInitAssignWdmIrpPreprocessCallback(init,Pnp,IRP_MJ_PNP,&minor,1);if(!NT_SUCCESS(s))return s;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);p.EvtDeviceSelfManagedIoInit=Resume;p.EvtDeviceSelfManagedIoRestart=Resume;p.EvtDeviceSelfManagedIoSuspend=Suspend;p.EvtDeviceSelfManagedIoCleanup=ManagedCleanup;WdfDeviceInitSetPnpPowerEventCallbacks(init,&p);
    WDF_FILEOBJECT_CONFIG_INIT(&f,WDF_NO_EVENT_CALLBACK,WDF_NO_EVENT_CALLBACK,FileCleanup);f.AutoForwardCleanupClose=WdfTrue;f.FileObjectClass=WdfFileObjectWdfCannotUseFsContexts;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,XUSB_FILE);a.ExecutionLevel=WdfExecutionLevelPassive;a.SynchronizationScope=WdfSynchronizationScopeNone;a.EvtDestroyCallback=FileDestroy;WdfDeviceInitSetFileObjectConfig(init,&f,&a);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,XUSB_DEVICE);a.ExecutionLevel=WdfExecutionLevelPassive;a.SynchronizationScope=WdfSynchronizationScopeNone;a.EvtCleanupCallback=Cleanup;
    s=WdfDeviceCreate(&init,&a,&device);if(!NT_SUCCESS(s))return s;
    d=XusbDevice(device);d->device=device;InitializeListHead(&d->files);InitializeListHead(&d->completions);KeInitializeEvent(&d->stop,NotificationEvent,FALSE);KeInitializeEvent(&d->done,SynchronizationEvent,FALSE);KeInitializeEvent(&d->started,NotificationEvent,FALSE);
#if defined(IC_COMPAT_NATIVE_XUSB)
    // Never transition back: native lower WAITs may remain pending while open.
    // CaptureReady rejects bypass FDOs and StartLocked creates no private pump.
    d->bypass=TRUE;
#endif
#if defined(IC_COMPAT_TRACE)
    KeInitializeSpinLock(&d->traceLock);
#endif
    WDF_DEVICE_PROPERTY_DATA_INIT(&property,&DEVPKEY_Device_InstanceId);
    s=WdfDeviceQueryPropertyEx(device,&property,sizeof(d->instanceBuffer)-sizeof(WCHAR),d->instanceBuffer,&bytes,&propertyType);
    if(NT_SUCCESS(s) && propertyType==DEVPROP_TYPE_STRING && bytes>=sizeof(WCHAR) && !(bytes%sizeof(WCHAR)))
        RtlInitUnicodeString(&d->instance,d->instanceBuffer);
    s=IcFdoAdded();if(!NT_SUCCESS(s))return s;d->counted=TRUE;
    d->info.type=IC_TYPE_CONTROLLER_XUSB;d->info.instance_token=(ULONG)InterlockedIncrement(&nextId);d->info.input_bytes=IC_XUSB_STATE_BYTES;
    if(!IcXusbProduct((const uint16_t*)d->instance.Buffer,d->instance.Length/sizeof(WCHAR),1,&d->info.vid,&d->info.pid))return STATUS_DEVICE_CONFIGURATION_ERROR;
    if(NT_SUCCESS(WdfDeviceQueryProperty(device,DevicePropertyHardwareID,sizeof(d->info.hardware_id)-2,d->info.hardware_id,&bytes)))d->info.flags|=IC_INFO_HARDWARE_ID_VALID;
    if(NT_SUCCESS(WdfDeviceQueryProperty(device,DevicePropertyDeviceDescription,sizeof(d->info.name)-2,d->info.name,&bytes)))d->info.flags|=IC_INFO_NAME_VALID;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;s=WdfWaitLockCreate(&a,&d->stateLock);if(!NT_SUCCESS(s))return s;
    WDF_WORKITEM_CONFIG_INIT(&work,CompleteWaits);work.AutomaticSerialization=FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,XUSB_WORK);a.ParentObject=device;
    s=WdfWorkItemCreate(&work,&a,&d->completionWork);if(!NT_SUCCESS(s))return s;
    XusbWork(d->completionWork)->device=d;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;
    s=WdfRequestCreate(&a,WdfDeviceGetIoTarget(device),&d->request);if(!NT_SUCCESS(s))return s;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=d->request;s=WdfMemoryCreatePreallocated(&a,d->input,sizeof(d->input),&d->inputMemory);if(!NT_SUCCESS(s))return s;
    s=WdfMemoryCreatePreallocated(&a,d->raw,sizeof(d->raw),&d->outputMemory);if(!NT_SUCCESS(s))return s;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q,WdfIoQueueDispatchParallel);q.PowerManaged=WdfFalse;q.EvtIoDeviceControl=Ioctl;q.EvtIoDefault=Default;
    s=WdfIoQueueCreate(device,&q,WDF_NO_OBJECT_ATTRIBUTES,WDF_NO_HANDLE);if(!NT_SUCCESS(s))return s;
    WDF_IO_QUEUE_CONFIG_INIT(&q,WdfIoQueueDispatchSequential);q.PowerManaged=WdfFalse;q.EvtIoDefault=Create;
    s=WdfIoQueueCreate(device,&q,WDF_NO_OBJECT_ATTRIBUTES,&creates);if(!NT_SUCCESS(s))return s;
    s=WdfDeviceConfigureRequestDispatching(device,creates,WdfRequestTypeCreate);if(!NT_SUCCESS(s))return s;
    s=WdfDeviceCreateDeviceInterface(device,&guid,NULL);if(!NT_SUCCESS(s))return s;
    WdfWaitLockAcquire(gGate,NULL);InsertTailList(&devices,&d->link);d->registered=TRUE;WdfWaitLockRelease(gGate);return STATUS_SUCCESS;
}
