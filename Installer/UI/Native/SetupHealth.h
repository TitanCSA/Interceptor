#pragma once
// Installer-only, read-only PnP inventory. Never opens the capture driver.
#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <iostream>
#include <string>
#include <vector>
#include <system_error>
namespace SetupHealth {
inline void Json(const std::wstring& value) {
    constexpr char hex[]="0123456789ABCDEF";
    std::cout<<'"';
    for(const wchar_t c:value) {
        const auto v=static_cast<unsigned short>(c);
        if(v>=0x20 && v<0x7f && c!=L'"' && c!=L'\\')std::cout<<static_cast<char>(c);
        else std::cout<<"\\u"<<hex[(v>>12)&15]<<hex[(v>>8)&15]<<hex[(v>>4)&15]<<hex[v&15];
    }
    std::cout<<'"';
}
inline std::vector<std::wstring> Strings(HDEVINFO set,SP_DEVINFO_DATA& device,DWORD property) {
    DWORD type=0,bytes=0;
    if(!SetupDiGetDeviceRegistryPropertyW(set,&device,property,&type,nullptr,0,&bytes)) {
        const auto error=GetLastError();
        if(error==ERROR_INVALID_DATA || error==ERROR_NO_SUCH_DEVINST || error==ERROR_DEVICE_NOT_CONNECTED)return {};
        if(error!=ERROR_INSUFFICIENT_BUFFER)throw std::system_error(error,std::system_category(),"Read setup property size");
    }
    if(bytes>65536 || bytes%sizeof(wchar_t))throw std::runtime_error("Invalid setup property size");
    std::vector<wchar_t> data(bytes/sizeof(wchar_t)+2,0);
    if(!SetupDiGetDeviceRegistryPropertyW(set,&device,property,&type,
        reinterpret_cast<BYTE*>(data.data()),bytes,&bytes)) {
        const auto error=GetLastError();
        if(error==ERROR_INVALID_DATA || error==ERROR_NO_SUCH_DEVINST || error==ERROR_DEVICE_NOT_CONNECTED)return {};
        throw std::system_error(error,std::system_category(),"Read setup property");
    }
    if(type!=REG_SZ && type!=REG_MULTI_SZ)throw std::runtime_error("Invalid setup property type");
    std::vector<std::wstring> result;
    for(std::size_t start=0;start<data.size() && data[start];) {
        std::size_t end=start;
        while(end<data.size() && data[end])++end;
        if(end==data.size())throw std::runtime_error("Unterminated setup property");
        result.emplace_back(data.data()+start,end-start);
        if(type==REG_SZ)break;
        start=end+1;
    }
    return result;
}
inline bool Has(const std::vector<std::wstring>& values,const wchar_t* service) {
    for(const auto& value:values)if(!_wcsicmp(value.c_str(),service))return true;
    return false;
}
inline int Print(const wchar_t* controlHardware) {
    struct Set {HDEVINFO value;~Set(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}};
    Set set{SetupDiGetClassDevsW(nullptr,nullptr,nullptr,DIGCF_ALLCLASSES|DIGCF_PRESENT)};
    if(set.value==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Enumerate setup devices");
    const std::wstring root=std::wstring(controlHardware)+L"\\";
    std::cout<<"{\"schema\":1,\"service\":";Json(IC_SDK_SERVICE);std::cout<<",\"devices\":[";
    bool comma=false;
    for(DWORD index=0;;++index) {
        SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
        if(!SetupDiEnumDeviceInfo(set.value,index,&device)) {
            const auto error=GetLastError();
            if(error==ERROR_NO_MORE_ITEMS)break;
            throw std::system_error(error,std::system_category(),"Enumerate setup device");
        }
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if(!SetupDiGetDeviceInstanceIdW(set.value,&device,id,MAX_DEVICE_ID_LEN,nullptr)) {
            if(GetLastError()==ERROR_NO_SUCH_DEVINST)continue;
            throw std::system_error(GetLastError(),std::system_category(),"Read setup instance");
        }
        const auto service=Strings(set.value,device,SPDRP_SERVICE);
        const auto upper=Strings(set.value,device,SPDRP_UPPERFILTERS);
        const auto lower=Strings(set.value,device,SPDRP_LOWERFILTERS);
        const bool control=_wcsnicmp(id,root.c_str(),root.size())==0;
        const bool owned=Has(service,IC_SDK_SERVICE)||Has(upper,IC_SDK_SERVICE)||Has(lower,IC_SDK_SERVICE);
        if(!control&&!owned)continue;
        ULONG status=0,problem=0;
        const auto error=CM_Get_DevNode_Status(&status,&problem,device.DevInst,0);
        if(comma)std::cout<<',';comma=true;
        std::cout<<"{\"instance\":";Json(id);
        std::cout<<",\"root\":"<<(control?"true":"false")<<",\"owned\":"<<(owned?"true":"false")
            <<",\"status_error\":"<<error<<",\"status\":"<<status<<",\"problem\":"<<problem<<'}';
    }
    std::cout<<"]}\n";return 0;
}
} // namespace SetupHealth

