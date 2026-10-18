#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include "../helper/SetupBinding.h"
#include <system_error>
extern "C" __declspec(dllexport) DWORD __stdcall RestoreInterceptorMapper(const wchar_t* instance,const wchar_t* inf,BOOL mouse,BOOL* reboot) noexcept {
    if(!instance || !inf || !reboot)return ERROR_INVALID_PARAMETER;
    *reboot=FALSE;
    HDEVINFO set=SetupDiCreateDeviceInfoList(nullptr,nullptr);
    if(set==INVALID_HANDLE_VALUE)return GetLastError();
    DWORD result=ERROR_SUCCESS;
    try {
        SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
        if(!SetupDiOpenDeviceInfoW(set,instance,nullptr,0,&device))throw std::system_error(GetLastError(),std::system_category());
        const GUID expected=mouse?GUID_DEVCLASS_MOUSE:GUID_DEVCLASS_KEYBOARD;
        if(!IsEqualGUID(device.ClassGuid,expected))throw std::system_error(ERROR_INVALID_DATA,std::system_category());
        wchar_t service[256]{};DWORD type=0;
        if(!SetupDiGetDeviceRegistryPropertyW(set,&device,SPDRP_SERVICE,&type,reinterpret_cast<BYTE*>(service),sizeof(service),nullptr))throw std::system_error(GetLastError(),std::system_category());
        if(type!=REG_SZ)throw std::system_error(ERROR_INVALID_DATA,std::system_category());
        // Never overwrite a binding changed by another installer meanwhile.
        if(!_wcsicmp(service,L"Interceptor"))*reboot=Interceptor::SetupDetail::BindInstance(set,device,inf)?TRUE:FALSE;
    } catch(const std::system_error& e){result=static_cast<DWORD>(e.code().value());}
      catch(...){result=ERROR_UNHANDLED_EXCEPTION;}
    SetupDiDestroyDeviceInfoList(set);return result;
}
