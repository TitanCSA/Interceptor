#include "ControllerHid.h"
#include "ControllerXusb.h"
#include "ControllerXinputHid.h"
#include "ControllerGip.h"
#include "../Shared/ControllerNative.h"
#include "../Shared/InputValidation.h"
#include "../Shared/DeviceRouting.h"
#include "../Shared/DeviceAvailability.h"
#include "Trial.h"
#include "ControllerMonitor.h"

typedef struct FILE_CONTEXT {
    UCHAR mask; BOOLEAN closed,counted,allCapture; ULONG deviceCount; PKEVENT events[IC_STREAM_COUNT]; PKEVENT revoked;
    DEVICE_CONTEXT** targets;ULONG counts[2];
    IC_MONITOR_OBSERVER monitor;
} FILE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILE_CONTEXT,IcFile)
WDFSPINLOCK gLock;
WDFWAITLOCK gGate;
static WDFWAITLOCK gLifetime;
static WDFDEVICE gControl;
static LONG gFdos;
static ULONG gOpenFiles;
static LIST_ENTRY gDevices;
static FILE_CONTEXT* gOwners[IC_STREAM_COUNT];
static IC_RING_INDEX gRings[2];
static IC_KEYBOARD_INPUT gKeyboard[IC_PRODUCT_HAS_KEYBOARD_MOUSE?IC_CAPACITY:1];
static IC_MOUSE_INPUT gMouse[IC_PRODUCT_HAS_KEYBOARD_MOUSE?IC_CAPACITY:1];
static ULONGLONG gSequence,gGeneration,gOverflows;
static ULONG gResetReason;
static BOOLEAN gChord;

EVT_WDF_DEVICE_FILE_CREATE IcFileCreate;
EVT_WDF_FILE_CLEANUP IcFileCleanup;
EVT_WDF_FILE_CLOSE IcFileClose;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL IcControl;
EVT_WDF_DRIVER_UNLOAD IcUnload;
DRIVER_INITIALIZE DriverEntry;

// gGate serializes ownership, FIFO snapshots, callbacks and device lifetime.
// No spin lock is held while copying batches or calling a class service.
static void ResetLocked(UCHAR mask,ULONG reason) {
    ULONG i;PLIST_ENTRY p;
    ++gGeneration;gResetReason=reason;
    for(i=0;i<IC_STREAM_COUNT;++i)if(mask&(1u<<i)) {
        FILE_CONTEXT* f=gOwners[i];
        if(f) {
            f->mask&=(UCHAR)~(1u<<i);
            KeSetEvent(f->revoked,IO_NO_INCREMENT,FALSE);
            if(!f->mask && f->targets) {
                ExFreePoolWithTag(f->targets,FILTER_TAG);f->targets=NULL;f->counts[0]=f->counts[1]=0;
            }
        }
        gOwners[i]=NULL;if(i<2)gRings[i].head=gRings[i].count=0;
        else if(i==2)IcHidResetLocked();else if(i==3)IcXusbResetLocked();else if(i==4)IcXinputHidResetLocked();
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
        else if(i==5)IcGipResetLocked();
#endif
    }
    for(p=gDevices.Flink;p!=&gDevices;p=p->Flink) {
        DEVICE_CONTEXT* d=CONTAINING_RECORD(p,DEVICE_CONTEXT,link);
        if(mask&(d->keyboard?IC_KEYBOARD:IC_MOUSE)) {
            InterlockedExchange(&d->recover,TRUE);KeSetEvent(&d->maintenanceEvent,IO_NO_INCREMENT,FALSE);
        }
    }
    IcHidMonitorChangedLocked();IcXinputHidMonitorChangedLocked();
}
void IcRevokeAllLocked(ULONG reason) {if(reason==IC_RESET_OVERFLOW)++gOverflows;ResetLocked(IC_ALL_STREAMS,reason);}
ULONG IcActiveMaskLocked(void) {
    ULONG i,mask=0;for(i=0;i<IC_STREAM_COUNT;++i)if(gOwners[i])mask|=1u<<i;return mask;
}
void IcTrialRevokeLocked(void) {
    // Only captured streams require recovery. In particular, do not release
    // a normal keyboard/mouse when a controller-only trial expires, or when
    // the final capture was already closed long before the deadline.
    IcMonitorRevokeAllLocked();
    ResetLocked((UCHAR)IcActiveMaskLocked(),IC_RESET_TRIAL_EXPIRED);
    IcHidMonitorChangedLocked();IcXinputHidMonitorChangedLocked();
}
BOOLEAN IcAllCaptureLocked(ULONG type) {
    return type>=1 && type<=IC_STREAM_COUNT && gOwners[type-1] && gOwners[type-1]->allCapture;
}
BOOLEAN IcAllAttachLocked(ULONG type) {
    FILE_CONTEXT* f;
    if(!IcAllCaptureLocked(type))return FALSE;
    f=gOwners[type-1];
    if(f->deviceCount>=IC_MAX_DEVICES) {IcRevokeAllLocked(IC_RESET_DEVICE);return FALSE;}
    ++f->deviceCount;return TRUE;
}
void IcAllReadersReadyLocked(void) {
    // Configuration/first-validation notifications only: never a polling timer.
    ULONG stream;
    for(stream=0;stream<IC_STREAM_COUNT;++stream)if(IcAllCaptureLocked(stream+1))break;
    if(stream==IC_STREAM_COUNT)return;
    IcHidRefreshCaptureLocked();IcXusbRefreshCaptureLocked();IcXinputHidRefreshCaptureLocked();
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
    IcGipRefreshCaptureLocked();
#endif
}
ULONGLONG IcNextSequenceLocked(void) {return ++gSequence;}
static ULONG KeyIndex(const KEYBOARD_INPUT_DATA* k) {
    return (k->MakeCode&127u)+((k->Flags&KEY_E1)?256u:(k->Flags&KEY_E0)?128u:0u);
}
static ULONG DeliverBatchLocked(DEVICE_CONTEXT* d,PVOID packets,ULONG count) {
    KIRQL old;ULONG consumed=0,i,j;
    SIZE_T size=d->keyboard?sizeof(KEYBOARD_INPUT_DATA):sizeof(MOUSE_INPUT_DATA);
    if(!d->connection.ClassService || !count)return 0;
    KeRaiseIrql(DISPATCH_LEVEL,&old);
    ((PSERVICE_CALLBACK_ROUTINE)d->connection.ClassService)(d->connection.ClassDeviceObject,packets,(PUCHAR)packets+size*count,&consumed);
    KeLowerIrql(old);
    NT_ASSERT(consumed<=count);consumed=min(consumed,count);
    for(j=0;j<consumed;++j) {
        if(d->keyboard) {
            KEYBOARD_INPUT_DATA* k=(KEYBOARD_INPUT_DATA*)packets+j;
            d->deliveredKeys[KeyIndex(k)]=(UCHAR)((k->Flags&KEY_BREAK)==0);
        } else {
            MOUSE_INPUT_DATA* m=(MOUSE_INPUT_DATA*)packets+j;
            // Motion packets do not change delivered button ownership.
            if(m->ButtonFlags)for(i=0;i<5;++i) {
                if(m->ButtonFlags&(1u<<(2*i)))d->deliveredButtons|=(UCHAR)(1u<<i);
                if(m->ButtonFlags&(1u<<(2*i+1)))d->deliveredButtons&=(UCHAR)~(1u<<i);
            }
        }
    }
    return consumed;
}
static BOOLEAN DeliverLocked(DEVICE_CONTEXT* d,PVOID packet) {
    return (BOOLEAN)(DeliverBatchLocked(d,packet,1)==1);
}
static BOOLEAN RecoverLocked(DEVICE_CONTEXT* d) {
    ULONG i;
    if(!d->recover)return TRUE;
    if(!d->connection.ClassService)return FALSE;
    if(d->keyboard) {
        KEYBOARD_INPUT_DATA keys[32];ULONG count=0;
        for(i=0;i<384;++i)if(d->deliveredKeys[i]) {
            KEYBOARD_INPUT_DATA* k=&keys[count++];RtlZeroMemory(k,sizeof(*k));
            k->MakeCode=(USHORT)(i%128);k->Flags=KEY_BREAK|(i>=256?KEY_E1:i>=128?KEY_E0:0);
            if(count==RTL_NUMBER_OF(keys)) {
                if(DeliverBatchLocked(d,keys,count)!=count)return FALSE;
                count=0;
            }
        }
        if(count && DeliverBatchLocked(d,keys,count)!=count)return FALSE;
    } else if(d->deliveredButtons) {
        MOUSE_INPUT_DATA m={0};
        for(i=0;i<5;++i)if(d->deliveredButtons&(1u<<i))m.ButtonFlags|=(USHORT)(1u<<(2*i+1));
        if(!DeliverLocked(d,&m))return FALSE;
    }
    InterlockedExchange(&d->recover,FALSE);return TRUE;
}
BOOLEAN IcRecovery(DEVICE_CONTEXT* d) {
    BOOLEAN ready;
    if(!InterlockedCompareExchange(&d->recover,0,0))return TRUE;
    WdfWaitLockAcquire(gGate,NULL);ready=RecoverLocked(d);WdfWaitLockRelease(gGate);
    return ready;
}
NTSTATUS IcRegister(DEVICE_CONTEXT* d) {
    WdfWaitLockAcquire(gGate,NULL);
    InsertTailList(&gDevices,&d->link);d->registered=TRUE;
    WdfWaitLockRelease(gGate);return STATUS_SUCCESS;
}
static BOOLEAN IcPhysicalChordLocked(void) {
    BOOLEAN ctrl=FALSE,alt=FALSE,del=FALSE;PLIST_ENTRY p;
    for(p=gDevices.Flink;p!=&gDevices;p=p->Flink) {
        DEVICE_CONTEXT* other=CONTAINING_RECORD(p,DEVICE_CONTEXT,link);
        ctrl|=other->physicalCtrl;alt|=other->physicalAlt;del|=other->physicalDelete;
    }
    return (BOOLEAN)(ctrl&&alt&&del);
}
void IcUnregister(DEVICE_CONTEXT* d) {
    WdfWaitLockAcquire(gGate,NULL);
    if(d->registered) {
        if(gOwners[d->keyboard?0:1] && d->captureId)ResetLocked(IC_ALL_STREAMS,IC_RESET_DEVICE);
        RemoveEntryList(&d->link);d->registered=FALSE;
        gChord=IcPhysicalChordLocked();
    }
    WdfWaitLockRelease(gGate);
}
void IcDeviceStopping(DEVICE_CONTEXT* d) {
    WdfWaitLockAcquire(gGate,NULL);
    // A replacement snapshot can be created after stop=1 but before this
    // callback acquires gGate. Only a member of the current snapshot may revoke
    // it; CreateLocked clears captureId on stopped devices of that type.
    if(gOwners[d->keyboard?0:1] && d->captureId)ResetLocked(IC_ALL_STREAMS,IC_RESET_DEVICE);
    RecoverLocked(d);
    d->physicalCtrl=d->physicalAlt=d->physicalDelete=FALSE;
    gChord=IcPhysicalChordLocked();
    WdfWaitLockRelease(gGate);
}
void IcDeviceReadyLocked(DEVICE_CONTEXT* d) {
    // Called under gGate in the same critical section that publishes thread.
    // Snapshots have fixed membership. A new input source revokes the old
    // capture instead of silently reusing or shifting its IDs.
    FILE_CONTEXT* f=gOwners[d->keyboard?0:1];ULONG stream=d->keyboard?0:1;
    if(!f)return;
    if(!f->allCapture) {ResetLocked(IC_ALL_STREAMS,IC_RESET_DEVICE);return;}
    if(d->captureId || d->stop || !d->thread || !d->connection.ClassService)return;
    if(!IcAllAttachLocked(stream+1))return;
    d->captureId=++f->counts[stream];
    f->targets[(stream?IC_MAX_DEVICES:0)+d->captureId-1]=d;
}
void IcPhysicalKeys(DEVICE_CONTEXT* d,const UCHAR current[256]) {
    BOOLEAN chord;
    // keys is owned by this device's reader; no global lock or device walk is
    // needed when the three physical modifier states have not changed.
    if((BOOLEAN)(d->keys[0xE0]||d->keys[0xE4])==(BOOLEAN)(current[0xE0]||current[0xE4]) &&
       (BOOLEAN)(d->keys[0xE2]||d->keys[0xE6])==(BOOLEAN)(current[0xE2]||current[0xE6]) &&
       (BOOLEAN)(d->keys[0x4C]!=0)==(BOOLEAN)(current[0x4C]!=0))return;
    WdfWaitLockAcquire(gGate,NULL);
    if(d->physicalCtrl==(BOOLEAN)(current[0xE0]||current[0xE4]) &&
       d->physicalAlt==(BOOLEAN)(current[0xE2]||current[0xE6]) &&
       d->physicalDelete==(BOOLEAN)(current[0x4C]!=0)) { WdfWaitLockRelease(gGate);return; }
    d->physicalCtrl=(BOOLEAN)(current[0xE0]||current[0xE4]);
    d->physicalAlt=(BOOLEAN)(current[0xE2]||current[0xE6]);
    d->physicalDelete=(BOOLEAN)(current[0x4C]!=0);
    chord=IcPhysicalChordLocked();
    if(chord&&!gChord) {
        ULONG i;for(i=0;i<IC_STREAM_COUNT;++i)if(gOwners[i]){ResetLocked(IC_ALL_STREAMS,IC_RESET_HOTKEY);break;}
    }
    gChord=chord;
    WdfWaitLockRelease(gGate);
}
void IcRoute(DEVICE_CONTEXT* d,PVOID packet) {
    ULONG stream=d->keyboard?0:1,retryMs=1;
    while(!InterlockedCompareExchange(&d->stop,0,0)) {
        BOOLEAN done=FALSE;
        WdfWaitLockAcquire(gGate,NULL);
        // Stop may have won the gate after the outer check. A newer snapshot
        // can already exclude this reader; it must neither route nor revoke it.
        if(d->stop) {WdfWaitLockRelease(gGate);break;}
        if(gOwners[stream] && !d->captureId) {IcDeviceReadyLocked(d);if(gOwners[stream] && !d->captureId)ResetLocked(IC_ALL_STREAMS,IC_RESET_DEVICE);}
        if(gOwners[stream] && gRings[stream].count==IC_CAPACITY) {
            ++gOverflows;ResetLocked(IC_ALL_STREAMS,IC_RESET_OVERFLOW);
        }
        if(gOwners[stream]) {
            ULONG tail=IcRingTail(&gRings[stream]);
            ULONGLONG sequence=++gSequence,timestamp=KeQueryPerformanceCounter(NULL).QuadPart;
            if(d->keyboard) {
                KEYBOARD_INPUT_DATA* k=packet;IC_KEYBOARD_INPUT* out=&gKeyboard[tail];
                RtlZeroMemory(out,sizeof(*out));out->sequence=sequence;out->timestamp=timestamp;out->device=d->captureId;
                out->stroke.unit_id=k->UnitId;out->stroke.make_code=k->MakeCode;out->stroke.flags=k->Flags;
                out->stroke.extra_information=k->ExtraInformation;
            } else {
                MOUSE_INPUT_DATA* m=packet;IC_MOUSE_INPUT* out=&gMouse[tail];
                RtlZeroMemory(out,sizeof(*out));out->sequence=sequence;out->timestamp=timestamp;out->device=d->captureId;
                out->stroke.unit_id=m->UnitId;out->stroke.flags=m->Flags;
                out->stroke.button_flags=m->ButtonFlags;out->stroke.button_data=m->ButtonData;
                out->stroke.raw_buttons=m->RawButtons;out->stroke.x=m->LastX;out->stroke.y=m->LastY;
                out->stroke.extra_information=m->ExtraInformation;
            }
            if(++gRings[stream].count==1)
                KeSetEvent(gOwners[stream]->events[stream],IO_NO_INCREMENT,FALSE);
            done=TRUE;
        } else if(RecoverLocked(d))done=DeliverLocked(d,packet);
        WdfWaitLockRelease(gGate);
        if(done)return;
        {
            PVOID events[2]={&d->stopEvent,&d->maintenanceEvent};LARGE_INTEGER delay;
            delay.QuadPart=-(LONGLONG)retryMs*10000;
            if(KeWaitForMultipleObjects(2,events,WaitAny,Executive,KernelMode,FALSE,&delay,NULL)==STATUS_WAIT_0)return;
            retryMs=min(retryMs*2,16u);
        }
    }
}

void IcFileCreate(WDFDEVICE device,WDFREQUEST request,WDFFILEOBJECT file) {
    NTSTATUS status=STATUS_DEVICE_NOT_CONNECTED;
    WdfWaitLockAcquire(gLifetime,NULL);
    if(device==gControl) { ++gOpenFiles;IcFile(file)->counted=TRUE;status=STATUS_SUCCESS; }
    WdfWaitLockRelease(gLifetime);WdfRequestComplete(request,status);
}
void IcFileClose(WDFFILEOBJECT file) {
    WdfWaitLockAcquire(gLifetime,NULL);
    if(IcFile(file)->counted) {
        IcFile(file)->counted=FALSE;NT_ASSERT(gOpenFiles);--gOpenFiles;
        if(!gOpenFiles && !gFdos)IcControlDelete();
    }
    WdfWaitLockRelease(gLifetime);
}
void IcFileCleanup(WDFFILEOBJECT file) {
    FILE_CONTEXT* f=IcFile(file);ULONG i;
    WdfWaitLockAcquire(gGate,NULL);
    f->closed=TRUE;
    IcMonitorClose(&f->monitor);
    IcHidMonitorChangedLocked();IcXinputHidMonitorChangedLocked();
    if(f->mask)ResetLocked(f->mask,IC_RESET_CLOSE);
    for(i=0;i<IC_STREAM_COUNT;++i)if(f->events[i]) { ObDereferenceObject(f->events[i]);f->events[i]=NULL; }
    if(f->revoked) { ObDereferenceObject(f->revoked);f->revoked=NULL; }
    if(f->targets) {ExFreePoolWithTag(f->targets,FILTER_TAG);f->targets=NULL;}
    WdfWaitLockRelease(gGate);
}
static NTSTATUS QueryDeviceLocked(ULONG type,ULONG token,BOOLEAN ordinal,IC_DEVICE_QUERY_RESULT* result) {
    NTSTATUS status=STATUS_DEVICE_NOT_CONNECTED;
    if(!IcProductTypeAllowed(type))return STATUS_NOT_SUPPORTED;
    if(type<=IC_TYPE_MOUSE) {
        PLIST_ENTRY p;ULONG position=0;
        for(p=gDevices.Flink;p!=&gDevices;p=p->Flink) {
            DEVICE_CONTEXT* d=CONTAINING_RECORD(p,DEVICE_CONTEXT,link);
            if(d->info.type!=type)continue;
            if((ordinal?++position:d->id)!=token)continue;
            result->instance_token=d->id;
            result->flags=(d->thread && !d->stop && d->connection.ClassService?1u:0u)|
                (d->thread && !d->stop?2u:0u)|(d->connection.ClassService?16u:0u);
            result->last_read_status=(ULONG)d->readStatus;result->report_bytes=d->info.input_bytes;
            RtlCopyMemory(result->instance,d->instance,sizeof(result->instance));
            if(result->instance[0])result->flags|=IC_QUERY_IDENTITY_VALID;
            status=STATUS_SUCCESS;break;
        }
    } else if(type==IC_TYPE_CONTROLLER_HID)status=IcHidQueryLocked(token,result,ordinal);
#ifdef INTERCEPTOR_EXPERIMENTAL_XUSB
    else if(type==IC_TYPE_CONTROLLER_XUSB)status=IcXusbQueryLocked(token,result,ordinal);
#endif
#ifdef INTERCEPTOR_EXPERIMENTAL_XINPUTHID
    else if(type==IC_TYPE_CONTROLLER_XINPUTHID)status=IcXinputHidQueryLocked(token,result,ordinal);
#endif
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
    else if(type==IC_TYPE_CONTROLLER_GIP)status=IcGipQueryLocked(token,result,ordinal);
#endif
    if(NT_SUCCESS(status)){IcDeviceAvailability(result,result->reserved[1]);IcTrialDeviceStateLocked(result);}
    return status;
}
static NTSTATUS FamilyUnavailableLocked(ULONG type) {
    ULONG ordinal;BOOLEAN present=FALSE,preparing=FALSE;
    for(ordinal=1;ordinal<=IC_MAX_DEVICES;++ordinal) {
        IC_DEVICE_QUERY_RESULT result={0};NTSTATUS status;
        result.type=type;status=QueryDeviceLocked(type,ordinal,TRUE,&result);
        if(status==STATUS_DEVICE_NOT_CONNECTED)break;
        if(!NT_SUCCESS(status))return status;
        present=TRUE;
        if(result.reserved[0]!=IC_DEVICE_UNSUPPORTED)preparing=TRUE;
    }
    // Presence and readiness are different. Never call a registered pending
    // reader disconnected, or create an empty keyboard/mouse-only context.
    return !present?STATUS_DEVICE_NOT_CONNECTED:preparing?STATUS_DEVICE_NOT_READY:STATUS_NOT_SUPPORTED;
}
static NTSTATUS SnapshotLocked(FILE_CONTEXT* f,PVOID output,SIZE_T capacity,SIZE_T* written) {
    IC_CREATE_RESULT* header=output;IC_DEVICE_INFO* out;ULONG i,n=0,stream;
    if(f->closed)return STATUS_FILE_CLOSED;
    if(!f->mask || !f->allCapture)return STATUS_CANCELLED;
    if(capacity<sizeof(*header)+(SIZE_T)f->deviceCount*sizeof(IC_DEVICE_INFO))return STATUS_BUFFER_TOO_SMALL;
    out=(IC_DEVICE_INFO*)(header+1);
    for(stream=0;stream<2;++stream)for(i=0;i<f->counts[stream];++i) {
        DEVICE_CONTEXT* d=f->targets[(stream?IC_MAX_DEVICES:0)+i];out[n]=d->info;out[n++].id=i+1;
    }
    n+=IcHidSnapshotLocked(out+n,IC_MAX_DEVICES-n);
    n+=IcXusbSnapshotLocked(out+n,IC_MAX_DEVICES-n);
    n+=IcXinputHidSnapshotLocked(out+n,IC_MAX_DEVICES-n);
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
    n+=IcGipSnapshotLocked(out+n,IC_MAX_DEVICES-n);
#endif
    NT_ASSERT(n==f->deviceCount);
    header->version=IC_VERSION;header->count=n;header->generation=gGeneration;
    *written=sizeof(*header)+(SIZE_T)n*sizeof(*out);return STATUS_SUCCESS;
}
static NTSTATUS CreateLocked(FILE_CONTEXT* f,const IC_CREATE* in,ULONGLONG xusbEvent,ULONGLONG xinputHidEvent,ULONGLONG gipEvent,PVOID output,size_t capacity,size_t* written,BOOLEAN all) {
    NTSTATUS status=STATUS_SUCCESS;ULONG i;PKEVENT events[IC_STREAM_COUNT]={0},revoked=NULL;
    ULONG count=0,ids[2]={0};PLIST_ENTRY entry;IC_CREATE_RESULT* header=output;IC_DEVICE_INFO* devices;
    DEVICE_CONTEXT** targets=NULL;ULONG counts[2]={0},hidCount=0,xusbCount=0,xinputHidCount=0,gipCount=0;BOOLEAN hidStarted=FALSE,xusbStarted=FALSE,xinputHidStarted=FALSE,gipStarted=FALSE;
    UCHAR mask=in->mask|((in->controller_mask&IC_CONTROLLER_HID)?IC_READY_HID:0)|((in->controller_mask&IC_CONTROLLER_XUSB)?IC_READY_XUSB:0)|((in->controller_mask&IC_CONTROLLER_XINPUTHID)?IC_READY_XINPUTHID:0)|((in->controller_mask&IC_CONTROLLER_GIP)?IC_READY_GIP:0);
    if(all) {
        UCHAR expected=IC_CONTROLLER_HID;
#ifdef INTERCEPTOR_EXPERIMENTAL_XUSB
        expected|=IC_CONTROLLER_XUSB;
#endif
#ifdef INTERCEPTOR_EXPERIMENTAL_XINPUTHID
        expected|=IC_CONTROLLER_XINPUTHID;
#endif
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
        expected|=IC_CONTROLLER_GIP;
#endif
        if(in->mask!=IC_PRODUCT_INPUT_MASK || in->controller_mask!=(expected & IC_PRODUCT_CONTROLLER_MASK))return STATUS_INVALID_PARAMETER;
    }
    if(f->closed)return STATUS_FILE_CLOSED;
    if(!IcProductCreateMasksAllowed(in->mask,in->controller_mask))return STATUS_NOT_SUPPORTED;
    if(in->controller_mask&~(IC_CONTROLLER_HID|IC_CONTROLLER_XUSB|IC_CONTROLLER_XINPUTHID|IC_CONTROLLER_GIP))return STATUS_NOT_SUPPORTED;
#ifndef INTERCEPTOR_EXPERIMENTAL_XUSB
    if(in->controller_mask&IC_CONTROLLER_XUSB)return STATUS_NOT_SUPPORTED;
#endif
#ifndef INTERCEPTOR_EXPERIMENTAL_XINPUTHID
    if(in->controller_mask&IC_CONTROLLER_XINPUTHID)return STATUS_NOT_SUPPORTED;
#endif
#ifndef INTERCEPTOR_EXPERIMENTAL_GIP
    if(in->controller_mask&IC_CONTROLLER_GIP)return STATUS_NOT_SUPPORTED;
#endif
    if(in->version!=IC_VERSION || !mask || (in->mask&~IC_BOTH) || in->reserved[0] || in->reserved[1])return STATUS_INVALID_PARAMETER;
    IcTrialCheckLocked();status=IcTrialAccessLocked();if(!NT_SUCCESS(status))return status;
    if(f->revoked)return STATUS_INVALID_DEVICE_STATE;
    // Do not let a new capture swallow the already-held emergency chord.
    // The next physical transition must release the chord before capture resumes.
    if(gChord)return STATUS_DEVICE_BUSY;
    for(i=0;i<IC_STREAM_COUNT;++i)if((mask&(1u<<i))&&gOwners[i])return STATUS_SHARING_VIOLATION;
    for(entry=gDevices.Flink;entry!=&gDevices;entry=entry->Flink) {
        DEVICE_CONTEXT* d=CONTAINING_RECORD(entry,DEVICE_CONTEXT,link);
        if((in->mask&(d->keyboard?IC_KEYBOARD:IC_MOUSE)) && !d->stop && d->thread && d->connection.ClassService) {++count;++counts[d->keyboard?0:1];}
    }
    if(mask&IC_READY_HID)hidCount=IcHidCountLocked();
    if(mask&IC_READY_XUSB)xusbCount=IcXusbCountLocked();
    if(mask&IC_READY_XINPUTHID)xinputHidCount=IcXinputHidCountLocked();
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
    if(mask&IC_READY_GIP)gipCount=IcGipCountLocked();
#endif
    if(!all && (mask&IC_KEYBOARD) && !counts[0])return FamilyUnavailableLocked(IC_TYPE_KEYBOARD);
    if(!all && (mask&IC_MOUSE) && !counts[1])return FamilyUnavailableLocked(IC_TYPE_MOUSE);
    if(!all && (mask&IC_READY_HID) && !hidCount)return FamilyUnavailableLocked(IC_TYPE_CONTROLLER_HID);
    if(!all && (mask&IC_READY_XUSB) && !xusbCount)return FamilyUnavailableLocked(IC_TYPE_CONTROLLER_XUSB);
    if(!all && (mask&IC_READY_XINPUTHID) && !xinputHidCount)return FamilyUnavailableLocked(IC_TYPE_CONTROLLER_XINPUTHID);
    if(!all && (mask&IC_READY_GIP) && !gipCount)return FamilyUnavailableLocked(IC_TYPE_CONTROLLER_GIP);
    count+=hidCount+xusbCount+xinputHidCount+gipCount;
    if(count>IC_MAX_DEVICES)return STATUS_INSUFFICIENT_RESOURCES;
    if(capacity<sizeof(*header)+(SIZE_T)count*sizeof(IC_DEVICE_INFO))return STATUS_BUFFER_TOO_SMALL;
    targets=ExAllocatePoolZero(NonPagedPoolNx,(SIZE_T)(all?(IC_PRODUCT_HAS_KEYBOARD_MOUSE?2*IC_MAX_DEVICES:1):max(count,1u))*sizeof(*targets),FILTER_TAG);
    if(!targets)return STATUS_INSUFFICIENT_RESOURCES;
    // Event handles are referenced in the requestor's process by EvtIoInCallerContext.
    for(i=0;i<IC_STREAM_COUNT;++i)if(mask&(1u<<i)) {
        ULONG_PTR handle=(ULONG_PTR)(i==5?gipEvent:i==4?xinputHidEvent:i==3?xusbEvent:i==2?in->controller_hid_event:i?in->mouse_event:in->keyboard_event);
        status=ObReferenceObjectByHandle((HANDLE)handle,EVENT_MODIFY_STATE,*ExEventObjectType,UserMode,(PVOID*)&events[i],NULL);
        if(!NT_SUCCESS(status))goto fail;
    }
    status=ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)in->revoked_event,EVENT_MODIFY_STATE,*ExEventObjectType,UserMode,(PVOID*)&revoked,NULL);
    if(!NT_SUCCESS(status))goto fail;
    // Reusing one event for readiness and revocation would make waits ambiguous.
    for(i=0;i<IC_STREAM_COUNT;++i) {ULONG j;if(revoked==events[i]) {status=STATUS_INVALID_PARAMETER;goto fail;}
        for(j=0;j<i;++j)if(events[i] && events[i]==events[j]) {status=STATUS_INVALID_PARAMETER;goto fail;}}
    devices=(IC_DEVICE_INFO*)(header+1);
    if(mask&IC_READY_HID) {
        status=all?IcHidArmLocked(events[2]):IcHidCaptureLocked(events[2],devices+counts[0]+counts[1],hidCount,&hidCount);
        if(!NT_SUCCESS(status))goto fail;
        hidStarted=TRUE;
    }
    if(mask&IC_READY_XUSB) {
        status=all?IcXusbArmLocked(events[3]):IcXusbCaptureLocked(events[3],devices+counts[0]+counts[1]+hidCount,xusbCount,&xusbCount);
        if(!NT_SUCCESS(status))goto fail;
        xusbStarted=TRUE;
    }
    if(mask&IC_READY_XINPUTHID) {
        status=all?IcXinputHidArmLocked(events[4]):IcXinputHidCaptureLocked(events[4],devices+counts[0]+counts[1]+hidCount+xusbCount,xinputHidCount,&xinputHidCount);
        if(!NT_SUCCESS(status))goto fail;
        xinputHidStarted=TRUE;
    }
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
    if(mask&IC_READY_GIP) {
        status=all?IcGipArmLocked(events[5]):IcGipCaptureLocked(events[5],devices+counts[0]+counts[1]+hidCount+xusbCount+xinputHidCount,gipCount,&gipCount);
        if(!NT_SUCCESS(status))goto fail;
        gipStarted=TRUE;
    }
#endif
    // Last fallible step: only an otherwise successful context starts the
    // budget. Persistence is written once per boot, before ownership publishes.
    status=IcTrialCommitLocked();if(!NT_SUCCESS(status))goto fail;
    f->revoked=revoked;KeClearEvent(revoked);
    f->mask=mask;f->allCapture=all;f->deviceCount=counts[0]+counts[1];
    f->targets=targets;f->counts[0]=counts[0];f->counts[1]=counts[1];
    header->version=IC_VERSION;header->count=count;header->generation=gGeneration;
    devices=(IC_DEVICE_INFO*)(header+1);count=0;
    for(entry=gDevices.Flink;entry!=&gDevices;entry=entry->Flink) {
        DEVICE_CONTEXT* d=CONTAINING_RECORD(entry,DEVICE_CONTEXT,link);
        if(!(in->mask&(d->keyboard?IC_KEYBOARD:IC_MOUSE)))continue;
        d->captureId=0;
        if(!d->stop && d->thread && d->connection.ClassService) {
            d->captureId=++ids[d->keyboard?0:1];devices[count]=d->info;
            devices[count++].id=d->captureId;
            targets[(d->keyboard?0:all?IC_MAX_DEVICES:counts[0])+d->captureId-1]=d;
        }
    }
    *written=sizeof(*header)+(SIZE_T)(count+hidCount+xusbCount+xinputHidCount+gipCount)*sizeof(IC_DEVICE_INFO);
    for(i=0;i<IC_STREAM_COUNT;++i)if(events[i]) {
        f->events[i]=events[i];gOwners[i]=f;if(i<2)gRings[i].head=gRings[i].count=0;
        KeClearEvent(events[i]);
    }
    {
        PLIST_ENTRY p;
        // A reader waiting for room in the class queue can now enqueue to the
        // capture FIFO. Wake it instead of waiting for its retry deadline.
        for(p=gDevices.Flink;p!=&gDevices;p=p->Flink) {
            DEVICE_CONTEXT* d=CONTAINING_RECORD(p,DEVICE_CONTEXT,link);
            if(in->mask&(d->keyboard?IC_KEYBOARD:IC_MOUSE))KeSetEvent(&d->maintenanceEvent,IO_NO_INCREMENT,FALSE);
        }
    }
    if(all) {
        IcAllReadersReadyLocked();
        return SnapshotLocked(f,output,capacity,written);
    }
    return STATUS_SUCCESS;
fail:
    if(hidStarted)IcHidResetLocked();
    if(xusbStarted)IcXusbResetLocked();
    if(xinputHidStarted)IcXinputHidResetLocked();
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
    if(gipStarted)IcGipResetLocked();
#else
    UNREFERENCED_PARAMETER(gipStarted);
#endif
    if(targets)ExFreePoolWithTag(targets,FILTER_TAG);
    if(revoked)ObDereferenceObject(revoked);
    for(i=0;i<IC_STREAM_COUNT;++i)if(events[i])ObDereferenceObject(events[i]);
    return status;
}
static NTSTATUS GetLocked(FILE_CONTEXT* f,ULONG stream,ULONG requested,PVOID out,size_t outBytes,size_t* bytes) {
    LONG n;ULONG size=stream?sizeof(IC_MOUSE_INPUT):sizeof(IC_KEYBOARD_INPUT);
    PUCHAR array=stream?(PUCHAR)gMouse:(PUCHAR)gKeyboard;
    if(gOwners[stream]!=f)return STATUS_CANCELLED;
    n=IcRingRead(&gRings[stream],array,size,out,(ULONG)min(outBytes/size,IC_CAPACITY),requested);
    if(n<0)return STATUS_BUFFER_TOO_SMALL;
    *bytes=(SIZE_T)n*size;
    if(!gRings[stream].count)KeClearEvent(f->events[stream]);
    return STATUS_SUCCESS;
}
static ULONG SelectedReadType(ULONG code) {
    switch(code) {
    case IOCTL_IC_GET_KEYBOARD:return IC_TYPE_KEYBOARD;
    case IOCTL_IC_GET_MOUSE:return IC_TYPE_MOUSE;
    case IOCTL_IC_GET_HID:return IC_TYPE_CONTROLLER_HID;
    case IOCTL_IC_GET_XUSB:return IC_TYPE_CONTROLLER_XUSB;
    case IOCTL_IC_GET_XINPUTHID:return IC_TYPE_CONTROLLER_XINPUTHID;
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
    case IOCTL_IC_GET_GIP:case IOCTL_IC_GET_GIP_RAW:return IC_TYPE_CONTROLLER_GIP;
#endif
    default:return 0;
    }
}
static NTSTATUS GetSelectedLocked(FILE_CONTEXT* owner,ULONG code,const IC_GET_SELECT* request,
    PVOID output,SIZE_T length,SIZE_T* written) {
    const ULONG type=SelectedReadType(code);
    if(!IcSelectedGetValid(request) || !type)return STATUS_INVALID_PARAMETER;
    if(gOwners[type-1]!=owner)return STATUS_CANCELLED;
    switch(type) {
    case IC_TYPE_KEYBOARD:case IC_TYPE_MOUSE: {
        const ULONG stream=type-1;
        const ULONG stride=stream?sizeof(IC_MOUSE_INPUT):sizeof(IC_KEYBOARD_INPUT);
        const BOOLEAN consume=(request->flags&IC_GET_PEEK)==0;
        LONG n=IcInputReadSelected(&gRings[stream].head,&gRings[stream].count,
            stream?(PVOID)gMouse:(PVOID)gKeyboard,IC_CAPACITY,stride,
            FIELD_OFFSET(IC_KEYBOARD_INPUT,device),output,stride,(ULONG)min(length/stride,IC_CAPACITY),
            request->base.count,request->device,consume);
        if(n<0)return STATUS_BUFFER_TOO_SMALL;
        if(consume && !gRings[stream].count)KeClearEvent(owner->events[stream]);
        *written=(SIZE_T)n*stride;return STATUS_SUCCESS;
    }
    case IC_TYPE_CONTROLLER_HID:return IcHidGetSelectedLocked(request,output,length,written);
    case IC_TYPE_CONTROLLER_XUSB:return IcXusbGetSelectedLocked(request,output,length,written);
    case IC_TYPE_CONTROLLER_XINPUTHID:return IcXinputHidGetSelectedLocked(request,output,length,written);
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
    case IC_TYPE_CONTROLLER_GIP:return IcGipGetSelectedLocked(request,output,length,written,code==IOCTL_IC_GET_GIP_RAW);
#endif
    default:return STATUS_NOT_SUPPORTED;
    }
}
static DEVICE_CONTEXT* FindDeviceLocked(ULONG id,BOOLEAN keyboard,FILE_CONTEXT* owner) {
    PLIST_ENTRY p;DEVICE_CONTEXT* first=NULL;ULONG ordinal=0;
    if(owner) {
        ULONG resolved=IcResolveDeviceId(id,owner->counts[keyboard?0:1]);
        ULONG base=keyboard?0:owner->allCapture?IC_MAX_DEVICES:owner->counts[0],i;DEVICE_CONTEXT* target;
        // Caller has checked ownership under gGate. Device removal revokes it
        // before freeing the target, so this O(1) lookup cannot use a stale node.
        if(!resolved)return NULL;
        target=owner->targets[base+resolved-1];
        if(!target->stop && target->thread && target->connection.ClassService)return target;
        for(i=0;i<owner->counts[keyboard?0:1];++i) {
            target=owner->targets[base+i];
            if(!target->stop && target->thread && target->connection.ClassService)return target;
        }
        return NULL;
    }
    for(p=gDevices.Flink;p!=&gDevices;p=p->Flink) {
        DEVICE_CONTEXT* d=CONTAINING_RECORD(p,DEVICE_CONTEXT,link);
        if(d->keyboard!=keyboard || d->stop || !d->thread || !d->connection.ClassService)continue;
        ++ordinal;if(!first)first=d;
        if(id==ordinal)return d;
    }
    return first; // Unknown ID, including zero, falls back within this type only.
}
static NTSTATUS SendLocked(BOOLEAN keyboard,FILE_CONTEXT* owner,PVOID in,size_t length,IC_SEND_RESULT* result) {
    SIZE_T size=keyboard?sizeof(IC_KEYBOARD_INPUT):sizeof(IC_MOUSE_INPUT);
    ULONG i,count=(ULONG)(length/size);
    if(!length || length%size || count>IC_MAX_SEND)return STATUS_INVALID_PARAMETER;
    // Validate the entire batch before performing any callback.
    for(i=0;i<count;++i) {
        if(keyboard) {
            IC_KEYBOARD_INPUT* k=(IC_KEYBOARD_INPUT*)in+i;
            if(!IcValidKeyboard(k))return STATUS_INVALID_PARAMETER;
        } else {
            IC_MOUSE_INPUT* m=(IC_MOUSE_INPUT*)in+i;
            if(!IcValidMouse(m))return STATUS_INVALID_PARAMETER;
        }
    }
    result->accepted=0;result->status=STATUS_SUCCESS;
    for(i=0;i<count;) {
        // Bounded native batches reduce callback/IRQL transitions without
        // reserving a full 256-packet array on the kernel stack.
        union { KEYBOARD_INPUT_DATA keys[32];MOUSE_INPUT_DATA mice[32]; } batch;
        ULONG id=keyboard?((IC_KEYBOARD_INPUT*)in)[i].device:((IC_MOUSE_INPUT*)in)[i].device;
        ULONG n=0,consumed;DEVICE_CONTEXT* d=FindDeviceLocked(id,keyboard,owner);
        if(!d) { result->status=(ULONG)STATUS_DEVICE_NOT_CONNECTED;break; }
        if(!RecoverLocked(d)) { result->status=(ULONG)STATUS_DEVICE_BUSY;break; }
        // Initialize only submitted packets: a one-packet Send must not clear
        // the unused remainder of this 32-packet scratch buffer.
        while(n<32 && i+n<count) {
            ULONG nextId=keyboard?((IC_KEYBOARD_INPUT*)in)[i+n].device:((IC_MOUSE_INPUT*)in)[i+n].device;
            if(nextId!=id)break;
            if(keyboard) {
                IC_KEYBOARD_STROKE* src=&((IC_KEYBOARD_INPUT*)in)[i+n].stroke;
                KEYBOARD_INPUT_DATA* k=&batch.keys[n];
                RtlZeroMemory(k,sizeof(*k));
                k->UnitId=src->unit_id;k->MakeCode=src->make_code;k->Flags=src->flags;k->ExtraInformation=src->extra_information;
            } else {
                IC_MOUSE_STROKE* src=&((IC_MOUSE_INPUT*)in)[i+n].stroke;
                MOUSE_INPUT_DATA* m=&batch.mice[n];
                RtlZeroMemory(m,sizeof(*m));
                m->UnitId=src->unit_id;m->Flags=src->flags;m->ButtonFlags=src->button_flags;m->ButtonData=src->button_data;
                m->RawButtons=src->raw_buttons;m->LastX=src->x;m->LastY=src->y;m->ExtraInformation=src->extra_information;
            }
            ++n;
        }
        consumed=DeliverBatchLocked(d,&batch,n);result->accepted+=consumed;i+=consumed;
        if(consumed!=n) { result->status=(ULONG)STATUS_DEVICE_BUSY;break; }
    }
    return STATUS_SUCCESS;
}
// CREATE references user event handles, so it must run in the caller's process.
EVT_WDF_IO_IN_CALLER_CONTEXT IcCallerContext;
void IcCallerContext(WDFDEVICE device,WDFREQUEST request) {
    WDF_REQUEST_PARAMETERS p;NTSTATUS status;
    WDF_REQUEST_PARAMETERS_INIT(&p);WdfRequestGetParameters(request,&p);
    if(p.Type==WdfRequestTypeDeviceControl && !IcProductIoctlFunctionAllowed((p.Parameters.DeviceIoControl.IoControlCode>>2)&0xFFFu)) {
        WdfRequestComplete(request,STATUS_NOT_SUPPORTED);return;
    }
    if(p.Type==WdfRequestTypeDeviceControl && p.Parameters.DeviceIoControl.IoControlCode==IOCTL_IC_MONITOR_SUBSCRIBE) {
        IC_MONITOR_SUBSCRIBE* input;WDFFILEOBJECT file=WdfRequestGetFileObject(request);
        if(WdfRequestGetRequestorMode(request)!=UserMode || !file){WdfRequestComplete(request,STATUS_ACCESS_DENIED);return;}
        status=WdfRequestRetrieveInputBuffer(request,sizeof(*input),(PVOID*)&input,NULL);
        if(NT_SUCCESS(status)) {
            if(p.Parameters.DeviceIoControl.InputBufferLength!=sizeof(*input) || p.Parameters.DeviceIoControl.OutputBufferLength)
                status=STATUS_INVALID_PARAMETER;
            else {IC_MONITOR_SUBSCRIBE copy=*input;FILE_CONTEXT* f=IcFile(file);
                WdfWaitLockAcquire(gGate,NULL);
                IcTrialCheckLocked();
                status=f->closed?STATUS_FILE_CLOSED:copy.event_handle?IcTrialAccessLocked():STATUS_SUCCESS;
                if(NT_SUCCESS(status)) {
                    status=IcMonitorSubscribe(&f->monitor,&copy);
                    if(NT_SUCCESS(status) && copy.event_handle) {
                        status=IcTrialCommitLocked();
                        if(!NT_SUCCESS(status))IcMonitorClose(&f->monitor);
                    }
                }
                if(NT_SUCCESS(status)){IcHidMonitorChangedLocked();IcXinputHidMonitorChangedLocked();}
                WdfWaitLockRelease(gGate);
            }
        }
        WdfRequestComplete(request,status);return;
    }
    if(p.Type==WdfRequestTypeDeviceControl && (p.Parameters.DeviceIoControl.IoControlCode==IOCTL_IC_CREATE || p.Parameters.DeviceIoControl.IoControlCode==IOCTL_IC_CREATE_XUSB || p.Parameters.DeviceIoControl.IoControlCode==IOCTL_IC_CREATE_NATIVE || p.Parameters.DeviceIoControl.IoControlCode==IOCTL_IC_CREATE_ALL)) {
        IC_CREATE* in;PVOID output=NULL;size_t capacity=0,written=0;WDFFILEOBJECT file=WdfRequestGetFileObject(request);
        if(WdfRequestGetRequestorMode(request)!=UserMode || !file) { WdfRequestComplete(request,STATUS_ACCESS_DENIED);return; }
        status=WdfRequestRetrieveInputBuffer(request,sizeof(*in),(PVOID*)&in,NULL);
        if(NT_SUCCESS(status)) {
            BOOLEAN native=p.Parameters.DeviceIoControl.IoControlCode==IOCTL_IC_CREATE_XUSB;
            BOOLEAN all=p.Parameters.DeviceIoControl.IoControlCode==IOCTL_IC_CREATE_ALL;
            BOOLEAN extended=all || p.Parameters.DeviceIoControl.IoControlCode==IOCTL_IC_CREATE_NATIVE;
            if(p.Parameters.DeviceIoControl.InputBufferLength!=(extended?sizeof(IC_CREATE_NATIVE):native?sizeof(IC_CREATE_XUSB):sizeof(*in)))status=STATUS_INVALID_PARAMETER;
            else {
                IC_CREATE copy=*in;
                ULONGLONG nativeEvent=(native||extended)?((IC_CREATE_XUSB*)in)->xusb_event:0;
                ULONGLONG xinputHidEvent=extended?((IC_CREATE_NATIVE*)in)->xinputhid_event:0;
                ULONGLONG gipEvent=extended?((IC_CREATE_NATIVE*)in)->gip_event:0;
                status=WdfRequestRetrieveOutputBuffer(request,sizeof(IC_CREATE_RESULT),&output,&capacity);
                if(NT_SUCCESS(status)) {WdfWaitLockAcquire(gGate,NULL);status=CreateLocked(IcFile(file),&copy,nativeEvent,xinputHidEvent,gipEvent,output,capacity,&written,all);WdfWaitLockRelease(gGate);}
            }
        }
        WdfRequestCompleteWithInformation(request,status,NT_SUCCESS(status)?written:0);return;
    }
    status=WdfDeviceEnqueueRequest(device,request);
    if(!NT_SUCCESS(status))WdfRequestComplete(request,status);
}
void IcControl(WDFQUEUE queue,WDFREQUEST request,size_t outSize,size_t inSize,ULONG code) {
    NTSTATUS status=STATUS_INVALID_DEVICE_REQUEST;PVOID input=NULL,output=NULL;size_t bytes=0;
    WDFFILEOBJECT file=WdfRequestGetFileObject(request);FILE_CONTEXT* f;
    UNREFERENCED_PARAMETER(queue);
    if(!file) { WdfRequestComplete(request,STATUS_INVALID_HANDLE);return; }
    if(!IcProductIoctlFunctionAllowed((code>>2)&0xFFFu)) {WdfRequestComplete(request,STATUS_NOT_SUPPORTED);return;}
    f=IcFile(file);
    if(inSize) { status=WdfRequestRetrieveInputBuffer(request,inSize,&input,NULL);if(!NT_SUCCESS(status))goto complete; }
    if(outSize) { status=WdfRequestRetrieveOutputBuffer(request,outSize,&output,NULL);if(!NT_SUCCESS(status))goto complete; }
    status=STATUS_INVALID_DEVICE_REQUEST;
    WdfWaitLockAcquire(gGate,NULL);
    // Enforce the deadline at API boundaries even if a passive timer callback
    // is delayed. Device completion/routing paths still have no clock reads.
    IcTrialCheckLocked();
    if(f->closed)status=STATUS_FILE_CLOSED;
    else if(code==IOCTL_IC_MONITOR_QUERY) {
        status=inSize?STATUS_INVALID_PARAMETER:IcMonitorQuery(output,outSize,&bytes);
    } else if(code==IOCTL_IC_MONITOR_READ) {
        status=IcTrialAccessLocked();
        if(!NT_SUCCESS(status)){}
        else if(inSize!=sizeof(IC_MONITOR_READ))status=STATUS_INVALID_PARAMETER;
        else {IC_MONITOR_READ copy=*(IC_MONITOR_READ*)input;status=IcMonitorRead(&f->monitor,&copy,output,outSize,&bytes);}
    } else if(code==IOCTL_IC_MONITOR_DEVICE) {
        IC_MONITOR_DEVICE_REQUEST copy;IC_MONITOR_DEVICE_RESULT* result=output;IC_DEVICE_QUERY_RESULT query;
        if(inSize!=sizeof(copy) || outSize<sizeof(*result))status=STATUS_INVALID_BUFFER_SIZE;
        else {
            copy=*(IC_MONITOR_DEVICE_REQUEST*)input;
            if(copy.version!=IC_MONITOR_VERSION || copy.type<IC_TYPE_CONTROLLER_HID || copy.type>IC_TYPE_CONTROLLER_GIP ||
               !copy.instance_token || (copy.flags&~(IC_MONITOR_BY_ORDINAL|IC_MONITOR_PREPARSED)))status=STATUS_INVALID_PARAMETER;
            else {
                RtlZeroMemory(&query,sizeof(query));status=QueryDeviceLocked(copy.type,copy.instance_token,(copy.flags&IC_MONITOR_BY_ORDINAL)!=0,&query);
                if(NT_SUCCESS(status)) {
                    ULONG descriptorBytes=0;BOOLEAN preparsed=(copy.flags&IC_MONITOR_PREPARSED)!=0;
                    RtlZeroMemory(result,sizeof(*result));result->size=sizeof(*result);result->version=IC_MONITOR_VERSION;result->flags=query.flags;
                    RtlCopyMemory(result->instance,query.instance,sizeof(result->instance));
                    if(copy.type==IC_TYPE_CONTROLLER_HID)status=IcHidMonitorDeviceLocked(query.instance_token,&result->info,result+1,outSize-sizeof(*result),&descriptorBytes,preparsed);
                    else if(copy.type==IC_TYPE_CONTROLLER_XINPUTHID)status=IcXinputHidMonitorDeviceLocked(query.instance_token,&result->info,result+1,outSize-sizeof(*result),&descriptorBytes,preparsed);
                    else if(preparsed)status=STATUS_NOT_SUPPORTED;
                    else if(copy.type==IC_TYPE_CONTROLLER_XUSB)status=IcXusbMonitorDeviceLocked(query.instance_token,&result->info);
                    else status=IcGipMonitorDeviceLocked(query.instance_token,&result->info);
                    if(NT_SUCCESS(status)){result->descriptor_bytes=descriptorBytes;bytes=sizeof(*result)+(preparsed?descriptorBytes:0);}
                }
            }
        }
    } else if(code==IOCTL_IC_TRIAL_STATUS) {
        if(inSize)status=STATUS_INVALID_PARAMETER;
        else if(outSize<sizeof(IC_TRIAL_STATUS))status=STATUS_BUFFER_TOO_SMALL;
        else {IcTrialQueryLocked(output);bytes=sizeof(IC_TRIAL_STATUS);status=STATUS_SUCCESS;}
    }
    else if(code==IOCTL_IC_CONTEXT_DEVICES) {
        if(inSize)status=STATUS_INVALID_PARAMETER;
        else status=SnapshotLocked(f,output,outSize,&bytes);
    }
    else if(code==IOCTL_IC_DEVICE_QUERY) {
        IC_DEVICE_QUERY query;IC_DEVICE_QUERY_RESULT* result=output;
        if(inSize!=sizeof(query) || outSize<sizeof(*result))status=STATUS_INVALID_BUFFER_SIZE;
        else {
            RtlCopyMemory(&query,input,sizeof(query));
            if((query.version!=IC_DEVICE_QUERY_VERSION && query.version!=IC_DEVICE_STATE_VERSION) || query.by_ordinal>1 || query.type>IC_TYPE_CONTROLLER_GIP ||
               (!query.type && (query.instance_token||query.by_ordinal)) || (query.type && !query.instance_token) ||
               (query.by_ordinal && query.instance_token>IC_MAX_DEVICES))status=STATUS_INVALID_PARAMETER;
            else {
                RtlZeroMemory(result,sizeof(*result));result->size=sizeof(*result);result->version=query.version;
                result->type=query.type;result->instance_token=query.instance_token;result->hid_profiles=IC_PRODUCT_HAS_CONTROLLERS?IC_HID_PROFILES_ALL:0;
                status=STATUS_DEVICE_NOT_CONNECTED;
                if(!query.type)status=STATUS_SUCCESS;
                else if(!IcProductTypeAllowed(query.type))status=STATUS_NOT_SUPPORTED;
                else status=QueryDeviceLocked(query.type,query.instance_token,(BOOLEAN)query.by_ordinal,result);
                if(query.version==IC_DEVICE_QUERY_VERSION)RtlZeroMemory(result->reserved,sizeof(result->reserved));
                if(NT_SUCCESS(status))bytes=sizeof(*result);
            }
        }
    } else if(code==IOCTL_IC_CAPABILITIES) {
        if(inSize)status=STATUS_INVALID_PARAMETER;
        else if(outSize<sizeof(IC_CAPABILITIES))status=STATUS_BUFFER_TOO_SMALL;
        else {
            IC_CAPABILITIES caps={0};
            caps.size=sizeof(caps);caps.version=IC_CAPABILITIES_VERSION;caps.abi_version=IC_VERSION;
            caps.input_mask=IC_PRODUCT_INPUT_MASK;caps.controller_mask=IC_PRODUCT_CONTROLLER_MASK & IC_CONTROLLER_HID;
            caps.max_devices=IC_MAX_DEVICES;caps.queue_capacity=IC_CAPACITY;
            caps.max_send=IC_MAX_SEND;caps.hid_report_max=IC_PRODUCT_HAS_CONTROLLERS?IC_HID_REPORT_MAX:0;
#ifdef INTERCEPTOR_EXPERIMENTAL_XUSB
            caps.controller_mask|=IC_CONTROLLER_XUSB;caps.xusb_profiles=IC_PROFILE_XUSB_WIRED_028E|IC_PROFILE_XUSB_WIRED_COMPATIBLE|IC_PROFILE_XUSB_USB_FILTER;
#endif
#ifdef INTERCEPTOR_EXPERIMENTAL_XINPUTHID
            caps.controller_mask|=IC_CONTROLLER_XINPUTHID;caps.xinputhid_profiles=IC_PROFILE_XINPUTHID_GENERIC6;
#endif
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
        caps.controller_mask|=IC_CONTROLLER_GIP;caps.gip_profiles=IC_PROFILE_GIP_USB_GAMEPAD14|IC_PROFILE_GIP_USB_GAMEPAD_RAW;
#endif
            RtlCopyMemory(output,&caps,sizeof(caps));bytes=sizeof(caps);status=STATUS_SUCCESS;
        }
    } else if((code==IOCTL_IC_SEND_KEYBOARD || code==IOCTL_IC_SEND_MOUSE ||
              code==IOCTL_IC_SEND_OWNED_KEYBOARD || code==IOCTL_IC_SEND_OWNED_MOUSE ||
              code==IOCTL_IC_SEND_HID || code==IOCTL_IC_SEND_OWNED_HID ||
              code==IOCTL_IC_SEND_XUSB || code==IOCTL_IC_SEND_OWNED_XUSB ||
              code==IOCTL_IC_SEND_XINPUTHID || code==IOCTL_IC_SEND_OWNED_XINPUTHID ||
              code==IOCTL_IC_SEND_GIP || code==IOCTL_IC_SEND_OWNED_GIP ||
              code==IOCTL_IC_SEND_GIP_RAW || code==IOCTL_IC_SEND_OWNED_GIP_RAW) &&
              !NT_SUCCESS(status=IcTrialSendAccessLocked())) {
        // One cached state check per send batch. Physical routing is unchanged.
    } else if(inSize==sizeof(IC_GET_SELECT) && SelectedReadType(code)) {
        // METHOD_BUFFERED may alias input/output. Copy the request before any
        // report is written; the whole snapshot/removal holds gGate.
        IC_GET_SELECT selected;RtlCopyMemory(&selected,input,sizeof(selected));
        status=GetSelectedLocked(f,code,&selected,output,outSize,&bytes);
    } else if(code==IOCTL_IC_GET_KEYBOARD || code==IOCTL_IC_GET_MOUSE) {
        if(inSize!=sizeof(IC_GET) || ((IC_GET*)input)->version!=IC_VERSION)status=STATUS_INVALID_PARAMETER;
        else { ULONG count=((IC_GET*)input)->count;status=GetLocked(f,code==IOCTL_IC_GET_MOUSE,count,output,outSize,&bytes); }
    } else if(code==IOCTL_IC_SEND_KEYBOARD || code==IOCTL_IC_SEND_MOUSE ||
              code==IOCTL_IC_SEND_OWNED_KEYBOARD || code==IOCTL_IC_SEND_OWNED_MOUSE) {
        IC_SEND_RESULT result;
        BOOLEAN keyboard=(BOOLEAN)(code==IOCTL_IC_SEND_KEYBOARD || code==IOCTL_IC_SEND_OWNED_KEYBOARD);
        BOOLEAN owned=(BOOLEAN)(code==IOCTL_IC_SEND_OWNED_KEYBOARD || code==IOCTL_IC_SEND_OWNED_MOUSE);
        if(owned && gOwners[keyboard?0:1]!=f)status=STATUS_CANCELLED;
        else if(outSize<sizeof(result))status=STATUS_BUFFER_TOO_SMALL;
        else {
            status=SendLocked(keyboard,owned?f:NULL,input,inSize,&result);
            if(NT_SUCCESS(status)) { RtlCopyMemory(output,&result,sizeof(result));bytes=sizeof(result); }
        }
    } else if(code==IOCTL_IC_GET_HID || code==IOCTL_IC_HID_DESCRIPTOR) {
        if(gOwners[2]!=f)status=STATUS_CANCELLED;
        else if(inSize!=sizeof(IC_GET) || ((IC_GET*)input)->version!=IC_VERSION)status=STATUS_INVALID_PARAMETER;
        else if(code==IOCTL_IC_GET_HID)status=IcHidGetLocked(((IC_GET*)input)->count,output,outSize,&bytes);
        else status=IcHidDescriptorLocked(((IC_GET*)input)->count,output,outSize,&bytes);
    } else if(code==IOCTL_IC_SEND_HID || code==IOCTL_IC_SEND_OWNED_HID) {
        IC_SEND_RESULT result;BOOLEAN owned=code==IOCTL_IC_SEND_OWNED_HID;
        if(owned && gOwners[2]!=f)status=STATUS_CANCELLED;
        else if(outSize<sizeof(result))status=STATUS_BUFFER_TOO_SMALL;
        else {
            status=IcHidSendLocked(input,inSize,&result,owned);
            if(NT_SUCCESS(status)) {RtlCopyMemory(output,&result,sizeof(result));bytes=sizeof(result);}
        }
    } else if(code==IOCTL_IC_GET_XINPUTHID || code==IOCTL_IC_XINPUTHID_DESCRIPTOR) {
        if(gOwners[4]!=f)status=STATUS_CANCELLED;
        else if(inSize!=sizeof(IC_GET) || ((IC_GET*)input)->version!=IC_VERSION)status=STATUS_INVALID_PARAMETER;
        else if(code==IOCTL_IC_GET_XINPUTHID)status=IcXinputHidGetLocked(((IC_GET*)input)->count,output,outSize,&bytes);
        else status=IcXinputHidDescriptorLocked(((IC_GET*)input)->count,output,outSize,&bytes);
    } else if(code==IOCTL_IC_SEND_XINPUTHID || code==IOCTL_IC_SEND_OWNED_XINPUTHID) {
        IC_SEND_RESULT result;BOOLEAN owned=code==IOCTL_IC_SEND_OWNED_XINPUTHID;
        if(owned && gOwners[4]!=f)status=STATUS_CANCELLED;
        else if(outSize<sizeof(result))status=STATUS_BUFFER_TOO_SMALL;
        else {status=IcXinputHidSendLocked(input,inSize,&result,owned);if(NT_SUCCESS(status)){RtlCopyMemory(output,&result,sizeof(result));bytes=sizeof(result);}}
    } else if(code==IOCTL_IC_GET_XUSB) {
        if(gOwners[3]!=f)status=STATUS_CANCELLED;
        else if(inSize!=sizeof(IC_GET) || ((IC_GET*)input)->version!=IC_VERSION)status=STATUS_INVALID_PARAMETER;
        else status=IcXusbGetLocked(((IC_GET*)input)->count,output,outSize,&bytes);
    } else if(code==IOCTL_IC_SEND_XUSB || code==IOCTL_IC_SEND_OWNED_XUSB) {
        IC_SEND_RESULT result;BOOLEAN owned=code==IOCTL_IC_SEND_OWNED_XUSB;
        if(owned && gOwners[3]!=f)status=STATUS_CANCELLED;
        else if(outSize<sizeof(result))status=STATUS_BUFFER_TOO_SMALL;
        else {status=IcXusbSendLocked(input,inSize,&result,owned);if(NT_SUCCESS(status)){RtlCopyMemory(output,&result,sizeof(result));bytes=sizeof(result);}}
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
    } else if(code==IOCTL_IC_GIP_DIAGNOSTICS) {
        if(inSize!=sizeof(IC_GET) || ((IC_GET*)input)->version!=IC_VERSION)status=STATUS_INVALID_PARAMETER;
        else {
            status=IcTrialAccessLocked();
            if(NT_SUCCESS(status))status=IcGipDiagnosticsLocked(((IC_GET*)input)->count,output,outSize,&bytes);
            if(NT_SUCCESS(status))status=IcTrialCommitLocked();
            if(!NT_SUCCESS(status))bytes=0;
        }
    } else if(code==IOCTL_IC_GET_GIP || code==IOCTL_IC_GET_GIP_RAW || code==IOCTL_IC_GIP_DESCRIPTOR) {
        if(gOwners[5]!=f)status=STATUS_CANCELLED;
        else if(inSize!=sizeof(IC_GET) || ((IC_GET*)input)->version!=IC_VERSION)status=STATUS_INVALID_PARAMETER;
        else if(code!=IOCTL_IC_GIP_DESCRIPTOR)status=IcGipGetLocked(((IC_GET*)input)->count,output,outSize,&bytes,code==IOCTL_IC_GET_GIP_RAW);
        else status=IcGipDescriptorLocked(((IC_GET*)input)->count,output,outSize,&bytes);
    } else if(code==IOCTL_IC_SEND_GIP || code==IOCTL_IC_SEND_OWNED_GIP || code==IOCTL_IC_SEND_GIP_RAW || code==IOCTL_IC_SEND_OWNED_GIP_RAW) {
        IC_SEND_RESULT result;BOOLEAN owned=code==IOCTL_IC_SEND_OWNED_GIP || code==IOCTL_IC_SEND_OWNED_GIP_RAW;
        if(owned && gOwners[5]!=f)status=STATUS_CANCELLED;
        else if(outSize<sizeof(result))status=STATUS_BUFFER_TOO_SMALL;
        else {status=IcGipSendLocked(input,inSize,&result,owned,code==IOCTL_IC_SEND_GIP_RAW || code==IOCTL_IC_SEND_OWNED_GIP_RAW);if(NT_SUCCESS(status)){RtlCopyMemory(output,&result,sizeof(result));bytes=sizeof(result);}}
#endif
    } else if(code==IOCTL_IC_DESTROY) {
        if(f->mask)ResetLocked(f->mask,IC_RESET_CLOSE);
        status=STATUS_SUCCESS;
    } else if(code==IOCTL_IC_STATUS) {
        IcTrialCheckLocked();
        if(outSize<sizeof(IC_STATUS))status=STATUS_BUFFER_TOO_SMALL;
        else {
            IC_STATUS* s=output;PLIST_ENTRY p;LARGE_INTEGER frequency;
            RtlZeroMemory(s,sizeof(*s));s->version=IC_VERSION;s->active_mask=f->mask;
            s->controller_hid_count=IcHidQueuedLocked();s->controller_hid_devices=IcHidCountLocked();
            s->keyboard_count=gRings[0].count;s->mouse_count=gRings[1].count;
            s->last_reset_reason=gResetReason;s->capacity=IC_CAPACITY;s->generation=gGeneration;s->overflows=gOverflows;
            KeQueryPerformanceCounter(&frequency);s->timestamp_frequency=frequency.QuadPart;
            for(p=gDevices.Flink;p!=&gDevices;p=p->Flink) {
                DEVICE_CONTEXT* d=CONTAINING_RECORD(p,DEVICE_CONTEXT,link);
                if(d->keyboard)++s->keyboard_devices;else ++s->mouse_devices;
            }
            bytes=sizeof(*s);status=STATUS_SUCCESS;
        }
    }
    WdfWaitLockRelease(gGate);
complete:
    WdfRequestCompleteWithInformation(request,status,bytes);
}
NTSTATUS IcControlInit(WDFDRIVER driver) {
    PWDFDEVICE_INIT init;WDF_OBJECT_ATTRIBUTES a;WDF_FILEOBJECT_CONFIG f;WDF_IO_QUEUE_CONFIG q;NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name,IC_PRODUCT_NT_DEVICE);
    DECLARE_CONST_UNICODE_STRING(link,IC_PRODUCT_DOS_DEVICE);
    DECLARE_CONST_UNICODE_STRING(sddl,L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    if(gControl)return STATUS_SUCCESS;
    init=WdfControlDeviceInitAllocate(driver,&sddl);if(!init)return STATUS_INSUFFICIENT_RESOURCES;
    WdfDeviceInitSetDeviceType(init,IC_DEVICE_TYPE);
    WdfDeviceInitSetCharacteristics(init,FILE_DEVICE_SECURE_OPEN,FALSE);
    status=WdfDeviceInitAssignName(init,&name);if(!NT_SUCCESS(status)) { WdfDeviceInitFree(init);return status; }
    WdfDeviceInitSetIoInCallerContextCallback(init,IcCallerContext);
    WDF_FILEOBJECT_CONFIG_INIT(&f,IcFileCreate,IcFileClose,IcFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,FILE_CONTEXT);a.ExecutionLevel=WdfExecutionLevelPassive;
    WdfDeviceInitSetFileObjectConfig(init,&f,&a);
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ExecutionLevel=WdfExecutionLevelPassive;a.SynchronizationScope=WdfSynchronizationScopeNone;
    status=WdfDeviceCreate(&init,&a,&gControl);
    if(!NT_SUCCESS(status)) { if(init)WdfDeviceInitFree(init);return status; }
    status=WdfDeviceCreateSymbolicLink(gControl,&link);if(!NT_SUCCESS(status))goto fail;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q,WdfIoQueueDispatchParallel);
    q.PowerManaged=WdfFalse;q.EvtIoDeviceControl=IcControl;
    status=WdfIoQueueCreate(gControl,&q,WDF_NO_OBJECT_ATTRIBUTES,WDF_NO_HANDLE);if(!NT_SUCCESS(status))goto fail;
    IcTrialAttachTimer(gControl);
    WdfControlFinishInitializing(gControl);return STATUS_SUCCESS;
fail:
    IcControlDelete();return status;
}
void IcControlDelete(void) { if(gControl) { WDFDEVICE d=gControl;gControl=NULL;IcTrialDetachTimer();WdfObjectDelete(d); } }
NTSTATUS IcFdoAdded(void) {
    NTSTATUS status;WdfWaitLockAcquire(gLifetime,NULL);
    status=IcControlInit(WdfGetDriver());if(NT_SUCCESS(status))++gFdos;
    WdfWaitLockRelease(gLifetime);return status;
}
void IcFdoRemoved(void) {
    WdfWaitLockAcquire(gLifetime,NULL);
    NT_ASSERT(gFdos>0);--gFdos;
    // Retain the endpoint while clients hold it, so unplug/replug reuses it
    // rather than trying to create a second device with the same NT name.
    if(!gFdos && !gOpenFiles)IcControlDelete();
    WdfWaitLockRelease(gLifetime);
}
void IcUnload(WDFDRIVER driver) { UNREFERENCED_PARAMETER(driver);IcControlDelete();IcTrialUninitialize();IcMonitorUninitialize(); }
NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING registry) {
    WDF_DRIVER_CONFIG c;WDFDRIVER driver;WDF_OBJECT_ATTRIBUTES a;NTSTATUS status;
    InitializeListHead(&gDevices);IcMonitorInitialize();IcHidInitialize();IcXusbInitialize();IcXinputHidInitialize();
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
    IcGipInitialize();
#endif
    ExInitializeDriverRuntime(0);
    WDF_DRIVER_CONFIG_INIT(&c,FilterAdd);c.EvtDriverUnload=IcUnload;
    status=WdfDriverCreate(object,registry,WDF_NO_OBJECT_ATTRIBUTES,&c,&driver);if(!NT_SUCCESS(status))return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=driver;
    status=WdfSpinLockCreate(&a,&gLock);if(!NT_SUCCESS(status))return status;
    status=WdfWaitLockCreate(&a,&gGate);if(!NT_SUCCESS(status))return status;
    status=WdfWaitLockCreate(&a,&gLifetime);if(NT_SUCCESS(status))IcTrialInitialize();return status;
}
