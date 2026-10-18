// Per-instance binding of the private XUSB HID mirror. Parent identities come
// from Configuration Manager, never from a same-VID/PID search or player index.
Policy::NativeNode ReadNativeNode(HDEVINFO set,SP_DEVINFO_DATA& device,const std::wstring& id) {
    Policy::NativeNode n;n.instance=id;n.deviceClass=Property(set,device,SPDRP_CLASSGUID);
    try {n.service=Property(set,device,SPDRP_SERVICE);}
    catch(const std::system_error& e) {if(e.code().value()!=ERROR_INVALID_DATA && e.code().value()!=ERROR_FILE_NOT_FOUND)throw;}
    n.upper=FilterNames(MultiProperty(set,device,SPDRP_UPPERFILTERS));
    n.hardware=FilterNames(MultiProperty(set,device,SPDRP_HARDWAREID));
    n.compatible=FilterNames(MultiProperty(set,device,SPDRP_COMPATIBLEIDS));return n;
}
bool MirrorParents(HDEVINFO set,SP_DEVINFO_DATA& child,const std::wstring& id,Policy::NativeNode& hid,Policy::NativeNode& xusb) {
    DEVINST parent=0,grandparent=0;wchar_t parentId[MAX_DEVICE_ID_LEN]{},grandparentId[MAX_DEVICE_ID_LEN]{};
    if(CM_Get_Parent(&parent,child.DevInst,0)!=CR_SUCCESS || CM_Get_Parent(&grandparent,parent,0)!=CR_SUCCESS ||
       CM_Get_Device_IDW(parent,parentId,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS || CM_Get_Device_IDW(grandparent,grandparentId,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS)return false;
    SP_DEVINFO_DATA p{};p.cbSize=sizeof(p);SP_DEVINFO_DATA g{};g.cbSize=sizeof(g);
    if(!SetupDiOpenDeviceInfoW(set,parentId,nullptr,0,&p) || !SetupDiOpenDeviceInfoW(set,grandparentId,nullptr,0,&g))return false;
    hid=ReadNativeNode(set,p,parentId);xusb=ReadNativeNode(set,g,grandparentId);
    return Policy::XusbMirror(ReadNativeNode(set,child,id),hid,xusb);
}
struct MirrorRegistry {
    HKEY value;
    ~MirrorRegistry(){if(value!=INVALID_HANDLE_VALUE)RegCloseKey(value);}
};
bool SetMirrorIdentity(HKEY key,const wchar_t* name,const std::wstring& value) {
    if(value.empty() || value.size()>=MAX_DEVICE_ID_LEN)throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Invalid mirror identity");
    wchar_t old[MAX_DEVICE_ID_LEN]{};DWORD type=0,bytes=sizeof(old);
    const auto query=RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(old),&bytes);
    if(query==ERROR_SUCCESS && type==REG_SZ && bytes==(value.size()+1)*sizeof(wchar_t) && bytes<=sizeof(old) &&
       old[bytes/sizeof(wchar_t)-1]==0 && !_wcsicmp(old,value.c_str()))return false;
    if(query!=ERROR_SUCCESS && query!=ERROR_FILE_NOT_FOUND && query!=ERROR_MORE_DATA)
        throw std::system_error(query,std::system_category(),"Read XUSB mirror identity");
    const auto status=RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
    if(status!=ERROR_SUCCESS)throw std::system_error(status,std::system_category(),"Write XUSB mirror identity");return true;
}
void RemoveMirrorIdentity(HDEVINFO set,SP_DEVINFO_DATA& device) {
    MirrorRegistry key{SetupDiOpenDevRegKey(set,&device,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_SET_VALUE)};
    if(key.value==INVALID_HANDLE_VALUE) {
        const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND || error==ERROR_INVALID_DATA)return;
        throw std::system_error(error,std::system_category(),"Open XUSB mirror hardware key");
    }
    for(const auto* name:{IC_XUSB_MIRROR_HID_PARENT_VALUE,IC_XUSB_MIRROR_PARENT_VALUE}) {
        const auto status=RegDeleteValueW(key.value,name);
        if(status!=ERROR_SUCCESS && status!=ERROR_FILE_NOT_FOUND)throw std::system_error(status,std::system_category(),"Remove XUSB mirror identity");
    }
}
void AssociateXusbMirrors(Init_Result& result,const IC_CAPABILITIES* capabilities,bool remove) {
    struct Candidate {std::wstring id,hid,xusb;};std::vector<Candidate> candidates;
    DeviceSet snapshot{SetupDiGetClassDevsW(&GUID_DEVCLASS_HIDCLASS,nullptr,nullptr,remove?0:DIGCF_PRESENT)};
    if(snapshot.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());return;}
    std::vector<std::wstring> ids;
    for(DWORD index=0;;++index) {
        SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);
        if(!SetupDiEnumDeviceInfo(snapshot.value,index,&d)) {if(GetLastError()!=ERROR_NO_MORE_ITEMS)RecordError(result,GetLastError());break;}
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if(!SetupDiGetDeviceInstanceIdW(snapshot.value,&d,id,MAX_DEVICE_ID_LEN,nullptr)) {RecordError(result,GetLastError());continue;}
        // Removal includes absent instances but only the precise device family
        // to which this code can have written identity values.
        if(Policy::XusbMirrorGroup(id)>=0)ids.emplace_back(id);
    }
    for(const auto& id:ids)try {
        DeviceSet set{SetupDiCreateDeviceInfoList(nullptr,nullptr)};SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);
        if(set.value==INVALID_HANDLE_VALUE || !SetupDiOpenDeviceInfoW(set.value,id.c_str(),nullptr,0,&d)) {RecordError(result,GetLastError());continue;}
        if(remove){RemoveMirrorIdentity(set.value,d);continue;}
        if(!capabilities || !(Policy::SupportedControllers(*capabilities)&IC_CONTROLLER_XUSB)){++result.skipped;continue;}
        Policy::NativeNode hid,xusb;
        if(!MirrorParents(set.value,d,id,hid,xusb)){++result.skipped;continue;}
        if(!Policy::XusbProfileAvailable(xusb,*capabilities)){++result.skipped;continue;}
        candidates.push_back({id,hid.instance,xusb.instance});
    } catch(const std::system_error& e){RecordError(result,static_cast<DWORD>(e.code().value()));}
    // Refuse every candidate when a parent has more than one mirror, instead of
    // silently selecting whichever instance SetupAPI enumerated first.
    for(const auto& candidate:candidates)try {
        const auto count=std::count_if(candidates.begin(),candidates.end(),[&](const auto& c){return !_wcsicmp(c.xusb.c_str(),candidate.xusb.c_str());});
        if(count!=1){++result.skipped;RecordError(result,ERROR_NOT_SUPPORTED);continue;}
        DeviceSet set{SetupDiCreateDeviceInfoList(nullptr,nullptr)};SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);
        if(set.value==INVALID_HANDLE_VALUE || !SetupDiOpenDeviceInfoW(set.value,candidate.id.c_str(),nullptr,0,&d)) {RecordError(result,GetLastError());continue;}
        Policy::NativeNode hid,xusb;
        if(!MirrorParents(set.value,d,candidate.id,hid,xusb) || _wcsicmp(hid.instance.c_str(),candidate.hid.c_str()) || _wcsicmp(xusb.instance.c_str(),candidate.xusb.c_str())) {RecordError(result,ERROR_DEVICE_NOT_CONNECTED);continue;}
        if(Policy::Has(FilterNames(MultiProperty(set.value,d,SPDRP_LOWERFILTERS)),L"Interceptor")){RecordError(result,ERROR_INVALID_DATA);continue;}
        MirrorRegistry key{SetupDiOpenDevRegKey(set.value,&d,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_QUERY_VALUE|KEY_SET_VALUE)};
        if(key.value==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Open XUSB mirror identity key");
        bool changed=SetMirrorIdentity(key.value,IC_XUSB_MIRROR_HID_PARENT_VALUE,candidate.hid);
        changed=SetMirrorIdentity(key.value,IC_XUSB_MIRROR_PARENT_VALUE,candidate.xusb)||changed;
        const auto original=FilterNames(MultiProperty(set.value,d,SPDRP_UPPERFILTERS));
        const auto updated=Policy::Filters(original,IC_CONTROLLER_XUSB,false);
        if(updated!=original){WriteNativeFilters(set.value,d,SPDRP_UPPERFILTERS,updated);changed=true;}
        if(changed){++result.associated;result.reboot_required=true;}
        else {
            ++result.already_associated;CheckStarted(d,result);
            const GUID guid=IC_XUSB_MIRROR_INTERFACE_GUID;if(!ActiveControllerFilter(candidate.id,guid))result.reboot_required=true;
        }
    } catch(const std::system_error& e){RecordError(result,static_cast<DWORD>(e.code().value()));}
}
