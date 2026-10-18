// Included by InitDevices.cpp: shares its RAII wrappers and error reporting.
// Changes only per-instance UpperFilters, never the HID class-wide filter list.
#include "../Shared/ControllerDiscovery.h"
std::vector<wchar_t> MultiProperty(HDEVINFO set,SP_DEVINFO_DATA& device,DWORD property) {
    DWORD bytes=0,type=0;
    if(!SetupDiGetDeviceRegistryPropertyW(set,&device,property,&type,nullptr,0,&bytes)) {
        const auto error=GetLastError();
        if(error==ERROR_INVALID_DATA || error==ERROR_FILE_NOT_FOUND)return {0,0};
        if(error!=ERROR_INSUFFICIENT_BUFFER)throw std::system_error(error,std::system_category(),"Read HID property");
    }
    std::vector<wchar_t> result(bytes/sizeof(wchar_t)+2,0);
    if(bytes && !SetupDiGetDeviceRegistryPropertyW(set,&device,property,&type,reinterpret_cast<BYTE*>(result.data()),bytes,nullptr))
        throw std::system_error(GetLastError(),std::system_category(),"Read HID property");
    if(bytes && type!=REG_MULTI_SZ)throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Expected multi-string HID property");
    return result;
}
bool ControllerCollection(HDEVINFO set,SP_DEVINFO_DATA& device,unsigned profiles=IC_HID_PROFILE_DESKTOP) {
    bool game=false;
    for(const auto property:{SPDRP_HARDWAREID,SPDRP_COMPATIBLEIDS}) {
        const auto ids=MultiProperty(set,device,property);
        for(const wchar_t* id=ids.data();*id;id+=wcslen(id)+1) {
            if(IcControllerCollectionProfile(id)&profiles)return true;
            if(!_wcsicmp(id,L"HID_DEVICE_SYSTEM_GAME"))game=true;
        }
    }
    // Some miniports publish the Windows game-controller marker without a
    // usable UP/U identity. Inspect only metadata of this exact collection;
    // never infer its protocol from its manufacturer or friendly name.
    return game && SetupDetail::ProbeControllerProfile(set,device,profiles).supported;
}
bool NativeXbox(DEVINST node) {
    // The HID view of an Xbox controller is not its native game-input path.
    for(unsigned depth=0;depth<32;++depth) {
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if(CM_Get_Device_IDW(node,id,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS)return true;
        for(auto* p=id;*p;++p)*p=static_cast<wchar_t>(towupper(*p));
        if(wcsstr(id,L"&IG_") || wcsstr(id,L"GIP\\"))return true;
        wchar_t service[256]{};ULONG type=0,bytes=sizeof(service)-sizeof(wchar_t);
        const auto query=CM_Get_DevNode_Registry_PropertyW(node,CM_DRP_SERVICE,&type,service,&bytes,0);
        if(query==CR_SUCCESS) {
            if(IcNativeControllerService(service) || !_wcsicmp(service,L"dc1-controller"))return true;
        } else if(query!=CR_NO_SUCH_VALUE)return true; // Fail closed on ambiguous ancestry.
        // xinputhid is commonly an UpperFilter, not the function Service.
        // Treating that node as plain HID would leave native XInput bypassing us.
        for(const auto property:{CM_DRP_UPPERFILTERS,CM_DRP_LOWERFILTERS}) {
            ULONG required=0;
            const auto first=CM_Get_DevNode_Registry_PropertyW(node,property,&type,nullptr,&required,0);
            if(first==CR_NO_SUCH_VALUE)continue;
            if(first!=CR_BUFFER_SMALL || !required || required>65536 || required%sizeof(wchar_t))return true;
            std::vector<wchar_t> filters(required/sizeof(wchar_t)+2,0);
            if(CM_Get_DevNode_Registry_PropertyW(node,property,&type,filters.data(),&required,0)!=CR_SUCCESS || type!=REG_MULTI_SZ)return true;
            for(const auto* name=filters.data();*name;name+=wcslen(name)+1)
                if(IcNativeControllerService(name))return true;
        }
        DEVINST parent=0;const auto result=CM_Get_Parent(&parent,node,0);
        if(result==CR_NO_SUCH_DEVNODE)return false;
        if(result!=CR_SUCCESS)return true;
        node=parent;
    }
    return true;
}
void WriteFilters(HDEVINFO set,SP_DEVINFO_DATA& device,const std::vector<wchar_t>& filters) {
    const bool empty=!filters[0];
    if(!SetupDiSetDeviceRegistryPropertyW(set,&device,SPDRP_UPPERFILTERS,
        empty?nullptr:reinterpret_cast<const BYTE*>(filters.data()),empty?0:static_cast<DWORD>(filters.size()*sizeof(wchar_t))))
        throw std::system_error(GetLastError(),std::system_category(),"Write per-device HID filters");
}
bool ActiveControllerFilter(const std::wstring& id,const GUID& guid=IC_HID_INTERFACE_GUID) {
    DeviceSet set{SetupDiGetClassDevsW(&guid,id.c_str(),nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
    if(set.value==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Query active controller filter");
    SP_DEVICE_INTERFACE_DATA item{};item.cbSize=sizeof(item);
    if(!SetupDiEnumDeviceInterfaces(set.value,nullptr,&guid,0,&item)) {
        if(GetLastError()==ERROR_NO_MORE_ITEMS)return false;
        throw std::system_error(GetLastError(),std::system_category(),"Query controller interface");
    }
    return (item.Flags&SPINT_ACTIVE)!=0;
}
void AssociateControllers(Init_Result& result,bool remove) {
    unsigned profiles=IC_HID_PROFILE_DESKTOP;
    if(!remove)try{profiles=Get_Device_Details().hid_profiles;}
    catch(const std::system_error&){/* Old ABI4 drivers retain their original HID contract. */}
    // Snapshot IDs; restarting a devnode can invalidate live enumeration.
    std::vector<std::wstring> targets;
    DeviceSet snapshot{SetupDiGetClassDevsW(&GUID_DEVCLASS_HIDCLASS,nullptr,nullptr,remove?0:DIGCF_PRESENT)};
    if(snapshot.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());return;}
    for(DWORD i=0;;++i) {
        SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);
        if(!SetupDiEnumDeviceInfo(snapshot.value,i,&d)) {if(GetLastError()!=ERROR_NO_MORE_ITEMS)RecordError(result,GetLastError());break;}
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if(!SetupDiGetDeviceInstanceIdW(snapshot.value,&d,id,MAX_DEVICE_ID_LEN,nullptr)) {RecordError(result,GetLastError());continue;}
        targets.emplace_back(id);
    }
    for(const auto& id:targets)try {
        DeviceSet set{SetupDiCreateDeviceInfoList(nullptr,nullptr)};
        if(set.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());continue;}
        SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
        if(!SetupDiOpenDeviceInfoW(set.value,id.c_str(),nullptr,0,&device)) {RecordError(result,GetLastError());continue;}
        if(!remove && (NativeXbox(device.DevInst) || !ControllerCollection(set.value,device,profiles)))continue;
        const auto original=MultiProperty(set.value,device,SPDRP_UPPERFILTERS);
        std::vector<wchar_t> updated;bool found=false;
        for(const auto* name=original.data();*name;name+=wcslen(name)+1) {
            if(!_wcsicmp(name,L"Interceptor")) {found=true;if(remove)continue;}
            updated.insert(updated.end(),name,name+wcslen(name)+1);
        }
        if(found==!remove) {
            if(!remove) {
                ++result.already_associated;CheckStarted(device,result);
                if(!ActiveControllerFilter(id))result.reboot_required=true;
            }
            continue;
        }
        if(!remove) {const wchar_t name[]=L"Interceptor";updated.insert(updated.end(),std::begin(name),std::end(name));}
        updated.push_back(0);if(updated.size()==1)updated.push_back(0);
        WriteFilters(set.value,device,updated);
        ++result.associated;
        // Defer stack reconstruction to an explicit reboot. This avoids restarting
        // another application's controller while it has pending requests open.
        // An absent instance needs no immediate restart when removing its filter.
        ULONG status=0,problem=0;
        if(CM_Get_DevNode_Status(&status,&problem,device.DevInst,0)==CR_SUCCESS && (status&DN_STARTED))result.reboot_required=true;
        else if(!remove)result.reboot_required=true;
    } catch(const std::system_error& e) {RecordError(result,static_cast<DWORD>(e.code().value()));}
}
