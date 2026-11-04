#include "Interceptor.h"
namespace Interceptor {Init_Result Restore_Controller_Hid_Filters();}
#include <SetupAPI.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devguid.h>
#include <string>
#include <vector>
#include <set>
#include <iostream>
#include <iomanip>
#include <system_error>
#include "SetupBinding.h"

namespace {
void Check(BOOL ok,const char* operation) { if(!ok)throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),operation); }
struct Devices { HDEVINFO value;~Devices(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);} };
struct Target { std::wstring instance,hardware,inf,service;bool mouse; };
bool RecheckMapper(HDEVINFO set,SP_DEVINFO_DATA& device,const Target& target,bool restore,bool onlyNew) {
    const auto& expectedClass=target.mouse?GUID_DEVCLASS_MOUSE:GUID_DEVCLASS_KEYBOARD;
    if(!IsEqualGUID(device.ClassGuid,expectedClass))throw std::system_error(ERROR_RETRY,std::system_category(),"Mapper class changed");
    wchar_t hardware[4096]{},service[256]{};DWORD type=0;
    Check(SetupDiGetDeviceRegistryPropertyW(set,&device,SPDRP_HARDWAREID,&type,reinterpret_cast<BYTE*>(hardware),sizeof(hardware)-2*sizeof(wchar_t),nullptr),"Recheck mapper hardware ID");
    if(type!=REG_MULTI_SZ || _wcsicmp(hardware,target.hardware.c_str()))throw std::system_error(ERROR_RETRY,std::system_category(),"Mapper identity changed");
    Check(SetupDiGetDeviceRegistryPropertyW(set,&device,SPDRP_SERVICE,&type,reinterpret_cast<BYTE*>(service),sizeof(service)-sizeof(wchar_t),nullptr),"Recheck mapper service");
    if(type!=REG_SZ)throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Invalid mapper service property");
    if(_wcsicmp(service,target.service.c_str()))return false;
    const bool ours=!_wcsicmp(service,L"Interceptor");
    return !((restore && !ours) || (!restore && !ours && _wcsicmp(service,target.mouse?L"mouhid":L"kbdhid")) || (onlyNew && ours));
}
constexpr wchar_t ControlHardware[]=L"ROOT\\InterceptorControl";
constexpr wchar_t ControlInstance[]=L"ROOT\\InterceptorControl\\0000";
bool IsControl(HDEVINFO set,SP_DEVINFO_DATA& d) {
    wchar_t id[MAX_DEVICE_ID_LEN]{},hardware[4096]{};DWORD type=0;
    Check(SetupDiGetDeviceInstanceIdW(set,&d,id,MAX_DEVICE_ID_LEN,nullptr),"Read control instance");
    if(_wcsnicmp(id,L"ROOT\\InterceptorControl\\",_countof(ControlHardware)))return false;
    if(!SetupDiGetDeviceRegistryPropertyW(set,&d,SPDRP_HARDWAREID,&type,reinterpret_cast<BYTE*>(hardware),sizeof(hardware)-2*sizeof(wchar_t),nullptr)) {
        if(GetLastError()==ERROR_INVALID_DATA)return false;Check(FALSE,"Read control hardware ID");
    }
    if(type!=REG_MULTI_SZ)return false;
    for(const auto* p=hardware;*p;p+=wcslen(p)+1)if(!_wcsicmp(p,ControlHardware))return true;
    return false;
}
bool RemoveControlDevice(HDEVINFO set,SP_DEVINFO_DATA& d) {
    SP_REMOVEDEVICE_PARAMS remove{};remove.ClassInstallHeader.cbSize=sizeof(SP_CLASSINSTALL_HEADER);
    remove.ClassInstallHeader.InstallFunction=DIF_REMOVE;remove.Scope=DI_REMOVEDEVICE_GLOBAL;
    Check(SetupDiSetClassInstallParamsW(set,&d,&remove.ClassInstallHeader,sizeof(remove)),"Prepare root control removal");
    Check(SetupDiCallClassInstaller(DIF_REMOVE,set,&d),"Remove root control device");
    SP_DEVINSTALL_PARAMS_W params{};params.cbSize=sizeof(params);
    Check(SetupDiGetDeviceInstallParamsW(set,&d,&params),"Query root removal restart");
    return (params.Flags&(DI_NEEDREBOOT|DI_NEEDRESTART))!=0;
}
bool ConfigureControl(const wchar_t* inf,bool remove) {
    // Removing a package can leave its root instance without a setup class.
    // Include those instances so retrying installation reuses the stable ID.
    Devices scan{SetupDiGetClassDevsW(nullptr,nullptr,nullptr,DIGCF_ALLCLASSES)};
    Check(scan.value!=INVALID_HANDLE_VALUE,"Enumerate root control devices");
    std::vector<std::wstring> ids;
    for(DWORD index=0;;++index) {
        SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);
        if(!SetupDiEnumDeviceInfo(scan.value,index,&d)){if(GetLastError()==ERROR_NO_MORE_ITEMS)break;Check(FALSE,"Enumerate root control device");}
        if(!IsControl(scan.value,d))continue;
        wchar_t id[MAX_DEVICE_ID_LEN]{};Check(SetupDiGetDeviceInstanceIdW(scan.value,&d,id,MAX_DEVICE_ID_LEN,nullptr),"Read root control ID");ids.emplace_back(id);
    }
    // A class-specific set cannot open an unbound/classless root instance.
    Devices set{SetupDiCreateDeviceInfoList(nullptr,nullptr)};
    Check(set.value!=INVALID_HANDLE_VALUE,"Create root control information set");
    bool reboot=false;
    if(remove) {
        for(const auto& id:ids) {
            SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);Check(SetupDiOpenDeviceInfoW(set.value,id.c_str(),nullptr,0,&d),"Open root control device");
            reboot|=RemoveControlDevice(set.value,d);
        }
        std::cout<<ids.size()<<" root control devices removed\n";return reboot;
    }
    SP_DEVINFO_DATA created{};created.cbSize=sizeof(created);bool registered=false;
    try {
        if(ids.empty()) {
            // Stable instance ID makes repeated installation idempotent. Unlike
            // DICD_GENERATE_ID, this cannot create a new device on every click.
            Check(SetupDiCreateDeviceInfoW(set.value,ControlInstance,&GUID_DEVCLASS_SYSTEM,L"Interceptor Control",nullptr,0,&created),"Create root control device");
            constexpr wchar_t hardware[]=L"ROOT\\InterceptorControl\0";
            Check(SetupDiSetDeviceRegistryPropertyW(set.value,&created,SPDRP_HARDWAREID,reinterpret_cast<const BYTE*>(hardware),sizeof(hardware)),"Set root control hardware ID");
            Check(SetupDiCallClassInstaller(DIF_REGISTERDEVICE,set.value,&created),"Register root control device");registered=true;
        }
        BOOL needsRestart=FALSE;
        Check(UpdateDriverForPlugAndPlayDevicesW(nullptr,ControlHardware,inf,INSTALLFLAG_FORCE|INSTALLFLAG_NONINTERACTIVE,&needsRestart),"Bind root control driver");
        reboot=needsRestart!=FALSE;
    } catch(...) {
        if(registered)try {RemoveControlDevice(set.value,created);}
        catch(const std::system_error& e){
            const auto code=static_cast<unsigned long>(e.code().value());
            std::cerr<<"Root rollback failed: "<<e.what()<<" [Win32 "<<std::dec<<code
                <<" / 0x"<<std::hex<<std::uppercase<<std::setw(8)<<std::setfill('0')<<code<<std::dec<<"]\n";
        }catch(const std::exception& e){std::cerr<<"Root rollback failed: "<<e.what()<<'\n';}
        throw;
    }
    std::cout<<"Root control configured; reboot="<<reboot<<'\n';return reboot;
}
}
int ConfigureControlDriver(const wchar_t* inf,bool remove) {return ConfigureControl(inf,remove)?ERROR_SUCCESS_REBOOT_REQUIRED:0;}
int InstallDriver(const wchar_t* directory,bool restore,bool onlyNew,const wchar_t* mouseInf,const wchar_t* controlInf) {
    const GUID* classes[]={&GUID_DEVCLASS_KEYBOARD,&GUID_DEVCLASS_MOUSE};
    std::vector<Target> targets;std::set<std::wstring> seen;
    wchar_t windows[MAX_PATH]{};Check(GetWindowsDirectoryW(windows,MAX_PATH)!=0,"GetWindowsDirectory");
    // Snapshot all devices before changes: updating one device may invalidate enumeration.
    for(unsigned kind=0;kind<2;++kind) {
        Devices devices{SetupDiGetClassDevsW(classes[kind],nullptr,nullptr,restore?0:DIGCF_PRESENT)};
        Check(devices.value!=INVALID_HANDLE_VALUE,"Enumerate devices");
        for(DWORD i=0;;++i) {
            SP_DEVINFO_DATA data{};data.cbSize=sizeof(data);
            if(!SetupDiEnumDeviceInfo(devices.value,i,&data)) { if(GetLastError()==ERROR_NO_MORE_ITEMS)break;Check(FALSE,"Enumerate device"); }
            wchar_t hardware[4096]{},service[256]{};DWORD type=0;
            if(!SetupDiGetDeviceRegistryPropertyW(devices.value,&data,SPDRP_HARDWAREID,&type,reinterpret_cast<BYTE*>(hardware),sizeof(hardware)-2*sizeof(wchar_t),nullptr))continue;
            if(_wcsnicmp(hardware,L"HID\\",4))continue;
            if(!SetupDiGetDeviceRegistryPropertyW(devices.value,&data,SPDRP_SERVICE,&type,reinterpret_cast<BYTE*>(service),sizeof(service)-sizeof(wchar_t),nullptr))continue;
            if(onlyNew && !_wcsicmp(service,L"Interceptor"))continue;
            if(restore) { if(_wcsicmp(service,L"Interceptor"))continue; }
            else if(_wcsicmp(service,kind?L"mouhid":L"kbdhid") && _wcsicmp(service,L"Interceptor")) {
                std::wcerr<<L"Skipped custom HID driver: "<<hardware<<L" ("<<service<<L")\n";continue;
            }
            wchar_t instance[MAX_DEVICE_ID_LEN]{};
            Check(SetupDiGetDeviceInstanceIdW(devices.value,&data,instance,MAX_DEVICE_ID_LEN,nullptr),"Read mapper instance ID");
            if(!seen.insert(instance).second)continue;
            std::wstring inf;
            if(onlyNew)inf=kind?mouseInf:directory;
            else {
                inf=restore?std::wstring(windows)+L"\\INF":directory;
                inf+=restore?(kind?L"\\msmouse.inf":L"\\keyboard.inf"):(kind?L"\\InterceptorMouse.inf":L"\\InterceptorKeyboard.inf");
            }
            targets.push_back({instance,hardware,inf,service,kind!=0});
        }
    }
    bool needsReboot=false;
    if(!restore && (!onlyNew || controlInf)) {
        const auto path=onlyNew?std::wstring(controlInf):std::wstring(directory)+L"\\InterceptorControl.inf";
        needsReboot=ConfigureControl(path.c_str(),false);
    }
    if(restore) {
        const auto hid=Interceptor::Restore_Controller_Hid_Filters();
        if(hid.error)throw std::system_error(hid.error,std::system_category(),"Remove HID controller filters");
        needsReboot|=hid.reboot_required;
    }
    for(const auto& target:targets) {
        Devices devices{SetupDiCreateDeviceInfoList(nullptr,nullptr)};
        Check(devices.value!=INVALID_HANDLE_VALUE,"Open mapper information set");
        SP_DEVINFO_DATA data{};data.cbSize=sizeof(data);
        if(!SetupDiOpenDeviceInfoW(devices.value,target.instance.c_str(),nullptr,0,&data)) {
            const DWORD error=GetLastError();
            if(restore && error==ERROR_NO_SUCH_DEVINST)continue;
            throw std::system_error(static_cast<int>(error),std::system_category(),"Open mapper instance");
        }
        // Recheck ownership after the snapshot: another identical device with
        // a custom driver must not be changed by a hardware-ID-wide update.
        if(!RecheckMapper(devices.value,data,target,restore,onlyNew)) {
            std::wcout<<L"Skipped changed mapper instance: "<<target.instance<<L"\n";continue;
        }
        // The profile is inspected through the Windows HID mapper before a
        // new association. An existing Interceptor mapper does not expose that
        // metadata route; an unchanged binding can still update its INF. The
        // service snapshot above prevents skipping this probe after a race back
        // to kbdhid/mouhid, or adopting a binding changed by another installer.
        if(!restore && _wcsicmp(target.service.c_str(),L"Interceptor")) {
            const auto profile=Interceptor::SetupDetail::ProbeMapperProfile(devices.value,data,target.mouse);
            if(!profile.supported) {
                std::wcout<<L"Skipped mapper association; current driver unchanged: "<<target.instance
                    <<L"; "<<profile.reason<<L"; error="<<profile.error<<L"\n";continue;
            }
            if(!RecheckMapper(devices.value,data,target,restore,onlyNew)) {
                std::wcout<<L"Skipped changed mapper instance after profile inspection: "<<target.instance<<L"\n";continue;
            }
        }
        bool reboot=false;
        try {reboot=Interceptor::SetupDetail::BindInstance(devices.value,data,target.inf.c_str());}
        catch(const std::system_error& e) {
            // Package removal also restores nonpresent instances.
            if(restore && e.code().value()==ERROR_NO_SUCH_DEVINST)continue;
            throw;
        }
        needsReboot|=reboot;
        std::wcout<<L"Configured "<<target.instance<<L"; reboot="<<reboot<<L"\n";
    }
    std::cout<<targets.size()<<" device instances processed\n";
    if(onlyNew && targets.empty())std::cout<<"No additional supported HID devices to associate.\n";
    if(restore)needsReboot|=ConfigureControl(nullptr,true);
    return needsReboot?3010:0;
}
