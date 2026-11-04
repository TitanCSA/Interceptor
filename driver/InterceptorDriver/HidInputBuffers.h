#pragma once
// HIDClass buffers reports per open, independently of Interceptor's capture
// FIFO. Reserve headroom once before starting the reader: no extra per-report
// work or polling. 128 payloads are at most 512 KiB for our 4096-byte profile
// (HIDClass's own allocation metadata is additional).
#define FILTER_HID_INPUT_RESERVE 128u
static NTSTATUS FilterInputBufferIoctl(WDFDEVICE device,PFILE_OBJECT file,ULONG code,ULONG* value) {
    WDFREQUEST request;WDFMEMORY memory;WDF_OBJECT_ATTRIBUTES attributes;
    WDF_REQUEST_SEND_OPTIONS options;NTSTATUS status;
    WDFIOTARGET target=WdfDeviceGetIoTarget(device);
    status=WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES,target,&request);
    if(!NT_SUCCESS(status))return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);attributes.ParentObject=request;
    status=WdfMemoryCreatePreallocated(&attributes,value,sizeof(*value),&memory);
    if(NT_SUCCESS(status)) {
        const BOOLEAN set=code==IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS;
        status=WdfIoTargetFormatRequestForIoctl(target,request,code,set?memory:NULL,NULL,set?NULL:memory,NULL);
    }
    if(NT_SUCCESS(status)) {
        // A separate open would configure a different queue. Preserve the
        // exact successful CREATE file object used by FilterRead.
        IoGetNextIrpStackLocation(WdfRequestWdmGetIrp(request))->FileObject=file;
        WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_SYNCHRONOUS|WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,WDF_REL_TIMEOUT_IN_SEC(2));
        WdfRequestSend(request,target,&options);status=WdfRequestGetStatus(request);
        if(NT_SUCCESS(status) && code==IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS && WdfRequestGetInformation(request)!=sizeof(*value))
            status=STATUS_INVALID_BUFFER_SIZE;
    }
    WdfObjectDelete(request);return status;
}
static NTSTATUS FilterReserveInputBuffers(WDFDEVICE device,PFILE_OBJECT file) {
    ULONG count=0;
    NTSTATUS status=FilterInputBufferIoctl(device,file,IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS,&count);
    if(!NT_SUCCESS(status))return status;
    // Never shrink an existing larger reserve configured by Windows/another
    // component. Failure leaves the original functioning HIDClass open intact.
    if(count>=FILTER_HID_INPUT_RESERVE)return STATUS_SUCCESS;
    count=FILTER_HID_INPUT_RESERVE;
    return FilterInputBufferIoctl(device,file,IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS,&count);
}
