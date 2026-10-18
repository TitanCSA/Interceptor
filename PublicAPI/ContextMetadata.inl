// User-side metadata only. Included after AutomaticInput.inl.
#include "UsbReportDescriptor.inl"
namespace Interceptor {
namespace Detail {
Captured_Device ResolveToken(Automatic_State& state,Context& context,const std::string& token) {
    for(const auto& device:state.devices)if(device.token==token)return device;
    const auto snapshot=state.SnapshotDevices(context);
    for(const auto& device:snapshot)if(device.token==token)return device;
    return {};
}
Captured_Device ExistingDevice(Automatic_State& state,const std::string& token) {
    if(token.empty())Fail("Device token must not be empty",ERROR_INVALID_PARAMETER);
    if(state.fault)Fail("Automatic capture is stopped",state.fault);
    if(!state.capture)Fail("Device token has no current capture",ERROR_NOT_FOUND);
    const auto device=ResolveToken(state,state.capture.Get(),token);
    if(device.token.empty())Fail("Unknown or stale device token",ERROR_NOT_FOUND);
    return device;
}
Device_Details PublicDetails(const Device& device,const IC_DEVICE_QUERY_RESULT& raw) {
    Device_Details result;result.token=device.token;
    result.size=raw.size;result.version=raw.version;result.type=raw.type;result.hid_profiles=raw.hid_profiles;result.flags=raw.flags;
    result.last_read_status=raw.last_read_status;result.last_usb_status=raw.last_usb_status;
    result.physical_reports=raw.physical_reports;result.report_bytes=raw.report_bytes;
    result.metadata_bytes=raw.metadata_bytes;result.metadata_input_bytes=raw.metadata_input_bytes;
    std::copy(std::begin(raw.reserved),std::end(raw.reserved),std::begin(result.reserved));
    std::copy(std::begin(raw.instance),std::end(raw.instance),std::begin(result.instance));
    return result;
}
Descriptor_Result IdentifyDescriptor(Automatic_State& state,Context& context,const std::string& token) {
    Descriptor_Result result;const auto device=ResolveToken(state,context,token);result.device=device;
    if(result.device.token.empty()){result.error=ERROR_NOT_FOUND;return result;}
    try{result.details=PublicDetails(device,DeviceDetails(context.driver.value,device.type,device.source,false,IC_DEVICE_QUERY_VERSION));}
    catch(const std::system_error& e){result.details_error=static_cast<DWORD>(e.code().value());}
    return result;
}
}
Device_Details Get_Device_Details(const std::string& token) {
    if(token.empty())Fail("Device token must not be empty",ERROR_INVALID_PARAMETER);
    Detail::Automatic_Lease lease;auto& state=lease.state;
    const auto device=Detail::ExistingDevice(state,token);
    return Detail::PublicDetails(device,DeviceDetails(state.capture.Get().driver.value,device.type,device.source,false,IC_DEVICE_QUERY_VERSION));
}
Device_Availability Get_Device_Availability(const std::string& token) {
    if(token.empty())Fail("Device token must not be empty",ERROR_INVALID_PARAMETER);
    Detail::Automatic_Lease lease;auto& state=lease.state;
    const auto device=Detail::ExistingDevice(state,token);
    const auto native=Availability(state.capture.Get().driver.value,device.type,device.source,false);
    Device_Availability result;result.details=Detail::PublicDetails(device,native.details);
    result.state=native.state;result.reasons=native.reasons;result.reported_by_driver=native.reported_by_driver;
    return result;
}
Descriptor_Result Get_descriptor(const std::string& token) {
    if(token.empty()){Descriptor_Result result;result.error=ERROR_INVALID_PARAMETER;return result;}
    Detail::Automatic_Lease lease;auto& state=lease.state;auto& context=state.Ensure();
    auto result=Detail::IdentifyDescriptor(state,context,token);if(!result)return result;
    try {
        switch(result.device.type) {
        case IC_TYPE_CONTROLLER_HID:
            result.format=Descriptor_Format::Hid_Preparsed;result.data=Get_Controller_Hid_Descriptor(context,result.device.id);break;
        case IC_TYPE_CONTROLLER_XINPUTHID:
            result.format=Descriptor_Format::Hid_Preparsed;result.data=Get_Controller_XinputHid_Descriptor(context,result.device.id);break;
        case IC_TYPE_CONTROLLER_GIP: {
            result.format=Descriptor_Format::Gip_Metadata;result.data.resize(4096);
            const IC_GET request{IC_VERSION,result.device.id};
            const auto bytes=Io(context.driver.value,IOCTL_IC_GIP_DESCRIPTOR,&request,sizeof(request),result.data.data(),static_cast<DWORD>(result.data.size()));
            if(!bytes || bytes>result.data.size())Fail("Invalid GIP metadata response",ERROR_INVALID_DATA);
            result.data.resize(bytes);break;
        }
        default:break;
        }
    }catch(const std::system_error& e){result.error=static_cast<DWORD>(e.code().value());result.data.clear();}
    return result;
}
Report_Descriptor Get_Report_Descriptor(const std::string& token) {
    if(token.empty()){Report_Descriptor result;result.error=ERROR_INVALID_PARAMETER;return result;}
    Detail::Automatic_Lease lease;auto& state=lease.state;auto& context=state.Ensure();
    auto result=Detail::IdentifyDescriptor(state,context,token);if(!result)return result;
    if(result.device.type==IC_TYPE_CONTROLLER_XUSB || result.device.type==IC_TYPE_CONTROLLER_GIP) {
        result.applicable=false;result.error=ERROR_NOT_SUPPORTED;return result;
    }
    result.format=Descriptor_Format::Hid_Report;
    if(result.details_error){result.error=result.details_error;return result;}
    try{
        const auto before=Get_Status(&context);
        if(!(before.active_mask&(1u<<(result.device.type-1))))Fail("Capture no longer owns HID source",ERROR_OPERATION_ABORTED);
        result.data=Detail::rawReportReader(result.device,result.details);
        const auto after=Get_Status(&context);
        if(after.generation!=before.generation || after.active_mask!=before.active_mask)Fail("Capture changed during descriptor query",ERROR_OPERATION_ABORTED);
        if(result.data.empty())Fail("Empty hardware HID report descriptor",ERROR_INVALID_DATA);
    }
    catch(const std::system_error& e){result.error=static_cast<DWORD>(e.code().value());result.data.clear();}
    return result;
}
}
