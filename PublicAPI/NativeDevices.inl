// Included by InitDevices.cpp after HidDevices.inl. Per-instance associations
// only; never install a class-wide filter or replace a native function service.
std::vector<std::wstring> FilterNames(const std::vector<wchar_t>& values) {
    std::vector<std::wstring> out;
    for(auto* p=values.data();*p;p+=wcslen(p)+1)out.emplace_back(p);
    return out;
}
bool NativeClassFilterMissing(const GUID& deviceClass,DWORD property) {
    const HKEY key=SetupDiOpenClassRegKeyExW(&deviceClass,KEY_QUERY_VALUE,DIOCR_INSTALLER,nullptr,nullptr);
    if(key==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Open native setup-class key");
    DWORD bytes=0;const auto status=RegQueryValueExW(key,property==SPCRP_UPPERFILTERS?L"UpperFilters":L"LowerFilters",
        nullptr,nullptr,nullptr,&bytes);RegCloseKey(key);
    if(status==ERROR_FILE_NOT_FOUND)return true;
    if(status!=ERROR_SUCCESS)throw std::system_error(status,std::system_category(),"Check missing native class filters");
    return false;
}
std::vector<std::wstring> NativeClassFilters(const GUID& deviceClass,DWORD property,
    decltype(&SetupDiGetClassRegistryPropertyW) query=SetupDiGetClassRegistryPropertyW,
    bool (*missing)(const GUID&,DWORD)=NativeClassFilterMissing) {
    // Query the setup-class property through SetupAPI, never derive a registry
    // path from device text. Missing filters are distinct from an unreadable or
    // malformed value; only the former means no class association.
    if(property!=SPCRP_UPPERFILTERS && property!=SPCRP_LOWERFILTERS)
        throw std::system_error(ERROR_INVALID_PARAMETER,std::system_category(),"Invalid native class filter property");
    DWORD bytes=0,type=0;
    const BOOL queried=query(&deviceClass,property,&type,nullptr,0,&bytes,nullptr,nullptr);
    const DWORD error=queried?ERROR_SUCCESS:GetLastError();
    if(!queried && (error==ERROR_INVALID_DATA || error==ERROR_FILE_NOT_FOUND)) {
        // ERROR_INVALID_DATA is also possible for invalid registry data. Verify
        // absence instead of interpreting a corrupt existing value as empty.
        if(missing(deviceClass,property))return {};
        throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Unreadable existing native class filters");
    }
    if((!queried && error!=ERROR_INSUFFICIENT_BUFFER) || bytes<2*sizeof(wchar_t) ||
       bytes>65536 || bytes%sizeof(wchar_t))
        throw std::system_error(static_cast<int>(!queried && error!=ERROR_INSUFFICIENT_BUFFER?error:ERROR_INVALID_DATA),
            std::system_category(),"Read native class filters size");
    std::vector<wchar_t> data(bytes/sizeof(wchar_t));const DWORD capacity=bytes;
    if(!query(&deviceClass,property,&type,reinterpret_cast<BYTE*>(data.data()),capacity,&bytes,nullptr,nullptr))
        throw std::system_error(GetLastError(),std::system_category(),"Read native class filters");
    if(type!=REG_MULTI_SZ || bytes<2*sizeof(wchar_t) || bytes>capacity || bytes%sizeof(wchar_t))
        throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Invalid native class filter type/size");
    const auto length=bytes/sizeof(wchar_t);
    if(data[length-1] || data[length-2])
        throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Unterminated native class filters");
    std::vector<std::wstring> result;std::size_t at=0;
    while(at<length && data[at]) {
        const auto end=std::find(data.begin()+at,data.begin()+length,L'\0');
        result.emplace_back(data.begin()+at,end);at=static_cast<std::size_t>(end-data.begin())+1;
    }
    if(std::any_of(data.begin()+at,data.begin()+length,[](wchar_t c){return c!=0;}))
        throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Trailing native class filter data");
    return result;
}
std::uint32_t NativePropertyFlags(HDEVINFO set,SP_DEVINFO_DATA& device) {
    // This is xinputhid's per-devnode SOFTWARE key. Never set/guess these flags:
    // GIP's translated child normally has 14, while our generic profile needs 6.
    HKEY key=SetupDiOpenDevRegKey(set,&device,DICS_FLAG_GLOBAL,0,DIREG_DRV,KEY_QUERY_VALUE);
    if(key==INVALID_HANDLE_VALUE)return UINT32_MAX;
    DWORD value=0,type=0,bytes=sizeof(value);
    const auto status=RegQueryValueExW(key,L"DevicePropertyFlags",nullptr,&type,reinterpret_cast<BYTE*>(&value),&bytes);
    RegCloseKey(key);
    return status==ERROR_SUCCESS && type==REG_DWORD && bytes==sizeof(value)?value:UINT32_MAX;
}
bool NativeControllerParent(DEVINST node) {
    DEVINST parent=0;const auto result=CM_Get_Parent(&parent,node,0);
    if(result==CR_NO_SUCH_DEVNODE)return false;
    return result!=CR_SUCCESS || NativeXbox(parent);
}
void WriteNativeFilters(HDEVINFO set,SP_DEVINFO_DATA& device,DWORD property,const std::vector<std::wstring>& values) {
    std::vector<wchar_t> bytes;
    for(const auto& value:values)bytes.insert(bytes.end(),value.c_str(),value.c_str()+value.size()+1);
    bytes.push_back(0);if(bytes.size()==1)bytes.push_back(0);
    if(!SetupDiSetDeviceRegistryPropertyW(set,&device,property,values.empty()?nullptr:reinterpret_cast<const BYTE*>(bytes.data()),
        values.empty()?0:static_cast<DWORD>(bytes.size()*sizeof(wchar_t))))
        throw std::system_error(GetLastError(),std::system_category(),"Write per-device native controller filters");
}
void AssociateNativeControllers(Init_Result& result,const IC_CAPABILITIES* capabilities,bool remove) {
    const GUID classes[]={GUID_DEVCLASS_HIDCLASS,
        {0xd61ca365,0x5af4,0x4486,{0x99,0x8b,0x9d,0xb4,0x73,0x4c,0x6c,0xa3}},
        {0x05f5cfe2,0x4733,0x4950,{0xa6,0xbb,0x07,0xaa,0xd0,0x1a,0x3a,0x84}}};
    std::vector<std::wstring> targets;
    for(const auto& cls:classes) {
        DeviceSet set{SetupDiGetClassDevsW(&cls,nullptr,nullptr,remove?0:DIGCF_PRESENT)};
        if(set.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());continue;}
        for(DWORD index=0;;++index) {
            SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
            if(!SetupDiEnumDeviceInfo(set.value,index,&device)) {if(GetLastError()!=ERROR_NO_MORE_ITEMS)RecordError(result,GetLastError());break;}
            wchar_t id[MAX_DEVICE_ID_LEN]{};
            if(!SetupDiGetDeviceInstanceIdW(set.value,&device,id,MAX_DEVICE_ID_LEN,nullptr)) {RecordError(result,GetLastError());continue;}
            targets.emplace_back(id);
        }
    }
    for(const auto& id:targets)try {
        DeviceSet set{SetupDiCreateDeviceInfoList(nullptr,nullptr)};
        if(set.value==INVALID_HANDLE_VALUE) {RecordError(result,GetLastError());continue;}
        SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
        if(!SetupDiOpenDeviceInfoW(set.value,id.c_str(),nullptr,0,&device)) {RecordError(result,GetLastError());continue;}
        const auto upper=FilterNames(MultiProperty(set.value,device,SPDRP_UPPERFILTERS));
        const auto lower=FilterNames(MultiProperty(set.value,device,SPDRP_LOWERFILTERS));
        if(remove) {
            // Includes absent instances and legacy placement. Remove only our
            // exact service name, preserving every other name and its order.
            bool changed=false;
            for(const auto property:{SPDRP_UPPERFILTERS,SPDRP_LOWERFILTERS}) {
                const auto& original=property==SPDRP_UPPERFILTERS?upper:lower;
                const auto updated=Policy::Filters(original,0,true);
                if(updated!=original){WriteNativeFilters(set.value,device,property,updated);changed=true;}
            }
            if(changed) {
                ++result.associated;ULONG state=0,problem=0;
                if(CM_Get_DevNode_Status(&state,&problem,device.DevInst,0)==CR_SUCCESS && (state&DN_STARTED))result.reboot_required=true;
            }
            continue;
        }
        Policy::NativeNode node;node.instance=id;node.upper=upper;
        node.deviceClass=Property(set.value,device,SPDRP_CLASSGUID);
        try {node.service=Property(set.value,device,SPDRP_SERVICE);}
        catch(const std::system_error& e) {if(e.code().value()!=ERROR_INVALID_DATA && e.code().value()!=ERROR_FILE_NOT_FOUND)throw;}
        node.hardware=FilterNames(MultiProperty(set.value,device,SPDRP_HARDWAREID));
        node.compatible=FilterNames(MultiProperty(set.value,device,SPDRP_COMPATIBLEIDS));
        if(Policy::Has(upper,L"xinputhid")) {node.propertyFlags=NativePropertyFlags(set.value,device);node.blockedAncestry=NativeControllerParent(device.DevInst);}
        const auto transport=Policy::Transport(node);if(!transport) {if(Policy::Has(upper,L"xinputhid"))++result.skipped;continue;}
        if(!capabilities || !(Policy::SupportedControllers(*capabilities)&transport)) {++result.skipped;continue;}
        if(transport==IC_CONTROLLER_XUSB && !Policy::XusbProfileAvailable(node,*capabilities)){++result.skipped;continue;}
        const auto classUpper=NativeClassFilters(device.ClassGuid,SPCRP_UPPERFILTERS);
        const auto classLower=NativeClassFilters(device.ClassGuid,SPCRP_LOWERFILTERS);
        const auto placement=Policy::ClassFilterAssociation(transport,upper,lower,classUpper,classLower);
        if(placement==Policy::ClassAssociation::Conflict) {RecordError(result,ERROR_INVALID_DATA);continue;}
        if(placement==Policy::ClassAssociation::ExistingXusbUpper) {
            ++result.already_associated;CheckStarted(device,result);
            const GUID xusbGuid=IC_XUSB_INTERFACE_GUID;
            if(!ActiveControllerFilter(id,xusbGuid))result.reboot_required=true;
            continue;
        }
        const bool gip=transport==IC_CONTROLLER_GIP;
        if(Policy::Has(gip?upper:lower,L"Interceptor")) {
            // Do not introduce two Interceptor instances on one stack. A legacy
            // wrong-side association needs explicit uninstall/reinstall first.
            RecordError(result,ERROR_INVALID_DATA);continue;
        }
        const auto property=gip?SPDRP_LOWERFILTERS:SPDRP_UPPERFILTERS;
        const auto& original=gip?lower:upper;
        const auto updated=Policy::Filters(original,transport,false);
        if(original==updated) {
            ++result.already_associated;CheckStarted(device,result);
            const GUID gipGuid=IC_GIP_INTERFACE_GUID,xusbGuid=IC_XUSB_INTERFACE_GUID,xihGuid=IC_XINPUTHID_INTERFACE_GUID;
            const auto& guid=gip?gipGuid:transport==IC_CONTROLLER_XUSB?xusbGuid:xihGuid;
            if(!ActiveControllerFilter(id,guid))result.reboot_required=true;
        } else {
            WriteNativeFilters(set.value,device,property,updated);++result.associated;
            // Pending I/O may belong to other applications. Only the explicit
            // restart reconstructs the stack; never restart a controller here.
            result.reboot_required=true;
        }
    } catch(const std::system_error& e) {RecordError(result,static_cast<DWORD>(e.code().value()));}
}
