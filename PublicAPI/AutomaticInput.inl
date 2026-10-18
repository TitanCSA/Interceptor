// Included once by Interceptor.cpp, after the explicit-context implementation.
#include <array>
#include <bitset>
#include <exception>
#include <cstring>
#include <objbase.h>
#pragma comment(lib,"ole32.lib")

namespace Interceptor {
namespace Detail {
struct Captured_Device : Device {
    std::uint32_t source=0;
};
Device PublicDevice(const Device_Info& info) {
    Device device;
    device.id=info.id;device.type=info.type;device.flags=info.flags;
    device.vid=info.vid;device.pid=info.pid;device.revision=info.revision;
    device.usage_page=info.usage_page;device.usage=info.usage;
    device.input_bytes=info.input_bytes;device.output_bytes=info.output_bytes;device.feature_bytes=info.feature_bytes;
    std::copy(std::begin(info.name),std::end(info.name),std::begin(device.name));
    std::copy(std::begin(info.hardware_id),std::end(info.hardware_id),std::begin(device.hardware_id));
    return device;
}
struct Automatic_State {
    struct Codec_Cache {std::string token;std::shared_ptr<const Hid_Codec> codec;DWORD error=ERROR_SUCCESS;};
    std::recursive_mutex mutex;
    Context_Result capture;
    Input_Processing_Storage storage;
    std::vector<Input_Event> events;
    std::vector<Captured_Device> devices;
    std::string token;
    std::array<std::array<int,IC_MAX_DEVICES+1>,IC_STREAM_COUNT> deviceIndex{};
    std::array<std::array<Codec_Cache,IC_MAX_DEVICES+1>,2> codecs;
    std::uint64_t batch=0;
    bool activeBatch=false;
    DWORD fault=ERROR_SUCCESS;

    void IndexDevices() noexcept {
        for(auto& family:deviceIndex)family.fill(-1);
        for(std::size_t i=0;i<devices.size();++i) {
            const auto& d=devices[i];
            if(d.type>=1 && d.type<=IC_STREAM_COUNT && d.id<=IC_MAX_DEVICES)
                deviceIndex[d.type-1][d.id]=static_cast<int>(i);
        }
    }
    const Captured_Device* Find(Device_Type type,std::uint32_t id) const noexcept {
        const auto family=static_cast<std::uint32_t>(type);
        if(id<=IC_MAX_DEVICES) {
            const auto index=deviceIndex[family-1][id];
            return index<0?nullptr:&devices[static_cast<std::size_t>(index)];
        }
        for(const auto& d:devices)if(d.type==family && d.id==id)return &d;
        return nullptr;
    }
    Context& Ensure() {
        if(fault)Fail("Automatic capture stopped; release it with Destroy_Context before restarting",fault);
        if(!capture) {
            auto created=Create_Context();
            GUID guid{};
            if(FAILED(CoCreateGuid(&guid)))Fail("Create capture identity",ERROR_GEN_FAILURE);
            static constexpr char hex[]="0123456789abcdef";
            std::string nextToken="ic:capture:";nextToken.reserve(nextToken.size()+32);
            std::array<std::uint8_t,sizeof(GUID)> guidBytes{};std::memcpy(guidBytes.data(),&guid,sizeof(guid));
            for(auto byte:guidBytes) {
                nextToken.push_back(hex[byte>>4]);nextToken.push_back(hex[byte&15]);
            }
            auto nextDevices=Identify(created.devices,nextToken);
            token=std::move(nextToken);devices=std::move(nextDevices);
            for(auto& family:codecs)for(auto& cached:family)cached=Codec_Cache{};
            IndexDevices();
            capture=std::move(created);
        }
        return capture.Get();
    }
    std::vector<Captured_Device> Identify(const std::vector<Device_Info>& source,const std::string& session) const {
        std::vector<Captured_Device> result;result.reserve(source.size());
        std::bitset<IC_STREAM_COUNT*(IC_MAX_DEVICES+1)> seen;
        for(const auto& info:source) {
            if(!info.id || info.id>IC_MAX_DEVICES || !info.instance_token || info.type<1 || info.type>IC_STREAM_COUNT)
                Fail("Invalid captured device identity",ERROR_INVALID_DATA);
            const auto slot=(info.type-1)*(IC_MAX_DEVICES+1)+info.id;
            if(seen.test(slot))Fail("Duplicate captured device identity",ERROR_INVALID_DATA);
            seen.set(slot);
            const auto* known=capture?Find(static_cast<Device_Type>(info.type),info.id):nullptr;
            Captured_Device device;static_cast<Device&>(device)=PublicDevice(info);device.source=info.instance_token;
            device.token=known && known->source==info.instance_token?known->token:
                session+":"+std::to_string(info.type)+":"+std::to_string(info.instance_token)+":"+std::to_string(info.id);
            // The token contains family, source identity and local ID. Unique
            // (family, ID) pairs therefore cannot produce duplicate tokens.
            result.push_back(std::move(device));
        }
        return result;
    }
    std::vector<Captured_Device> SnapshotDevices(Context& context) {
        auto snapshot=Identify(Get_Devices(context),token);
        if(!activeBatch){devices=snapshot;IndexDevices();}
        return snapshot;
    }
    void ReleaseResources() noexcept {
        storage=Input_Processing_Storage{};
        std::vector<Input_Event>{}.swap(events);
        std::vector<Captured_Device>{}.swap(devices);
        std::string{}.swap(token);
        for(auto& family:codecs)for(auto& cached:family)cached=Codec_Cache{};
        IndexDevices();
    }
    // A failure must not silently reacquire interception on the next call.
    void Abort(DWORD error) noexcept {
        fault=error?error:ERROR_GEN_FAILURE;
        Destroy_Context(capture);
        if(!activeBatch)ReleaseResources();
    }
    void AbortException() noexcept {
        try {throw;}
        catch(const std::system_error& e){Abort(static_cast<DWORD>(e.code().value()));}
        catch(...){Abort(ERROR_GEN_FAILURE);}
    }
    void RequireReader() const {
        if(activeBatch)Fail("Finish the previous Input_Batch before receiving again",ERROR_BUSY);
    }
    template<class T> void DecodeInputs(Context& context,std::vector<T>& inputs) {
        if constexpr(DecodedHid<T>) {
            constexpr auto type=std::is_same_v<T,Controller_Hid_Input>?Device_Type::Controller_Hid:Device_Type::Controller_XinputHid;
            bool refreshed=false;
            for(auto& input:inputs) {
                const auto* device=Find(type,input.device);
                if(!device&&!refreshed){(void)SnapshotDevices(context);refreshed=true;device=Find(type,input.device);}
                if(!device||!input.device||input.device>IC_MAX_DEVICES){input.decode_error=ERROR_NOT_FOUND;continue;}
                input.token=device->token;auto& cache=codecs[std::is_same_v<T,Controller_Hid_Input>?0:1][input.device];
                if(cache.token!=device->token) {
                    cache=Codec_Cache{};cache.token=device->token;
                    try {
                        auto descriptor=[&]{if constexpr(std::is_same_v<T,Controller_Hid_Input>)return Get_Controller_Hid_Descriptor(context,input.device);else return Get_Controller_XinputHid_Descriptor(context,input.device);}();
                        cache.codec=Hid_Codec::Create(std::move(descriptor));
                    }catch(const std::system_error& error){cache.error=static_cast<DWORD>(error.code().value());}
                }
                if(cache.error){input.decode_error=cache.error;continue;}
                Hid_Codec::Decode(cache.codec,input,input.report,input.length,input.device);
            }
        }
    }
    template<class T> bool ValidTokens(Detail::Input_View<const T> inputs) const {
        if constexpr(DecodedHid<T>)for(const auto& input:inputs)if(!input.token.empty()) {
            const auto* device=Find(std::is_same_v<T,Controller_Hid_Input>?Device_Type::Controller_Hid:Device_Type::Controller_XinputHid,input.device);
            if(!device||device->token!=input.token)return false;
        }
        return true;
    }
    template<class T> void Append(std::vector<T>& inputs,Device_Type type) {
        for(auto& input:inputs) {
            Input_Event event;
            const auto& header=[&]() -> const auto& {
                if constexpr(std::is_same_v<T,Controller_GIP_Raw_Input>)return input.input;
                else return input;
            }();
            event.payload_=&input;event.owner_=this;event.batch_=batch;
            event.index_=events.size();event.type=type;event.device=header.device;
            event.sequence=header.sequence;event.timestamp=header.timestamp;
            events.push_back(event);
        }
    }
    Input_Batch Receive(std::unique_lock<std::recursive_mutex> lock,DWORD timeout,HANDLE stop) {
        RequireReader();
        auto& context=Ensure();
        try {
            const auto ready=Wait_For_Input(context,timeout,stop);
            if(!ready)return {};
            // Exactly the signalled families are read. GET buffers and the
            // descriptor array retain their capacity between calls.
            events.clear();++batch;
#define IC_AUTO_READ(bit,Name,buffer,type) if(ready&(bit)) {Get_##Name##_Input(context,storage.buffer);DecodeInputs(context,storage.buffer);Append(storage.buffer,Device_Type::type);}
            IC_AUTO_READ(Keyboard,Keyboard,keyboard,Keyboard);
            IC_AUTO_READ(Mouse,Mouse,mouse,Mouse);
            IC_AUTO_READ(Ready_Controller_Hid,Controller_Hid,hid,Controller_Hid);
            IC_AUTO_READ(Ready_Controller_Xinput,Controller_Xinput,xusb,Controller_Xinput);
            IC_AUTO_READ(Ready_Controller_XinputHid,Controller_XinputHid,xinputhid,Controller_XinputHid);
            if(context.gipRawSupported) {IC_AUTO_READ(Ready_Controller_GIP,Controller_GIP_Raw,gipRaw,Controller_GIP);}
            else {IC_AUTO_READ(Ready_Controller_GIP,Controller_GIP,gip,Controller_GIP);}
#undef IC_AUTO_READ
            if(events.empty())return {};
            bool refresh=false;
            for(const auto& event:events)if(!Find(event.type,event.device)){refresh=true;break;}
            if(refresh){(void)SnapshotDevices(context);}
            for(auto& event:events)event.device_info=Find(event.type,event.device);
            activeBatch=true;
            Input_Batch result;
            result.state_=this;result.lock_=std::move(lock);
            result.inputs_={events.data(),events.size()};
            result.exceptions_=std::uncaught_exceptions();
            return result;
        }catch(...){AbortException();throw;}
    }
    bool Valid(const Input_Event& event) const noexcept {
        return activeBatch && event.owner_==this && event.batch_==batch && event.index_<events.size()
            && event.payload_==events[event.index_].payload_;
    }
    Send_Result SendEvents(Detail::Input_View<Input_Event> inputs) {
        if(inputs.empty())return {};
        for(const auto& event:inputs)if(!Valid(event))return {0,ERROR_OPERATION_ABORTED};
        if(fault || !capture)return {0,fault?fault:ERROR_OPERATION_ABORTED};
        std::size_t accepted=0;
        for(std::size_t i=0;i<inputs.size();) {
            auto& event=inputs[i];
            if(event.blocked || events[event.index_].sent_){++i;continue;}
            std::size_t run=1;
            const auto result=std::visit([&](auto* first)->Send_Result {
                using T=std::remove_pointer_t<decltype(first)>;
                while(i+run<inputs.size()) {
                    auto& next=inputs[i+run];
                    const auto pointer=std::get_if<T*>(&next.payload_);
                    if(next.blocked || events[next.index_].sent_ || !pointer || *pointer!=first+run)break;
                    ++run;
                }
                return Send_Input(capture.Get(),Detail::Input_View<const T>(first,run));
            },event.payload_);
            for(std::size_t n=0;n<result.accepted;++n)events[inputs[i+n].index_].sent_=true;
            accepted+=result.accepted;
            if(result.error || result.accepted!=run) {
                const auto error=result.error?result.error:ERROR_WRITE_FAULT;
                if(error!=ERROR_BUSY)Abort(error);
                return {accepted,error};
            }
            i+=run;
        }
        return {accepted,ERROR_SUCCESS};
    }
    static Send_Result SendBatch(Input_Batch& inputs);
};
Automatic_State& Automatic(){static Automatic_State state;return state;}
struct Automatic_Lease {
    Automatic_State& state;
    std::unique_lock<std::recursive_mutex> lock;
    Automatic_Lease():state(Automatic()),lock(state.mutex,std::try_to_lock) {
        if(!lock.owns_lock())Fail("Automatic Interceptor session is being used by another thread",ERROR_BUSY);
    }
};
bool Stopped(HANDLE event) {
    if(!event)return false;
    const auto result=WaitForSingleObject(event,0);
    if(result==WAIT_FAILED)Fail("Check stop event");
    return result==WAIT_OBJECT_0;
}
Send_Result Automatic_State::SendBatch(Input_Batch& inputs) {
    if(inputs.empty())return {};
    Automatic_Lease lease;
    if(inputs.state_!=&lease.state)return {0,ERROR_OPERATION_ABORTED};
    return lease.state.SendEvents(inputs.inputs_);
}
}

void Input_Batch::Release() noexcept {
    if(state_) {
        // A transformation exception outside the SDK still restores physical
        // input while unwinding, even if the application catches it later.
        if(std::uncaught_exceptions()>exceptions_)state_->Abort(ERROR_OPERATION_ABORTED);
        state_->activeBatch=false;
        if(state_->fault)state_->ReleaseResources();
        state_=nullptr;inputs_={};
    }
    lock_=std::unique_lock<std::recursive_mutex>{};
}
Input_Batch::~Input_Batch(){Release();}
Input_Batch::Input_Batch(Input_Batch&& other) noexcept
    :state_(other.state_),lock_(std::move(other.lock_)),inputs_(other.inputs_),exceptions_(other.exceptions_) {
    other.state_=nullptr;other.inputs_={};
}
Input_Batch& Input_Batch::operator=(Input_Batch&& other) noexcept {
    if(this!=&other){Release();state_=other.state_;lock_=std::move(other.lock_);inputs_=other.inputs_;exceptions_=other.exceptions_;other.state_=nullptr;other.inputs_={};}
    return *this;
}
Input_Batch Get_Input_With_Timeout(DWORD timeout,HANDLE stopEvent) {
    if(Detail::Stopped(stopEvent))return {};
    Detail::Automatic_Lease lease;
    return lease.state.Receive(std::move(lease.lock),timeout,stopEvent);
}
std::vector<Device> Get_Devices() {
    Detail::Automatic_Lease lease;auto& state=lease.state;auto& context=state.Ensure();
    // Return a separate snapshot: metadata referenced by a live batch is stable.
    try{const auto snapshot=state.SnapshotDevices(context);return {snapshot.begin(),snapshot.end()};}
    catch(...){state.AbortException();throw;}
}
Context_Info Get_Context() {
    Detail::Automatic_Lease lease;auto& state=lease.state;auto& context=state.Ensure();
    Context_Info result;result.token=state.token;result.generation=state.capture.generation;
    try{const auto snapshot=state.SnapshotDevices(context);result.devices.assign(snapshot.begin(),snapshot.end());}
    catch(...){state.AbortException();throw;}
    result.active=true;
    try{result.status=Get_Status(&context);result.active=result.status.active_mask!=0;}
    catch(const std::system_error& e){result.status_error=static_cast<DWORD>(e.code().value());}
    try{result.capabilities=Capabilities(context.driver.value);}
    catch(const std::system_error& e){result.capabilities_error=static_cast<DWORD>(e.code().value());}
    try{result.trial=Get_Trial_Status();}
    catch(const std::system_error& e){result.trial_error=static_cast<DWORD>(e.code().value());}
    return result;
}
void Destroy_Context() {
    Detail::Automatic_Lease lease;auto& state=lease.state;state.RequireReader();
    Destroy_Context(state.capture);state.fault=ERROR_SUCCESS;state.ReleaseResources();
}
Send_Result Send_Input(Input_Batch& inputs){return Detail::Automatic_State::SendBatch(inputs);}
Send_Result Send_Input(Input_Event& input) {
    Detail::Automatic_Lease lease;
    return lease.state.SendEvents({&input,1});
}
namespace Detail {
template<class T> void SelectedRead(Context& context,std::vector<T>& out,std::vector<Wire<T>>& scratch,
    unsigned stream,DWORD code,std::uint32_t device,bool consume) {
    if(!(context.mask&(1u<<stream)))throw std::invalid_argument("Context does not own this input family");
    if constexpr(std::is_same_v<T,Controller_GIP_Raw_Input>)
        if(!context.gipRawSupported)Fail("Loaded driver has no complete GIP report API",ERROR_NOT_SUPPORTED);
    if(context.selectedReadSupport==0)
        Fail("Device selection and non-consuming reads require Interceptor 1.4.14 or newer",ERROR_NOT_SUPPORTED);
    std::lock_guard<std::mutex> lock(context.readers[stream]);
    const auto capacity=stream<2?IC_CAPACITY:IC_HID_CAPACITY;
    out.reserve(capacity);if(scratch.size()<capacity)scratch.resize(capacity);
    const IC_GET_SELECT request{{IC_VERSION,0},device,consume?0u:IC_GET_PEEK};
    DWORD bytes=0;
    try {bytes=Io(context.driver.value,code,&request,sizeof(request),scratch.data(),static_cast<DWORD>(capacity*sizeof(Wire<T>)));}
    catch(const std::system_error& e) {
        const auto error=e.code().value();
        if(context.selectedReadSupport<0 && (error==ERROR_INVALID_PARAMETER || error==ERROR_INVALID_FUNCTION || error==ERROR_NOT_SUPPORTED)) {
            context.selectedReadSupport=0;
            Fail("Device selection and non-consuming reads require Interceptor 1.4.14 or newer",ERROR_NOT_SUPPORTED);
        }
        throw;
    }
    if(bytes%sizeof(Wire<T>) || bytes>capacity*sizeof(Wire<T>))Fail("Invalid selected input response",ERROR_INVALID_DATA);
    context.selectedReadSupport=1;
    AssignWire(out,scratch.data(),bytes/sizeof(Wire<T>));
}
}
#define IC_AUTO_IMPL(Name,Type,stream,buffer,readIoctl) \
std::vector<Type> Get_##Name##_Input(std::uint32_t deviceId,bool consume) { \
    Detail::Automatic_Lease lease;auto& state=lease.state;state.RequireReader();auto& context=state.Ensure(); \
    std::vector<Type> out; \
    try{ \
        if(!deviceId && consume)Get_##Name##_Input(context,out); \
        else Detail::SelectedRead(context,out,context.buffer,stream,readIoctl,deviceId,consume); \
        state.DecodeInputs(context,out); \
    }catch(const std::system_error& e){if(e.code().value()!=ERROR_NOT_SUPPORTED)state.AbortException();throw;} \
    catch(...){state.AbortException();throw;} \
    return out; \
} \
Send_Result Detail::SendAuto(Detail::Input_View<const Type> inputs) { \
    if(inputs.empty())return {}; \
    Detail::Automatic_Lease lease;auto& state=lease.state;auto& context=state.Ensure(); \
    if(!state.ValidTokens(inputs))return {0,ERROR_NOT_FOUND}; \
    const auto result=Send_Input(context,inputs.data(),inputs.size()); \
    if(result.error && result.error!=ERROR_BUSY)state.Abort(result.error);return result; \
}
IC_AUTO_IMPL(Keyboard,Keyboard_Input,0,keyboard,IOCTL_IC_GET_KEYBOARD)
IC_AUTO_IMPL(Mouse,Mouse_Input,1,mouse,IOCTL_IC_GET_MOUSE)
IC_AUTO_IMPL(Controller_Hid,Controller_Hid_Input,2,hid,IOCTL_IC_GET_HID)
IC_AUTO_IMPL(Controller_Xinput,Controller_Xinput_Input,3,xusb,IOCTL_IC_GET_XUSB)
IC_AUTO_IMPL(Controller_XinputHid,Controller_XinputHid_Input,4,xinputhid,IOCTL_IC_GET_XINPUTHID)
IC_AUTO_IMPL(Controller_GIP,Controller_GIP_Input,5,gip,IOCTL_IC_GET_GIP)
IC_AUTO_IMPL(Controller_GIP_Raw,Controller_GIP_Raw_Input,5,gipRaw,IOCTL_IC_GET_GIP_RAW)
#undef IC_AUTO_IMPL
IC_STATUS Get_Status() {
    Detail::Automatic_Lease lease;
    return Get_Status(lease.state.capture?&lease.state.capture.Get():nullptr);
}
}
