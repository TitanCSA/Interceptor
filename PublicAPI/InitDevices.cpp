#include "ContextAPI.h"
#include "NativePolicy.h"
#include "HidProfileProbe.h"
#include "../Shared/ControllerXusbMirror.h"
#include <SetupAPI.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <commctrl.h>
#include <initguid.h>
#include <devguid.h>
#include <string>
#include <cwctype>
#include <iterator>
#include <vector>
#include <mutex>
#include <system_error>
#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"newdev.lib")
#pragma comment(lib,"cfgmgr32.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace Interceptor {
namespace {
struct DeviceSet {
    HDEVINFO value=INVALID_HANDLE_VALUE;
    ~DeviceSet(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}
};
struct DriverList {
    HDEVINFO set;SP_DEVINFO_DATA* device;
    ~DriverList(){SetupDiDestroyDriverInfoList(set,device,SPDIT_COMPATDRIVER);}
};
struct MutexHandle {
    HANDLE value=nullptr;bool owned=false;
    ~MutexHandle(){if(owned)ReleaseMutex(value);if(value)CloseHandle(value);}
};
// A volatile key survives application exits but is discarded by a full Windows
// restart. Otherwise a second EXE could miss a reboot requested by the first.
constexpr auto RestartKey=L"SOFTWARE\\Interceptor\\PendingDeviceRestart";
DWORD PendingRestart(bool& pending) {
    DWORD flag=0,bytes=sizeof(flag);
    const LSTATUS status=RegGetValueW(HKEY_LOCAL_MACHINE,RestartKey,L"Required",RRF_RT_REG_DWORD|RRF_SUBKEY_WOW6464KEY,nullptr,&flag,&bytes);
    pending=status==ERROR_SUCCESS && flag!=0;
    return status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND?ERROR_SUCCESS:static_cast<DWORD>(status);
}
DWORD RememberRestart() {
    HKEY key=nullptr;
    LSTATUS status=RegCreateKeyExW(HKEY_LOCAL_MACHINE,RestartKey,0,nullptr,REG_OPTION_VOLATILE,KEY_SET_VALUE|KEY_WOW64_64KEY,nullptr,&key,nullptr);
    if(status!=ERROR_SUCCESS)return static_cast<DWORD>(status);
    const DWORD flag=1;status=RegSetValueExW(key,L"Required",0,REG_DWORD,reinterpret_cast<const BYTE*>(&flag),sizeof(flag));
    RegCloseKey(key);return static_cast<DWORD>(status);
}
void RecordError(Init_Result& result,DWORD error) {
    if(!result.error)result.error=error?error:ERROR_GEN_FAILURE;
}
bool IsAdministrator() {
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;PSID sid=nullptr;BOOL member=FALSE;
    if(!AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid))return false;
    const BOOL ok=CheckTokenMembership(nullptr,sid,&member);FreeSid(sid);return ok && member;
}
std::wstring Property(HDEVINFO set,SP_DEVINFO_DATA& device,DWORD property) {
    DWORD size=0,type=0;
    if(!SetupDiGetDeviceRegistryPropertyW(set,&device,property,&type,nullptr,0,&size) && GetLastError()!=ERROR_INSUFFICIENT_BUFFER)
        throw std::system_error(GetLastError(),std::system_category(),"Read device property");
    std::vector<wchar_t> data(size/sizeof(wchar_t)+2,0);
    if(!SetupDiGetDeviceRegistryPropertyW(set,&device,property,&type,reinterpret_cast<BYTE*>(data.data()),size,nullptr))
        throw std::system_error(GetLastError(),std::system_category(),"Read device property");
    return data.data(); // First hardware ID is sufficient to check the HID bus.
}
void CheckStarted(SP_DEVINFO_DATA& device,Init_Result& result) {
    ULONG status=0,problem=0;
    if(CM_Get_DevNode_Status(&status,&problem,device.DevInst,0)!=CR_SUCCESS) {RecordError(result,ERROR_DEVICE_NOT_CONNECTED);return;}
    if(problem==CM_PROB_NEED_RESTART)result.reboot_required=true;
    else if(!(status&DN_STARTED) || (status&DN_HAS_PROBLEM))RecordError(result,ERROR_NOT_READY);
}
void Associate(HDEVINFO set,SP_DEVINFO_DATA& device,bool mouse,Init_Result& result) {
    const auto hardware=Property(set,device,SPDRP_HARDWAREID);
    if(_wcsnicmp(hardware.c_str(),L"HID\\",4)) {++result.skipped;return;}
    const auto service=Property(set,device,SPDRP_SERVICE);
    if(!_wcsicmp(service.c_str(),L"Interceptor")) {++result.already_associated;CheckStarted(device,result);return;}
    if(_wcsicmp(service.c_str(),mouse?L"mouhid":L"kbdhid")) {++result.skipped;return;}
    const auto profile=SetupDetail::ProbeMapperProfile(set,device,mouse);
    if(!profile.supported) {
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        if(!SetupDiGetDeviceInstanceIdW(set,&device,instance,_countof(instance),nullptr))instance[0]=0;
        ++result.skipped;result.skipped_devices.push_back({instance,profile.reason,profile.error});return;
    }
    SP_DEVINSTALL_PARAMS_W params{};params.cbSize=sizeof(params);
    if(!SetupDiGetDeviceInstallParamsW(set,&device,&params)) {RecordError(result,GetLastError());return;}
    params.Flags|=DI_QUIETINSTALL;
    if(!SetupDiSetDeviceInstallParamsW(set,&device,&params) || !SetupDiBuildDriverInfoList(set,&device,SPDIT_COMPATDRIVER)) {RecordError(result,GetLastError());return;}
    DriverList list{set,&device};SP_DRVINFO_DATA_W selected{};bool found=false;
    for(DWORD index=0;;++index) {
        SP_DRVINFO_DATA_W candidate{};candidate.cbSize=sizeof(candidate);
        if(!SetupDiEnumDriverInfoW(set,&device,SPDIT_COMPATDRIVER,index,&candidate)) {
            if(GetLastError()!=ERROR_NO_MORE_ITEMS) {RecordError(result,GetLastError());return;}
            break;
        }
        if(_wcsicmp(candidate.ProviderName,L"Interceptor Project") ||
           _wcsicmp(candidate.Description,mouse?L"Interceptor HID Mouse Mapper":L"Interceptor HID Keyboard Mapper"))continue;
        if(!found || candidate.DriverVersion>selected.DriverVersion ||
           (candidate.DriverVersion==selected.DriverVersion && CompareFileTime(&candidate.DriverDate,&selected.DriverDate)>0)) {selected=candidate;found=true;}
    }
    if(!found) {RecordError(result,ERROR_NOT_FOUND);return;}
    // A different installer or PnP may have changed the instance since the
    // metadata probe. Preserve a newly selected custom driver rather than bind.
    if(_wcsicmp(Property(set,device,SPDRP_SERVICE).c_str(),service.c_str()) ||
       _wcsicmp(Property(set,device,SPDRP_HARDWAREID).c_str(),hardware.c_str())) {
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        if(!SetupDiGetDeviceInstanceIdW(set,&device,instance,_countof(instance),nullptr))instance[0]=0;
        ++result.skipped;result.skipped_devices.push_back({instance,L"Mapper identity/service changed before association",ERROR_RETRY});return;
    }
    BOOL reboot=FALSE;
    // Exact devnode, not an update of every instance sharing a hardware ID.
    const BOOL installed=DiInstallDevice(nullptr,set,&device,&selected,DIIDFLAG_NOFINISHINSTALLUI,&reboot);
    const DWORD error=installed?ERROR_SUCCESS:GetLastError();
    result.reboot_required|=reboot!=FALSE;
    if(SetupDiGetDeviceInstallParamsW(set,&device,&params))result.reboot_required|=(params.Flags&(DI_NEEDREBOOT|DI_NEEDRESTART))!=0;
    if(!installed) {RecordError(result,error);return;}
    ++result.associated;
    if(!result.reboot_required)CheckStarted(device,result);
}
#include "HidDevices.inl"
#include "NativeDevices.inl"
#include "XusbMirrors.inl"
}

Init_Result Init_AllDevices() {
    static std::mutex mutex;std::lock_guard<std::mutex> guard(mutex);
    Init_Result result;
    if(!IsAdministrator()) {result.error=ERROR_ACCESS_DENIED;return result;}
    MutexHandle interprocess{CreateMutexW(nullptr,FALSE,L"Global\\Interceptor.InitAllDevices"),false};
    if(!interprocess.value) {result.error=GetLastError();return result;}
    const DWORD wait=WaitForSingleObject(interprocess.value,30000);
    if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED) {result.error=wait==WAIT_TIMEOUT?ERROR_BUSY:GetLastError();return result;}
    interprocess.owned=true;
    result.error=PendingRestart(result.reboot_required);
    if(result.error)return result;
    if(result.reboot_required)return result;
    // Snapshot instance IDs before changing stacks; updating one device can
    // invalidate an ongoing enumeration. Reopen and recheck each instance.
    struct Target {std::wstring id;bool mouse;};std::vector<Target> targets;
    const GUID* classes[]={&GUID_DEVCLASS_KEYBOARD,&GUID_DEVCLASS_MOUSE};
    for(unsigned kind=0;kind<2;++kind) {
        DeviceSet devices{SetupDiGetClassDevsW(classes[kind],nullptr,nullptr,DIGCF_PRESENT)};
        if(devices.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());continue;}
        for(DWORD index=0;;++index) {
            SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
            if(!SetupDiEnumDeviceInfo(devices.value,index,&device)) {if(GetLastError()!=ERROR_NO_MORE_ITEMS)RecordError(result,GetLastError());break;}
            wchar_t id[MAX_DEVICE_ID_LEN]{};
            if(!SetupDiGetDeviceInstanceIdW(devices.value,&device,id,MAX_DEVICE_ID_LEN,nullptr)) {RecordError(result,GetLastError());continue;}
            targets.push_back({id,kind!=0});
        }
    }
    for(const auto& target:targets) {
        DeviceSet devices{SetupDiCreateDeviceInfoList(nullptr,nullptr)};
        if(devices.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());break;}
        SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
        if(!SetupDiOpenDeviceInfoW(devices.value,target.id.c_str(),nullptr,0,&device)) {RecordError(result,GetLastError());continue;}
        try {Associate(devices.value,device,target.mouse,result);}
        catch(const std::system_error& e) {RecordError(result,static_cast<DWORD>(e.code().value()));}
    }
    if(!result.error && !result.reboot_required) {
        // Never attach the HID filter when an older keyboard-only driver is loaded.
        try {
            if(Get_Status().version!=IC_VERSION)throw std::system_error(ERROR_REVISION_MISMATCH,std::system_category());
            IC_CAPABILITIES capabilities{};bool available=false;
            try {capabilities=Get_Capabilities();available=true;}
            catch(const std::system_error& e) {
                // ABI4 keyboard/HID drivers predate the immutable endpoint.
                // Missing endpoint permits no native association whatsoever.
                if(e.code().value()!=ERROR_INVALID_FUNCTION && e.code().value()!=ERROR_NOT_SUPPORTED)throw;
            }
            AssociateControllers(result,false);
            if(!result.error)AssociateNativeControllers(result,available?&capabilities:nullptr,false);
            if(!result.error)AssociateXusbMirrors(result,available?&capabilities:nullptr,false);
        }
        catch(const std::system_error& e) {RecordError(result,static_cast<DWORD>(e.code().value()));}
    }
    if(result.reboot_required) {
        const DWORD error=RememberRestart();if(error)RecordError(result,error);
    }
    if(!result.error && !result.reboot_required) {
        try {if(Get_Status().version!=IC_VERSION)RecordError(result,ERROR_REVISION_MISMATCH);}
        catch(const std::system_error& e) {RecordError(result,static_cast<DWORD>(e.code().value()));}
    }
    result.ready=!result.error && !result.reboot_required;
    return result;
}
// Installer helper. Caller must be elevated and serialize setup operations.
Init_Result Restore_Controller_Hid_Filters() {
    Init_Result result;
    if(!IsAdministrator()) {result.error=ERROR_ACCESS_DENIED;return result;}
    AssociateControllers(result,true);AssociateNativeControllers(result,nullptr,true);AssociateXusbMirrors(result,nullptr,true);return result;
}

}
