#pragma once
// This SDK belongs exclusively to Interceptor.
// IC_SDK_INTERNAL enables installer/viewer compatibility declarations only.
#if defined(IC_PRODUCT_INPUT_MASK) && IC_PRODUCT_INPUT_MASK != 3u
#error This SDK cannot enable input families belonging to another product.
#endif
#ifndef IC_PRODUCT_INPUT_MASK
#define IC_PRODUCT_INPUT_MASK 3u
#endif
#if defined(IC_PRODUCT_CONTROLLER_MASK) && IC_PRODUCT_CONTROLLER_MASK != 15u
#error This SDK cannot enable controller families belonging to another product.
#endif
#ifndef IC_PRODUCT_CONTROLLER_MASK
#define IC_PRODUCT_CONTROLLER_MASK 15u
#endif
#ifndef IC_PRODUCT_HAS_KEYBOARD_MOUSE
#define IC_PRODUCT_HAS_KEYBOARD_MOUSE 1
#endif
#ifndef IC_PRODUCT_HAS_CONTROLLERS
#define IC_PRODUCT_HAS_CONTROLLERS 1
#endif
#define IC_SDK_SERVICE L"Interceptor"
#define IC_SDK_DOS_DEVICE L"\\\\.\\Interceptor"

// Interceptor SDK 1.4.22.0. Compile Interceptor.cpp once alongside your application.
// This header has no local-header dependencies. Windows x64, C++17.

// ---- Interceptor.h ----

// ---- InputTypes.h ----
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

// ---- InputView.h ----
#include <array>
#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace Interceptor::Detail {
// A non-owning, sized range. Containers/arrays/single inputs supply their size
// automatically; pointer+size is only needed when adapting an external buffer.
template<class T> class Input_View {
    T* data_=nullptr;
    std::size_t size_=0;
    template<class U> using Compatible=std::enable_if_t<std::is_convertible_v<U(*)[],T(*)[]>,int>;
public:
    using element_type=T;
    using value_type=std::remove_cv_t<T>;
    constexpr Input_View() noexcept=default;
    Input_View(T* data,std::size_t size):data_(data),size_(size) {
        if(size && !data)throw std::invalid_argument("Input_View: null nonempty buffer");
    }
    constexpr Input_View(T& input) noexcept:data_(&input),size_(1){}
    template<std::size_t N> constexpr Input_View(T (&inputs)[N]) noexcept:data_(inputs),size_(N){}
    template<class U,std::size_t N,Compatible<U> =0>
    constexpr Input_View(std::array<U,N>& inputs) noexcept:data_(inputs.data()),size_(N){}
    template<class U,std::size_t N,Compatible<const U> =0>
    constexpr Input_View(const std::array<U,N>& inputs) noexcept:data_(inputs.data()),size_(N){}
    template<class U,class A,Compatible<U> =0>
    Input_View(std::vector<U,A>& inputs) noexcept:data_(inputs.data()),size_(inputs.size()){}
    template<class U,class A,Compatible<const U> =0>
    Input_View(const std::vector<U,A>& inputs) noexcept:data_(inputs.data()),size_(inputs.size()){}
    template<class U,Compatible<U> =0>
    constexpr Input_View(Input_View<U> inputs) noexcept:data_(inputs.data()),size_(inputs.size()){}
    constexpr T* data() const noexcept{return data_;}
    constexpr std::size_t size() const noexcept{return size_;}
    constexpr bool empty() const noexcept{return !size_;}
    constexpr T* begin() const noexcept{return data_;}
    constexpr T* end() const noexcept{return size_?data_+size_:data_;}
    constexpr T& operator[](std::size_t index) const noexcept{return data_[index];}
    Input_View first(std::size_t count) const {
        if(count>size_)throw std::out_of_range("Input_View: prefix exceeds input size");
        return {data_,count};
    }
    Input_View subview(std::size_t offset) const {
        if(offset>size_)throw std::out_of_range("Input_View: offset exceeds input size");
        return {offset?data_+offset:data_,size_-offset};
    }
};
template<class T> Input_View(T&)->Input_View<T>;
template<class T,std::size_t N> Input_View(T (&)[N])->Input_View<T>;
template<class T,std::size_t N> Input_View(std::array<T,N>&)->Input_View<T>;
template<class T,std::size_t N> Input_View(const std::array<T,N>&)->Input_View<const T>;
template<class T,class A> Input_View(std::vector<T,A>&)->Input_View<T>;
template<class T,class A> Input_View(const std::vector<T,A>&)->Input_View<const T>;
}

#if IC_PRODUCT_HAS_CONTROLLERS || defined(IC_SDK_INTERNAL)
// ---- DecodedHid.h ----
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Interceptor {
namespace Detail {struct Hid_Codec;struct Hid_Input_Access;}
enum class Hid_Value_Kind {Value,Axis,Hat,Trigger};
struct Hid_Value {
    std::uint16_t usage_page=0,usage=0,link_collection=0,bit_size=0,data_index=0;
    std::uint32_t element=0,units=0,units_exponent=0;
    std::uint8_t report_id=0;
    bool relative=false,has_null=false,is_null=false;
    Hid_Value_Kind kind=Hid_Value_Kind::Value;
    std::int64_t value=0,minimum=0,maximum=0,physical_minimum=0,physical_maximum=0;
    // Absolute axes: -1..1. Identified analog triggers: 0..1.
    // Relative, null and constant-range values have normalized_valid=false.
    double normalized=0;
    bool normalized_valid=false;
private:
    std::int64_t original_=0;
    double original_normalized_=0;
    std::uint32_t definition_=0; // Codec expansion is capped below 2^18.
    std::uint32_t original_element_=0;
    friend struct Detail::Hid_Codec;
};
struct Hid_Button {
    std::uint16_t usage_page=0,usage=0,link_collection=0,data_index=0;
    std::uint8_t report_id=0;
    bool pressed=false;
    bool trigger=false; // Explicit trigger usage or trigger collection.
private:
    bool original_=false;
    std::size_t definition_=0;
    friend struct Detail::Hid_Codec;
};
// Fields wider than 32 bits retain their exact packed little-endian bits.
struct Hid_Binary_Value {
    std::uint16_t usage_page=0,usage=0,link_collection=0,bit_size=0,data_index=0;
    std::uint32_t count=0;
    std::uint8_t report_id=0;
    std::vector<std::uint8_t> data;
private:
    std::vector<std::uint8_t> original_;
    std::size_t definition_=0;
    friend struct Detail::Hid_Codec;
};
// Each scalar occurs in exactly one list. Repeated X/Y usages remain separate
// axes with their report/collection/data-index/element identity intact.
struct Hid_State {
    std::vector<Hid_Value> axes;
    std::vector<Hid_Value> triggers;
    std::vector<Hid_Value> hats;
    std::vector<Hid_Value> values; // Other scalars, including vendor usages.
    std::vector<Hid_Button> buttons; // Includes released buttons in this report.
    std::vector<Hid_Binary_Value> binary_values;
};
struct Hid_Decoded_Input {
    std::string token;
    Hid_State state; // Current report only; never a guessed cumulative state.
    DWORD decode_error=ERROR_SUCCESS;
    std::uint32_t parser_status=0;
    bool decode_complete=false;
private:
    std::shared_ptr<const Detail::Hid_Codec> codec_;
    std::uint8_t report_id_=0;
    std::uint32_t source_id_=0;
    friend struct Detail::Hid_Codec;
    friend struct Detail::Hid_Input_Access;
};
}

#endif

// ---- internal/ControllerHid.h ----

// ---- internal/InterceptorProtocol.h ----
#include <stdint.h>

// Versioned, pointer-free ABI. All reserved fields must be zero.
#define IC_VERSION 4u
#define IC_KEYBOARD 1u
#define IC_MOUSE 2u
#define IC_BOTH 3u
#define IC_CAPACITY 8192u
#define IC_MAX_SEND 256u
#define IC_MAX_DEVICES 256u
#define IC_TYPE_KEYBOARD 1u
#define IC_TYPE_MOUSE 2u
#define IC_TYPE_CONTROLLER_HID 3u
#define IC_TYPE_CONTROLLER_XUSB 4u
#define IC_TYPE_CONTROLLER_XINPUTHID 5u
#define IC_TYPE_CONTROLLER_GIP 6u
#define IC_INFO_HID_VALID 1u
#define IC_INFO_NAME_VALID 2u
#define IC_INFO_HARDWARE_ID_VALID 4u
typedef struct IC_DEVICE_INFO {
    uint32_t id,type,instance_token,flags;
    uint16_t vid,pid,revision,usage_page,usage,input_bytes,output_bytes,feature_bytes;
    uint16_t name[128],hardware_id[256]; // UTF-16; empty if unavailable.
} IC_DEVICE_INFO;
typedef struct IC_CREATE_RESULT { uint32_t version,count;uint64_t generation; } IC_CREATE_RESULT;
#define IC_DEVICE_TYPE 0x8349u
#define IC_IOCTL(n) CTL_CODE(IC_DEVICE_TYPE,n,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define IOCTL_IC_CREATE IC_IOCTL(0xA00)
#define IOCTL_IC_GET_KEYBOARD IC_IOCTL(0xA01)
#define IOCTL_IC_GET_MOUSE IC_IOCTL(0xA02)
#define IOCTL_IC_SEND_KEYBOARD IC_IOCTL(0xA03)
#define IOCTL_IC_SEND_MOUSE IC_IOCTL(0xA04)
#define IOCTL_IC_STATUS IC_IOCTL(0xA05)
#define IOCTL_IC_DESTROY IC_IOCTL(0xA06)
#define IOCTL_IC_SEND_OWNED_KEYBOARD IC_IOCTL(0xA07)
#define IOCTL_IC_SEND_OWNED_MOUSE IC_IOCTL(0xA08)
#define IOCTL_IC_CAPABILITIES IC_IOCTL(0xA26)
// Retired function numbers 0xA27 and 0xA28 are reserved; do not reuse them.
#define IC_CAPABILITIES_VERSION 1u
#define IC_PROFILE_XUSB_WIRED_028E 1u
#define IC_PROFILE_XUSB_WIRED_COMPATIBLE 2u
#define IC_PROFILE_XUSB_USB_FILTER 4u
#define IC_PROFILE_XINPUTHID_GENERIC6 1u
#define IC_PROFILE_GIP_USB_GAMEPAD14 1u
#define IC_PROFILE_GIP_USB_GAMEPAD_RAW 2u
// Immutable capabilities. No input or capture is required. These profile bits
// identify supported stack/protocol shapes, not every product sharing a brand.
typedef struct IC_CAPABILITIES {
    uint32_t size,version,abi_version,input_mask;
    uint32_t controller_mask,flags,max_devices,queue_capacity;
    uint32_t max_send,hid_report_max,xusb_profiles,xinputhid_profiles;
    uint32_t gip_profiles,reserved[3];
} IC_CAPABILITIES;

typedef struct IC_KEYBOARD_STROKE {
    uint16_t unit_id, make_code, flags, reserved;
    uint32_t extra_information;
} IC_KEYBOARD_STROKE;
typedef struct IC_MOUSE_STROKE {
    uint16_t unit_id, flags, button_flags, button_data;
    uint32_t raw_buttons;
    int32_t x, y;
    uint32_t extra_information;
} IC_MOUSE_STROKE;
typedef struct IC_KEYBOARD_INPUT {
    uint64_t sequence, timestamp;
    uint32_t device, reserved;
    IC_KEYBOARD_STROKE stroke;
    uint32_t padding;
} IC_KEYBOARD_INPUT;
typedef struct IC_MOUSE_INPUT {
    uint64_t sequence, timestamp;
    uint32_t device, reserved;
    IC_MOUSE_STROKE stroke;
} IC_MOUSE_INPUT;
typedef struct IC_CREATE {
    uint32_t version;
    uint8_t mask;
    uint8_t controller_mask, reserved[2];
    uint64_t keyboard_event, mouse_event;
    uint64_t revoked_event, controller_hid_event;
} IC_CREATE;
typedef struct IC_GET { uint32_t version, count; } IC_GET;
typedef struct IC_SEND_RESULT { uint32_t accepted, status; } IC_SEND_RESULT;
typedef struct IC_STATUS {
    uint32_t version, active_mask, keyboard_count, mouse_count;
    uint32_t keyboard_devices, mouse_devices, last_reset_reason, capacity;
    uint64_t generation, overflows, timestamp_frequency;
    uint32_t controller_hid_count,controller_hid_devices;
} IC_STATUS;
#define IC_RESET_NONE 0u
#define IC_RESET_HOTKEY 1u
#define IC_RESET_OVERFLOW 2u
#define IC_RESET_CLOSE 3u
#define IC_RESET_DEVICE 4u
#define IC_RESET_TRIAL_EXPIRED 5u

#if defined(__cplusplus)
static_assert(sizeof(IC_DEVICE_INFO)==800 && sizeof(IC_CREATE_RESULT)==16,"Device snapshot ABI");
static_assert(sizeof(IC_KEYBOARD_INPUT)==40 && sizeof(IC_MOUSE_INPUT)==48,"ABI layout");
static_assert(sizeof(IC_CREATE)==40 && sizeof(IC_STATUS)==64,"ABI layout");
static_assert(sizeof(IC_CAPABILITIES)==64,"Capabilities ABI");
#else
typedef char IC_ABI_CHECK[(sizeof(IC_DEVICE_INFO)==800 && sizeof(IC_CREATE_RESULT)==16 && sizeof(IC_KEYBOARD_INPUT)==40 && sizeof(IC_MOUSE_INPUT)==48 && sizeof(IC_CREATE)==40 && sizeof(IC_STATUS)==64)?1:-1];
typedef char IC_CAPABILITIES_ABI_CHECK[(sizeof(IC_CAPABILITIES)==64)?1:-1];
#endif
#define IC_CONTROLLER_HID 1u
#define IC_CONTROLLER_XUSB 2u
#define IC_CONTROLLER_XINPUTHID 4u
#define IC_CONTROLLER_GIP 8u
#define IC_CONTROLLER_ALL 15u
#define IC_READY_HID 4u
#define IC_STREAM_COUNT 6u
#define IC_ALL_STREAMS 63u
#define IC_HID_REPORT_MAX 1024u
#define IC_HID_CAPACITY 256u
#define IC_HID_FILE_CAPACITY 16u
#define IOCTL_IC_GET_HID IC_IOCTL(0xA09)
#define IOCTL_IC_SEND_HID IC_IOCTL(0xA0A)
#define IOCTL_IC_SEND_OWNED_HID IC_IOCTL(0xA0B)
#define IOCTL_IC_HID_DESCRIPTOR IC_IOCTL(0xA0C)
#define IOCTL_IC_HID_SOURCE IC_IOCTL(0xA0D)
// Collection-level reports, including byte zero (Report ID, zero if absent).
typedef struct IC_CONTROLLER_HID_INPUT {
    uint64_t sequence,timestamp;
    uint32_t device,length;
    uint32_t reserved[2];
    uint8_t report[IC_HID_REPORT_MAX];
} IC_CONTROLLER_HID_INPUT;
// {9CE34440-4716-4F39-922B-995C28A52C87}
#define IC_HID_INTERFACE_GUID {0x9ce34440,0x4716,0x4f39,{0x92,0x2b,0x99,0x5c,0x28,0xa5,0x2c,0x87}}
#if defined(__cplusplus)
static_assert(sizeof(IC_CONTROLLER_HID_INPUT)==1056,"HID report ABI");
#endif

// ---- internal/ControllerXusb.h ----

// ---- internal/ControllerNativeProtocol.h ----
// User-mode native gamepad wire state. Transport and queue implementations
// remain private to the binary driver and are not part of this SDK.
typedef struct IC_NATIVE_GAMEPAD_STATE {
    uint32_t packet;
    uint16_t buttons,left_trigger,right_trigger;
    short left_x,left_y,right_x,right_y;
    uint8_t extra_buttons,reserved;
} IC_NATIVE_GAMEPAD_STATE;
#define IC_GIP_INPUT_BYTES 18u
// The raw USB descriptor query in the SDK reads USB little-endian fields.
static __inline uint16_t IcNativeRead16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8));
}
#define IC_XUSB_CAPACITY 256u
#define IC_READY_XUSB 8u
#define IOCTL_IC_CREATE_XUSB IC_IOCTL(0xA10)
#define IOCTL_IC_GET_XUSB IC_IOCTL(0xA11)
#define IOCTL_IC_SEND_XUSB IC_IOCTL(0xA12)
#define IOCTL_IC_SEND_OWNED_XUSB IC_IOCTL(0xA13)
#define IOCTL_IC_XUSB_SOURCE IC_IOCTL(0xA14)
#define IC_XUSB_INTERFACE_GUID {0xf631b58e,0x571b,0x44ba,{0xa3,0x43,0x47,0xe2,0x25,0x7a,0x91,0xb3}}
// Additive ABI: the original 40-byte IC_CREATE is unchanged.
typedef struct IC_CREATE_XUSB { IC_CREATE base;uint64_t xusb_event; } IC_CREATE_XUSB;
typedef struct IC_CONTROLLER_XUSB_INPUT {
    uint64_t sequence,timestamp;
    uint32_t device,reserved;
    IC_NATIVE_GAMEPAD_STATE state;
    uint32_t padding;
} IC_CONTROLLER_XUSB_INPUT;
#if defined(__cplusplus)
static_assert(sizeof(IC_CREATE_XUSB)==48,"Native create ABI");
static_assert(sizeof(IC_CONTROLLER_XUSB_INPUT)==48,"XUSB state ABI");
#endif

// ---- internal/ControllerNative.h ----

// ---- internal/ControllerXinputHid.h ----

#define IC_READY_XINPUTHID 16u
#define IOCTL_IC_GET_XINPUTHID IC_IOCTL(0xA17)
#define IOCTL_IC_SEND_XINPUTHID IC_IOCTL(0xA18)
#define IOCTL_IC_SEND_OWNED_XINPUTHID IC_IOCTL(0xA19)
#define IOCTL_IC_XINPUTHID_DESCRIPTOR IC_IOCTL(0xA1A)
#define IOCTL_IC_XINPUTHID_SOURCE IC_IOCTL(0xA1B)
// Source HID report BEFORE xinputhid translation, including the report ID.
// Descriptor and length belong to the physical collection: they are not the
// synthetic 16-byte DirectInput report exposed above the Windows translator.
typedef struct IC_CONTROLLER_XINPUTHID_INPUT {
    uint64_t sequence,timestamp;
    uint32_t device,length;
    uint32_t reserved[2];
    uint8_t report[IC_HID_REPORT_MAX];
} IC_CONTROLLER_XINPUTHID_INPUT;
#define IC_XINPUTHID_INTERFACE_GUID {0x2788ce3e,0x51bd,0x43eb,{0xa6,0x6d,0x13,0x74,0xb8,0x48,0xc5,0x8c}}
#if defined(__cplusplus)
static_assert(sizeof(IC_CONTROLLER_XINPUTHID_INPUT)==sizeof(IC_CONTROLLER_HID_INPUT),"XInputHID wire layout");
#endif
#define IOCTL_IC_CREATE_NATIVE IC_IOCTL(0xA16)
// Additive request: neither IC_CREATE nor IC_CREATE_XUSB changes layout.
typedef struct IC_CREATE_NATIVE {
    IC_CREATE base;
    uint64_t xusb_event,xinputhid_event,gip_event;
} IC_CREATE_NATIVE;
#if defined(__cplusplus)
static_assert(sizeof(IC_CREATE_NATIVE)==64,"Extended native create ABI");
#endif

// ---- internal/ControllerGip.h ----

#define IC_GIP_CAPACITY 256u
#define IC_READY_GIP 32u
#define IOCTL_IC_GET_GIP IC_IOCTL(0xA20)
#define IOCTL_IC_SEND_GIP IC_IOCTL(0xA21)
#define IOCTL_IC_SEND_OWNED_GIP IC_IOCTL(0xA22)
#define IOCTL_IC_GIP_DESCRIPTOR IC_IOCTL(0xA23)
#define IOCTL_IC_GIP_SOURCE IC_IOCTL(0xA24)
#define IOCTL_IC_GET_GIP_RAW IC_IOCTL(0xA29)
#define IOCTL_IC_SEND_GIP_RAW IC_IOCTL(0xA2A)
#define IOCTL_IC_SEND_OWNED_GIP_RAW IC_IOCTL(0xA2B)
#define IC_GIP_REPORT_MAX 64u
#define IC_GIP_INTERFACE_GUID {0x78310f48,0xccae,0x4a2d,{0x88,0x35,0xac,0xac,0xcf,0x26,0x6c,0xd2}}
// Native GIP state, not an XUSB envelope. state.buttons uses GIP bit positions;
// triggers are 0..1023, axes signed 16-bit, extra_buttons bit 0 is Guide.
// Input transport sequence/keep-alive remain owned by the USB transport.
typedef struct IC_CONTROLLER_GIP_INPUT {
    uint64_t sequence,timestamp;
    uint32_t device,reserved;
    IC_NATIVE_GAMEPAD_STATE state;
    uint32_t padding;
} IC_CONTROLLER_GIP_INPUT;
// Optional ABI4 extension. The legacy 48-byte state API remains unchanged.
// state controls the standard prefix; report retains the original envelope
// and opaque extension bytes. Send owns sequence/keep-alive, and accepts only
// the current device's validated gamepad shape. report_length=0 preserves the
// latest physical extension, as does the legacy state-only Send.
typedef struct IC_CONTROLLER_GIP_RAW_INPUT {
    IC_CONTROLLER_GIP_INPUT input;
    uint32_t report_length,report_reserved;
    uint8_t report[IC_GIP_REPORT_MAX];
} IC_CONTROLLER_GIP_RAW_INPUT;
#if defined(__cplusplus)
static_assert(sizeof(IC_CONTROLLER_GIP_INPUT)==48,"GIP state ABI");
static_assert(sizeof(IC_CONTROLLER_GIP_RAW_INPUT)==120,"GIP raw extension ABI");
#endif

// ---- internal/DeviceQuery.h ----

// ---- internal/HidGameProfile.h ----
// USB-IF HID Usage Tables, application collections. Descriptor validation is
// still mandatory; these are protocol shapes, never a product allow-list.
#define IC_HID_PROFILE_DESKTOP 1u
#define IC_HID_PROFILE_SIMULATION 2u
#define IC_HID_PROFILE_SPORT 4u
#define IC_HID_PROFILE_GAME 8u
#define IC_HID_PROFILE_SPATIAL 16u
#define IC_HID_PROFILES_ALL 31u
static __inline unsigned IcHidGameProfile(unsigned short page,unsigned short usage) {
    if(page==1 && (usage==4 || usage==5 || usage==8))return IC_HID_PROFILE_DESKTOP;
    if(page==1 && usage==0x0F)return IC_HID_PROFILE_SPATIAL;
    if(page==2 && ((usage>=1 && usage<=12) || usage==0x20 || usage==0x21 || usage==0x24))return IC_HID_PROFILE_SIMULATION;
    if(page==4 && usage>=1 && usage<=4)return IC_HID_PROFILE_SPORT;
    if(page==5 && usage>=1 && usage<=3)return IC_HID_PROFILE_GAME;
    return 0;
}
// Optional ABI4 extension. Old binaries and the original 64-byte capabilities
// remain unchanged. type=token=0 queries the supported HID profiles only.
// by_ordinal=1 selects a registered reader by 1-based ordinal, even when it is
// not ready. Result.instance_token is always the real token. No capture starts.
#define IOCTL_IC_DEVICE_QUERY IC_IOCTL(0xA2C)
#define IC_DEVICE_QUERY_VERSION 1u
#define IC_DEVICE_STATE_VERSION 2u
#define IC_QUERY_IDENTITY_VALID 0x10000u
// Version 2 preserves the version-1 layout/flags and defines reserved[0] as
// availability and reserved[1] as reasons. Version-1 reserved fields stay zero.
#define IC_DEVICE_ABSENT 0u // Enumeration ended; never returned for a live token.
#define IC_DEVICE_PREPARING 1u
#define IC_DEVICE_READY 2u // Capture can start; input activity is not required.
#define IC_DEVICE_UNSUPPORTED 3u
#define IC_DEVICE_UNAVAILABLE 4u
#define IC_DEVICE_WAIT_POWER 0x1u
#define IC_DEVICE_WAIT_READER 0x2u
#define IC_DEVICE_WAIT_PROFILE 0x4u
#define IC_DEVICE_WAIT_HELLO 0x8u
#define IC_DEVICE_WAIT_METADATA 0x10u
#define IC_DEVICE_WAIT_FIRST_REPORT 0x20u
#define IC_DEVICE_WAIT_MIRROR 0x40u
#define IC_DEVICE_TRANSPARENT 0x80u
#define IC_DEVICE_PROFILE_REJECTED 0x100u
#define IC_DEVICE_WAIT_CONNECTION 0x200u
#define IC_DEVICE_TRIAL_EXPIRED 0x400u
#define IC_DEVICE_TRIAL_UNAVAILABLE 0x800u
typedef struct IC_DEVICE_QUERY {uint32_t version,type,instance_token,by_ordinal;} IC_DEVICE_QUERY;
typedef struct IC_DEVICE_QUERY_RESULT {
    uint32_t size,version,type,instance_token,hid_profiles,flags,last_read_status,last_usb_status;
    uint64_t physical_reports;
    uint32_t report_bytes,metadata_bytes,metadata_input_bytes,reserved[3];
    uint16_t instance[256];
} IC_DEVICE_QUERY_RESULT;
#if defined(__cplusplus)
static_assert(sizeof(IC_DEVICE_QUERY)==16 && sizeof(IC_DEVICE_QUERY_RESULT)==576,"Device query extension ABI");
#endif

// ---- internal/Trial.h ----

// Optional ABI4 extension. Queries never start, pause or reset the trial.
#define IOCTL_IC_TRIAL_STATUS IC_IOCTL(0xA2D)
#define IC_TRIAL_VERSION 1u
#define IC_TRIAL_NOT_STARTED 0u
#define IC_TRIAL_ACTIVE 1u
#define IC_TRIAL_EXPIRED 2u
#define IC_TRIAL_UNAVAILABLE 3u
#define IC_TRIAL_DURATION_100NS (3600ull*10000000ull)
// STATUS_EVALUATION_EXPIRATION -> ERROR_EVALUATION_EXPIRATION.
#define IC_ERROR_TRIAL_EXPIRED 622u
typedef struct IC_TRIAL_STATUS {
    uint32_t size,version,state,error;
    uint64_t duration_100ns,first_context_100ns,elapsed_100ns,remaining_100ns;
    uint32_t active_mask,reserved[3];
} IC_TRIAL_STATUS;
#if defined(__cplusplus)
static_assert(sizeof(IC_TRIAL_STATUS)==64,"Trial status extension ABI");
#endif

namespace Interceptor {
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE || defined(IC_SDK_INTERNAL)
inline constexpr std::uint8_t Keyboard=IC_KEYBOARD, Mouse=IC_MOUSE, Both=IC_BOTH;
using Keyboard_Input=IC_KEYBOARD_INPUT;
using Mouse_Input=IC_MOUSE_INPUT;
#endif
#if IC_PRODUCT_HAS_CONTROLLERS || defined(IC_SDK_INTERNAL)
inline constexpr std::uint8_t Controller_Hid=IC_CONTROLLER_HID, Controller_Xinput=IC_CONTROLLER_XUSB,
    Controller_XinputHid=IC_CONTROLLER_XINPUTHID, Controller_GIP=IC_CONTROLLER_GIP, Ready_Controller_Hid=IC_READY_HID;
inline constexpr std::uint8_t Ready_Controller_Xinput=IC_READY_XUSB,
    Ready_Controller_XinputHid=IC_READY_XINPUTHID, Ready_Controller_GIP=IC_READY_GIP;
struct Controller_Hid_Input : IC_CONTROLLER_HID_INPUT,Hid_Decoded_Input {
    Controller_Hid_Input():IC_CONTROLLER_HID_INPUT{}{}
};
using Controller_Xinput_Input=IC_CONTROLLER_XUSB_INPUT;
struct Controller_XinputHid_Input : IC_CONTROLLER_XINPUTHID_INPUT,Hid_Decoded_Input {
    Controller_XinputHid_Input():IC_CONTROLLER_XINPUTHID_INPUT{}{}
};
using Controller_GIP_Input=IC_CONTROLLER_GIP_INPUT;
using Controller_GIP_Raw_Input=IC_CONTROLLER_GIP_RAW_INPUT;
#endif
// Application identity contains only the capture-scoped string token.
// Kernel wire records and their numeric source identity stay private.
struct Device {
    std::uint32_t id=0,type=0,flags=0;
    std::uint16_t vid=0,pid=0,revision=0,usage_page=0,usage=0;
    std::uint16_t input_bytes=0,output_bytes=0,feature_bytes=0;
    std::uint16_t name[128]{},hardware_id[256]{};
    std::string token;
};
struct Device_Details {
    std::string token;
    // Size/version describe the validated driver response, not this C++ object.
    std::uint32_t size=0,version=0,type=0,hid_profiles=0,flags=0;
    std::uint32_t last_read_status=0,last_usb_status=0;
    std::uint64_t physical_reports=0;
    std::uint32_t report_bytes=0,metadata_bytes=0,metadata_input_bytes=0,reserved[3]{};
    std::uint16_t instance[256]{};
};
struct Context_Info {
    std::string token;
    std::uint64_t generation=0;
    bool active=false;
    std::vector<Device> devices;
    IC_STATUS status{};
    IC_CAPABILITIES capabilities{};
    IC_TRIAL_STATUS trial{};
    DWORD status_error=ERROR_SUCCESS,capabilities_error=ERROR_SUCCESS,trial_error=ERROR_SUCCESS;
};
enum class Descriptor_Format { Identity, Hid_Preparsed, Gip_Metadata, Hid_Report };
struct Descriptor_Result {
    Device device;
    Descriptor_Format format=Descriptor_Format::Identity;
    std::vector<std::uint8_t> data;
    Device_Details details;
    DWORD error=ERROR_SUCCESS,details_error=ERROR_SUCCESS;
    bool applicable=true;
    explicit operator bool() const noexcept{return applicable&&error==ERROR_SUCCESS;}
};
using Report_Descriptor=Descriptor_Result;
enum class Device_Type : std::uint32_t {
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE || defined(IC_SDK_INTERNAL)
    Keyboard=IC_TYPE_KEYBOARD,Mouse=IC_TYPE_MOUSE,
#endif
#if IC_PRODUCT_HAS_CONTROLLERS || defined(IC_SDK_INTERNAL)
    Controller_Hid=IC_TYPE_CONTROLLER_HID,Controller_Xinput=IC_TYPE_CONTROLLER_XUSB,
    Controller_XinputHid=IC_TYPE_CONTROLLER_XINPUTHID,Controller_GIP=IC_TYPE_CONTROLLER_GIP
#endif
};
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE || defined(IC_SDK_INTERNAL)
inline constexpr std::uint16_t KeyUp=1, KeyE0=2, KeyE1=4;
inline constexpr std::uint16_t MouseAbsolute=1, MouseVirtualDesktop=2;
inline constexpr std::uint16_t LeftDown=1, LeftUp=2, RightDown=4, RightUp=8;
inline constexpr std::uint16_t MiddleDown=16, MiddleUp=32, Wheel=0x400, HorizontalWheel=0x800;
#endif
struct Init_Skip_Diagnostic {std::wstring instance,reason;DWORD error=ERROR_SUCCESS;};
struct Init_Result {
    bool ready=false,reboot_required=false;
    std::uint32_t associated=0,already_associated=0,skipped=0;
    DWORD error=ERROR_SUCCESS;
    std::vector<Init_Skip_Diagnostic> skipped_devices;
    // First setup failure, including the operation and exact device when known.
    // Empty on success. Reading input never performs this diagnostic work.
    std::string error_detail;
    std::wstring error_device;
};
struct Send_Result {
    std::size_t accepted=0;DWORD error=ERROR_SUCCESS;
    explicit operator bool() const noexcept{return error==ERROR_SUCCESS;}
};
struct Device_Availability {
    Device_Details details;
    std::uint32_t state=IC_DEVICE_ABSENT,reasons=0;
    bool reported_by_driver=false;
};
}

// ---- InputBatch.h ----
#include <memory>
#include <mutex>
#include <variant>

namespace Interceptor {
namespace Detail {struct Automatic_State;}
// Small descriptors reference reusable typed buffers. Small events do not
// embed a maximum-sized HID report or duplicate Device metadata.
class Input_Event {
    using Payload=std::variant<Keyboard_Input*,Mouse_Input*,Controller_Hid_Input*,Controller_Xinput_Input*,
        Controller_XinputHid_Input*,Controller_GIP_Input*,Controller_GIP_Raw_Input*>;
    Payload payload_=static_cast<Controller_Hid_Input*>(nullptr);
    Detail::Automatic_State* owner_=nullptr;
    std::uint64_t batch_=0;
    std::size_t index_=0;
    bool sent_=false;
    friend struct Detail::Automatic_State;
public:
    Device_Type type=Device_Type::Controller_Hid;
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

namespace Interceptor {
// Uses installed packages. Elevated; no UI or automatic Windows restart.
Init_Result Init_AllDevices();
// One library-owned session, lazily opened on the first input operation.
// No installation, retry delay, timer or worker thread is started here.
Input_Batch Get_Input_With_Timeout(DWORD timeout=INFINITE,HANDLE stopEvent=nullptr);
// Independent snapshots; neither owns nor closes the internal capture.
Context_Info Get_Context();
std::vector<Device> Get_Devices();
// Optional explicit release. Finish the batch/reader before calling. A later
// input operation opens a new session; trial state is never reset.
void Destroy_Context();

#define IC_AUTOMATIC_INPUT(Name,Type) \
std::vector<Type> Get_##Name##_Input(std::uint32_t deviceId=0,bool consume=true)
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE
IC_AUTOMATIC_INPUT(Keyboard,Keyboard_Input);
IC_AUTOMATIC_INPUT(Mouse,Mouse_Input);
#endif
#if IC_PRODUCT_HAS_CONTROLLERS
IC_AUTOMATIC_INPUT(Controller_Hid,Controller_Hid_Input);
IC_AUTOMATIC_INPUT(Controller_Xinput,Controller_Xinput_Input);
IC_AUTOMATIC_INPUT(Controller_XinputHid,Controller_XinputHid_Input);
IC_AUTOMATIC_INPUT(Controller_GIP,Controller_GIP_Input);
IC_AUTOMATIC_INPUT(Controller_GIP_Raw,Controller_GIP_Raw_Input);
#endif
#undef IC_AUTOMATIC_INPUT
// Sending a batch again retries only its unaccepted, unblocked entries.
Send_Result Send_Input(Input_Batch& inputs);
Send_Result Send_Input(Input_Event& input);
// Resolve exact string tokens from Get_Context().devices / Get_Devices().
// HID data is Windows preparsed data, not a raw hardware report descriptor.
Descriptor_Result Get_descriptor(const std::string& token);
Report_Descriptor Get_Report_Descriptor(const std::string& token);

// Read-only queries do not create capture or activate the per-boot trial.
IC_STATUS Get_Status();
IC_CAPABILITIES Get_Capabilities();
IC_TRIAL_STATUS Get_Trial_Status();
// Exact token of an existing automatic capture. No capture is created here.
Device_Details Get_Device_Details(const std::string& token);
Device_Availability Get_Device_Availability(const std::string& token);
}

// ---- SendInput.h ----
namespace Interceptor {
namespace Detail {
// Internal type dispatch. No family selection or size is needed by callers.
#define IC_INFER_SEND(Type) Send_Result SendAuto(Input_View<const Type> inputs);
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE
IC_INFER_SEND(Keyboard_Input)
IC_INFER_SEND(Mouse_Input)
#endif
#if IC_PRODUCT_HAS_CONTROLLERS
IC_INFER_SEND(Controller_Hid_Input)
IC_INFER_SEND(Controller_Xinput_Input)
IC_INFER_SEND(Controller_XinputHid_Input)
IC_INFER_SEND(Controller_GIP_Input)
IC_INFER_SEND(Controller_GIP_Raw_Input)
#endif
#undef IC_INFER_SEND
}
template<class Inputs> auto Send_Input(const Inputs& inputs)
    -> decltype(Detail::SendAuto(Detail::Input_View(inputs))) {
    return Detail::SendAuto(Detail::Input_View(inputs));
}
}

#if defined(IC_SDK_INTERNAL)
// ---- ContextAPI.h ----
// Internal compatibility interface for installer/viewer workers and archived
// regressions. New application code includes only Interceptor.h.

// ---- WireInputs.h ----
#include <type_traits>
namespace Interceptor::Detail {
// Internal pure parser adapter for passive telemetry. No device is opened and
// no capture/automatic state is created by these functions.
std::shared_ptr<const Hid_Codec> Create_Hid_Decoder(std::vector<std::uint8_t> preparsed);
void Decode_Hid_Input(const std::shared_ptr<const Hid_Codec>& decoder,Hid_Decoded_Input& output,
                     const std::uint8_t* report,std::uint32_t length,std::uint32_t source);
struct Hid_Input_Access {
    static void Clear(Hid_Decoded_Input& output) {
        output.token.clear();output.state.axes.clear();output.state.triggers.clear();output.state.hats.clear();output.state.values.clear();output.state.buttons.clear();output.state.binary_values.clear();
        output.decode_error=ERROR_SUCCESS;output.parser_status=0;output.decode_complete=false;
        output.codec_.reset();output.report_id_=0;output.source_id_=0;
    }
};
template<class T> struct Wire_Input {using type=T;};
template<> struct Wire_Input<Controller_Hid_Input>{using type=IC_CONTROLLER_HID_INPUT;};
template<> struct Wire_Input<Controller_XinputHid_Input>{using type=IC_CONTROLLER_XINPUTHID_INPUT;};
template<class T> using Wire=typename Wire_Input<T>::type;
template<class T> inline constexpr bool DecodedHid=!std::is_same_v<T,Wire<T>>;
template<class T> void AssignWire(T& output,const Wire<T>& input) {
    if constexpr(DecodedHid<T>){static_cast<Wire<T>&>(output)=input;Hid_Input_Access::Clear(output);}
    else output=input;
}
template<class T> void AssignWire(std::vector<T>& output,const Wire<T>* input,std::size_t count) {
    if constexpr(!DecodedHid<T>) {
        // Bulk copy plain wire records without first value-initializing every
        // newly returned entry. Keep the small single-input path unchanged.
        if(count>1){output.assign(input,input+count);return;}
    }
    output.resize(count);for(std::size_t i=0;i<count;++i)AssignWire(output[i],input[i]);
}
}
#include <memory>
#include <stdexcept>
namespace Interceptor {
using Device_Info=IC_DEVICE_INFO;
namespace Detail {
struct Native_Device_Availability {
    IC_DEVICE_QUERY_RESULT details{};
    std::uint32_t state=IC_DEVICE_ABSENT,reasons=0;
    bool reported_by_driver=false;
};
}
class Context;
struct Context_Deleter {void operator()(Context*) const noexcept;};
using Context_Ptr=std::unique_ptr<Context,Context_Deleter>;
struct Context_Result {
    Context_Ptr context;
    std::vector<Device_Info> devices;
    std::uint64_t generation=0;
    Context& Get() const {if(!context)throw std::invalid_argument("Capture context is closed");return *context;}
    operator Context&() const {return Get();}
    Context& operator*() const {return Get();}
    Context* operator->() const noexcept{return context.get();}
    explicit operator bool() const noexcept{return static_cast<bool>(context);}
};
Context_Result Create_Context();
namespace Detail {bool Raw_Gip_Supported(const Context&) noexcept;}
std::vector<Device_Info> Get_Devices(Context&);
void Destroy_Context(Context_Ptr&) noexcept;
void Destroy_Context(Context_Result&) noexcept;
#define IC_EXPLICIT_INPUT(Name,Type) \
std::vector<Type> Get_##Name##_Input(Context&,std::uint32_t count=0); \
void Get_##Name##_Input(Context&,std::vector<Type>&,std::uint32_t count=0); \
std::uint32_t Get_##Name##_Input(Context&,Type*,std::uint32_t capacity,std::uint32_t count=0); \
Send_Result Send_Input(const Type*,std::size_t); \
Send_Result Send_Input(Context&,const Type*,std::size_t)
IC_EXPLICIT_INPUT(Keyboard,Keyboard_Input);
IC_EXPLICIT_INPUT(Mouse,Mouse_Input);
IC_EXPLICIT_INPUT(Controller_Hid,Controller_Hid_Input);
IC_EXPLICIT_INPUT(Controller_Xinput,Controller_Xinput_Input);
IC_EXPLICIT_INPUT(Controller_XinputHid,Controller_XinputHid_Input);
IC_EXPLICIT_INPUT(Controller_GIP,Controller_GIP_Input);
IC_EXPLICIT_INPUT(Controller_GIP_Raw,Controller_GIP_Raw_Input);
#undef IC_EXPLICIT_INPUT
std::vector<std::uint8_t> Get_Controller_Hid_Descriptor(Context&,std::uint32_t);
std::vector<std::uint8_t> Get_Controller_XinputHid_Descriptor(Context&,std::uint32_t);
std::uint8_t Wait_For_Input(Context&,DWORD timeout=INFINITE);
std::uint8_t Wait_For_Input(Context&,DWORD,HANDLE);
IC_STATUS Get_Status(Context*);
// Numeric query is private to installers, diagnostics and historical workers.
IC_DEVICE_QUERY_RESULT Get_Device_Details(std::uint32_t type=0,std::uint32_t instance_token=0,bool by_ordinal=false);
Detail::Native_Device_Availability Get_Device_Availability(std::uint32_t type,std::uint32_t instance_token,bool by_ordinal=false);
}

// ---- ContextHelpers.h ----
#include <algorithm>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>

namespace Interceptor {
// All seven send families accept a single input, vector, std::array, C array
// or Detail::Input_View. No allocation/copy and no separate count in the simple API.
#define IC_SIZED_INPUT(Name,Type,Limit) \
inline Send_Result Send_Input(Context& context,Detail::Input_View<const Type> inputs) {return Send_Input(context,inputs.data(),inputs.size());} \
inline Detail::Input_View<Type> Get_##Name##_Input(Context& context,Detail::Input_View<Type> storage) { \
    if(storage.empty())return storage; \
    const auto capacity=static_cast<std::uint32_t>(std::min<std::size_t>(storage.size(),Limit)); \
    return storage.first(Get_##Name##_Input(context,storage.data(),capacity,capacity)); \
}
IC_SIZED_INPUT(Keyboard,Keyboard_Input,IC_CAPACITY)
IC_SIZED_INPUT(Mouse,Mouse_Input,IC_CAPACITY)
IC_SIZED_INPUT(Controller_Hid,Controller_Hid_Input,IC_HID_CAPACITY)
IC_SIZED_INPUT(Controller_Xinput,Controller_Xinput_Input,IC_XUSB_CAPACITY)
IC_SIZED_INPUT(Controller_XinputHid,Controller_XinputHid_Input,IC_HID_CAPACITY)
IC_SIZED_INPUT(Controller_GIP,Controller_GIP_Input,IC_GIP_CAPACITY)
IC_SIZED_INPUT(Controller_GIP_Raw,Controller_GIP_Raw_Input,IC_GIP_CAPACITY)
#undef IC_SIZED_INPUT

inline std::vector<Controller_GIP_Raw_Input> Get_Controller_GIP_Raw_Input(Context& context,std::uint32_t count) {
    std::vector<Controller_GIP_Raw_Input> inputs;Get_Controller_GIP_Raw_Input(context,inputs,count);return inputs;
}

namespace Detail {
#define IC_AUTO_SEND(Name,Type) \
inline Send_Result SendAuto(Context& context,Detail::Input_View<const Type> inputs){return Send_Input(context,inputs);}
IC_AUTO_SEND(Keyboard,Keyboard_Input)
IC_AUTO_SEND(Mouse,Mouse_Input)
IC_AUTO_SEND(Controller_Hid,Controller_Hid_Input)
IC_AUTO_SEND(Controller_Xinput,Controller_Xinput_Input)
IC_AUTO_SEND(Controller_XinputHid,Controller_XinputHid_Input)
IC_AUTO_SEND(Controller_GIP,Controller_GIP_Input)
IC_AUTO_SEND(Controller_GIP_Raw,Controller_GIP_Raw_Input)
#undef IC_AUTO_SEND
}
// Input element type chooses the family; .device keeps the original target.

template<class Inputs> Send_Result Send_Input(Context& context,const Inputs& inputs) {return Detail::SendAuto(context,Detail::Input_View(inputs));}

inline IC_STATUS Get_Status(Context& context){return Get_Status(&context);}
inline IC_DEVICE_QUERY_RESULT Get_Device_Details(const Device_Info& d){return Get_Device_Details(d.type,d.instance_token);}
inline Detail::Native_Device_Availability Get_Device_Availability(const Device_Info& d){return Get_Device_Availability(d.type,d.instance_token);}
inline std::vector<std::uint8_t> Get_Controller_Descriptor(Context& context,const Device_Info& device) {
    if(device.type==IC_TYPE_CONTROLLER_HID)return Get_Controller_Hid_Descriptor(context,device.id);
    if(device.type==IC_TYPE_CONTROLLER_XINPUTHID)return Get_Controller_XinputHid_Descriptor(context,device.id);
    throw std::invalid_argument("This device has no HID descriptor API");
}


namespace Detail {
struct Input_Callbacks {
    void* user=nullptr;
    bool (*keyboard)(void*,Keyboard_Input&)=nullptr;
    bool (*mouse)(void*,Mouse_Input&)=nullptr;
    bool (*hid)(void*,Controller_Hid_Input&)=nullptr;
    bool (*xusb)(void*,Controller_Xinput_Input&)=nullptr;
    bool (*xinputhid)(void*,Controller_XinputHid_Input&)=nullptr;
    bool (*gip)(void*,Controller_GIP_Input&)=nullptr;
    bool (*gipRaw)(void*,Controller_GIP_Raw_Input&)=nullptr;
};
std::size_t ProcessInputs(Context& context,const Input_Callbacks& callbacks,DWORD timeout,HANDLE stop);
template<class Handler,class Input> bool HandleInput(Handler& handler,Input& input) {
    if constexpr(std::is_invocable_v<Handler&,Input&>) {
        using Result=std::invoke_result_t<Handler&,Input&>;
        static_assert(std::is_void_v<Result> || std::is_same_v<Result,bool>,"Input handler must return void (forward) or bool (false blocks)");
        if constexpr(std::is_void_v<Result>)return (std::invoke(handler,input),true);
        else return std::invoke(handler,input);
    } else if constexpr(std::is_same_v<Input,Controller_GIP_Raw_Input>) {
        // Standard GIP edits retain the complete physical extension bytes.
        return HandleInput(handler,input.input);
    } else return true;
}
template<class H,class T> bool InputThunk(void* user,T& input){return (*static_cast<H*>(user))(input);}
}
// One wait + batch: invoke the handler for compatible types, then forward all
// retained inputs. Unhandled families pass unchanged. false blocks one input;
// void/true forwards it. Processing failure closes the control handle before
// propagating the error; the owning Context_Result can then be destroyed.
// Storage is reused; FIFO order is preserved within each family.
template<class Handler,std::enable_if_t<!std::is_integral_v<std::remove_reference_t<Handler>>,int> =0>
std::size_t Process_Captured_Input(Context_Result& capture,Handler&& handler,DWORD timeout=INFINITE,HANDLE stop=nullptr) {
    using H=std::remove_reference_t<Handler>;
    static_assert(std::is_invocable_v<H&,Keyboard_Input&> || std::is_invocable_v<H&,Mouse_Input&> ||
        std::is_invocable_v<H&,Controller_Hid_Input&> || std::is_invocable_v<H&,Controller_Xinput_Input&> ||
        std::is_invocable_v<H&,Controller_XinputHid_Input&> || std::is_invocable_v<H&,Controller_GIP_Input&> ||
        std::is_invocable_v<H&,Controller_GIP_Raw_Input&>,"Handler must accept an Interceptor input by reference");
    auto invoke=[&handler](auto& input){return Detail::HandleInput(handler,input);};
    using F=decltype(invoke);
    const Detail::Input_Callbacks callbacks{&invoke,
        Detail::InputThunk<F,Keyboard_Input>,Detail::InputThunk<F,Mouse_Input>,
        Detail::InputThunk<F,Controller_Hid_Input>,Detail::InputThunk<F,Controller_Xinput_Input>,
        Detail::InputThunk<F,Controller_XinputHid_Input>,Detail::InputThunk<F,Controller_GIP_Input>,
        Detail::InputThunk<F,Controller_GIP_Raw_Input>};
    return Detail::ProcessInputs(capture.Get(),callbacks,timeout,stop);
}
inline std::size_t Process_Captured_Input(Context_Result& capture,DWORD timeout=INFINITE,HANDLE stop=nullptr) {
    return Process_Captured_Input(capture,[](auto&){},timeout,stop);
}
}

// ---- RelayInternal.h ----
#include <system_error>

// Shared by the shipped examples/diagnostic UI. Not an additional driver API.
namespace Interceptor { namespace Detail {
class Input_Relay {
    Context& context_;
    bool raw_;
    std::vector<Keyboard_Input> keys_;
    std::vector<Mouse_Input> mouse_inputs_;
    std::vector<Controller_Hid_Input> hid_;
    std::vector<Controller_Xinput_Input> xusb_;
    std::vector<Controller_XinputHid_Input> xih_;
    std::vector<Controller_GIP_Input> gip_;
    std::vector<Controller_GIP_Raw_Input> gipRaw_;
    template<class T,class Get,class Send> void Transfer(std::vector<T>& storage,Get get,Send send) {
        // Unused families occupy no heap storage. Reserve before reading so
        // an allocation failure cannot consume a physical report.
        if(storage.empty())storage.resize(256);
        const auto count=get(storage.data(),256);
        if(!count)return;
        const auto result=send(storage.data(),count);
        if(result.error || result.accepted!=count)
            throw std::system_error(result.error?result.error:ERROR_WRITE_FAULT,std::system_category(),"Forward untouched input families");
    }
public:
    explicit Input_Relay(Context& context):context_(context),raw_(Raw_Gip_Supported(context)){}
    void Forward(std::uint8_t ready) {
#define IC_RELAY(bit,buffer,Name) if(ready&(bit))Transfer(buffer, \
        [&](auto* p,std::uint32_t n){return Get_##Name##_Input(context_,p,n,n);}, \
        [&](const auto* p,std::size_t n){return Send_Input(context_,p,n);})
        IC_RELAY(Keyboard,keys_,Keyboard);
        IC_RELAY(Mouse,mouse_inputs_,Mouse);
        IC_RELAY(Ready_Controller_Hid,hid_,Controller_Hid);
        IC_RELAY(Ready_Controller_Xinput,xusb_,Controller_Xinput);
        IC_RELAY(Ready_Controller_XinputHid,xih_,Controller_XinputHid);
        if(raw_){IC_RELAY(Ready_Controller_GIP,gipRaw_,Controller_GIP_Raw);}
        else {IC_RELAY(Ready_Controller_GIP,gip_,Controller_GIP);}
#undef IC_RELAY
    }
};
}}

// ---- HidProfileProbe.h ----
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidclass.h>
#include <vector>
#include <cwchar>

// ---- internal/HidMapperProfile.h ----
/* Shared by the HID mapper and the user-mode association preflight. */
#define IC_MAPPER_DESCRIPTOR_MAX 65536u
#define IC_MAPPER_REPORT_MAX 4096u
#define IC_MAPPER_VALUE_CAPS_MAX 512u
typedef enum IC_MAPPER_PROFILE_RESULT {
    IC_MAPPER_PROFILE_OK=0,
    IC_MAPPER_PROFILE_DESCRIPTOR_SIZE,
    IC_MAPPER_PROFILE_USAGE,
    IC_MAPPER_PROFILE_INPUT_SIZE,
    IC_MAPPER_PROFILE_VALUE_CAPS
} IC_MAPPER_PROFILE_RESULT;
/* expectedUsage=0 accepts either mapper; 2 means mouse, 6 means keyboard. */
static __inline IC_MAPPER_PROFILE_RESULT IcMapperProfile(unsigned long descriptorBytes,
    unsigned short usagePage,unsigned short usage,unsigned short inputBytes,
    unsigned short valueCaps,unsigned short expectedUsage) {
    if(!descriptorBytes || descriptorBytes>IC_MAPPER_DESCRIPTOR_MAX)return IC_MAPPER_PROFILE_DESCRIPTOR_SIZE;
    if(usagePage!=1 || (usage!=2 && usage!=6) || (expectedUsage && usage!=expectedUsage))return IC_MAPPER_PROFILE_USAGE;
    if(!inputBytes || inputBytes>IC_MAPPER_REPORT_MAX)return IC_MAPPER_PROFILE_INPUT_SIZE;
    if(valueCaps>IC_MAPPER_VALUE_CAPS_MAX)return IC_MAPPER_PROFILE_VALUE_CAPS;
    return IC_MAPPER_PROFILE_OK;
}
#pragma comment(lib,"hid.lib")
#pragma comment(lib,"setupapi.lib")

namespace Interceptor::SetupDetail {
struct MapperProfileProbe {
    bool supported=false;
    IC_MAPPER_PROFILE_RESULT profile=IC_MAPPER_PROFILE_OK;
    DWORD error=ERROR_SUCCESS;
    const wchar_t* reason=L"HID profile could not be inspected; association skipped";
};
struct HidProfileApi {
    decltype(&HidD_GetHidGuid) hidGuid=HidD_GetHidGuid;
    decltype(&SetupDiGetDeviceInstanceIdW) instance=SetupDiGetDeviceInstanceIdW;
    decltype(&SetupDiGetClassDevsW) devices=SetupDiGetClassDevsW;
    decltype(&SetupDiDestroyDeviceInfoList) destroy=SetupDiDestroyDeviceInfoList;
    decltype(&SetupDiEnumDeviceInterfaces) enumerate=SetupDiEnumDeviceInterfaces;
    decltype(&SetupDiGetDeviceInterfaceDetailW) detail=SetupDiGetDeviceInterfaceDetailW;
    decltype(&CreateFileW) open=CreateFileW;
    decltype(&CloseHandle) close=CloseHandle;
    decltype(&DeviceIoControl) ioctl=DeviceIoControl;
    decltype(&HidD_GetPreparsedData) preparsed=HidD_GetPreparsedData;
    decltype(&HidD_FreePreparsedData) freePreparsed=HidD_FreePreparsedData;
    decltype(&HidP_GetCaps) caps=HidP_GetCaps;
};
inline const wchar_t* MapperProfileReason(IC_MAPPER_PROFILE_RESULT value) {
    switch(value) {
    case IC_MAPPER_PROFILE_DESCRIPTOR_SIZE:return L"HID preparsed descriptor exceeds the mapper profile";
    case IC_MAPPER_PROFILE_USAGE:return L"HID collection usage does not match this mapper";
    case IC_MAPPER_PROFILE_INPUT_SIZE:return L"HID input report length exceeds the mapper profile";
    case IC_MAPPER_PROFILE_VALUE_CAPS:return L"HID input value capabilities exceed the mapper profile";
    default:return L"HID mapper profile accepted";
    }
}
inline MapperProfileProbe ProbeHidProfile(HDEVINFO set,SP_DEVINFO_DATA& device,bool mouse,const HidProfileApi& api,unsigned controllerProfiles) {
    MapperProfileProbe result;
    auto failed=[&](DWORD error){result.error=error?error:ERROR_INVALID_DATA;return result;};
    wchar_t expected[MAX_DEVICE_ID_LEN]{};
    if(!api.instance(set,&device,expected,_countof(expected),nullptr))return failed(GetLastError());
    GUID guid{};api.hidGuid(&guid);
    struct Set {const HidProfileApi& api;HDEVINFO value;~Set(){if(value!=INVALID_HANDLE_VALUE)api.destroy(value);}} interfaces{api,api.devices(&guid,expected,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
    if(interfaces.value==INVALID_HANDLE_VALUE)return failed(GetLastError());
    SP_DEVICE_INTERFACE_DATA entry{};entry.cbSize=sizeof(entry);
    if(!api.enumerate(interfaces.value,nullptr,&guid,0,&entry))return failed(GetLastError());
    DWORD bytes=0;
    if(api.detail(interfaces.value,&entry,nullptr,0,&bytes,nullptr) || GetLastError()!=ERROR_INSUFFICIENT_BUFFER || bytes<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || bytes>1024*1024)
        return failed(ERROR_INVALID_DATA);
    std::vector<unsigned char> storage(bytes,0);
    auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());detail->cbSize=sizeof(*detail);
    SP_DEVINFO_DATA actual{};actual.cbSize=sizeof(actual);
    if(!api.detail(interfaces.value,&entry,detail,bytes,nullptr,&actual))return failed(GetLastError());
    wchar_t found[MAX_DEVICE_ID_LEN]{};
    if(!api.instance(interfaces.value,&actual,found,_countof(found),nullptr))return failed(GetLastError());
    if(actual.DevInst!=device.DevInst || _wcsicmp(found,expected))return failed(ERROR_RETRY);
    // Metadata access works for the exclusive Windows keyboard/mouse consumer.
    // Never request GENERIC_READ/WRITE or submit an input report here.
    struct Handle {const HidProfileApi& api;HANDLE value;~Handle(){if(value!=INVALID_HANDLE_VALUE)api.close(value);}} handle{api,api.open(detail->DevicePath,0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr)};
    if(handle.value==INVALID_HANDLE_VALUE)return failed(GetLastError());
    HID_COLLECTION_INFORMATION information{};DWORD returned=0;
    if(!api.ioctl(handle.value,IOCTL_HID_GET_COLLECTION_INFORMATION,nullptr,0,&information,sizeof(information),&returned,nullptr))return failed(GetLastError());
    if(returned!=sizeof(information))return failed(ERROR_INVALID_DATA);
    if(!information.DescriptorSize || information.DescriptorSize>IC_MAPPER_DESCRIPTOR_MAX) {
        result.profile=IC_MAPPER_PROFILE_DESCRIPTOR_SIZE;result.reason=controllerProfiles?L"HID controller descriptor exceeds the supported bounds":MapperProfileReason(result.profile);return result;
    }
    struct Data {const HidProfileApi& api;PHIDP_PREPARSED_DATA value=nullptr;~Data(){if(value)api.freePreparsed(value);}} data{api};
    if(!api.preparsed(handle.value,&data.value))return failed(GetLastError());
    HIDP_CAPS caps{};
    if(api.caps(data.value,&caps)!=HIDP_STATUS_SUCCESS)return failed(ERROR_INVALID_DATA);
    // Revalidate the devnode identity after metadata reads, before callers bind.
    wchar_t current[MAX_DEVICE_ID_LEN]{};
    if(!api.instance(set,&device,current,_countof(current),nullptr))return failed(GetLastError());
    if(_wcsicmp(current,expected))return failed(ERROR_RETRY);
    if(controllerProfiles) {
        if(!(IcHidGameProfile(caps.UsagePage,caps.Usage)&controllerProfiles))result.profile=IC_MAPPER_PROFILE_USAGE;
        else if(!caps.InputReportByteLength || caps.InputReportByteLength>IC_HID_REPORT_MAX)result.profile=IC_MAPPER_PROFILE_INPUT_SIZE;
        else if(caps.NumberInputValueCaps>1024 || caps.NumberInputButtonCaps>1024 ||
                (!caps.NumberInputValueCaps && !caps.NumberInputButtonCaps))result.profile=IC_MAPPER_PROFILE_VALUE_CAPS;
    } else result.profile=IcMapperProfile(information.DescriptorSize,caps.UsagePage,caps.Usage,caps.InputReportByteLength,caps.NumberInputValueCaps,mouse?2:6);
    result.supported=result.profile==IC_MAPPER_PROFILE_OK;
    result.reason=controllerProfiles?(result.supported?L"HID controller descriptor accepted":L"HID controller descriptor is outside the loaded driver's profiles"):MapperProfileReason(result.profile);
    return result;
}
inline MapperProfileProbe ProbeMapperProfile(HDEVINFO set,SP_DEVINFO_DATA& device,bool mouse,const HidProfileApi& api={}) {
    return ProbeHidProfile(set,device,mouse,api,0);
}
inline MapperProfileProbe ProbeControllerProfile(HDEVINFO set,SP_DEVINFO_DATA& device,unsigned profiles,const HidProfileApi& api={}) {
    if(!profiles)return {};
    return ProbeHidProfile(set,device,false,api,profiles);
}
}

// ---- internal/ControllerGipDiagnostics.h ----

// Experimental laboratory diagnostics through the administrator-only control
// device. IC_GET.count is a 1-based registered GIP device ordinal (0 = first).
#define IOCTL_IC_GIP_DIAGNOSTICS IC_IOCTL(0xA25)
#define IC_GIP_DIAGNOSTIC_VERSION 1u
#define IC_GIP_TRACE_CAPACITY 32u
#define IC_GIP_ORIGIN_PROTOCOL 0u
#define IC_GIP_ORIGIN_PHYSICAL 1u
#define IC_GIP_ORIGIN_SEND 2u
#define IC_GIP_ORIGIN_RESTORE 3u
typedef struct IC_GIP_TRACE {
    uint64_t ordinal,timestamp;
    uint32_t origin,length;
    uint8_t bytes[64];
} IC_GIP_TRACE;
typedef struct IC_GIP_DIAGNOSTICS {
    uint32_t version,flags,capture_id,output_count;
    uint64_t physical_reports,send_attempts,reset_calls,restore_attempts,restore_queued;
    uint32_t last_read_status,last_usb_status,trace_count,reserved;
    IC_NATIVE_GAMEPAD_STATE physical,scheduled;
    uint8_t last_input_sequence,reserved_bytes[7];
    IC_GIP_TRACE trace[IC_GIP_TRACE_CAPACITY];
} IC_GIP_DIAGNOSTICS;
// flags: ready=1, running=2, powered=4, valid=8, configured=16,
// restorePending=32, unsupported=64, helloValidated=128.
// metadataReceiving=256, metadataComplete=512, metadataGamepad14=1024.
#if defined(__cplusplus)
static_assert(sizeof(IC_GIP_TRACE)==88,"GIP trace ABI");
static_assert(sizeof(IC_GIP_DIAGNOSTICS)==2936,"GIP diagnostics ABI");
#endif

// Advanced installer entry point; caller must be elevated and serialize setup operations.
namespace Interceptor {Init_Result Restore_Controller_Hid_Filters();}

#endif // IC_SDK_INTERNAL
