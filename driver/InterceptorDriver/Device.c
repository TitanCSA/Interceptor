#include "ControllerHid.h"
#include "ControllerXusb.h"
#include "ControllerXusbUsb.h"
#include "ControllerXinputHid.h"
#include "ControllerGip.h"
#include "../Shared/XusbMirrorIdentity.h"
#include <initguid.h>
#include <devpkey.h>
static LONG gDeviceId;
void IcCacheDeviceInstance(WDFDEVICE device,USHORT instance[256]) {
    WDF_DEVICE_PROPERTY_DATA property;DEVPROPTYPE type;ULONG bytes=0;NTSTATUS status;
    RtlZeroMemory(instance,256*sizeof(USHORT));
    WDF_DEVICE_PROPERTY_DATA_INIT(&property,&DEVPKEY_Device_InstanceId);
    status=WdfDeviceQueryPropertyEx(device,&property,255*sizeof(USHORT),instance,&bytes,&type);
    if(!NT_SUCCESS(status) || type!=DEVPROP_TYPE_STRING || bytes<sizeof(USHORT) || bytes%sizeof(USHORT))
        RtlZeroMemory(instance,256*sizeof(USHORT));
}
static void IcMapperCleanup(WDFOBJECT object) { UNREFERENCED_PARAMETER(object);IcFdoRemoved(); }
static NTSTATUS IcControlAnchorAdd(PWDFDEVICE_INIT init) {
    WDFDEVICE device;WDFOBJECT token;WDF_OBJECT_ATTRIBUTES attributes;NTSTATUS status;
    WdfDeviceInitSetDeviceType(init,FILE_DEVICE_UNKNOWN);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);attributes.ExecutionLevel=WdfExecutionLevelPassive;
    attributes.SynchronizationScope=WdfSynchronizationScopeNone;
    status=WdfDeviceCreate(&init,&attributes,&device);
    if(!NT_SUCCESS(status))return status;
    status=IcFdoAdded();if(!NT_SUCCESS(status))return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);attributes.ParentObject=device;
    attributes.EvtCleanupCallback=IcMapperCleanup;
    status=WdfObjectCreate(&attributes,&token);
    if(!NT_SUCCESS(status))IcFdoRemoved();
    return status;
}
// An unsupported/stale FILTER association must not turn another device class
// into a mouse mapper or prevent Windows' function driver from starting.
static NTSTATUS IcUnrecognizedFilter(PWDFDEVICE_INIT init) {
    WDFDEVICE device;
    WdfFdoInitSetFilter(init);
    return WdfDeviceCreate(&init,WDF_NO_OBJECT_ATTRIBUTES,&device);
}
#if defined(INTERCEPTOR_EXPERIMENTAL_GIP) || defined(INTERCEPTOR_EXPERIMENTAL_XUSB)
static BOOLEAN IcConfiguredLowerFilter(PWDFDEVICE_INIT init) {
    WCHAR filters[512]={0};ULONG bytes=0,offset=0;DEVPROPTYPE type;
    WDF_DEVICE_PROPERTY_DATA property;UNICODE_STRING actual;
    DECLARE_CONST_UNICODE_STRING(ours,IC_PRODUCT_SERVICE);
    WDF_DEVICE_PROPERTY_DATA_INIT(&property,&DEVPKEY_Device_LowerFilters);
    if(!NT_SUCCESS(WdfFdoInitQueryPropertyEx(init,&property,sizeof(filters)-4,filters,&bytes,&type)) || type!=DEVPROP_TYPE_STRING_LIST)return FALSE;
    while(offset<RTL_NUMBER_OF(filters)-1 && filters[offset]) {
        RtlInitUnicodeString(&actual,filters+offset);
        if(RtlEqualUnicodeString(&actual,&ours,TRUE))return TRUE;
        offset+=actual.Length/sizeof(WCHAR)+1;
    }
    return FALSE;
}
#endif
#ifdef INTERCEPTOR_EXPERIMENTAL_XINPUTHID
static BOOLEAN IcXinputHidProfile(PWDFDEVICE_INIT init) {
    WDFKEY key;ULONG flags=0;NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name,L"DevicePropertyFlags");
    status=WdfFdoInitOpenRegistryKey(init,PLUGPLAY_REGKEY_DRIVER,KEY_QUERY_VALUE,WDF_NO_OBJECT_ATTRIBUTES,&key);
    if(!NT_SUCCESS(status))return FALSE;
    status=WdfRegistryQueryULong(key,&name,&flags);WdfRegistryClose(key);
    // GenericHid | XInputCompatible. GIP_Hid (14) has a different source
    // protocol; a legacy/manual association must not advertise that as this
    // backend. The installer reads the same software key without changing it.
    return NT_SUCCESS(status) && flags==6;
}
#endif
#ifdef INTERCEPTOR_EXPERIMENTAL_XUSB
static BOOLEAN IcBelowXusbFunction(PWDFDEVICE_INIT init,PDRIVER_OBJECT ours) {
    PDEVICE_OBJECT pdo=WdfFdoInitWdmGetPhysicalDevice(init),current;ULONG depth=0;
    BOOLEAN lower=FALSE;
    DECLARE_CONST_UNICODE_STRING(native,L"\\Driver\\xusb22");
    if(!pdo)return FALSE;
    current=IoGetAttachedDeviceReference(pdo);
    // PnP attaches all lower filters before the function driver, then upper
    // filters. Inspect that supported stack instead of private Class registry
    // keys: device-specific and class-wide lower associations both work.
    while(current && depth++<64) {
        PDEVICE_OBJECT next;ULONG bytes=0;
        union {OBJECT_NAME_INFORMATION info;UCHAR bytes[sizeof(OBJECT_NAME_INFORMATION)+128*sizeof(WCHAR)];} name;
        if(!current->DriverObject || current->DriverObject==ours ||
           !NT_SUCCESS(ObQueryNameString(current->DriverObject,&name.info,sizeof(name),&bytes)) ||
           !name.info.Name.Buffer || !name.info.Name.Length || name.info.Name.Length%sizeof(WCHAR) ||
           (PUCHAR)name.info.Name.Buffer<(PUCHAR)&name ||
           (PUCHAR)name.info.Name.Buffer>(PUCHAR)&name+sizeof(name) ||
           name.info.Name.Length>(PUCHAR)&name+sizeof(name)-(PUCHAR)name.info.Name.Buffer){
            ObDereferenceObject(current);return FALSE;
        }
        if(RtlEqualUnicodeString(&name.info.Name,&native,TRUE)){
            ObDereferenceObject(current);return FALSE;
        }
        if(current==pdo){lower=TRUE;ObDereferenceObject(current);current=NULL;break;}
        next=IoGetLowerDeviceObject(current);ObDereferenceObject(current);current=next;
    }
    if(current)ObDereferenceObject(current);return lower;
}
static BOOLEAN IcWiredXusbProfile(PWDFDEVICE_INIT init) {
    WCHAR hardware[512]={0},service[64]={0},className[64]={0},compatible[512]={0};
    ULONG bytes=0,offset=0;USHORT vid=0,pid=0;UNICODE_STRING actual;
    WDF_DEVICE_PROPERTY_DATA property;DEVPROPTYPE type;BOOLEAN xusb10=FALSE,wired=FALSE;
    DECLARE_CONST_UNICODE_STRING(expected,L"xusb22");
    DECLARE_CONST_UNICODE_STRING(xna,L"{D61CA365-5AF4-4486-998B-9DB4734C6CA3}");
    DECLARE_CONST_UNICODE_STRING(comp,L"USB\\MS_COMP_XUSB10");
    DECLARE_CONST_UNICODE_STRING(protocol,L"USB\\Class_ff&SubClass_5d&Prot_01");
    if(!NT_SUCCESS(WdfFdoInitQueryProperty(init,DevicePropertyHardwareID,sizeof(hardware)-2,hardware,&bytes)))return FALSE;
    RtlInitUnicodeString(&actual,hardware);
    if(!IcXusbProduct((const uint16_t*)hardware,actual.Length/sizeof(WCHAR),1,&vid,&pid))return FALSE;
    if(!NT_SUCCESS(WdfFdoInitQueryProperty(init,DevicePropertyClassGuid,sizeof(className)-2,className,&bytes)))return FALSE;
    RtlInitUnicodeString(&actual,className);if(!RtlEqualUnicodeString(&actual,&xna,TRUE))return FALSE;
    WDF_DEVICE_PROPERTY_DATA_INIT(&property,&DEVPKEY_Device_Service);
    if(!NT_SUCCESS(WdfFdoInitQueryPropertyEx(init,&property,sizeof(service)-2,service,&bytes,&type)) || type!=DEVPROP_TYPE_STRING)return FALSE;
    RtlInitUnicodeString(&actual,service);if(!RtlEqualUnicodeString(&actual,&expected,TRUE))return FALSE;
    // Preserve the original tested profile, including software bus fixtures.
    if(vid==0x045E && pid==0x028E)return TRUE;
    if(!NT_SUCCESS(WdfFdoInitQueryProperty(init,DevicePropertyCompatibleIDs,sizeof(compatible)-4,compatible,&bytes)))return FALSE;
    while(offset<RTL_NUMBER_OF(compatible)-1 && compatible[offset]) {
        RtlInitUnicodeString(&actual,compatible+offset);
        if(RtlEqualUnicodeString(&actual,&comp,TRUE))xusb10=TRUE;
        if(RtlEqualUnicodeString(&actual,&protocol,TRUE))wired=TRUE;
        offset+=actual.Length/sizeof(WCHAR)+1;
    }
    return xusb10 && wired;
}
#endif
NTSTATUS FilterAdd(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    #if IC_PRODUCT_HAS_KEYBOARD_MOUSE
    WDFDEVICE device;WDF_OBJECT_ATTRIBUTES attributes;WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_IO_QUEUE_CONFIG queue;DEVICE_CONTEXT* d;
#endif
    NTSTATUS status;
    UNREFERENCED_PARAMETER(driver);
    {
        WCHAR hardware[128]={0};ULONG bytes;UNICODE_STRING actual;
        DECLARE_CONST_UNICODE_STRING(anchor,IC_PRODUCT_CONTROL_HWID);
        if(NT_SUCCESS(WdfFdoInitQueryProperty(init,DevicePropertyHardwareID,sizeof(hardware)-2,hardware,&bytes))) {
            RtlInitUnicodeString(&actual,hardware);
            if(RtlEqualUnicodeString(&actual,&anchor,TRUE))return IcControlAnchorAdd(init);
        }
    }
    // Kbdclass/mouclass above the mapper own power policy for this stack.
    WdfDeviceInitSetPowerPolicyOwnership(init,FALSE);
#ifdef INTERCEPTOR_EXPERIMENTAL_GIP
    {
        WCHAR service[64]={0};WDF_DEVICE_PROPERTY_DATA property;DEVPROPTYPE type;ULONG bytes;
        DECLARE_CONST_UNICODE_STRING(expected,L"dc1-controller");UNICODE_STRING actual;
        WDF_DEVICE_PROPERTY_DATA_INIT(&property,&DEVPKEY_Device_Service);
        if(NT_SUCCESS(WdfFdoInitQueryPropertyEx(init,&property,sizeof(service)-2,service,&bytes,&type)) && type==DEVPROP_TYPE_STRING) {
            RtlInitUnicodeString(&actual,service);
            if(RtlEqualUnicodeString(&actual,&expected,TRUE))
                return IcConfiguredLowerFilter(init)?IcGipAdd(init):IcUnrecognizedFilter(init);
        }
    }
#endif
#ifdef INTERCEPTOR_EXPERIMENTAL_XUSB
    if(IcWiredXusbProfile(init))return IcBelowXusbFunction(init,WdfDriverWdmGetDriverObject(driver))?IcXusbUsbAdd(init):IcUnrecognizedFilter(init);
#endif
    {
        WCHAR className[64]={0}; ULONG length; UNICODE_STRING actual;
        DECLARE_CONST_UNICODE_STRING(hidClass,L"{745A17A0-74D3-11D0-B6FE-00A0C90F57DA}");
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE
        DECLARE_CONST_UNICODE_STRING(keyboardClass,L"{4D36E96B-E325-11CE-BFC1-08002BE10318}");
        DECLARE_CONST_UNICODE_STRING(mouseClass,L"{4D36E96F-E325-11CE-BFC1-08002BE10318}");
 #endif
        status=WdfFdoInitQueryProperty(init,DevicePropertyClassGuid,sizeof(className)-sizeof(WCHAR),className,&length);
        if(!NT_SUCCESS(status)) return status;
        RtlInitUnicodeString(&actual,className);
        if(RtlEqualUnicodeString(&actual,&hidClass,TRUE)) {
#if IC_PRODUCT_HAS_CONTROLLERS
#ifdef INTERCEPTOR_EXPERIMENTAL_XUSB
            {
                WCHAR hardware[512]={0};ULONG bytes;
                if(NT_SUCCESS(WdfFdoInitQueryProperty(init,DevicePropertyHardwareID,sizeof(hardware)-2,hardware,&bytes))) {
                    RtlInitUnicodeString(&actual,hardware);
                    if(IcXusbMirrorGroup((const uint16_t*)hardware,actual.Length/sizeof(WCHAR),0)>=0)
                        return IcUnrecognizedFilter(init);
                }
            }
#endif
#ifdef INTERCEPTOR_EXPERIMENTAL_XINPUTHID
            WCHAR filters[512]={0};ULONG bytes=0,offset=0;BOOLEAN ours=FALSE;
            WDF_DEVICE_PROPERTY_DATA property;DEVPROPTYPE propertyType;
            DECLARE_CONST_UNICODE_STRING(ourService,IC_PRODUCT_SERVICE);
            DECLARE_CONST_UNICODE_STRING(translator,L"xinputhid");
            // UpperFilters are applied in list order: our source filter must
            // precede Microsoft's translator, otherwise native XInput bypasses it.
            WDF_DEVICE_PROPERTY_DATA_INIT(&property,&DEVPKEY_Device_UpperFilters);
            if(NT_SUCCESS(WdfFdoInitQueryPropertyEx(init,&property,sizeof(filters)-4,filters,&bytes,&propertyType)) && propertyType==DEVPROP_TYPE_STRING_LIST) {
                while(offset<RTL_NUMBER_OF(filters)-1 && filters[offset]) {
                    UNICODE_STRING name;RtlInitUnicodeString(&name,filters+offset);
                    if(RtlEqualUnicodeString(&name,&translator,TRUE))return ours && IcXinputHidProfile(init)?IcXinputHidAdd(init):IcUnrecognizedFilter(init);
                    if(RtlEqualUnicodeString(&name,&ourService,TRUE))ours=TRUE;
                    offset+=name.Length/sizeof(WCHAR)+1;
                }
            }
#endif
            return IcHidAdd(init);
#else
            return IcUnrecognizedFilter(init);
#endif
        }
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE
        if(!RtlEqualUnicodeString(&actual,&keyboardClass,TRUE) && !RtlEqualUnicodeString(&actual,&mouseClass,TRUE))return IcUnrecognizedFilter(init);
        WdfDeviceInitSetDeviceType(init,RtlEqualUnicodeString(&actual,&keyboardClass,TRUE)?FILE_DEVICE_KEYBOARD:FILE_DEVICE_MOUSE);
#else
        return IcUnrecognizedFilter(init);
#endif
    }
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
    {
        WDF_FILEOBJECT_CONFIG file;
        WDF_FILEOBJECT_CONFIG_INIT(&file,WDF_NO_EVENT_CALLBACK,WDF_NO_EVENT_CALLBACK,FilterHidCleanup);
        file.AutoForwardCleanupClose=WdfTrue;file.FileObjectClass=WdfFileObjectWdfCannotUseFsContexts;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);attributes.ExecutionLevel=WdfExecutionLevelPassive;
        attributes.SynchronizationScope=WdfSynchronizationScopeNone;
        WdfDeviceInitSetFileObjectConfig(init,&file,&attributes);
    }
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = FilterPrepare; pnp.EvtDeviceReleaseHardware = FilterRelease;
    pnp.EvtDeviceSelfManagedIoInit = FilterStart; pnp.EvtDeviceSelfManagedIoRestart = FilterStart;
    pnp.EvtDeviceSelfManagedIoCleanup = FilterStop;
    pnp.EvtDeviceSelfManagedIoSuspend = FilterSuspend;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);

    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    status = WdfDeviceCreate(&init, &attributes, &device); if (!NT_SUCCESS(status)) return status;
    status=IcFdoAdded();if(!NT_SUCCESS(status))return status;
    { WDF_OBJECT_ATTRIBUTES child; WDFOBJECT token;
      WDF_OBJECT_ATTRIBUTES_INIT(&child);child.ParentObject=device;child.EvtCleanupCallback=IcMapperCleanup;
      status=WdfObjectCreate(&child,&token);if(!NT_SUCCESS(status)) { IcFdoRemoved();return status; } }
    d = FilterDevice(device); d->device = device; d->id = (ULONG)InterlockedIncrement(&gDeviceId);
    IcCacheDeviceInstance(device,d->instance);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);attributes.ParentObject=device;
    status=WdfWaitLockCreate(&attributes,&d->stateLock);if(!NT_SUCCESS(status))return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.EvtIoInternalDeviceControl = FilterInternal;
    queue.EvtIoDeviceControl = FilterInternal;
    queue.PowerManaged = WdfFalse;
    status=WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if(!NT_SUCCESS(status))return status;
    {
        WDFQUEUE creates;
        WDF_IO_QUEUE_CONFIG_INIT(&queue,WdfIoQueueDispatchSequential);
        queue.PowerManaged=WdfFalse;queue.EvtIoDefault=FilterMapperCreate;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);attributes.ExecutionLevel=WdfExecutionLevelPassive;
        status=WdfIoQueueCreate(device,&queue,&attributes,&creates);
        if(!NT_SUCCESS(status))return status;
        return WdfDeviceConfigureRequestDispatching(device,creates,WdfRequestTypeCreate);
    }
#endif
}
