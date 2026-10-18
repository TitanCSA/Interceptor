#pragma once
#include <windows.h>
#include <SetupAPI.h>
#include <newdev.h>
#include <cwchar>
#include <cstring>
#include <system_error>

namespace Interceptor::SetupDetail {
// Installation is scoped to this devnode, even when another instance has the
// same hardware IDs. The package must already be in the Windows Driver Store.
struct BindingApi {
    decltype(&SetupDiGetDeviceInstallParamsW) getParams=SetupDiGetDeviceInstallParamsW;
    decltype(&SetupDiSetDeviceInstallParamsW) setParams=SetupDiSetDeviceInstallParamsW;
    decltype(&SetupDiBuildDriverInfoList) build=SetupDiBuildDriverInfoList;
    decltype(&SetupDiEnumDriverInfoW) enumerate=SetupDiEnumDriverInfoW;
    decltype(&SetupDiGetDriverInstallParamsW) getRank=SetupDiGetDriverInstallParamsW;
    decltype(&SetupDiDestroyDriverInfoList) destroy=SetupDiDestroyDriverInfoList;
    decltype(&DiInstallDevice) install=DiInstallDevice;
};
inline void CheckBinding(BOOL ok,const char* operation) {
    if(!ok)throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),operation);
}
inline bool BindInstance(HDEVINFO set,SP_DEVINFO_DATA& device,const wchar_t* inf,const BindingApi& api={}) {
    SP_DEVINSTALL_PARAMS_W params{};params.cbSize=sizeof(params);
    CheckBinding(api.getParams(set,&device,&params),"Read instance installation parameters");
    const auto length=wcslen(inf);
    if(length>=_countof(params.DriverPath))throw std::system_error(ERROR_FILENAME_EXCED_RANGE,std::system_category(),"INF path");
    params.Flags|=DI_ENUMSINGLEINF|DI_QUIETINSTALL;
    params.FlagsEx|=DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    std::memcpy(params.DriverPath,inf,(length+1)*sizeof(wchar_t));
    CheckBinding(api.setParams(set,&device,&params),"Select instance INF");
    struct List {
        const BindingApi& api;HDEVINFO set;SP_DEVINFO_DATA* device;
        ~List(){api.destroy(set,device,SPDIT_COMPATDRIVER);}
    } list{api,set,&device};
    CheckBinding(api.build(set,&device,SPDIT_COMPATDRIVER),"Build instance driver list");
    SP_DRVINFO_DATA_W selected{};DWORD bestRank=MAXDWORD;bool found=false;
    for(DWORD index=0;;++index) {
        SP_DRVINFO_DATA_W candidate{};candidate.cbSize=sizeof(candidate);
        if(!api.enumerate(set,&device,SPDIT_COMPATDRIVER,index,&candidate)) {
            if(GetLastError()!=ERROR_NO_MORE_ITEMS)CheckBinding(FALSE,"Enumerate instance driver list");
            break;
        }
        SP_DRVINSTALL_PARAMS rank{};rank.cbSize=sizeof(rank);
        CheckBinding(api.getRank(set,&device,&candidate,&rank),"Read instance driver rank");
        if(!found || rank.Rank<bestRank || (rank.Rank==bestRank &&
            (CompareFileTime(&candidate.DriverDate,&selected.DriverDate)>0 ||
             (CompareFileTime(&candidate.DriverDate,&selected.DriverDate)==0 && candidate.DriverVersion>selected.DriverVersion)))) {
            selected=candidate;bestRank=rank.Rank;found=true;
        }
    }
    if(!found)throw std::system_error(ERROR_NOT_FOUND,std::system_category(),"No compatible driver in the selected INF");
    BOOL reboot=FALSE;
    CheckBinding(api.install(nullptr,set,&device,&selected,DIIDFLAG_NOFINISHINSTALLUI,&reboot),"Bind HID mapper instance");
    CheckBinding(api.getParams(set,&device,&params),"Read instance restart requirements");
    return reboot || (params.Flags&(DI_NEEDREBOOT|DI_NEEDRESTART))!=0;
}
}
