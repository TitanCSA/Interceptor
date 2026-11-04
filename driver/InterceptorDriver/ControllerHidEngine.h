#include "ControllerHid.h"
#include "HidInputBuffers.h"
#include "ControllerMonitor.h"
#include "../Shared/HidCapture.h"

// The XInputHID instance uses the same collection engine below the Microsoft
// translator. Separate statics give each backend its own queue, IDs and owner.
// Transport-independent fixes therefore apply to both backends.
#ifndef IC_HID_BACKEND_TYPE
#define IC_HID_BACKEND_TYPE IC_TYPE_CONTROLLER_HID
#define IC_HID_BACKEND_GUID IC_HID_INTERFACE_GUID
#define IC_HID_BACKEND_SOURCE IOCTL_IC_HID_SOURCE
#endif

// Collection upper filter. One physical reader feeds all application files.
// Lock order: device stateLock -> gGate. Never join a reader under gGate.
typedef struct HID_DEVICE HID_DEVICE;
typedef struct HID_WORK {HID_DEVICE* device;} HID_WORK;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HID_WORK,HidWork)
typedef struct HID_COMPLETION {
    LIST_ENTRY link;
    WDFREQUEST request;
    NTSTATUS status;
    ULONG bytes;
    BOOLEAN forward;
    PVOID monitorReport;
} HID_COMPLETION;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HID_COMPLETION,HidCompletion)
typedef struct HID_FILE {
    LIST_ENTRY link;
    WDFFILEOBJECT file;
    WDFQUEUE reads;
    PUCHAR fifo;
    ULONG head,count,capacity,reportBytes;
    BOOLEAN linked,source,monitorSource;
} HID_FILE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HID_FILE,HidFile)
struct HID_DEVICE {
    WDFDEVICE device;
    WDFWAITLOCK stateLock;
    LIST_ENTRY link,files,completions;
    WDFWORKITEM completionWork;
    BOOLEAN completionScheduled;
    BOOLEAN registered,prepared,powered,used,counted,retired;
    ULONG id,captureId,descriptorBytes;
    IC_DEVICE_INFO info;
    USHORT instance[256];
    PHIDP_PREPARSED_DATA descriptor;
    HIDP_CAPS caps;
    // Report-ID caches only allocate slots actually declared in the descriptor.
    USHORT slots[256],slotCount;
    BOOLEAN physicalValid[256],deliveredValid[256];
    PUCHAR physical,delivered,report;
    HID_FILE* readerFile;
    WDFREQUEST request;
    WDFMEMORY memory;
    PETHREAD thread;
    KEVENT stop,done;
    NTSTATUS readStatus;
    ULONG_PTR readBytes;
    // Once lower READs are admitted, retain transparent mode for this FDO.
    // Older open files cannot safely be adopted by a later shared reader.
    BOOLEAN bypass,latchedBypass;
    // A kernel-owned open keeps passive observation independent of user-file
    // cleanup. Retain it while an attached capture still borrows its reader.
    WDFWORKITEM monitorWork;
    HANDLE monitorHandle;
    PETHREAD monitorOpeningThread;
    BOOLEAN monitorScheduled,monitorTransition,monitorQuiescing,monitorUncaptured,monitorOpening;
    ULONG monitorRevision;
    NTSTATUS monitorStatus;
#ifdef IC_HID_PRIVATE_MIRROR
    WCHAR mirrorParent[IC_XUSB_MIRROR_INSTANCE_CHARS];
    BOOLEAN mirrorProfile;
#endif
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HID_DEVICE,HidDevice)
static LIST_ENTRY devices;
static LONG nextId;
#ifndef IC_HID_PRIVATE_MIRROR
static PKEVENT readyEvent;
static IC_CONTROLLER_HID_INPUT* captured;
static HID_DEVICE* captureTargets[IC_MAX_DEVICES];
static ULONG captureDevices;
static uint32_t captureHead,captureCount;
#endif
static const GUID interfaceGuid=IC_HID_BACKEND_GUID;

static NTSTATUS StartLocked(HID_DEVICE* d);
static void StopLocked(HID_DEVICE* d);
static void Forward(WDFREQUEST request,WDFDEVICE device);
static void MonitorScheduleLocked(HID_DEVICE* d);
#ifdef IC_HID_PRIVATE_MIRROR
static BOOLEAN MirrorCapturePresent(void);
static void MirrorPrepare(HID_DEVICE* d);
static void MirrorPhysical(HID_DEVICE* d);
static void MirrorSeed(HID_DEVICE* d);
#define HID_CAPTURE_PRESENT() MirrorCapturePresent()
#else
#define HID_CAPTURE_PRESENT() (captured!=NULL)
#endif
static void ClearFiles(HID_DEVICE* d) {
    PLIST_ENTRY p;
    for(p=d->files.Flink;p!=&d->files;p=p->Flink) {
        HID_FILE* f=CONTAINING_RECORD(p,HID_FILE,link);f->head=f->count=0;
    }
}
static void CompleteReads(WDFWORKITEM work) {
    HID_DEVICE* d=HidWork(work)->device;
    for(;;) {
        HID_COMPLETION* pending;
        WdfWaitLockAcquire(gGate,NULL);
        if(IsListEmpty(&d->completions)) {
            d->completionScheduled=FALSE;WdfWaitLockRelease(gGate);return;
        }
        pending=CONTAINING_RECORD(RemoveHeadList(&d->completions),HID_COMPLETION,link);
        WdfWaitLockRelease(gGate);
        // An upper kernel client can synchronously resubmit from its completion
        // callback (xinputhid does). Never invoke that callback under gGate.
        if(pending->forward)Forward(pending->request,d->device);
        else {
            if(NT_SUCCESS(pending->status) && pending->bytes && pending->monitorReport)
                IcMonitorPublish(IC_HID_BACKEND_TYPE,d->id,IC_MONITOR_OUT,IC_MONITOR_RAW,
                    pending->monitorReport,pending->bytes,NULL,STATUS_SUCCESS);
            WdfRequestCompleteWithInformation(pending->request,pending->status,pending->bytes);
        }
    }
}
static BOOLEAN CopyRead(HID_DEVICE* d,WDFREQUEST request,const UCHAR* report,ULONG size) {
    HID_COMPLETION* pending=HidCompletion(request);
    PVOID output;size_t length;NTSTATUS status=WdfRequestRetrieveOutputBuffer(request,size,&output,&length);
    if(NT_SUCCESS(status))RtlCopyMemory(output,report,size);
    pending->request=request;pending->status=status;pending->bytes=NT_SUCCESS(status)?size:0;
    pending->forward=FALSE;
    pending->monitorReport=NT_SUCCESS(status)?output:NULL;
    InsertTailList(&d->completions,&pending->link);
    if(!d->completionScheduled) {d->completionScheduled=TRUE;WdfWorkItemEnqueue(d->completionWork);}
    return NT_SUCCESS(status);
}
static void EnterBypass(HID_DEVICE* d) {
    PLIST_ENTRY p;
    // Caller holds stateLock and has no running physical reader. Once ordinary
    // lower READs have been admitted, remain transparent for this FDO lifetime.
    // A later rebalance cannot turn old lower files into managed reader files.
    WdfWaitLockAcquire(gGate,NULL);
    if(d->captureId)IcRevokeAllLocked(IC_RESET_DEVICE);
    d->latchedBypass=TRUE;d->bypass=TRUE;d->readerFile=NULL;
    ClearFiles(d);
    for(p=d->files.Flink;p!=&d->files;p=p->Flink) {
        HID_FILE* f=CONTAINING_RECORD(p,HID_FILE,link);WDFREQUEST request;
        if(!f->reads)continue;
        while(NT_SUCCESS(WdfIoQueueRetrieveNextRequest(f->reads,&request))) {
            HID_COMPLETION* pending=HidCompletion(request);
            pending->request=request;pending->forward=TRUE;pending->status=STATUS_SUCCESS;pending->bytes=0;pending->monitorReport=NULL;
            InsertTailList(&d->completions,&pending->link);
            if(!d->completionScheduled) {d->completionScheduled=TRUE;WdfWorkItemEnqueue(d->completionWork);}
        }
    }
    WdfWaitLockRelease(gGate);
}
static void Broadcast(HID_DEVICE* d,const UCHAR* report) {
    PLIST_ENTRY p;ULONG size=d->caps.InputReportByteLength;UCHAR id=report[0];
    if(d->slots[id]) {
        RtlCopyMemory(d->delivered+(SIZE_T)(d->slots[id]-1)*size,report,size);
        d->deliveredValid[id]=TRUE;
    }
    for(p=d->files.Flink;p!=&d->files;p=p->Flink) {
        HID_FILE* f=CONTAINING_RECORD(p,HID_FILE,link);WDFREQUEST request;BOOLEAN sent=FALSE;ULONG tail;
        if(f->source || f->reportBytes!=size)continue;
        tail=f->head;
        // Read only queues a pending request when this FIFO is empty, under
        // the same gate. A backlogged reader therefore has nothing to retrieve.
        if(!f->count) {
            while(NT_SUCCESS(WdfIoQueueRetrieveNextRequest(f->reads,&request))) {
                if(CopyRead(d,request,report,size)) {sent=TRUE;break;}
            }
            if(sent)continue;
        }
        // Like HIDClass, a slow reader has a bounded private queue. Drop oldest.
        else if(f->count==f->capacity) {if(++f->head==f->capacity)f->head=0;--f->count;}
        // head and count are each bounded by capacity, including when the
        // caller selected a non-power-of-two HIDClass input-buffer count.
        else {tail+=f->count;if(tail>=f->capacity)tail-=f->capacity;}
        RtlCopyMemory(f->fifo+(SIZE_T)tail*size,report,size);++f->count;
    }
}
void IcHidInitialize(void) {InitializeListHead(&devices);}
#ifndef IC_HID_PRIVATE_MIRROR
static BOOLEAN CaptureReady(HID_DEVICE* d) {
    return !d->retired && !d->bypass && !d->monitorTransition && !d->monitorQuiescing &&
        d->prepared && d->powered && d->thread && !KeReadStateEvent(&d->stop);
}
#endif
#ifndef IC_HID_PRIVATE_MIRROR
NTSTATUS IcHidQueryLocked(ULONG token,IC_DEVICE_QUERY_RESULT* result,BOOLEAN ordinal) {
    PLIST_ENTRY p;ULONG position=0;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        HID_DEVICE* d=CONTAINING_RECORD(p,HID_DEVICE,link);
        if(d->retired)continue;
        if((ordinal?++position:d->id)!=token)continue;
        result->instance_token=d->id;
        result->flags=(CaptureReady(d)?1u:0u)|
            (d->thread && !KeReadStateEvent(&d->stop)?2u:0u)|(d->powered?4u:0u)|(d->prepared?16u:0u)|(d->bypass?64u:0u)|
            (d->monitorHandle && d->thread && !d->monitorTransition && !d->monitorQuiescing &&
             d->prepared && d->powered && !KeReadStateEvent(&d->stop)?IC_MONITOR_SOURCE_READY:0u);
        result->last_read_status=(ULONG)d->readStatus;result->report_bytes=d->prepared?d->info.input_bytes:0;
        RtlCopyMemory(result->instance,d->instance,sizeof(result->instance));
        if(result->instance[0])result->flags|=IC_QUERY_IDENTITY_VALID;
        return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_NOT_CONNECTED;
}
NTSTATUS IcHidMonitorDeviceLocked(ULONG token,IC_DEVICE_INFO* info,PVOID descriptor,
                                 SIZE_T capacity,ULONG* bytes,BOOLEAN preparsed) {
    PLIST_ENTRY p;*bytes=0;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        HID_DEVICE* d=CONTAINING_RECORD(p,HID_DEVICE,link);
        if(d->retired || d->id!=token)continue;
        if(!d->prepared) {
            // An unprepared collection is still enumerable. Do not read the
            // metadata being rebuilt outside gGate, or fail every other
            // controller's passive enumeration because one FDO is resuming.
            RtlZeroMemory(info,sizeof(*info));
            info->type=IC_HID_BACKEND_TYPE;info->id=info->instance_token=d->id;
            return preparsed?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS;
        }
        *info=d->info;info->type=IC_HID_BACKEND_TYPE;info->id=info->instance_token=d->id;
        *bytes=d->prepared?d->descriptorBytes:0;
        if(!preparsed)return STATUS_SUCCESS;
        if(!d->prepared || !d->descriptor || !*bytes)return STATUS_DEVICE_NOT_READY;
        if(capacity<*bytes)return STATUS_BUFFER_TOO_SMALL;
        RtlCopyMemory(descriptor,d->descriptor,*bytes);return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_NOT_CONNECTED;
}
void IcHidMonitorChangedLocked(void) {
    PLIST_ENTRY p;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        HID_DEVICE* d=CONTAINING_RECORD(p,HID_DEVICE,link);
        ++d->monitorRevision;
        if(d->monitorHandle || (!d->retired && IcMonitorEnabled()))MonitorScheduleLocked(d);
    }
}
ULONG IcHidCountLocked(void) {
    PLIST_ENTRY p;ULONG count=0;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        HID_DEVICE* d=CONTAINING_RECORD(p,HID_DEVICE,link);
        if(CaptureReady(d))++count;
    }
    return count;
}
NTSTATUS IcHidCaptureLocked(PKEVENT event,IC_DEVICE_INFO* output,ULONG capacity,ULONG* count) {
    PLIST_ENTRY p;ULONG n=IcHidCountLocked();
    if(!n)return STATUS_DEVICE_NOT_READY;
    if(n>capacity)return STATUS_BUFFER_TOO_SMALL;
    if(captured)return STATUS_SHARING_VIOLATION;
    captured=ExAllocatePoolZero(NonPagedPoolNx,sizeof(*captured)*IC_HID_CAPACITY,FILTER_TAG);
    if(!captured)return STATUS_INSUFFICIENT_RESOURCES;
    n=0;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        HID_DEVICE* d=CONTAINING_RECORD(p,HID_DEVICE,link);d->captureId=0;
        if(!CaptureReady(d))continue;
        d->monitorUncaptured=FALSE;
        d->captureId=++n;captureTargets[n-1]=d;output[n-1]=d->info;output[n-1].id=n;
        ClearFiles(d); // No queued pre-capture physical report may escape afterwards.
        RtlZeroMemory(d->deliveredValid,sizeof(d->deliveredValid));
    }
    captureDevices=n;captureHead=captureCount=0;readyEvent=event;KeClearEvent(event);*count=n;return STATUS_SUCCESS;
}
NTSTATUS IcHidArmLocked(PKEVENT event) {
    if(captured)return STATUS_SHARING_VIOLATION;
    captured=ExAllocatePoolZero(NonPagedPoolNx,sizeof(*captured)*IC_HID_CAPACITY,FILTER_TAG);
    if(!captured)return STATUS_INSUFFICIENT_RESOURCES;
    captureDevices=captureHead=captureCount=0;readyEvent=event;KeClearEvent(event);return STATUS_SUCCESS;
}
void IcHidRefreshCaptureLocked(void) {
    PLIST_ENTRY p;
    if(!captured || !IcAllCaptureLocked(IC_HID_BACKEND_TYPE))return;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        HID_DEVICE* d=CONTAINING_RECORD(p,HID_DEVICE,link);
        if(d->captureId || !CaptureReady(d))continue;
        if(!IcAllAttachLocked(IC_HID_BACKEND_TYPE))return;
        d->monitorUncaptured=FALSE;
        d->captureId=++captureDevices;captureTargets[captureDevices-1]=d;
        ClearFiles(d);RtlZeroMemory(d->deliveredValid,sizeof(d->deliveredValid));
    }
}
ULONG IcHidSnapshotLocked(IC_DEVICE_INFO* output,ULONG capacity) {
    ULONG i,n=min(captureDevices,capacity);
    for(i=0;i<n;++i){output[i]=captureTargets[i]->info;output[i].id=i+1;}
    return n;
}
void IcHidResetLocked(void) {
    PLIST_ENTRY p;
    if(!captured)return;
    ExFreePoolWithTag(captured,FILTER_TAG);captured=NULL;captureHead=captureCount=0;readyEvent=NULL;
    captureDevices=0;RtlZeroMemory(captureTargets,sizeof(captureTargets));
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        HID_DEVICE* d=CONTAINING_RECORD(p,HID_DEVICE,link);ULONG id;
        d->monitorUncaptured=FALSE;
        // An armed context may never have attached this collection. Its native
        // queues and delivered state belong to Windows, not to this capture.
        if(!d->captureId)continue;
        d->captureId=0;ClearFiles(d);
        if(d->retired || d->bypass)continue;
        // Restore the last physical state, including buttons held when the app died.
        for(id=0;id<256;++id)if(d->physicalValid[id])
            Broadcast(d,d->physical+(SIZE_T)(d->slots[id]-1)*d->caps.InputReportByteLength);
    }
    IcHidMonitorChangedLocked();
}
ULONG IcHidQueuedLocked(void) {return captureCount;}
NTSTATUS IcHidGetLocked(ULONG requested,PVOID output,SIZE_T length,SIZE_T* written) {
    LONG n=IcHidRead(&captureHead,&captureCount,captured,output,(ULONG)min(length/sizeof(*captured),IC_HID_CAPACITY),requested);
    if(n<0)return STATUS_BUFFER_TOO_SMALL;
    if(!captureCount && readyEvent)KeClearEvent(readyEvent);
    *written=(SIZE_T)n*sizeof(*captured);return STATUS_SUCCESS;
}
NTSTATUS IcHidGetSelectedLocked(const IC_GET_SELECT* request,PVOID output,SIZE_T length,SIZE_T* written) {
    const BOOLEAN consume=(request->flags&IC_GET_PEEK)==0;
    LONG n=IcInputReadSelected(&captureHead,&captureCount,captured,IC_HID_CAPACITY,sizeof(*captured),
        FIELD_OFFSET(IC_CONTROLLER_HID_INPUT,device),output,sizeof(*captured),
        (ULONG)min(length/sizeof(*captured),IC_HID_CAPACITY),request->base.count,request->device,consume);
    if(n<0)return STATUS_BUFFER_TOO_SMALL;
    if(consume && !captureCount && readyEvent)KeClearEvent(readyEvent);
    *written=(SIZE_T)n*sizeof(*captured);return STATUS_SUCCESS;
}
static HID_DEVICE* Find(ULONG id,BOOLEAN owned) {
    PLIST_ENTRY p;HID_DEVICE* first=NULL;ULONG ordinal=0;
    if(owned) {
        HID_DEVICE* d=captureDevices?captureTargets[id>=1 && id<=captureDevices?id-1:0]:NULL;
        return d && !d->retired && !d->bypass?d:NULL;
    }
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        HID_DEVICE* d=CONTAINING_RECORD(p,HID_DEVICE,link);
        if(d->retired || d->bypass || !d->prepared || !d->powered)continue;
        if(!first)first=d;
        if(id==++ordinal)return d;
    }
    return first;
}
NTSTATUS IcHidSendLocked(PVOID buffer,SIZE_T length,IC_SEND_RESULT* result,BOOLEAN owned) {
    IC_CONTROLLER_HID_INPUT* in=buffer;ULONG n=(ULONG)(length/sizeof(*in)),i;
    if(!length || length%sizeof(*in) || n>IC_MAX_SEND)return STATUS_INVALID_PARAMETER;
    // Validate the whole batch before delivering any report.
    for(i=0;i<n;++i) {
        HID_DEVICE* d=Find(in[i].device,owned);
        if(!IcHidValidReport(&in[i]))return STATUS_INVALID_PARAMETER;
        if(d && (in[i].length!=d->caps.InputReportByteLength || !d->slots[in[i].report[0]]))return STATUS_INVALID_PARAMETER;
    }
    result->accepted=0;result->status=STATUS_SUCCESS;
    for(i=0;i<n;++i) {
        HID_DEVICE* d=Find(in[i].device,owned);
        if(!d) {result->status=(ULONG)STATUS_DEVICE_NOT_CONNECTED;break;}
        Broadcast(d,in[i].report);++result->accepted;
    }
    return STATUS_SUCCESS;
}
NTSTATUS IcHidDescriptorLocked(ULONG id,PVOID buffer,SIZE_T length,SIZE_T* written) {
    HID_DEVICE* d=Find(id,TRUE);
    if(!d || d->captureId!=id)return STATUS_DEVICE_NOT_CONNECTED;
    if(length<d->descriptorBytes)return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(buffer,d->descriptor,d->descriptorBytes);*written=d->descriptorBytes;return STATUS_SUCCESS;
}
#endif
static void CompleteRead(WDFREQUEST request,WDFIOTARGET target,PWDF_REQUEST_COMPLETION_PARAMS params,WDFCONTEXT context) {
    HID_DEVICE* d=context;UNREFERENCED_PARAMETER(request);UNREFERENCED_PARAMETER(target);
    d->readStatus=params->IoStatus.Status;d->readBytes=params->IoStatus.Information;
    KeSetEvent(&d->done,IO_NO_INCREMENT,FALSE);
}
static NTSTATUS ReadPhysical(HID_DEVICE* d) {
    WDFIOTARGET target=WdfDeviceGetIoTarget(d->device);NTSTATUS status;
    PVOID waits[2]={&d->stop,&d->done};WDF_REQUEST_REUSE_PARAMS reuse;
    if(d->used) {
        WDF_REQUEST_REUSE_PARAMS_INIT(&reuse,WDF_REQUEST_REUSE_NO_FLAGS,STATUS_SUCCESS);
        status=WdfRequestReuse(d->request,&reuse);if(!NT_SUCCESS(status))return status;
    }
    d->used=TRUE;KeClearEvent(&d->done);
    status=WdfIoTargetFormatRequestForRead(target,d->request,d->memory,NULL,NULL);if(!NT_SUCCESS(status))return status;
    IoGetNextIrpStackLocation(WdfRequestWdmGetIrp(d->request))->FileObject=WdfFileObjectWdmGetFileObject(d->readerFile->file);
    WdfRequestSetCompletionRoutine(d->request,CompleteRead,d);
    if(!WdfRequestSend(d->request,target,NULL))return WdfRequestGetStatus(d->request);
    status=KeWaitForMultipleObjects(2,waits,WaitAny,Executive,KernelMode,FALSE,NULL,NULL);
    if(status==STATUS_WAIT_0) {
        WdfRequestCancelSentRequest(d->request);
        KeWaitForSingleObject(&d->done,Executive,KernelMode,FALSE,NULL);return STATUS_CANCELLED;
    }
    return d->readStatus;
}
static void Reader(PVOID context) {
    HID_DEVICE* d=context;ULONG retry=10;
    while(!KeReadStateEvent(&d->stop)) {
        NTSTATUS status=ReadPhysical(d);
        if(KeReadStateEvent(&d->stop))break;
        if(!NT_SUCCESS(status) || d->readBytes!=d->caps.InputReportByteLength || !d->slots[d->report[0]]) {
            LARGE_INTEGER due;due.QuadPart=-(LONGLONG)retry*10000;retry=min(retry*2,250u);
            WdfWaitLockAcquire(gGate,NULL);
            // Stop can remove this reader from the capture snapshot while we
            // acquire gGate. Never revoke a newer owner's remaining devices.
            if(KeReadStateEvent(&d->stop) || d->retired){WdfWaitLockRelease(gGate);break;}
            IcMonitorPublish(IC_HID_BACKEND_TYPE,d->id,IC_MONITOR_IN,
                IC_MONITOR_RAW|IC_MONITOR_ERROR,d->report,
                NT_SUCCESS(status)?(ULONG)min(d->readBytes,d->caps.InputReportByteLength):0,
                NULL,NT_SUCCESS(status)?STATUS_NOT_SUPPORTED:status);
            if(HID_CAPTURE_PRESENT() && d->captureId)IcRevokeAllLocked(IC_RESET_DEVICE);
            WdfWaitLockRelease(gGate);
            KeWaitForSingleObject(&d->stop,Executive,KernelMode,FALSE,&due);continue;
        }
        retry=10;WdfWaitLockAcquire(gGate,NULL);
        if(KeReadStateEvent(&d->stop) || d->retired){WdfWaitLockRelease(gGate);break;}
        IcMonitorPublish(IC_HID_BACKEND_TYPE,d->id,IC_MONITOR_IN,IC_MONITOR_RAW,
            d->report,d->caps.InputReportByteLength,NULL,STATUS_SUCCESS);
        RtlCopyMemory(d->physical+(SIZE_T)(d->slots[d->report[0]]-1)*d->caps.InputReportByteLength,d->report,d->caps.InputReportByteLength);
        d->physicalValid[d->report[0]]=TRUE;
#ifndef IC_HID_PRIVATE_MIRROR
        // CREATE_ALL can arm other families while this reader is undergoing a
        // monitor-open/close transition. It is not yet part of that snapshot;
        // keep its ordinary passthrough until the ready callback attaches it.
        if(captured && !d->captureId && !IcAllCaptureLocked(IC_HID_BACKEND_TYPE) &&
           (d->monitorTransition || d->monitorHandle))d->monitorUncaptured=TRUE;
        if(captured && !d->captureId && (d->monitorUncaptured ||
           ((d->monitorTransition || d->monitorQuiescing) && IcAllCaptureLocked(IC_HID_BACKEND_TYPE)))) {
            Broadcast(d,d->report);WdfWaitLockRelease(gGate);continue;
        }
#endif
#ifdef IC_HID_PRIVATE_MIRROR
        // The native XUSB reader owns capture ordering and the public FIFO.
        // Retain physical HID solely for restoration; never leak it to legacy
        // readers while this exact parent is captured.
        MirrorPhysical(d);
#else
        if(captured && (!d->captureId || captureCount==IC_HID_CAPACITY))IcRevokeAllLocked(captureCount==IC_HID_CAPACITY?IC_RESET_OVERFLOW:IC_RESET_DEVICE);
        if(captured) {
            IC_CONTROLLER_HID_INPUT* out=captured+(captureHead+captureCount)%IC_HID_CAPACITY;
            IcHidWriteCapture(out,IcNextSequenceLocked(),KeQueryPerformanceCounter(NULL).QuadPart,
                d->captureId,d->report,d->caps.InputReportByteLength);
            if(++captureCount==1)KeSetEvent(readyEvent,IO_NO_INCREMENT,FALSE);
        } else Broadcast(d,d->report);
#endif
        WdfWaitLockRelease(gGate);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}
static NTSTATUS FlushSource(HID_DEVICE* d) {
    WDFREQUEST request;WDFIOTARGET target=WdfDeviceGetIoTarget(d->device);
    WDF_REQUEST_SEND_OPTIONS options;NTSTATUS status=WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES,target,&request);
    if(!NT_SUCCESS(status))return status;
    status=WdfIoTargetFormatRequestForIoctl(target,request,IOCTL_HID_FLUSH_QUEUE,NULL,NULL,NULL,NULL);
    if(NT_SUCCESS(status)) {
        IoGetNextIrpStackLocation(WdfRequestWdmGetIrp(request))->FileObject=WdfFileObjectWdmGetFileObject(d->readerFile->file);
        WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SYNCHRONOUS|WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,WDF_REL_TIMEOUT_IN_SEC(2));
        WdfRequestSend(request,target,&options);status=WdfRequestGetStatus(request);
    }
    WdfObjectDelete(request);return status;
}
static NTSTATUS StartLocked(HID_DEVICE* d) {
    NTSTATUS status;HANDLE handle=NULL;OBJECT_ATTRIBUTES attributes;
    if(d->bypass)return STATUS_SUCCESS;
    if(d->thread || !d->prepared || !d->powered)return STATUS_SUCCESS;
    d->readerFile=NULL;
    if(!IsListEmpty(&d->files))d->readerFile=CONTAINING_RECORD(d->files.Flink,HID_FILE,link);
    if(!d->readerFile)return STATUS_SUCCESS;
    // Reserve the exact borrowed HIDClass open before its shared reader runs.
    // Application queue sizes below do not enlarge this physical input queue.
    // A failed optional reserve must preserve the functioning original open.
    (void)FilterReserveInputBuffers(d->device,WdfFileObjectWdmGetFileObject(d->readerFile->file));
    // A promoted application file has an unused lower HIDClass queue. Discard
    // its historical physical reports before using it as the new shared reader.
    status=FlushSource(d);if(!NT_SUCCESS(status))return status;
    // Hold gGate through publication: Create_Context never sees a half-started device.
    WdfWaitLockAcquire(gGate,NULL);
    // Surprise removal does not acquire stateLock and can race FlushSource.
    // An old file promotion must not start a retired reader or revoke a newer
    // capture belonging to other parents.
    if(d->retired) {d->readerFile=NULL;WdfWaitLockRelease(gGate);return STATUS_SUCCESS;}
    KeClearEvent(&d->stop);
    if(HID_CAPTURE_PRESENT() && !IcAllCaptureLocked(IC_HID_BACKEND_TYPE)) {
        if(d->monitorTransition || d->monitorUncaptured)d->monitorUncaptured=TRUE;
        else IcRevokeAllLocked(IC_RESET_DEVICE);
    }
    InitializeObjectAttributes(&attributes,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
    status=PsCreateSystemThread(&handle,THREAD_ALL_ACCESS,&attributes,NULL,NULL,Reader,d);
    if(NT_SUCCESS(status)) {
        status=ObReferenceObjectByHandle(handle,SYNCHRONIZE,*PsThreadType,KernelMode,(PVOID*)&d->thread,NULL);
        if(!NT_SUCCESS(status))KeSetEvent(&d->stop,IO_NO_INCREMENT,FALSE);
    }
#ifdef IC_HID_PRIVATE_MIRROR
    if(NT_SUCCESS(status))MirrorSeed(d);
#endif
    if(NT_SUCCESS(status))IcAllReadersReadyLocked();
    WdfWaitLockRelease(gGate);
    if(NT_SUCCESS(status))ZwClose(handle);
    else if(handle) {ZwWaitForSingleObject(handle,FALSE,NULL);ZwClose(handle);}
    return status;
}
static void StopLocked(HID_DEVICE* d) {
    PETHREAD thread;
    WdfWaitLockAcquire(gGate,NULL);
    thread=d->thread;d->thread=NULL;
    if(thread && HID_CAPTURE_PRESENT() && d->captureId)IcRevokeAllLocked(IC_RESET_DEVICE);
    KeSetEvent(&d->stop,IO_NO_INCREMENT,FALSE);WdfWaitLockRelease(gGate);
    if(thread) {KeWaitForSingleObject(thread,Executive,KernelMode,FALSE,NULL);ObDereferenceObject(thread);}
    d->readerFile=NULL;
}
static NTSTATUS MonitorOpen(HID_DEVICE* d,HANDLE* result) {
    WDFSTRING path=NULL;UNICODE_STRING name;OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;HANDLE handle=NULL;PFILE_OBJECT object=NULL;NTSTATUS status;
    *result=NULL;
    status=WdfStringCreate(NULL,WDF_NO_OBJECT_ATTRIBUTES,&path);
    if(!NT_SUCCESS(status))return status;
    status=WdfDeviceRetrieveDeviceInterfaceString(d->device,&interfaceGuid,NULL,path);
    if(NT_SUCCESS(status)) {
        WdfStringGetUnicodeString(path,&name);
        InitializeObjectAttributes(&attributes,&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
        WdfWaitLockAcquire(gGate,NULL);
        d->monitorOpening=TRUE;d->monitorOpeningThread=PsGetCurrentThread();
        WdfWaitLockRelease(gGate);
        status=ZwCreateFile(&handle,GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,&attributes,&io,NULL,0,
            FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN,FILE_NON_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT,NULL,0);
        WdfWaitLockAcquire(gGate,NULL);
        d->monitorOpening=FALSE;d->monitorOpeningThread=NULL;
        WdfWaitLockRelease(gGate);
    }
    WdfObjectDelete(path);
    if(NT_SUCCESS(status))status=ObReferenceObjectByHandle(handle,0,*IoFileObjectType,KernelMode,(PVOID*)&object,NULL);
    if(NT_SUCCESS(status)) {
        WDFFILEOBJECT file=WdfDeviceGetFileObject(d->device,object);HID_FILE* source=file?HidFile(file):NULL;
        WdfWaitLockAcquire(gGate,NULL);
        // Mark only our private open as a producer, without sending a
        // power-managed IOCTL to this FDO from its power/lifetime work item.
        if(d->retired || d->bypass || !d->prepared || !d->thread || !source || !source->linked)
            status=STATUS_DEVICE_NOT_READY;
        else {source->source=source->monitorSource=TRUE;source->head=source->count=0;}
        WdfWaitLockRelease(gGate);
        ObDereferenceObject(object);
    }
    if(!NT_SUCCESS(status)) {if(handle)ZwClose(handle);return status;}
    *result=handle;return STATUS_SUCCESS;
}
static void MonitorSource(WDFWORKITEM work) {
    HID_DEVICE* d=HidWork(work)->device;
    for(;;) {
        HANDLE handle;BOOLEAN opening,usable,needed;ULONG revision;NTSTATUS status=STATUS_SUCCESS;
        WdfWaitLockAcquire(gGate,NULL);
        usable=!d->retired && !d->monitorQuiescing && !d->bypass && d->prepared && d->powered;
        handle=d->monitorHandle;opening=handle==NULL;
        // Keep the private file before all debug-consumer files across D0 and
        // resource rebalance. Reopening it behind a surviving consumer would
        // let that user's cleanup revoke a later foreign capture.
        needed=handle?!d->retired && !d->latchedBypass &&
            (IcMonitorEnabled() || (HID_CAPTURE_PRESENT() && d->captureId)):
            usable && IcMonitorEnabled();
        if((handle && needed) || (!handle && !needed)) {
            if(handle && d->thread && !d->retired && d->powered)
                IcMonitorPublish(IC_HID_BACKEND_TYPE,d->id,IC_MONITOR_DEVICE,0,NULL,0,NULL,STATUS_SUCCESS);
            d->monitorScheduled=FALSE;WdfWaitLockRelease(gGate);return;
        }
        // Exclude this source from a newly starting capture across CREATE and
        // CLEANUP. An existing owner keeps the private open until it releases.
        d->monitorTransition=!opening || !d->thread;
        revision=d->monitorRevision;
        if(!opening)d->monitorHandle=NULL;
        WdfWaitLockRelease(gGate);
        if(opening)status=MonitorOpen(d,&handle);
        else ZwClose(handle);
        WdfWaitLockAcquire(gGate,NULL);
        if(opening && NT_SUCCESS(status))d->monitorHandle=handle;
        d->monitorStatus=status;d->monitorTransition=FALSE;
        if(!d->retired)IcAllReadersReadyLocked();
        if(!NT_SUCCESS(status)) {
            IcMonitorPublish(IC_HID_BACKEND_TYPE,d->id,IC_MONITOR_DEVICE,IC_MONITOR_ERROR,
                NULL,0,NULL,status);
            if(revision==d->monitorRevision) {
                d->monitorScheduled=FALSE;WdfWaitLockRelease(gGate);return;
            }
        }
        WdfWaitLockRelease(gGate);
    }
}
static void MonitorScheduleLocked(HID_DEVICE* d) {
    if(d->monitorWork && !d->monitorScheduled) {
        d->monitorScheduled=TRUE;WdfWorkItemEnqueue(d->monitorWork);
    }
}
static void Forward(WDFREQUEST request,WDFDEVICE device) {
    WDF_REQUEST_SEND_OPTIONS options;
    WdfRequestFormatRequestUsingCurrentType(request);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    if(!WdfRequestSend(request,WdfDeviceGetIoTarget(device),&options))WdfRequestComplete(request,WdfRequestGetStatus(request));
}
static void Read(WDFQUEUE queue,WDFREQUEST request,size_t length) {
    HID_DEVICE* d=HidDevice(WdfIoQueueGetDevice(queue));WDFFILEOBJECT file=WdfRequestGetFileObject(request);
    HID_FILE* f=file?HidFile(file):NULL;NTSTATUS status=STATUS_SUCCESS;
    WdfWaitLockAcquire(gGate,NULL);
    if(d->retired) {WdfWaitLockRelease(gGate);WdfRequestComplete(request,STATUS_DEVICE_NOT_CONNECTED);return;}
    if(d->bypass) {WdfWaitLockRelease(gGate);Forward(request,d->device);return;}
    if(!d->prepared || !d->thread || !f || !f->linked || f->source)status=STATUS_INVALID_DEVICE_STATE;
    else if(f->reportBytes!=d->caps.InputReportByteLength)status=STATUS_DEVICE_NOT_READY;
    else if(length<d->caps.InputReportByteLength)status=STATUS_BUFFER_TOO_SMALL;
    else if(f->count) {
        CopyRead(d,request,f->fifo+(SIZE_T)f->head*d->caps.InputReportByteLength,d->caps.InputReportByteLength);
        if(++f->head==f->capacity)f->head=0;--f->count;WdfWaitLockRelease(gGate);return;
    } else status=WdfRequestForwardToIoQueue(request,f->reads);
    WdfWaitLockRelease(gGate);
    if(!NT_SUCCESS(status))WdfRequestComplete(request,status);
}
static NTSTATUS QueryInputReport(HID_DEVICE* d,WDFREQUEST original,UCHAR id,PUCHAR report,ULONG size,ULONG* bytes) {
    WDFREQUEST request;WDFMEMORY memory;WDF_OBJECT_ATTRIBUTES attributes;
    WDF_REQUEST_SEND_OPTIONS options;WDFIOTARGET target=WdfDeviceGetIoTarget(d->device);
    PIO_STACK_LOCATION originalStack=IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(original));
    NTSTATUS status;*bytes=0;report[0]=id;
    // Only the private kernel buffer is submitted. A new capture can begin
    // while the lower driver is answering; its physical data must not already
    // be visible through the original user's direct-I/O buffer.
    status=WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES,target,&request);
    if(!NT_SUCCESS(status))return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);attributes.ParentObject=request;
    status=WdfMemoryCreatePreallocated(&attributes,report,size,&memory);
    if(NT_SUCCESS(status)) {
        if(originalStack->MajorFunction==IRP_MJ_INTERNAL_DEVICE_CONTROL)
            status=WdfIoTargetFormatRequestForInternalIoctl(target,request,IOCTL_HID_GET_INPUT_REPORT,NULL,NULL,memory,NULL);
        else
            status=WdfIoTargetFormatRequestForIoctl(target,request,IOCTL_HID_GET_INPUT_REPORT,NULL,NULL,memory,NULL);
    }
    if(NT_SUCCESS(status)) {
        IoGetNextIrpStackLocation(WdfRequestWdmGetIrp(request))->FileObject=originalStack->FileObject;
        WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SYNCHRONOUS|WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,WDF_REL_TIMEOUT_IN_SEC(2));
        WdfRequestSend(request,target,&options);status=WdfRequestGetStatus(request);
        if(NT_SUCCESS(status)) {
            // HIDClass completes this collection IOCTL with Information=0;
            // some virtual/native collections instead return the report size.
            // Neither convention changes the caller's full, private buffer.
            if((WdfRequestGetInformation(request)!=0 && WdfRequestGetInformation(request)!=size) ||
               report[0]!=id)status=STATUS_INVALID_BUFFER_SIZE;
            else *bytes=size;
        }
    }
    WdfObjectDelete(request);return status;
}
static void GetUncachedInputReport(HID_DEVICE* d,WDFREQUEST request,UCHAR id) {
    NTSTATUS status=STATUS_DEVICE_NOT_READY;PVOID output=NULL;PUCHAR report=NULL;
    ULONG bytes=0;BOOLEAN forward=FALSE;
    WdfWaitLockAcquire(d->stateLock,NULL);WdfWaitLockAcquire(gGate,NULL);
    if(d->retired)status=STATUS_DEVICE_NOT_CONNECTED;
    else if(d->bypass)forward=TRUE;
    else if(d->prepared && d->powered && d->caps.InputReportByteLength && d->slots[id]) {
        const ULONG size=d->caps.InputReportByteLength;
        status=WdfRequestRetrieveOutputBuffer(request,size,&output,NULL);
        if(NT_SUCCESS(status)) {
            if(d->captureId && d->deliveredValid[id]) {
                RtlCopyMemory(output,d->delivered+(SIZE_T)(d->slots[id]-1)*size,size);bytes=size;
            } else if(d->captureId)status=STATUS_DEVICE_NOT_READY;
            else {
                // Serialize preparation/freeing, but never hold the capture
                // gate over allocation, lower I/O or a completion callback.
                WdfWaitLockRelease(gGate);
                report=ExAllocatePoolZero(NonPagedPoolNx,size,FILTER_TAG);
                if(!report)status=STATUS_INSUFFICIENT_RESOURCES;
                else status=QueryInputReport(d,request,id,report,size,&bytes);
                WdfWaitLockAcquire(gGate,NULL);
                if(d->retired) {status=STATUS_DEVICE_NOT_CONNECTED;bytes=0;}
                else if(NT_SUCCESS(status)) {
                    IcMonitorPublish(IC_HID_BACKEND_TYPE,d->id,IC_MONITOR_IN,IC_MONITOR_RAW,
                        report,size,NULL,STATUS_SUCCESS);
                    if(d->captureId && d->deliveredValid[id]) {
                        // A capture that began during the query must expose
                        // only its delivered state, never physical query data.
                        RtlCopyMemory(output,d->delivered+(SIZE_T)(d->slots[id]-1)*size,size);
                    } else if(d->captureId) {status=STATUS_DEVICE_NOT_READY;bytes=0;}
                    else RtlCopyMemory(output,report,size);
                }
            }
        }
    }
    WdfWaitLockRelease(gGate);WdfWaitLockRelease(d->stateLock);
    if(report)ExFreePoolWithTag(report,FILTER_TAG);
    if(forward)Forward(request,d->device);
    // Collection-level GET_INPUT_REPORT returns its data in the complete
    // caller buffer; HIDClass's completion Information contract is zero.
    else {
        if(NT_SUCCESS(status) && bytes && output)
            IcMonitorPublish(IC_HID_BACKEND_TYPE,d->id,IC_MONITOR_OUT,IC_MONITOR_RAW,
                output,bytes,NULL,STATUS_SUCCESS);
        WdfRequestCompleteWithInformation(request,status,0);
    }
}
static void GetInputReport(HID_DEVICE* d,WDFREQUEST request) {
    NTSTATUS status=STATUS_DEVICE_NOT_READY;PVOID output=NULL;ULONG bytes=0;UCHAR id=0;BOOLEAN query=FALSE;
    WdfWaitLockAcquire(gGate,NULL);
    if(d->retired)status=STATUS_DEVICE_NOT_CONNECTED;
    else if(d->bypass) {WdfWaitLockRelease(gGate);Forward(request,d->device);return;}
    else
    if(d->prepared && d->caps.InputReportByteLength) {
        status=WdfRequestRetrieveOutputBuffer(request,d->caps.InputReportByteLength,&output,NULL);
        if(NT_SUCCESS(status)) {
            id=*(UCHAR*)output;
            if(!d->slots[id] || !d->captureId || !d->deliveredValid[id]) {
                status=STATUS_DEVICE_NOT_READY;
                // Outside capture, GET_INPUT_REPORT is a current-state query.
                // A streaming READ cache can be old (or seeded once by XUSB);
                // polling clients must retain the lower collection's behavior.
                query=d->slots[id] && !d->captureId && d->powered;
            } else {
                bytes=d->caps.InputReportByteLength;
                RtlCopyMemory(output,d->delivered+(SIZE_T)(d->slots[id]-1)*bytes,bytes);
            }
        }
    }
    WdfWaitLockRelease(gGate);
    if(query) {GetUncachedInputReport(d,request,id);return;}
    if(NT_SUCCESS(status) && bytes && output)
        IcMonitorPublish(IC_HID_BACKEND_TYPE,d->id,IC_MONITOR_OUT,IC_MONITOR_RAW,
            output,bytes,NULL,STATUS_SUCCESS);
    WdfRequestCompleteWithInformation(request,status,0);
}

static void Ioctl(WDFQUEUE queue,WDFREQUEST request,size_t outSize,size_t inSize,ULONG code) {
    HID_DEVICE* d=HidDevice(WdfIoQueueGetDevice(queue));NTSTATUS status;PVOID output;
    UNREFERENCED_PARAMETER(inSize);
    // Lifecycle may switch to transparent operation while this request arrives.
    // Decide under the same gate used by EnterBypass and retirement.
    WdfWaitLockAcquire(gGate,NULL);
    if(d->retired) {WdfWaitLockRelease(gGate);WdfRequestComplete(request,STATUS_DEVICE_NOT_CONNECTED);return;}
    if(d->bypass && code!=IC_HID_BACKEND_SOURCE) {WdfWaitLockRelease(gGate);Forward(request,d->device);return;}
    WdfWaitLockRelease(gGate);
    if(code==IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS || code==IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS || code==IOCTL_HID_FLUSH_QUEUE) {
        WDFFILEOBJECT file=WdfRequestGetFileObject(request);HID_FILE* f=file?HidFile(file):NULL;
        ULONG desired=0;PVOID input=NULL;PUCHAR replacement=NULL,previous=NULL;SIZE_T transferred=0;
        if(code==IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS) {
            status=WdfRequestRetrieveInputBuffer(request,sizeof(ULONG),&input,NULL);
            if(!NT_SUCCESS(status)) {WdfRequestComplete(request,status);return;}
            desired=*(ULONG*)input;
            if(desired<2 || desired>512) {WdfRequestComplete(request,STATUS_INVALID_PARAMETER);return;}
            if(!f || !f->reportBytes) {WdfRequestComplete(request,STATUS_INVALID_DEVICE_STATE);return;}
            replacement=ExAllocatePoolZero(NonPagedPoolNx,(SIZE_T)desired*f->reportBytes,FILTER_TAG);
            if(!replacement) {WdfRequestComplete(request,STATUS_INSUFFICIENT_RESOURCES);return;}
        }
        WdfWaitLockAcquire(gGate,NULL);
        if(d->retired) {
            WdfWaitLockRelease(gGate);if(replacement)ExFreePoolWithTag(replacement,FILTER_TAG);
            WdfRequestComplete(request,STATUS_DEVICE_NOT_CONNECTED);return;
        }
        if(d->bypass) {
            WdfWaitLockRelease(gGate);if(replacement)ExFreePoolWithTag(replacement,FILTER_TAG);
            Forward(request,d->device);return;
        }
        status=f && f->linked?STATUS_SUCCESS:STATUS_INVALID_DEVICE_STATE;
        if(NT_SUCCESS(status)) {
            if(code==IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS) {
                status=WdfRequestRetrieveOutputBuffer(request,sizeof(ULONG),&output,NULL);
                if(NT_SUCCESS(status)) {*(ULONG*)output=f->capacity;transferred=sizeof(ULONG);}
            } else if(code==IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS) {
                ULONG i,n=min(f->count,desired),skip=f->count-n;
                for(i=0;i<n;++i)RtlCopyMemory(replacement+(SIZE_T)i*f->reportBytes,
                    f->fifo+(SIZE_T)((f->head+skip+i)%f->capacity)*f->reportBytes,f->reportBytes);
                previous=f->fifo;f->fifo=replacement;replacement=NULL;f->head=0;f->count=n;f->capacity=desired;
            } else f->head=f->count=0;
        }
        WdfWaitLockRelease(gGate);
        if(previous)ExFreePoolWithTag(previous,FILTER_TAG);if(replacement)ExFreePoolWithTag(replacement,FILTER_TAG);
        WdfRequestCompleteWithInformation(request,status,transferred);return;
    }
    if(code==IC_HID_BACKEND_SOURCE) {
        WDFFILEOBJECT file=WdfRequestGetFileObject(request);HID_FILE* f=file?HidFile(file):NULL;
        WdfWaitLockAcquire(gGate,NULL);
        // Explicitly distinguish an unsupported transparent collection from a
        // supported reader that is temporarily unavailable. The SDK may omit
        // only the former from an otherwise valid capture inventory.
        status=d->bypass && !d->retired?STATUS_NOT_SUPPORTED:
            (f && f->linked && d->thread && !d->retired && d->prepared && d->powered
            ?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY);
        if(NT_SUCCESS(status)) {f->source=TRUE;f->count=f->head=0;}
        WdfWaitLockRelease(gGate);WdfRequestComplete(request,status);return;
    }
    if(code==IOCTL_HID_GET_INPUT_REPORT) {GetInputReport(d,request);return;}
    UNREFERENCED_PARAMETER(outSize);Forward(request,d->device);
}
static void Default(WDFQUEUE queue,WDFREQUEST request) {Forward(request,WdfIoQueueGetDevice(queue));}
static void FileCreate(WDFQUEUE queue,WDFREQUEST request) {
    WDFDEVICE device=WdfIoQueueGetDevice(queue);HID_DEVICE* d=HidDevice(device);
    WDFFILEOBJECT file=WdfRequestGetFileObject(request);HID_FILE* f=HidFile(file);
    PIO_STACK_LOCATION stack=IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(request));
    ACCESS_MASK access=stack->Parameters.Create.SecurityContext?stack->Parameters.Create.SecurityContext->DesiredAccess:0;
    WDF_REQUEST_SEND_OPTIONS options;WDF_OBJECT_ATTRIBUTES a;WDF_IO_QUEUE_CONFIG config;NTSTATUS status=STATUS_SUCCESS;
    BOOLEAN monitorOpen,monitorOptional;
    WdfWaitLockAcquire(d->stateLock,NULL);f->file=file;
    WdfWaitLockAcquire(gGate,NULL);
    // Internal observation is optional. Its producer needs no consumer FIFO;
    // a resource failure must not latch transparent mode or revoke an owner.
    // User-mode CREATEs retain their normal allocation/fallback semantics.
    monitorOptional=d->monitorOpening && WdfRequestGetRequestorMode(request)==KernelMode &&
        (!WdfRequestWdmGetIrp(request)->Tail.Overlay.Thread ||
         WdfRequestWdmGetIrp(request)->Tail.Overlay.Thread==d->monitorOpeningThread);
    monitorOpen=monitorOptional && d->monitorOpeningThread &&
        WdfRequestWdmGetIrp(request)->Tail.Overlay.Thread==d->monitorOpeningThread;
    // Some kernel request creators omit the originating thread. Keep normal
    // consumer semantics in that case; only resource-failure fallback is
    // optional until MonitorOpen can identify its exact returned FILE_OBJECT.
    if(monitorOptional)f->monitorSource=TRUE;
    if(monitorOpen)f->source=f->monitorSource=TRUE;
    if(d->retired)status=STATUS_DEVICE_NOT_CONNECTED;
    // Release may have won after MonitorSource chose to open. A transient
    // resource stop must not turn that late CREATE into permanent bypass.
    if((access&(FILE_READ_DATA|GENERIC_READ)) && d->monitorQuiescing && !d->latchedBypass) {
        status=STATUS_DEVICE_NOT_READY;
    }
    WdfWaitLockRelease(gGate);
    if(!NT_SUCCESS(status))goto done;
    if((access&(FILE_READ_DATA|GENERIC_READ)) && !d->bypass && !monitorOpen) {
        if(!d->prepared) {status=STATUS_DEVICE_NOT_READY;goto allocationFailed;}
        f->capacity=IC_HID_FILE_CAPACITY;f->reportBytes=d->caps.InputReportByteLength;
        f->fifo=ExAllocatePoolZero(NonPagedPoolNx,(SIZE_T)f->capacity*f->reportBytes,FILTER_TAG);
        if(!f->fifo) {status=STATUS_INSUFFICIENT_RESOURCES;goto allocationFailed;}
        WDF_IO_QUEUE_CONFIG_INIT(&config,WdfIoQueueDispatchManual);config.PowerManaged=WdfFalse;
        WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;
        status=WdfIoQueueCreate(device,&config,&a,&f->reads);if(!NT_SUCCESS(status))goto allocationFailed;
    }
    WdfRequestFormatRequestUsingCurrentType(request);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    WdfRequestSend(request,WdfDeviceGetIoTarget(device),&options);status=WdfRequestGetStatus(request);
    if(NT_SUCCESS(status) && (f->fifo || (monitorOpen && !d->bypass))) {
        WdfWaitLockAcquire(gGate,NULL);InsertTailList(&d->files,&f->link);f->linked=TRUE;WdfWaitLockRelease(gGate);
        // Do not fail an already forwarded successful CREATE: its lower file
        // must receive cleanup/close. A failed reader switches to transparent
        // operation for this device lifetime.
        NTSTATUS started=StartLocked(d);
        if(!NT_SUCCESS(started) && !monitorOptional)EnterBypass(d);
    }
    else if(NT_SUCCESS(status) && (access&(FILE_READ_DATA|GENERIC_READ)))EnterBypass(d);
    goto done;
allocationFailed:
    if(monitorOptional)goto done;
    // Optional bookkeeping must not make an otherwise usable lower CREATE
    // fail. Stop the shared reader, transfer its pending clients to the lower
    // stack, and retain this transparent mode for the FDO's entire lifetime.
    StopLocked(d);EnterBypass(d);
    if(f->reads) {WdfObjectDelete(f->reads);f->reads=NULL;}
    if(f->fifo) {ExFreePoolWithTag(f->fifo,FILTER_TAG);f->fifo=NULL;}
    f->capacity=f->reportBytes=0;
    WdfRequestFormatRequestUsingCurrentType(request);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    WdfRequestSend(request,WdfDeviceGetIoTarget(device),&options);status=WdfRequestGetStatus(request);
done:
    WdfWaitLockRelease(d->stateLock);WdfRequestComplete(request,status);
}
static void FileCleanup(WDFFILEOBJECT file) {
    HID_DEVICE* d=HidDevice(WdfFileObjectGetDevice(file));HID_FILE* f=HidFile(file);BOOLEAN reader;
    WDFQUEUE reads;PUCHAR fifo;
    WdfWaitLockAcquire(d->stateLock,NULL);reader=d->readerFile==f;
    if(reader)StopLocked(d);
    WdfWaitLockAcquire(gGate,NULL);
    if(f->linked) {RemoveEntryList(&f->link);f->linked=FALSE;}
    reads=f->reads;f->reads=NULL;fifo=f->fifo;f->fifo=NULL;
    WdfWaitLockRelease(gGate);
    if(reader) {
        NTSTATUS started=StartLocked(d);
        if(!NT_SUCCESS(started) && !f->monitorSource)EnterBypass(d);
    }
    WdfWaitLockAcquire(gGate,NULL);
    if(IcMonitorEnabled())MonitorScheduleLocked(d);
    WdfWaitLockRelease(gGate);
    WdfWaitLockRelease(d->stateLock);
    // A cancellation/completion callback above us can close another file and
    // enter this function again. Neither wait for that callback nor hold a lock
    // while purging; WDF defers queue deletion until owned requests finish.
    if(reads) {WdfIoQueuePurge(reads,NULL,NULL);WdfObjectDelete(reads);}
    if(fifo)ExFreePoolWithTag(fifo,FILTER_TAG);
}
static void FileDestroy(WDFOBJECT object) {
    HID_FILE* f=HidFile(object);
    // Failed CREATE may not receive cleanup.
    if(f->reads)WdfObjectDelete(f->reads);
    if(f->fifo)ExFreePoolWithTag(f->fifo,FILTER_TAG);
}
static NTSTATUS LowerIoctl(HID_DEVICE* d,ULONG code,PVOID buffer,ULONG size) {
    WDF_MEMORY_DESCRIPTOR memory;WDF_REQUEST_SEND_OPTIONS options;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memory,buffer,size);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,WDF_REL_TIMEOUT_IN_SEC(2));
    return WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(d->device),NULL,code,NULL,&memory,&options,NULL);
}
static void FreePrepared(HID_DEVICE* d) {
    if(d->request) {WdfObjectDelete(d->request);d->request=NULL;}
    if(d->descriptor)ExFreePoolWithTag(d->descriptor,FILTER_TAG);
    if(d->physical)ExFreePoolWithTag(d->physical,FILTER_TAG);
    if(d->delivered)ExFreePoolWithTag(d->delivered,FILTER_TAG);
    if(d->report)ExFreePoolWithTag(d->report,FILTER_TAG);
    d->descriptor=NULL;d->physical=d->delivered=d->report=NULL;d->prepared=FALSE;d->used=FALSE;
    RtlZeroMemory(d->slots,sizeof(d->slots));d->slotCount=0;
    RtlZeroMemory(d->physicalValid,sizeof(d->physicalValid));RtlZeroMemory(d->deliveredValid,sizeof(d->deliveredValid));
    d->bypass=TRUE;
#ifdef IC_HID_PRIVATE_MIRROR
    d->mirrorProfile=FALSE;RtlZeroMemory(d->mirrorParent,sizeof(d->mirrorParent));
#endif
}
static NTSTATUS PrepareLocked(WDFDEVICE device,WDFCMRESLIST raw,WDFCMRESLIST translated) {
    HID_DEVICE* d=HidDevice(device);HID_COLLECTION_INFORMATION collection={0};NTSTATUS status;WDF_OBJECT_ATTRIBUTES a;
    PHIDP_VALUE_CAPS values=NULL;PHIDP_BUTTON_CAPS buttons=NULL;USHORT count;ULONG i,bytes;
    UNREFERENCED_PARAMETER(raw);UNREFERENCED_PARAMETER(translated);
    status=LowerIoctl(d,IOCTL_HID_GET_COLLECTION_INFORMATION,&collection,sizeof(collection));if(!NT_SUCCESS(status))return status;
    if(!collection.DescriptorSize || collection.DescriptorSize>65536)return STATUS_INVALID_BUFFER_SIZE;
    d->descriptorBytes=collection.DescriptorSize;
    d->descriptor=ExAllocatePoolZero(NonPagedPoolNx,d->descriptorBytes,FILTER_TAG);
    if(!d->descriptor)return STATUS_INSUFFICIENT_RESOURCES;
    status=LowerIoctl(d,IOCTL_HID_GET_COLLECTION_DESCRIPTOR,d->descriptor,d->descriptorBytes);if(!NT_SUCCESS(status))goto fail;
    status=HidP_GetCaps(d->descriptor,&d->caps);if(status!=HIDP_STATUS_SUCCESS)goto fail;
    // EvtDevicePrepareHardware must not return STATUS_NOT_SUPPORTED. An
    // associated collection outside this engine's profile cannot be captured.
    // The wrapper keeps the lower Windows stack usable in transparent mode.
    status=STATUS_DEVICE_CONFIGURATION_ERROR;
    if(!IcHidGameProfile(d->caps.UsagePage,d->caps.Usage) ||
       (IC_HID_BACKEND_TYPE!=IC_TYPE_CONTROLLER_HID && IcHidGameProfile(d->caps.UsagePage,d->caps.Usage)!=IC_HID_PROFILE_DESKTOP) ||
       !d->caps.InputReportByteLength || d->caps.InputReportByteLength>IC_HID_REPORT_MAX ||
       d->caps.NumberInputValueCaps>1024 || d->caps.NumberInputButtonCaps>1024)goto fail;
    if(d->caps.NumberInputValueCaps) {
        count=d->caps.NumberInputValueCaps;values=ExAllocatePoolZero(NonPagedPoolNx,(SIZE_T)count*sizeof(*values),FILTER_TAG);
        if(!values) {status=STATUS_INSUFFICIENT_RESOURCES;goto fail;}
        status=HidP_GetValueCaps(HidP_Input,values,&count,d->descriptor);
        if(status!=HIDP_STATUS_SUCCESS || count>d->caps.NumberInputValueCaps) {status=STATUS_INVALID_BUFFER_SIZE;goto fail;}
        for(i=0;i<count;++i)d->slots[values[i].ReportID]=1;
    }
    if(d->caps.NumberInputButtonCaps) {
        count=d->caps.NumberInputButtonCaps;buttons=ExAllocatePoolZero(NonPagedPoolNx,(SIZE_T)count*sizeof(*buttons),FILTER_TAG);
        if(!buttons) {status=STATUS_INSUFFICIENT_RESOURCES;goto fail;}
        status=HidP_GetButtonCaps(HidP_Input,buttons,&count,d->descriptor);
        if(status!=HIDP_STATUS_SUCCESS || count>d->caps.NumberInputButtonCaps) {status=STATUS_INVALID_BUFFER_SIZE;goto fail;}
        for(i=0;i<count;++i)d->slots[buttons[i].ReportID]=1;
    }
    for(i=0;i<256;++i)if(d->slots[i])d->slots[i]=++d->slotCount;
    if(!d->slotCount) {status=STATUS_DEVICE_CONFIGURATION_ERROR;goto fail;}
    d->physical=ExAllocatePoolZero(NonPagedPoolNx,(SIZE_T)d->slotCount*d->caps.InputReportByteLength,FILTER_TAG);
    d->delivered=ExAllocatePoolZero(NonPagedPoolNx,(SIZE_T)d->slotCount*d->caps.InputReportByteLength,FILTER_TAG);
    d->report=ExAllocatePoolZero(NonPagedPoolNx,d->caps.InputReportByteLength,FILTER_TAG);
    if(!d->physical || !d->delivered || !d->report) {status=STATUS_INSUFFICIENT_RESOURCES;goto fail;}
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;
    status=WdfRequestCreate(&a,WdfDeviceGetIoTarget(device),&d->request);if(!NT_SUCCESS(status))goto fail;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=d->request;
    status=WdfMemoryCreatePreallocated(&a,d->report,d->caps.InputReportByteLength,&d->memory);if(!NT_SUCCESS(status))goto fail;
    RtlZeroMemory(&d->info,sizeof(d->info));
    d->info.type=IC_HID_BACKEND_TYPE;d->info.instance_token=d->id;d->info.flags=IC_INFO_HID_VALID;
    d->info.vid=collection.VendorID;d->info.pid=collection.ProductID;d->info.revision=collection.VersionNumber;
    d->info.usage_page=d->caps.UsagePage;d->info.usage=d->caps.Usage;
    d->info.input_bytes=d->caps.InputReportByteLength;d->info.output_bytes=d->caps.OutputReportByteLength;d->info.feature_bytes=d->caps.FeatureReportByteLength;
    if(NT_SUCCESS(WdfDeviceQueryProperty(device,DevicePropertyHardwareID,sizeof(d->info.hardware_id)-2,d->info.hardware_id,&bytes)))d->info.flags|=IC_INFO_HARDWARE_ID_VALID;
    else RtlZeroMemory(d->info.hardware_id,sizeof(d->info.hardware_id));
    if(NT_SUCCESS(WdfDeviceQueryProperty(device,DevicePropertyDeviceDescription,sizeof(d->info.name)-2,d->info.name,&bytes)))d->info.flags|=IC_INFO_NAME_VALID;
    else RtlZeroMemory(d->info.name,sizeof(d->info.name));
#ifdef IC_HID_PRIVATE_MIRROR
    MirrorPrepare(d);
#endif
    WdfWaitLockAcquire(gGate,NULL);d->prepared=TRUE;WdfWaitLockRelease(gGate);status=STATUS_SUCCESS;
fail:
    if(values)ExFreePoolWithTag(values,FILTER_TAG);if(buttons)ExFreePoolWithTag(buttons,FILTER_TAG);
    if(!NT_SUCCESS(status))FreePrepared(d);return status;
}
static NTSTATUS Prepare(WDFDEVICE device,WDFCMRESLIST raw,WDFCMRESLIST translated) {
    HID_DEVICE* d=HidDevice(device);NTSTATUS status;
    WdfWaitLockAcquire(d->stateLock,NULL);
    if(d->latchedBypass) {WdfWaitLockRelease(d->stateLock);return STATUS_SUCCESS;}
    status=PrepareLocked(device,raw,translated);
    // A missing association, unfamiliar descriptor or preparation failure must
    // never make this collection filter prevent the normal Windows HID path.
    // Such a node is excluded from capture and forwards its requests.
    if(!NT_SUCCESS(status) || d->latchedBypass
#ifdef IC_HID_PRIVATE_MIRROR
       || !d->mirrorProfile
#endif
       )EnterBypass(d);
    else {
        WdfWaitLockAcquire(gGate,NULL);
        d->bypass=FALSE;d->monitorQuiescing=FALSE;++d->monitorRevision;
        if(IcMonitorEnabled())MonitorScheduleLocked(d);
        WdfWaitLockRelease(gGate);
    }
    status=STATUS_SUCCESS;
    WdfWaitLockRelease(d->stateLock);return status;
}
static NTSTATUS Release(WDFDEVICE device,WDFCMRESLIST translated) {
    HID_DEVICE* d=HidDevice(device);UNREFERENCED_PARAMETER(translated);
    WdfWaitLockAcquire(gGate,NULL);d->monitorQuiescing=TRUE;++d->monitorRevision;MonitorScheduleLocked(d);WdfWaitLockRelease(gGate);
    WdfWaitLockAcquire(d->stateLock,NULL);StopLocked(d);
    WdfWaitLockAcquire(gGate,NULL);FreePrepared(d);WdfWaitLockRelease(gGate);
    WdfWaitLockRelease(d->stateLock);WdfWorkItemFlush(d->completionWork);return STATUS_SUCCESS;
}
static NTSTATUS Resume(WDFDEVICE device) {
    HID_DEVICE* d=HidDevice(device);NTSTATUS status;
    WdfWaitLockAcquire(d->stateLock,NULL);
    WdfWaitLockAcquire(gGate,NULL);IcTrialResumeLocked();d->powered=TRUE;WdfWaitLockRelease(gGate);status=StartLocked(d);
    if(!NT_SUCCESS(status)) {EnterBypass(d);status=STATUS_SUCCESS;}
    WdfWaitLockRelease(d->stateLock);
    WdfWaitLockAcquire(gGate,NULL);++d->monitorRevision;if(IcMonitorEnabled())MonitorScheduleLocked(d);WdfWaitLockRelease(gGate);
    return status;
}
static NTSTATUS Suspend(WDFDEVICE device) {
    HID_DEVICE* d=HidDevice(device);
    WdfWaitLockAcquire(d->stateLock,NULL);
    WdfWaitLockAcquire(gGate,NULL);d->powered=FALSE;++d->monitorRevision;MonitorScheduleLocked(d);WdfWaitLockRelease(gGate);StopLocked(d);WdfWaitLockRelease(d->stateLock);WdfWorkItemFlush(d->completionWork);return STATUS_SUCCESS;
}
static void Retire(WDFDEVICE device) {
    HID_DEVICE* d=HidDevice(device);PLIST_ENTRY p;
    WdfWaitLockAcquire(gGate,NULL);d->retired=TRUE;++d->monitorRevision;
    IcMonitorRemoved(IC_HID_BACKEND_TYPE,d->id);MonitorScheduleLocked(d);
    if(d->captureId)IcRevokeAllLocked(IC_RESET_DEVICE);
    ClearFiles(d);
    // Non-power-managed per-file queues otherwise survive surprise removal
    // until the application's old handle closes. Take ownership of pending
    // READs now, but neither purge nor complete from this PnP callback. The
    // existing work item invokes upper completion routines outside all locks.
    for(p=d->files.Flink;p!=&d->files;p=p->Flink) {
        HID_FILE* f=CONTAINING_RECORD(p,HID_FILE,link);WDFREQUEST request;
        if(!f->reads)continue;
        while(NT_SUCCESS(WdfIoQueueRetrieveNextRequest(f->reads,&request))) {
            HID_COMPLETION* pending=HidCompletion(request);
            pending->request=request;
            pending->forward=FALSE;
            pending->status=STATUS_DEVICE_NOT_CONNECTED;pending->bytes=0;pending->monitorReport=NULL;
            InsertTailList(&d->completions,&pending->link);
            if(!d->completionScheduled) {d->completionScheduled=TRUE;WdfWorkItemEnqueue(d->completionWork);}
        }
    }
    WdfWaitLockRelease(gGate);
}
static void ManagedCleanup(WDFDEVICE device) {
    HID_DEVICE* d=HidDevice(device);
    Retire(device);
    (void)Suspend(device);
    // No stateLock/gGate is held: the owned open's cleanup takes both.
    WdfWorkItemFlush(d->monitorWork);
}
static void Cleanup(WDFOBJECT object) {
    HID_DEVICE* d=HidDevice(object);
    WdfWaitLockAcquire(gGate,NULL);
    if(d->registered) {RemoveEntryList(&d->link);d->registered=FALSE;}
    WdfWaitLockRelease(gGate);if(d->counted)IcFdoRemoved();
}
NTSTATUS IcHidAdd(PWDFDEVICE_INIT init) {
    WDFDEVICE device;HID_DEVICE* d;WDF_OBJECT_ATTRIBUTES a;WDF_FILEOBJECT_CONFIG file;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;WDF_IO_QUEUE_CONFIG q;WDFQUEUE creates;NTSTATUS status;WDF_WORKITEM_CONFIG work;
    WdfFdoInitSetFilter(init); // Inherit the lower device IO type, as required for filters.
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);pnp.EvtDevicePrepareHardware=Prepare;pnp.EvtDeviceReleaseHardware=Release;
    pnp.EvtDeviceSelfManagedIoInit=Resume;pnp.EvtDeviceSelfManagedIoRestart=Resume;pnp.EvtDeviceSelfManagedIoSuspend=Suspend;
    pnp.EvtDeviceSelfManagedIoCleanup=ManagedCleanup;
    pnp.EvtDeviceSurpriseRemoval=Retire;
    WdfDeviceInitSetPnpPowerEventCallbacks(init,&pnp);
    WDF_FILEOBJECT_CONFIG_INIT(&file,WDF_NO_EVENT_CALLBACK,WDF_NO_EVENT_CALLBACK,FileCleanup);
    file.AutoForwardCleanupClose=WdfTrue;file.FileObjectClass=WdfFileObjectWdfCannotUseFsContexts;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,HID_FILE);a.ExecutionLevel=WdfExecutionLevelPassive;
    a.SynchronizationScope=WdfSynchronizationScopeNone;a.EvtDestroyCallback=FileDestroy;
    WdfDeviceInitSetFileObjectConfig(init,&file,&a);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,HID_COMPLETION);
    WdfDeviceInitSetRequestAttributes(init,&a);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,HID_DEVICE);a.ExecutionLevel=WdfExecutionLevelPassive;
    a.SynchronizationScope=WdfSynchronizationScopeNone;a.EvtCleanupCallback=Cleanup;
    status=WdfDeviceCreate(&init,&a,&device);if(!NT_SUCCESS(status))return status;
    d=HidDevice(device);d->device=device;d->id=(ULONG)InterlockedIncrement(&nextId);
    IcCacheDeviceInstance(device,d->instance);
    d->bypass=TRUE;
    InitializeListHead(&d->files);InitializeListHead(&d->completions);KeInitializeEvent(&d->stop,NotificationEvent,FALSE);KeInitializeEvent(&d->done,SynchronizationEvent,FALSE);
    WDF_WORKITEM_CONFIG_INIT(&work,CompleteReads);work.AutomaticSerialization=FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,HID_WORK);a.ParentObject=device;
    status=WdfWorkItemCreate(&work,&a,&d->completionWork);if(!NT_SUCCESS(status))return status;
    HidWork(d->completionWork)->device=d;
    WDF_WORKITEM_CONFIG_INIT(&work,MonitorSource);work.AutomaticSerialization=FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,HID_WORK);a.ParentObject=device;
    status=WdfWorkItemCreate(&work,&a,&d->monitorWork);if(!NT_SUCCESS(status))return status;
    HidWork(d->monitorWork)->device=d;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;
    status=WdfWaitLockCreate(&a,&d->stateLock);if(!NT_SUCCESS(status))return status;
    status=IcFdoAdded();if(!NT_SUCCESS(status))return status;
    d->counted=TRUE;
    WdfWaitLockAcquire(gGate,NULL);
#ifdef IC_HID_PRIVATE_MIRROR
    // A newly arriving, not-yet-associated legacy view invalidates an existing
    // native isolation snapshot even if it later falls back to passthrough.
    if(MirrorCapturePresent())IcRevokeAllLocked(IC_RESET_DEVICE);
#endif
    InsertTailList(&devices,&d->link);d->registered=TRUE;WdfWaitLockRelease(gGate);
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q,WdfIoQueueDispatchParallel);q.PowerManaged=WdfTrue;
    q.EvtIoRead=Read;q.EvtIoDeviceControl=Ioctl;q.EvtIoInternalDeviceControl=Ioctl;q.EvtIoDefault=Default;
    status=WdfIoQueueCreate(device,&q,WDF_NO_OBJECT_ATTRIBUTES,WDF_NO_HANDLE);if(!NT_SUCCESS(status))return status;
    WDF_IO_QUEUE_CONFIG_INIT(&q,WdfIoQueueDispatchSequential);q.PowerManaged=WdfFalse;q.EvtIoDefault=FileCreate;
    status=WdfIoQueueCreate(device,&q,WDF_NO_OBJECT_ATTRIBUTES,&creates);if(!NT_SUCCESS(status))return status;
    status=WdfDeviceConfigureRequestDispatching(device,creates,WdfRequestTypeCreate);if(!NT_SUCCESS(status))return status;
    return WdfDeviceCreateDeviceInterface(device,&interfaceGuid,NULL);
}
