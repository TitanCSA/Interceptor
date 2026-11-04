#pragma once
#include "Protocol.h"
typedef struct TC_CLIENT {PFILE_OBJECT file;PDEVICE_OBJECT device;} TC_CLIENT;
static __inline void TcClose(TC_CLIENT* client) {
    if(client->file)ObDereferenceObject(client->file);
    client->file=NULL;client->device=NULL;
}
static __inline NTSTATUS TcRead(TC_CLIENT* client,ULONG product,ULONG operation,TC_RESULT* result) {
    TC_REQUEST request={sizeof(request),TC_VERSION,product,operation};KEVENT event;IO_STATUS_BLOCK io={0};PIRP irp;NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name,TC_NAME);
    if(product>=TC_PRODUCTS || operation>TC_COMMIT)return STATUS_INVALID_PARAMETER;
    if(!client->file) {
        status=IoGetDeviceObjectPointer((PUNICODE_STRING)&name,FILE_READ_DATA|FILE_WRITE_DATA,&client->file,&client->device);
        if(!NT_SUCCESS(status)) {
            DECLARE_CONST_UNICODE_STRING(service0,TC_SERVICE0);
            DECLARE_CONST_UNICODE_STRING(service1,TC_SERVICE1);
            DECLARE_CONST_UNICODE_STRING(service2,TC_SERVICE2);
            DECLARE_CONST_UNICODE_STRING(service3,TC_SERVICE3);
            const UNICODE_STRING* service=product==0?&service0:product==1?&service1:product==2?&service2:&service3;
            // Fixed, product-owned service names; no registry writes. A clock
            // already resident from any product is reused before this path.
            (void)ZwLoadDriver((PUNICODE_STRING)service);
            status=IoGetDeviceObjectPointer((PUNICODE_STRING)&name,FILE_READ_DATA|FILE_WRITE_DATA,&client->file,&client->device);
            if(!NT_SUCCESS(status)){client->file=NULL;client->device=NULL;return status;}
        }
        {
            TC_IDENTITY* identity=IoGetDriverObjectExtension(client->device->DriverObject,TC_EXTENSION_TAG);
            if(!identity || identity->size!=sizeof(*identity) || identity->version!=TC_VERSION ||
               identity->owner!=client->device->DriverObject || identity->device!=client->device) {
                TcClose(client);return STATUS_OBJECT_TYPE_MISMATCH;
            }
        }
    }
    RtlZeroMemory(result,sizeof(*result));KeInitializeEvent(&event,NotificationEvent,FALSE);
    irp=IoBuildDeviceIoControlRequest(TC_IOCTL,client->device,&request,sizeof(request),result,sizeof(*result),TRUE,&event,&io);
    if(!irp)return STATUS_INSUFFICIENT_RESOURCES;
    status=IoCallDriver(client->device,irp);
    if(status==STATUS_PENDING){KeWaitForSingleObject(&event,Executive,KernelMode,FALSE,NULL);status=io.Status;}
    if(NT_SUCCESS(status) && (io.Information!=sizeof(*result) || result->size!=sizeof(*result) || result->version!=TC_VERSION ||
       result->duration!=TC_DURATION || result->started>1 || result->expired>1 || (!result->started && (result->first || result->expired))))status=STATUS_DATA_ERROR;
    return status;
}
