#include "ControllerXusbMirror.h"
#include <devpkey.h>
#include "../Shared/XusbMirrorProfile.h"
#include "../Shared/XusbMirrorValues.h"
#include "../Shared/XusbMirrorIdentity.h"

// This instance shares the proven HID file/reader/cancellation engine. It has
// no public input queue and is owned exclusively by its exact XUSB parent.
#define IC_HID_PRIVATE_MIRROR 1
#define IC_HID_BACKEND_TYPE IC_TYPE_CONTROLLER_XUSB
#define IC_HID_BACKEND_GUID IC_XUSB_MIRROR_INTERFACE_GUID
#define IC_HID_BACKEND_SOURCE IOCTL_IC_XUSB_MIRROR_SOURCE
#define IcHidInitialize IcXusbMirrorInitialize
#define IcHidAdd IcXusbMirrorAdd
#include "ControllerHidEngine.h"

static BOOLEAN MirrorCapturePresent(void) {
    PLIST_ENTRY p;
    for(p=devices.Flink;p!=&devices;p=p->Flink)
        if(CONTAINING_RECORD(p,HID_DEVICE,link)->captureId)return TRUE;
    return FALSE;
}
static BOOLEAN ReadInstance(WDFKEY key,PCWSTR name,PWSTR output,ULONG bytes) {
    UNICODE_STRING value;ULONG returned=0,type=0;NTSTATUS status;
    RtlInitUnicodeString(&value,name);RtlZeroMemory(output,bytes);
    status=WdfRegistryQueryValue(key,&value,bytes,output,&returned,&type);
    return NT_SUCCESS(status) && type==REG_SZ && returned>=2*sizeof(WCHAR) &&
        returned<=bytes && returned%sizeof(WCHAR)==0 && output[0] && output[returned/sizeof(WCHAR)-1]==0;
}
static BOOLEAN ValidateProfile(HID_DEVICE* d) {
    HIDP_VALUE_CAPS values[6];HIDP_BUTTON_CAPS buttons;USHORT count=6;
    if(d->caps.UsagePage!=1 || d->caps.Usage!=5 ||
       d->caps.InputReportByteLength!=15 || d->caps.NumberInputValueCaps!=6 ||
       d->caps.NumberInputButtonCaps!=1 || d->slotCount!=1 || d->slots[0]!=1)return FALSE;
    if(HidP_GetValueCaps(HidP_Input,values,&count,d->descriptor)!=HIDP_STATUS_SUCCESS || count!=6)return FALSE;
    count=1;
    if(HidP_GetButtonCaps(HidP_Input,&buttons,&count,d->descriptor)!=HIDP_STATUS_SUCCESS || count!=1)return FALSE;
    return (BOOLEAN)IcXusbMirrorProfileValid(&d->caps,values,6,&buttons,1);
}
static void MirrorPrepare(HID_DEVICE* d) {
    WDFKEY key;WCHAR expected[IC_XUSB_MIRROR_INSTANCE_CHARS],actual[IC_XUSB_MIRROR_INSTANCE_CHARS]={0};
    WDF_DEVICE_PROPERTY_DATA property;DEVPROPTYPE type;ULONG bytes;UNICODE_STRING a,b;USHORT vid=0,pid=0;
    d->mirrorProfile=FALSE;RtlZeroMemory(d->mirrorParent,sizeof(d->mirrorParent));
    if(!NT_SUCCESS(WdfDeviceOpenRegistryKey(d->device,PLUGPLAY_REGKEY_DEVICE,KEY_QUERY_VALUE,WDF_NO_OBJECT_ATTRIBUTES,&key)))return;
    if(!ReadInstance(key,IC_XUSB_MIRROR_HID_PARENT_VALUE,expected,sizeof(expected)) ||
       !ReadInstance(key,IC_XUSB_MIRROR_PARENT_VALUE,d->mirrorParent,sizeof(d->mirrorParent))) {
        WdfRegistryClose(key);RtlZeroMemory(d->mirrorParent,sizeof(d->mirrorParent));return;
    }
    WdfRegistryClose(key);WDF_DEVICE_PROPERTY_DATA_INIT(&property,&DEVPKEY_Device_Parent);
    if(!NT_SUCCESS(WdfDeviceQueryPropertyEx(d->device,&property,sizeof(actual)-sizeof(WCHAR),actual,&bytes,&type)) || type!=DEVPROP_TYPE_STRING)return;
    RtlInitUnicodeString(&a,actual);RtlInitUnicodeString(&b,expected);
    if(!RtlEqualUnicodeString(&a,&b,TRUE) || !ValidateProfile(d))return;
    if(!IcXusbProduct((const uint16_t*)actual,a.Length/sizeof(WCHAR),1,&vid,&pid) ||
       vid!=d->info.vid || pid!=d->info.pid)return;
    RtlInitUnicodeString(&b,d->mirrorParent);
    if(!IcXusbProduct((const uint16_t*)d->mirrorParent,b.Length/sizeof(WCHAR),1,&vid,&pid) ||
       vid!=d->info.vid || pid!=d->info.pid)return;
    // The SDK established the grand-parent relation through CM before writing
    // these administrative values. The parent backend must also resolve the
    // exact stored instance to a live, supported xusb22 device before calling us.
    d->mirrorProfile=TRUE;
}
static HID_DEVICE* FindMirror(PCUNICODE_STRING parent,BOOLEAN requireReader) {
    PLIST_ENTRY p;HID_DEVICE* result=NULL;
    if(!parent || !parent->Buffer || !parent->Length)return NULL;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        HID_DEVICE* d=CONTAINING_RECORD(p,HID_DEVICE,link);UNICODE_STRING actual;
        if(d->retired)continue;
        RtlInitUnicodeString(&actual,d->mirrorParent);
        if(!RtlEqualUnicodeString(parent,&actual,TRUE))continue;
        // A preparing or unsupported collection for another exact parent must
        // not disable this controller. Missing identity never matches a parent.
        // Even an inactive duplicate is ambiguous: never silently capture one
        // of several legacy views or make topology order decide isolation.
        if(result || !d->mirrorProfile || d->bypass || !d->prepared || !d->powered || (requireReader && !d->thread))return NULL;
        result=d;
    }
    return result;
}
BOOLEAN IcXusbMirrorReadyLocked(PCUNICODE_STRING parent) {return FindMirror(parent,TRUE)!=NULL;}
NTSTATUS IcXusbMirrorCaptureLocked(PCUNICODE_STRING parent) {
    HID_DEVICE* d=FindMirror(parent,TRUE);
    if(!d)return STATUS_DEVICE_NOT_READY;
    if(d->captureId)return STATUS_SHARING_VIOLATION;
    d->captureId=1;ClearFiles(d);RtlZeroMemory(d->deliveredValid,sizeof(d->deliveredValid));return STATUS_SUCCESS;
}
void IcXusbMirrorResetLocked(void) {
    PLIST_ENTRY p;
    for(p=devices.Flink;p!=&devices;p=p->Flink) {
        HID_DEVICE* d=CONTAINING_RECORD(p,HID_DEVICE,link);
        if(!d->captureId)continue;d->captureId=0;ClearFiles(d);
        // ControllerXusb restores its latest physical snapshot exactly once.
        // This reader's independent HID cache may be older than that snapshot.
    }
}
NTSTATUS IcXusbMirrorSendLocked(PCUNICODE_STRING parent,const IC_NATIVE_GAMEPAD_STATE* state,BOOLEAN owned) {
    HID_DEVICE* d=FindMirror(parent,FALSE);
    IC_XUSB_MIRROR_VALUES values;UCHAR report[IC_XUSB_MIRROR_REPORT_BYTES];ULONG i,buttonCount;NTSTATUS status;
    static const USAGE usages[6]={0x30,0x31,0x33,0x34,0x32,0x39};
    static const USHORT links[6]={1,1,2,2,3,0};
    if(!d || (owned && !d->captureId))return STATUS_DEVICE_NOT_READY;
    if(!IcXusbMirrorMakeValues(state,&values))return STATUS_NOT_SUPPORTED;
    status=HidP_InitializeReportForID(HidP_Input,0,d->descriptor,(PCHAR)report,sizeof(report));
    if(status!=HIDP_STATUS_SUCCESS)return STATUS_NOT_SUPPORTED;
    for(i=0;i<6;++i) {
        status=HidP_SetUsageValue(HidP_Input,1,links[i],usages[i],values.axes[i],d->descriptor,(PCHAR)report,sizeof(report));
        if(status!=HIDP_STATUS_SUCCESS)return STATUS_NOT_SUPPORTED;
    }
    buttonCount=values.button_count;
    if(buttonCount && (HidP_SetUsages(HidP_Input,9,0,(PUSAGE)values.buttons,&buttonCount,d->descriptor,(PCHAR)report,sizeof(report))!=HIDP_STATUS_SUCCESS || buttonCount!=values.button_count))return STATUS_NOT_SUPPORTED;
    Broadcast(d,report);return STATUS_SUCCESS;
}
void IcXusbMirrorPhysicalLocked(PCUNICODE_STRING parent,const IC_NATIVE_GAMEPAD_STATE* state) {
    HID_DEVICE* d=FindMirror(parent,FALSE);
    if(d && !d->captureId)(void)IcXusbMirrorSendLocked(parent,state,FALSE);
}
static void MirrorPhysical(HID_DEVICE* d) {
    UNICODE_STRING parent;IC_NATIVE_GAMEPAD_STATE state;IC_XUSB_MIRROR_VALUES values;
    if(d->captureId)return;
    RtlInitUnicodeString(&parent,d->mirrorParent);
    // When the native event reader is live it owns BOTH delivery views. An
    // independent HID READ can complete late, so it must not overwrite a Send
    // or a newer restoration. With no native pump, normal HID reads continue.
    if(!IcXusbPhysicalLocked(&parent,&state) || !IcXusbMirrorMakeValues(&state,&values))Broadcast(d,d->report);
}
static void MirrorSeed(HID_DEVICE* d) {
    UNICODE_STRING parent;IC_NATIVE_GAMEPAD_STATE state;
    RtlInitUnicodeString(&parent,d->mirrorParent);
    if(!d->captureId && IcXusbPhysicalLocked(&parent,&state))IcXusbMirrorPhysicalLocked(&parent,&state);
}
