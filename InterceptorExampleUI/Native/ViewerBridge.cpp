#include "ViewerDecoder.h"
#include "PublicationCadence.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace {
constexpr uint32_t Batch=128;
std::mutex lifecycleLock,snapshotLock;
std::condition_variable lifecycleChanged;
std::thread worker;
bool stopping=false,notificationPending=false;
HANDLE stopEvent=nullptr,changeEvent=nullptr;
VIEWER_SNAPSHOT snapshot{sizeof(VIEWER_SNAPSHOT),VIEWER_VERSION,ViewerStopped};
std::vector<VIEWER_DEVICE> published;

void NotifyLocked() noexcept {
    ++snapshot.revision;
    if(!notificationPending){notificationPending=true;if(changeEvent)SetEvent(changeEvent);}
}
void Status(uint32_t status,DWORD error=ERROR_SUCCESS,uint32_t resetReason=0) {
    std::lock_guard<std::mutex> lock(snapshotLock);
    snapshot.status=status;snapshot.win32Error=error;snapshot.resetReason=resetReason;NotifyLocked();
}
bool StopRequested() noexcept {return WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0;}
DWORD ExceptionCode() noexcept {
    try{throw;}catch(const std::system_error& e){return static_cast<DWORD>(e.code().value());}
    catch(const std::bad_alloc&){return ERROR_NOT_ENOUGH_MEMORY;}
    catch(const std::invalid_argument&){return ERROR_INVALID_PARAMETER;}
    catch(...){return ERROR_GEN_FAILURE;}
}
struct Device {
    VIEWER_DEVICE state{};
    std::unique_ptr<Viewer::HidDecoder> decoder;
};
struct Session {
    Interceptor::Context_Result capture;
    std::vector<Device> devices;
    std::array<std::array<int,VIEWER_MAX_DEVICES+1>,7> indices{};
    uint64_t received=0,forwarded=0;
    Viewer::PublicationCadence publication;
    uint8_t visibleInputs=0,visibleControllers=0;
    bool Visible(uint32_t type) const noexcept {return type>=1 && type<=6 && (type<=2?(visibleInputs&(1u<<(type-1))):(visibleControllers&(1u<<(type-3))));}
    Session(){for(auto& row:indices)row.fill(-1);}
    void Initialize() {
        capture.devices=Interceptor::Get_Devices(*capture);
        if(capture.devices.size()>VIEWER_MAX_DEVICES)throw std::system_error(ERROR_INVALID_DATA,std::system_category());
        devices.reserve(capture.devices.size());
        for(const auto& info:capture.devices){
            if(!Visible(info.type))continue;
            if(info.type<1 || info.type>6 || info.id<1 || info.id>VIEWER_MAX_DEVICES)
                throw std::system_error(ERROR_INVALID_DATA,std::system_category());
            if(indices[info.type][info.id]>=0){devices[static_cast<size_t>(indices[info.type][info.id])].state.info=info;continue;}
            Device d;d.state.info=info;d.state.hat=-1;
            // Decoder failure does not prevent transparent forwarding. It is
            // visible separately per device, without guessing report offsets.
            if(info.type==IC_TYPE_CONTROLLER_HID || info.type==IC_TYPE_CONTROLLER_XINPUTHID){
                try{
                    auto descriptor=info.type==IC_TYPE_CONTROLLER_HID?
                        Interceptor::Get_Controller_Hid_Descriptor(*capture,info.id):
                        Interceptor::Get_Controller_XinputHid_Descriptor(*capture,info.id);
                    d.decoder=std::make_unique<Viewer::HidDecoder>(std::move(descriptor));
                }catch(...){d.state.stateFlags|=ViewerDecodeUnsupported;d.state.decodeError=ExceptionCode();}
            }
            indices[info.type][info.id]=static_cast<int>(devices.size());devices.push_back(std::move(d));
        }
        std::lock_guard<std::mutex> lock(snapshotLock);
        published.resize(devices.size());snapshot.deviceCount=static_cast<uint32_t>(devices.size());snapshot.generation=capture.generation;
        for(size_t n=0;n<devices.size();++n)published[n]=devices[n].state;
        snapshot.status=devices.empty()?ViewerPreparing:ViewerRunning;NotifyLocked();
        publication.Committed(GetTickCount64());
    }
    Device* Find(uint32_t type,uint32_t id) noexcept {
        if(type>6 || id>VIEWER_MAX_DEVICES)return nullptr;
        const auto index=indices[type][id];return index<0?nullptr:&devices[static_cast<size_t>(index)];
    }
    void Publish() {
        std::lock_guard<std::mutex> lock(snapshotLock);
        snapshot.received=received;snapshot.forwarded=forwarded;
        for(size_t n=0;n<devices.size();++n)published[n]=devices[n].state;
        NotifyLocked();
        publication.Committed(GetTickCount64());
    }
    void PublishIfDue() {if(publication.Due(GetTickCount64()))Publish();}
    template<class T,class Getter,class Sender,class Decoder>
    void Transfer(uint32_t type,std::vector<T>& buffer,Getter get,Sender send,Decoder decode){
        const auto count=get(buffer.data(),Batch);if(!count)return;
        if(Visible(type))received+=count;
        // The original batch is sent intact FIRST. No decoding, notification,
        // UI locking, formatting, or logging precedes this synchronous send.
        const auto result=send(buffer.data(),count);if(Visible(type))forwarded+=result.accepted;
        if(result.error || result.accepted!=count){
            Publish();
            // Busy/partial/error closes capture instead of spinning or keeping
            // the physical devices captured with an unsent suffix.
            throw std::system_error(static_cast<int>(result.error?result.error:ERROR_WRITE_FAULT),std::system_category());
        }
        if(!Visible(type))return;
        for(uint32_t n=0;n<count;++n){const auto& input=Header(buffer[n]);auto* d=Find(type,input.device);
            if(!d){Initialize();d=Find(type,input.device);}
            if(!d)throw std::system_error(ERROR_INVALID_DATA,std::system_category());
            decode(buffer[n],*d);Viewer::Metadata(input.sequence,input.timestamp,d->state);
        }
        publication.Changed();
    }
    template<class T> static const T& Header(const T& input){return input;}
    static const IC_CONTROLLER_GIP_INPUT& Header(const IC_CONTROLLER_GIP_RAW_INPUT& input){return input.input;}
};
void Run(uint8_t mask,uint8_t controllers) noexcept {
    Session s;
    try{
        const auto init=Interceptor::Init_AllDevices();
        {std::lock_guard<std::mutex> lock(snapshotLock);snapshot.initAssociated=init.associated;snapshot.initExisting=init.already_associated;}
        if(StopRequested())return;
        if(init.reboot_required){Status(ViewerRebootRequired,init.error?init.error:ERROR_SUCCESS_REBOOT_REQUIRED);return;}
        if(init.error || !init.ready){Status(ViewerError,init.error?init.error:ERROR_NOT_READY);return;}
        // Create_Context checks immutable capabilities on the loaded driver.
        // SDK build switches cannot prove native support in the active SYS.
        Status(ViewerPreparing);
        s.visibleInputs=mask;s.visibleControllers=controllers;
        s.capture=Interceptor::Create_Context();s.Initialize();
        std::vector<Interceptor::Keyboard_Input> keyboard(Batch);
        std::vector<Interceptor::Mouse_Input> mouse(Batch);
        std::vector<Interceptor::Controller_Hid_Input> hid(Batch);
        std::vector<Interceptor::Controller_Xinput_Input> xusb(Batch);
        std::vector<Interceptor::Controller_XinputHid_Input> xih(Batch);
        std::vector<Interceptor::Controller_GIP_Input> gip(Batch);
        const bool rawGip= (Interceptor::Get_Capabilities().gip_profiles&IC_PROFILE_GIP_USB_GAMEPAD_RAW);
        std::vector<Interceptor::Controller_GIP_Raw_Input> gipRaw(rawGip?Batch:0);
        while(!StopRequested()){
            const auto ready=Interceptor::Wait_For_Input(*s.capture,s.publication.Wait(GetTickCount64()),stopEvent);
            if(StopRequested())break;
            if(!ready){s.PublishIfDue();continue;}
#define TRANSFER(bit,type,buffer,Name,Decode) \
            if(ready&(bit))s.Transfer(type,buffer, \
                [&](auto* p,uint32_t count){return Interceptor::Get_##Name##_Input(*s.capture,p,Batch,count);}, \
                [&](const auto* p,size_t count){return Interceptor::Send_Input(*s.capture,p,count);},Decode)
            TRANSFER(IC_KEYBOARD,IC_TYPE_KEYBOARD,keyboard,Keyboard,[](const auto& in,Device& d){Viewer::DecodeKeyboard(in,d.state);});
            TRANSFER(IC_MOUSE,IC_TYPE_MOUSE,mouse,Mouse,[](const auto& in,Device& d){Viewer::DecodeMouse(in,d.state);});
            TRANSFER(IC_READY_HID,IC_TYPE_CONTROLLER_HID,hid,Controller_Hid,[](const auto& in,Device& d){if(d.decoder)d.decoder->Decode(in.report,in.length,d.state);});
            TRANSFER(IC_READY_XUSB,IC_TYPE_CONTROLLER_XUSB,xusb,Controller_Xinput,[](const auto& in,Device& d){Viewer::DecodeNative(in.state,false,d.state);});
            TRANSFER(IC_READY_XINPUTHID,IC_TYPE_CONTROLLER_XINPUTHID,xih,Controller_XinputHid,[](const auto& in,Device& d){if(d.decoder)d.decoder->Decode(in.report,in.length,d.state);});
            if(rawGip){TRANSFER(IC_READY_GIP,IC_TYPE_CONTROLLER_GIP,gipRaw,Controller_GIP_Raw,[](const auto& in,Device& d){Viewer::DecodeNative(in.input.state,true,d.state);if(in.report_length>IC_GIP_INPUT_BYTES)d.state.stateFlags|=ViewerPartialDecode;});}
            else {TRANSFER(IC_READY_GIP,IC_TYPE_CONTROLLER_GIP,gip,Controller_GIP,[](const auto& in,Device& d){Viewer::DecodeNative(in.state,true,d.state);});}
#undef TRANSFER
            s.PublishIfDue();
        }
        Interceptor::Destroy_Context(s.capture);
    }catch(...){
        const DWORD error=ExceptionCode();uint32_t reason=0;
        if(s.capture){try{reason=Interceptor::Get_Status(s.capture.context.get()).last_reset_reason;}catch(...){}}
        Interceptor::Destroy_Context(s.capture);
        if(!StopRequested())Status(error==IC_ERROR_TRIAL_EXPIRED || reason==IC_RESET_TRIAL_EXPIRED?ViewerTrialExpired:
            error==ERROR_OPERATION_ABORTED?ViewerRevoked:
            error==ERROR_NOT_SUPPORTED?ViewerBackendUnavailable:
            error==ERROR_DEVICE_NOT_CONNECTED?ViewerNoDevices:
            error==ERROR_TIMEOUT?ViewerPreparationTimeout:ViewerError,error,reason);
    }
}
}

DWORD __cdecl Viewer_Start(uint8_t mask,uint8_t controllers,HANDLE event) noexcept {
    if((!mask&&!controllers)||(mask&~IC_BOTH)||(controllers&~IC_CONTROLLER_ALL)||!event||event==INVALID_HANDLE_VALUE)
        return ERROR_INVALID_PARAMETER;
    try{
        std::lock_guard<std::mutex> lifecycle(lifecycleLock);
        if(stopping || worker.joinable())return ERROR_BUSY;
        HANDLE duplicated=nullptr;
        if(!DuplicateHandle(GetCurrentProcess(),event,GetCurrentProcess(),&duplicated,EVENT_MODIFY_STATE,FALSE,0))return GetLastError();
        if(!ResetEvent(duplicated)){const auto error=GetLastError();CloseHandle(duplicated);return error;}
        const auto stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!stop){const auto error=GetLastError();CloseHandle(duplicated);return error;}
        stopEvent=stop;
        {std::lock_guard<std::mutex> lock(snapshotLock);
            changeEvent=duplicated;published.clear();snapshot={};snapshot.size=sizeof(snapshot);snapshot.version=VIEWER_VERSION;
            snapshot.status=ViewerStarting;snapshot.mask=mask;snapshot.controllers=controllers;notificationPending=false;NotifyLocked();
        }
        try{worker=std::thread(Run,mask,controllers);}
        catch(...){const auto error=ExceptionCode();Status(ViewerError,error);
            {std::lock_guard<std::mutex> lock(snapshotLock);CloseHandle(changeEvent);changeEvent=nullptr;}
            CloseHandle(stopEvent);stopEvent=nullptr;return error;
        }
        return ERROR_SUCCESS;
    }catch(...){return ExceptionCode();}
}
void __cdecl Viewer_Stop() noexcept {
    try{
        std::thread joining;
        {std::unique_lock<std::mutex> lock(lifecycleLock);lifecycleChanged.wait(lock,[]{return !stopping;});
            stopping=true;if(stopEvent)SetEvent(stopEvent);joining=std::move(worker);
        }
        // No lifecycle/snapshot lock is held while waiting for SDK cleanup.
        if(joining.joinable())joining.join();
        {std::lock_guard<std::mutex> lock(snapshotLock);
            snapshot.status=ViewerStopped;snapshot.deviceCount=0;published.clear();NotifyLocked();
            if(changeEvent){CloseHandle(changeEvent);changeEvent=nullptr;}
        }
        {std::lock_guard<std::mutex> lock(lifecycleLock);
            if(stopEvent){CloseHandle(stopEvent);stopEvent=nullptr;}stopping=false;
        }
        lifecycleChanged.notify_all();
    }catch(...){/* C ABI; no exceptions escape. Process teardown still closes context. */}
}
DWORD __cdecl Viewer_ReadSnapshot(VIEWER_SNAPSHOT* header,VIEWER_DEVICE* devices,uint32_t capacity,uint32_t* required) noexcept {
    if(!header || !required || (capacity&&!devices) || header->size!=sizeof(VIEWER_SNAPSHOT) || header->version!=VIEWER_VERSION)
        return ERROR_INVALID_PARAMETER;
    try{
        std::lock_guard<std::mutex> lock(snapshotLock);*header=snapshot;*required=static_cast<uint32_t>(published.size());
        if(capacity<published.size())return ERROR_INSUFFICIENT_BUFFER;
        if(!published.empty())memcpy(devices,published.data(),published.size()*sizeof(VIEWER_DEVICE));
        notificationPending=false;return ERROR_SUCCESS;
    }catch(...){return ExceptionCode();}
}
