#pragma once
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidclass.h>
#include <vector>
#include <cwchar>
#include "../Shared/HidMapperProfile.h"
#include "../Shared/HidGameProfile.h"
#include "../Shared/ControllerHid.h"
#pragma comment(lib,"hid.lib")
#pragma comment(lib,"setupapi.lib")

namespace Interceptor::SetupDetail {
struct MapperProfileProbe {
    bool supported=false;
    IC_MAPPER_PROFILE_RESULT profile=IC_MAPPER_PROFILE_OK;
    DWORD error=ERROR_SUCCESS;
    const wchar_t* reason=L"HID profile could not be inspected; association skipped";
};
struct HidProfileApi {
    decltype(&HidD_GetHidGuid) hidGuid=HidD_GetHidGuid;
    decltype(&SetupDiGetDeviceInstanceIdW) instance=SetupDiGetDeviceInstanceIdW;
    decltype(&SetupDiGetClassDevsW) devices=SetupDiGetClassDevsW;
    decltype(&SetupDiDestroyDeviceInfoList) destroy=SetupDiDestroyDeviceInfoList;
    decltype(&SetupDiEnumDeviceInterfaces) enumerate=SetupDiEnumDeviceInterfaces;
    decltype(&SetupDiGetDeviceInterfaceDetailW) detail=SetupDiGetDeviceInterfaceDetailW;
    decltype(&CreateFileW) open=CreateFileW;
    decltype(&CloseHandle) close=CloseHandle;
    decltype(&DeviceIoControl) ioctl=DeviceIoControl;
    decltype(&HidD_GetPreparsedData) preparsed=HidD_GetPreparsedData;
    decltype(&HidD_FreePreparsedData) freePreparsed=HidD_FreePreparsedData;
    decltype(&HidP_GetCaps) caps=HidP_GetCaps;
};
inline const wchar_t* MapperProfileReason(IC_MAPPER_PROFILE_RESULT value) {
    switch(value) {
    case IC_MAPPER_PROFILE_DESCRIPTOR_SIZE:return L"HID preparsed descriptor exceeds the mapper profile";
    case IC_MAPPER_PROFILE_USAGE:return L"HID collection usage does not match this mapper";
    case IC_MAPPER_PROFILE_INPUT_SIZE:return L"HID input report length exceeds the mapper profile";
    case IC_MAPPER_PROFILE_VALUE_CAPS:return L"HID input value capabilities exceed the mapper profile";
    default:return L"HID mapper profile accepted";
    }
}
inline MapperProfileProbe ProbeHidProfile(HDEVINFO set,SP_DEVINFO_DATA& device,bool mouse,const HidProfileApi& api,unsigned controllerProfiles) {
    MapperProfileProbe result;
    auto failed=[&](DWORD error){result.error=error?error:ERROR_INVALID_DATA;return result;};
    wchar_t expected[MAX_DEVICE_ID_LEN]{};
    if(!api.instance(set,&device,expected,_countof(expected),nullptr))return failed(GetLastError());
    GUID guid{};api.hidGuid(&guid);
    struct Set {const HidProfileApi& api;HDEVINFO value;~Set(){if(value!=INVALID_HANDLE_VALUE)api.destroy(value);}} interfaces{api,api.devices(&guid,expected,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
    if(interfaces.value==INVALID_HANDLE_VALUE)return failed(GetLastError());
    SP_DEVICE_INTERFACE_DATA entry{};entry.cbSize=sizeof(entry);
    if(!api.enumerate(interfaces.value,nullptr,&guid,0,&entry))return failed(GetLastError());
    DWORD bytes=0;
    if(api.detail(interfaces.value,&entry,nullptr,0,&bytes,nullptr) || GetLastError()!=ERROR_INSUFFICIENT_BUFFER || bytes<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || bytes>1024*1024)
        return failed(ERROR_INVALID_DATA);
    std::vector<unsigned char> storage(bytes,0);
    auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());detail->cbSize=sizeof(*detail);
    SP_DEVINFO_DATA actual{};actual.cbSize=sizeof(actual);
    if(!api.detail(interfaces.value,&entry,detail,bytes,nullptr,&actual))return failed(GetLastError());
    wchar_t found[MAX_DEVICE_ID_LEN]{};
    if(!api.instance(interfaces.value,&actual,found,_countof(found),nullptr))return failed(GetLastError());
    if(actual.DevInst!=device.DevInst || _wcsicmp(found,expected))return failed(ERROR_RETRY);
    // Metadata access works for the exclusive Windows keyboard/mouse consumer.
    // Never request GENERIC_READ/WRITE or submit an input report here.
    struct Handle {const HidProfileApi& api;HANDLE value;~Handle(){if(value!=INVALID_HANDLE_VALUE)api.close(value);}} handle{api,api.open(detail->DevicePath,0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr)};
    if(handle.value==INVALID_HANDLE_VALUE)return failed(GetLastError());
    HID_COLLECTION_INFORMATION information{};DWORD returned=0;
    if(!api.ioctl(handle.value,IOCTL_HID_GET_COLLECTION_INFORMATION,nullptr,0,&information,sizeof(information),&returned,nullptr))return failed(GetLastError());
    if(returned!=sizeof(information))return failed(ERROR_INVALID_DATA);
    if(!information.DescriptorSize || information.DescriptorSize>IC_MAPPER_DESCRIPTOR_MAX) {
        result.profile=IC_MAPPER_PROFILE_DESCRIPTOR_SIZE;result.reason=controllerProfiles?L"HID controller descriptor exceeds the supported bounds":MapperProfileReason(result.profile);return result;
    }
    struct Data {const HidProfileApi& api;PHIDP_PREPARSED_DATA value=nullptr;~Data(){if(value)api.freePreparsed(value);}} data{api};
    if(!api.preparsed(handle.value,&data.value))return failed(GetLastError());
    HIDP_CAPS caps{};
    if(api.caps(data.value,&caps)!=HIDP_STATUS_SUCCESS)return failed(ERROR_INVALID_DATA);
    // Revalidate the devnode identity after metadata reads, before callers bind.
    wchar_t current[MAX_DEVICE_ID_LEN]{};
    if(!api.instance(set,&device,current,_countof(current),nullptr))return failed(GetLastError());
    if(_wcsicmp(current,expected))return failed(ERROR_RETRY);
    if(controllerProfiles) {
        if(!(IcHidGameProfile(caps.UsagePage,caps.Usage)&controllerProfiles))result.profile=IC_MAPPER_PROFILE_USAGE;
        else if(!caps.InputReportByteLength || caps.InputReportByteLength>IC_HID_REPORT_MAX)result.profile=IC_MAPPER_PROFILE_INPUT_SIZE;
        else if(caps.NumberInputValueCaps>1024 || caps.NumberInputButtonCaps>1024 ||
                (!caps.NumberInputValueCaps && !caps.NumberInputButtonCaps))result.profile=IC_MAPPER_PROFILE_VALUE_CAPS;
    } else result.profile=IcMapperProfile(information.DescriptorSize,caps.UsagePage,caps.Usage,caps.InputReportByteLength,caps.NumberInputValueCaps,mouse?2:6);
    result.supported=result.profile==IC_MAPPER_PROFILE_OK;
    result.reason=controllerProfiles?(result.supported?L"HID controller descriptor accepted":L"HID controller descriptor is outside the loaded driver's profiles"):MapperProfileReason(result.profile);
    return result;
}
inline MapperProfileProbe ProbeMapperProfile(HDEVINFO set,SP_DEVINFO_DATA& device,bool mouse,const HidProfileApi& api={}) {
    return ProbeHidProfile(set,device,mouse,api,0);
}
inline MapperProfileProbe ProbeControllerProfile(HDEVINFO set,SP_DEVINFO_DATA& device,unsigned profiles,const HidProfileApi& api={}) {
    if(!profiles)return {};
    return ProbeHidProfile(set,device,false,api,profiles);
}
}
