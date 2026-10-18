// Read-only USB descriptor queries, only on explicit metadata calls.
#include <cfgmgr32.h>
#include <usbioctl.h>
#include <usbiodef.h>
#pragma comment(lib,"cfgmgr32.lib")
namespace Interceptor { namespace Detail {
inline constexpr GUID usbHubGuid{0xf18a0e88,0xc30c,0x11d0,{0x88,0x15,0x00,0xa0,0xc9,0x06,0xbe,0xd8}};
struct HidReportLocation {std::uint16_t interface_number=0,length=0;};
HidReportLocation FindHidReport(const std::vector<std::uint8_t>& config,int requestedInterface) {
    if(config.size()<9 || config[0]<9 || config[1]!=2 || IcNativeRead16(config.data()+2)!=config.size())
        Fail("Invalid USB configuration descriptor",ERROR_INVALID_DATA);
    int current=-1;bool hid=false;HidReportLocation found;unsigned matches=0;
    for(std::size_t cursor=0;cursor<config.size();) {
        const auto length=config[cursor];
        if(length<2 || length>config.size()-cursor)Fail("Truncated USB descriptor",ERROR_INVALID_DATA);
        const auto* entry=config.data()+cursor;
        if(entry[1]==4) {
            if(length<9)Fail("Truncated USB interface descriptor",ERROR_INVALID_DATA);
            current=entry[2];hid=entry[3]==0 && entry[5]==3 && (requestedInterface<0 || current==requestedInterface);
        } else if(entry[1]==0x21 && hid) {
            if(length<6 || 6u+3u*entry[5]>length)Fail("Truncated USB HID descriptor",ERROR_INVALID_DATA);
            for(unsigned i=0;i<entry[5];++i)if(entry[6+3*i]==0x22) {
                const auto bytes=IcNativeRead16(entry+7+3*i);
                if(!bytes)Fail("Empty HID report descriptor",ERROR_INVALID_DATA);
                found={static_cast<std::uint16_t>(current),bytes};++matches;
            }
        }
        cursor+=length;
    }
    if(matches!=1)Fail("USB HID interface absent or ambiguous",ERROR_NOT_SUPPORTED);
    return found;
}
DWORD UsbDescriptorIo(HANDLE hub,DWORD code,void* buffer,DWORD size) {
    Handle event;event.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!event.value)Fail("Create USB descriptor event");
    OVERLAPPED operation{};operation.hEvent=event.value;DWORD bytes=0;
    if(!DeviceIoControl(hub,code,buffer,size,buffer,size,&bytes,&operation)) {
        if(GetLastError()!=ERROR_IO_PENDING)Fail("Read USB descriptor");
        const auto wait=WaitForSingleObject(event.value,2000);
        if(wait!=WAIT_OBJECT_0) {
            const auto error=wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();
            CancelIoEx(hub,&operation);
            // Storage must remain alive until the cancelled request completes.
            (void)GetOverlappedResult(hub,&operation,&bytes,TRUE);
            Fail("Wait for USB descriptor",error);
        }
        if(!GetOverlappedResult(hub,&operation,&bytes,FALSE))Fail("Complete USB descriptor read");
    }
    if(bytes>size)Fail("USB response exceeds buffer",ERROR_INVALID_DATA);
    return bytes;
}
std::vector<std::uint8_t> UsbGetDescriptor(HANDLE hub,ULONG port,UCHAR type,UCHAR index,USHORT interfaceNumber,USHORT length) {
    const auto header=static_cast<DWORD>(FIELD_OFFSET(USB_DESCRIPTOR_REQUEST,Data));
    std::vector<std::uint8_t> buffer(header+length);
    auto* request=reinterpret_cast<USB_DESCRIPTOR_REQUEST*>(buffer.data());request->ConnectionIndex=port;
    request->SetupPacket.bmRequest=0x80;request->SetupPacket.bRequest=6;
    request->SetupPacket.wValue=static_cast<USHORT>((type<<8)|index);
    request->SetupPacket.wIndex=interfaceNumber;request->SetupPacket.wLength=length;
    const auto bytes=UsbDescriptorIo(hub,IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(bytes<header || bytes-header>length)Fail("Invalid USB descriptor extent",ERROR_INVALID_DATA);
    return {buffer.begin()+header,buffer.begin()+bytes};
}
int InterfaceFromInstance(const std::wstring& instance) {
    const auto hex=[](wchar_t c)->int {if(c>=L'0'&&c<=L'9')return c-L'0';if(c>=L'a'&&c<=L'f')return c-L'a'+10;if(c>=L'A'&&c<=L'F')return c-L'A'+10;return -1;};
    for(std::size_t i=0;i+6<=instance.size();++i)if(instance[i]==L'&' &&
        (instance[i+1]==L'M'||instance[i+1]==L'm')&&(instance[i+2]==L'I'||instance[i+2]==L'i')&&instance[i+3]==L'_') {
        const int a=hex(instance[i+4]),b=hex(instance[i+5]);if(a>=0&&b>=0)return a*16+b;
    }
    return -1;
}
std::vector<std::uint8_t> ReadUsbReportDescriptor(const Device& device,const Device_Details& details) {
    if(!(details.flags&IC_QUERY_IDENTITY_VALID) || !details.instance[0])Fail("HID source identity unavailable",ERROR_NOT_SUPPORTED);
    std::wstring instance;for(auto c:details.instance){if(!c)break;instance.push_back(static_cast<wchar_t>(c));}
    int interfaceNumber=InterfaceFromInstance(instance);DEVINST node=0;
    const auto located=CM_Locate_DevNodeW(&node,instance.data(),CM_LOCATE_DEVNODE_NORMAL);
    if(located!=CR_SUCCESS)Fail("Locate HID devnode",CM_MapCrToWin32Err(located,ERROR_NOT_SUPPORTED));
    struct DeviceSet {HDEVINFO value;~DeviceSet(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}};
    DeviceSet hubs{SetupDiGetClassDevsW(&usbHubGuid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
    if(hubs.value==INVALID_HANDLE_VALUE)Fail("Enumerate USB hubs");
    for(unsigned depth=0;depth<32;++depth) {
        DEVINST parent=0;if(CM_Get_Parent(&parent,node,0)!=CR_SUCCESS)break;
        for(DWORD i=0;;++i) {
            SP_DEVICE_INTERFACE_DATA entry{};entry.cbSize=sizeof(entry);
            if(!SetupDiEnumDeviceInterfaces(hubs.value,nullptr,&usbHubGuid,i,&entry)) {
                if(GetLastError()!=ERROR_NO_MORE_ITEMS)Fail("Enumerate USB hub interface");break;
            }
            SP_DEVINFO_DATA info{};info.cbSize=sizeof(info);DWORD required=0;
            SetupDiGetDeviceInterfaceDetailW(hubs.value,&entry,nullptr,0,&required,&info);
            if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER || required<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))Fail("USB hub path extent",ERROR_INVALID_DATA);
            std::vector<std::uint8_t> path(required);auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(path.data());detail->cbSize=sizeof(*detail);
            if(!SetupDiGetDeviceInterfaceDetailW(hubs.value,&entry,detail,required,nullptr,&info))Fail("USB hub path");
            if(info.DevInst!=parent)continue;
            ULONG type=0,port=0,length=sizeof(port);
            const auto address=CM_Get_DevNode_Registry_PropertyW(node,CM_DRP_ADDRESS,&type,&port,&length,0);
            if(address!=CR_SUCCESS || type!=REG_DWORD || length!=sizeof(port) || !port)Fail("USB port address unavailable",ERROR_NOT_SUPPORTED);
            Handle hub;hub.value=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
            if(hub.value==INVALID_HANDLE_VALUE)Fail("Open USB hub for descriptor query");
            std::vector<std::uint8_t> connection(sizeof(USB_NODE_CONNECTION_INFORMATION_EX)+32*sizeof(USB_PIPE_INFO));
            auto* state=reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(connection.data());state->ConnectionIndex=port;
            if(UsbDescriptorIo(hub.value,IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,connection.data(),static_cast<DWORD>(connection.size()))<sizeof(*state) ||
                state->ConnectionStatus!=DeviceConnected || !state->CurrentConfigurationValue ||
                (device.vid && device.vid!=state->DeviceDescriptor.idVendor) || (device.pid && device.pid!=state->DeviceDescriptor.idProduct))
                Fail("USB source no longer matches capture",ERROR_DEVICE_NOT_CONNECTED);
            for(unsigned configuration=0;configuration<state->DeviceDescriptor.bNumConfigurations;++configuration) {
                const auto prefix=UsbGetDescriptor(hub.value,port,2,static_cast<UCHAR>(configuration),0,9);
                if(prefix.size()!=9 || prefix[0]<9 || prefix[1]!=2)Fail("Invalid USB configuration prefix",ERROR_INVALID_DATA);
                if(prefix[5]!=state->CurrentConfigurationValue)continue;
                const auto size=IcNativeRead16(prefix.data()+2);
                if(size<9)Fail("Invalid USB configuration size",ERROR_INVALID_DATA);
                const auto config=UsbGetDescriptor(hub.value,port,2,static_cast<UCHAR>(configuration),0,size);
                const auto report=FindHidReport(config,interfaceNumber);
                // The hub API forces recipient 0x80; hardware requiring 0x81
                // may reject it. Never substitute reconstructed/preparsed data.
                auto data=UsbGetDescriptor(hub.value,port,0x22,0,report.interface_number,report.length);
                if(data.size()!=report.length)Fail("Incomplete hardware HID report descriptor",ERROR_INVALID_DATA);
                return data;
            }
            Fail("Active USB configuration absent",ERROR_NOT_SUPPORTED);
        }
        node=parent;wchar_t name[MAX_DEVICE_ID_LEN]{};
        if(interfaceNumber<0 && CM_Get_Device_IDW(node,name,MAX_DEVICE_ID_LEN,0)==CR_SUCCESS)interfaceNumber=InterfaceFromInstance(name);
    }
    Fail("HID raw descriptor unavailable on this transport",ERROR_NOT_SUPPORTED);
}
using RawReportReader=std::vector<std::uint8_t>(*)(const Device&,const Device_Details&);
RawReportReader rawReportReader=ReadUsbReportDescriptor;
}}
