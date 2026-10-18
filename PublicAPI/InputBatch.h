#pragma once
#include "InputTypes.h"
#include <memory>
#include <mutex>
#include <variant>

namespace Interceptor {
namespace Detail {struct Automatic_State;}
// Small descriptors reference reusable typed buffers. Small events do not
// embed a maximum-sized HID report or duplicate Device metadata.
class Input_Event {
    using Payload=std::variant<Keyboard_Input*,Mouse_Input*,Controller_Hid_Input*,
        Controller_Xinput_Input*,Controller_XinputHid_Input*,Controller_GIP_Input*,Controller_GIP_Raw_Input*>;
    Payload payload_=static_cast<Keyboard_Input*>(nullptr);
    Detail::Automatic_State* owner_=nullptr;
    std::uint64_t batch_=0;
    std::size_t index_=0;
    bool sent_=false;
    friend struct Detail::Automatic_State;
public:
    Device_Type type=Device_Type::Keyboard;
    std::uint32_t device=0;
    std::uint64_t sequence=0,timestamp=0;
    const Device* device_info=nullptr;
    bool blocked=false;
    // Standard GIP access also works on a complete report, retaining its tail.
    template<class T> T* Get() noexcept {
        if(auto p=std::get_if<T*>(&payload_))return *p;
        if constexpr(std::is_same_v<T,Controller_GIP_Input>) {
            if(auto p=std::get_if<Controller_GIP_Raw_Input*>(&payload_))return *p?&(*p)->input:nullptr;
        }
        return nullptr;
    }
    template<class T> const T* Get() const noexcept{return const_cast<Input_Event*>(this)->Get<T>();}
};
// Move-only borrowed array. Entries, payloads and metadata remain valid until
// destruction. Keep a live batch on its creating thread. A second receive is
// rejected while a batch exists. Sending entries/batches remains supported.
class Input_Batch {
    Detail::Automatic_State* state_=nullptr;
    std::unique_lock<std::recursive_mutex> lock_;
    Detail::Input_View<Input_Event> inputs_;
    int exceptions_=0;
    friend struct Detail::Automatic_State;
    void Release() noexcept;
public:
    Input_Batch() noexcept=default;
    ~Input_Batch();
    Input_Batch(const Input_Batch&)=delete;
    Input_Batch& operator=(const Input_Batch&)=delete;
    Input_Batch(Input_Batch&&) noexcept;
    Input_Batch& operator=(Input_Batch&&) noexcept;
    std::size_t size() const noexcept{return inputs_.size();}
    bool empty() const noexcept{return inputs_.empty();}
    Input_Event* data() noexcept{return inputs_.data();}
    const Input_Event* data() const noexcept{return inputs_.data();}
    Input_Event* begin() noexcept{return inputs_.begin();}
    Input_Event* end() noexcept{return inputs_.end();}
    const Input_Event* begin() const noexcept{return inputs_.begin();}
    const Input_Event* end() const noexcept{return inputs_.end();}
    Input_Event& operator[](std::size_t n) noexcept{return inputs_[n];}
    const Input_Event& operator[](std::size_t n) const noexcept{return inputs_[n];}
};
}
