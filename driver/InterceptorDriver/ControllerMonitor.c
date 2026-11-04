#include "ControllerMonitor.h"
#include "../Shared/MonitorCursor.h"

// Lock order is gGate -> monitorLock. Publishers can run at DISPATCH_LEVEL
// and never acquire gGate, allocate, wait, or invoke a Windows completion.
static KSPIN_LOCK monitorLock;
static LIST_ENTRY observers;
static volatile LONG observerCount;
static IC_MONITOR_FRAME* frames;
static ULONGLONG sequence,session;

void IcMonitorInitialize(void) {KeInitializeSpinLock(&monitorLock);InitializeListHead(&observers);}
BOOLEAN IcMonitorEnabled(void) {return observerCount!=0;}
void IcMonitorUninitialize(void) {NT_ASSERT(!observerCount && !frames);}

void IcMonitorPublish(ULONG type,ULONG token,ULONG direction,ULONG flags,const VOID* report,ULONG length,
    const IC_NATIVE_GAMEPAD_STATE* state,NTSTATUS status) {
    KIRQL old;IC_MONITOR_FRAME* frame;PLIST_ENTRY p;ULONGLONG previous;
    if(!observerCount)return; // One aligned volatile read; no inactive report copies.
    if(type<IC_TYPE_CONTROLLER_HID || type>IC_TYPE_CONTROLLER_GIP || !token ||
       length>IC_HID_REPORT_MAX || (length && !report))return;
    KeAcquireSpinLock(&monitorLock,&old);
    if(!frames || !observerCount){KeReleaseSpinLock(&monitorLock,old);return;}
    previous=sequence;frame=&frames[sequence++%IC_MONITOR_CAPACITY];
    RtlZeroMemory(frame,sizeof(*frame));frame->sequence=sequence;
    frame->timestamp=KeQueryPerformanceCounter(NULL).QuadPart;
    frame->type=type;frame->instance_token=token;frame->direction=direction;
    frame->flags=flags;frame->length=length;frame->status=(ULONG)status;
    if(state)frame->state=*state;
    if(length)RtlCopyMemory(frame->report,report,length);
    for(p=observers.Flink;p!=&observers;p=p->Flink) {
        IC_MONITOR_OBSERVER* observer=CONTAINING_RECORD(p,IC_MONITOR_OBSERVER,link);
        if(observer->cursor==previous)KeSetEvent(observer->event,IO_NO_INCREMENT,FALSE);
    }
    KeReleaseSpinLock(&monitorLock,old);
}
void IcMonitorRemoved(ULONG type,ULONG token) {
    IcMonitorPublish(type,token,IC_MONITOR_DEVICE,IC_MONITOR_REMOVED,NULL,0,NULL,STATUS_DEVICE_NOT_CONNECTED);
}
NTSTATUS IcMonitorQuery(PVOID output,SIZE_T length,SIZE_T* written) {
    IC_MONITOR_CAPABILITIES* result=(IC_MONITOR_CAPABILITIES*)output;KIRQL old;LARGE_INTEGER frequency;
    if(length<sizeof(*result))return STATUS_BUFFER_TOO_SMALL;
    KeQueryPerformanceCounter(&frequency);RtlZeroMemory(result,sizeof(*result));
    result->size=sizeof(*result);result->version=IC_MONITOR_VERSION;
    result->frame_bytes=sizeof(IC_MONITOR_FRAME);result->capacity=IC_MONITOR_CAPACITY;
    result->timestamp_frequency=frequency.QuadPart;
    KeAcquireSpinLock(&monitorLock,&old);result->session=session;KeReleaseSpinLock(&monitorLock,old);
    *written=sizeof(*result);return STATUS_SUCCESS;
}
void IcMonitorClose(IC_MONITOR_OBSERVER* observer) {
    KIRQL old;PKEVENT event=NULL;IC_MONITOR_FRAME* retired=NULL;
    KeAcquireSpinLock(&monitorLock,&old);
    if(observer->linked) {
        RemoveEntryList(&observer->link);observer->linked=FALSE;
        event=observer->event;observer->event=NULL;
        --observerCount;
        if(!observerCount){retired=frames;frames=NULL;sequence=0;}
    }
    KeReleaseSpinLock(&monitorLock,old);
    if(event){KeSetEvent(event,IO_NO_INCREMENT,FALSE);ObDereferenceObject(event);}
    if(retired)ExFreePoolWithTag(retired,FILTER_TAG);
}
void IcMonitorRevokeAllLocked(void) {
    // Subscription/close and file lifetime are serialized by gGate. Publish
    // only borrows the list under monitorLock, so retire via the normal path.
    while(!IsListEmpty(&observers)) {
        IC_MONITOR_OBSERVER* observer=CONTAINING_RECORD(observers.Flink,IC_MONITOR_OBSERVER,link);
        IcMonitorClose(observer);
    }
}
NTSTATUS IcMonitorSubscribe(IC_MONITOR_OBSERVER* observer,const IC_MONITOR_SUBSCRIBE* request) {
    NTSTATUS status;PKEVENT event=NULL;IC_MONITOR_FRAME* allocated=NULL;KIRQL old;
    if(request->version!=IC_MONITOR_VERSION || request->flags)return STATUS_INVALID_PARAMETER;
    if(!request->event_handle){IcMonitorClose(observer);return STATUS_SUCCESS;}
    // Called in the originating user process by EvtIoInCallerContext, under
    // gGate. Never interpret a user event handle from an arbitrary work item.
    status=ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)request->event_handle,EVENT_MODIFY_STATE,
        *ExEventObjectType,UserMode,(PVOID*)&event,NULL);
    if(!NT_SUCCESS(status))return status;
    if(event->Header.Type!=NotificationEvent){ObDereferenceObject(event);return STATUS_INVALID_PARAMETER;}
    if(!observer->linked && observerCount>=IC_MONITOR_MAX_OBSERVERS){ObDereferenceObject(event);return STATUS_INSUFFICIENT_RESOURCES;}
    if(!frames) {
        allocated=ExAllocatePoolZero(NonPagedPoolNx,sizeof(IC_MONITOR_FRAME)*IC_MONITOR_CAPACITY,FILTER_TAG);
        if(!allocated){ObDereferenceObject(event);return STATUS_INSUFFICIENT_RESOURCES;}
    }
    // Retain an existing subscription on all validation/allocation failures.
    // Caller serialization ensures no other subscriber mutates this transition.
    if(observer->linked) {
        PKEVENT previous;
        KeAcquireSpinLock(&monitorLock,&old);previous=observer->event;
        observer->event=event;observer->cursor=sequence;observer->dropped=0;KeClearEvent(event);
        KeReleaseSpinLock(&monitorLock,old);
        if(previous!=event)KeSetEvent(previous,IO_NO_INCREMENT,FALSE);
        ObDereferenceObject(previous);return STATUS_SUCCESS;
    }
    KeAcquireSpinLock(&monitorLock,&old);
    if(allocated){frames=allocated;sequence=0;++session;}
    observer->event=event;observer->cursor=sequence;observer->dropped=0;
    observer->linked=TRUE;InsertTailList(&observers,&observer->link);
    KeClearEvent(event);++observerCount;KeReleaseSpinLock(&monitorLock,old);return STATUS_SUCCESS;
}
NTSTATUS IcMonitorRead(IC_MONITOR_OBSERVER* observer,const IC_MONITOR_READ* request,PVOID output,SIZE_T length,SIZE_T* written) {
    KIRQL old;ULONG maximum,n,i;IC_MONITOR_READ_RESULT* result=(IC_MONITOR_READ_RESULT*)output;IC_MONITOR_FRAME* target;
    if(request->version!=IC_MONITOR_VERSION || !request->max_frames || request->max_frames>IC_MONITOR_MAX_READ)
        return STATUS_INVALID_PARAMETER;
    if(length<sizeof(*result))return STATUS_BUFFER_TOO_SMALL;
    maximum=(ULONG)min((length-sizeof(*result))/sizeof(IC_MONITOR_FRAME),request->max_frames);
    KeAcquireSpinLock(&monitorLock,&old);
    if(!observer->linked){KeReleaseSpinLock(&monitorLock,old);return STATUS_INVALID_DEVICE_STATE;}
    if(sequence>observer->cursor && !maximum){KeReleaseSpinLock(&monitorLock,old);return STATUS_BUFFER_TOO_SMALL;}
    n=IcMonitorTake(sequence,&observer->cursor,&observer->dropped,maximum);
    target=(IC_MONITOR_FRAME*)(result+1);
    for(i=0;i<n;++i)target[i]=frames[(observer->cursor+i)%IC_MONITOR_CAPACITY];
    observer->cursor+=n;RtlZeroMemory(result,sizeof(*result));result->version=IC_MONITOR_VERSION;
    result->frame_bytes=sizeof(IC_MONITOR_FRAME);result->count=n;result->capacity=IC_MONITOR_CAPACITY;
    result->session=session;result->next_sequence=observer->cursor+1;result->dropped=observer->dropped;
    if(observer->cursor==sequence)KeClearEvent(observer->event);
    KeReleaseSpinLock(&monitorLock,old);*written=sizeof(*result)+(SIZE_T)n*sizeof(*target);return STATUS_SUCCESS;
}
