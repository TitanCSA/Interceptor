// Deterministic user-mode fixture only. This translation unit never opens a driver.
#include "ProductSDK.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
namespace {
bool AllowedKind(unsigned kind) {
    if(kind==1 || kind==2) return (IC_PRODUCT_INPUT_MASK & (1u<<(kind-1))) != 0;
    if(kind>=3 && kind<=6) return (IC_PRODUCT_CONTROLLER_MASK & (1u<<(kind-3))) != 0;
    return false;
}
std::atomic<unsigned> mode{0}, calls{0};
std::atomic<long long> seen[8][8];
} // namespace
extern "C" __declspec(dllexport) void interceptor_test_mode(unsigned value) {
    mode = value;
    calls = 0;
}
extern "C" __declspec(dllexport) long long interceptor_test_seen(unsigned family, unsigned field) {
    return family < 8 && field < 8 ? seen[family][field].load() : 0;
}
namespace Interceptor {
namespace Detail {
struct Automatic_State {
    static Input_Batch MakeBatch() {
        static Keyboard_Input key;
        static Mouse_Input mouse;
        static Controller_Hid_Input hid;
        static Controller_Xinput_Input xusb;
        static Controller_XinputHid_Input xhid;
        static Controller_GIP_Raw_Input gip;
        static Device devices[6];
        static Input_Event events[6];
        key = {};
        mouse = {};
        hid = {};
        xusb = {};
        xhid = {};
        gip = {};
        key.device = mouse.device = hid.device = xusb.device = xhid.device = gip.input.device = 1;
        key.sequence = 0x8000000000000001ull;
        key.timestamp = 1234;
        key.stroke.make_code = 0x1e;
        mouse.stroke.x = 3;
        mouse.stroke.y = 7;
        for (auto *h :
             {static_cast<Hid_Decoded_Input *>(&hid), static_cast<Hid_Decoded_Input *>(&xhid)}) {
            h->decode_complete = true;
            h->token = h == &hid ? "fixture:3" : "fixture:5";
            Hid_Value a;
            a.usage_page = 1;
            a.usage = 0x30;
            a.link_collection = 1;
            a.data_index = 4;
            a.value = 25;
            a.minimum = -100;
            a.maximum = 100;
            a.normalized_valid = true;
            a.normalized = .25;
            a.kind = Hid_Value_Kind::Axis;
            h->state.axes.push_back(a);
            a.link_collection = 2;
            a.data_index = 5;
            a.value = -10;
            a.normalized = -.1;
            h->state.axes.push_back(a);
            Hid_Value hat;
            hat.kind = Hid_Value_Kind::Hat;
            hat.usage_page = 1;
            hat.usage = 0x39;
            hat.minimum = 0;
            hat.maximum = 7;
            hat.value = 8;
            hat.has_null = true;
            hat.is_null = true;
            h->state.hats.push_back(hat);
            Hid_Button b;
            b.usage_page = 9;
            b.usage = 1;
            b.pressed = true;
            h->state.buttons.push_back(b);
            Hid_Binary_Value binary;
            binary.usage_page = 0xff00;
            binary.usage = 1;
            binary.bit_size = 64;
            binary.count = 1;
            binary.data = {1, 2, 3, 4, 5, 6, 7, 8};
            h->state.binary_values.push_back(binary);
        }
        hid.length = xhid.length = 4;
        hid.report[0] = xhid.report[0] = 1;
        hid.report[1] = xhid.report[1] = 25;
        xusb.state.left_x = 100;
        gip.input.state.left_x = 200;
        gip.report_length = 22;
        gip.report[0] = 0x20;
        for (unsigned i = 0; i < 6; ++i) {
            devices[i] = {};
            devices[i].id = 1;
            devices[i].type = i + 1;
            devices[i].vid = 0xffff;
            devices[i].pid = static_cast<uint16_t>(0xb100 + i);
            devices[i].token = "fixture:" + std::to_string(i + 1);
            events[i] = {};
            events[i].type = static_cast<Device_Type>(i + 1);
            events[i].device = 1;
            events[i].sequence = key.sequence + i;
            events[i].timestamp = 1234 + i;
            events[i].device_info = &devices[i];
        }
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE
        events[0].payload_ = &key;
        events[1].payload_ = &mouse;
#endif
#if IC_PRODUCT_HAS_CONTROLLERS
        events[2].payload_ = &hid;
        events[3].payload_ = &xusb;
        events[4].payload_ = &xhid;
        events[5].payload_ = &gip;
#endif
        Input_Batch result;
        static Input_Event filtered[6];unsigned count=0;for(unsigned n=0;n<6;++n)if(AllowedKind(n+1))filtered[count++]=events[n];
        result.inputs_ = {filtered, count};
        return result;
    }
};
} // namespace Detail
Input_Batch::~Input_Batch() = default;
Input_Batch::Input_Batch(Input_Batch &&other) noexcept : inputs_(other.inputs_) {
    other.inputs_ = {};
}
Input_Batch &Input_Batch::operator=(Input_Batch &&other) noexcept {
    inputs_ = other.inputs_;
    other.inputs_ = {};
    return *this;
}
Input_Batch Get_Input_With_Timeout(DWORD timeout, HANDLE stop) {
    if (mode == 1) {
        auto wait = WaitForSingleObject(stop, timeout);
        if (wait == WAIT_FAILED)
            throw std::system_error(GetLastError(), std::system_category());
        return {};
    }
    if (mode == 3)
        throw std::system_error(ERROR_EVALUATION_EXPIRATION, std::system_category());
    return Detail::Automatic_State::MakeBatch();
}
void Destroy_Context() {}
Init_Result Init_AllDevices() {
    Init_Result r;
    r.ready = true;
    r.already_associated = 6;
    return r;
}
std::vector<Device> Get_Devices() {
    std::vector<Device> result;
    for (unsigned i = 1; i <= 6; ++i) {
        if(!AllowedKind(i))continue;
        Device d;
        d.id = 1;
        d.type = i;
        d.vid = 0xffff;
        d.pid = static_cast<uint16_t>(0xb100 + i);
        d.token = "fixture:" + std::to_string(i);
        const wchar_t *name = L"Fixture \u00e9 \U0001f3ae";
        memcpy(d.name, name, (wcslen(name) + 1) * 2);
        result.push_back(d);
    }
    return result;
}
IC_STATUS Get_Status() {
    IC_STATUS s{};
    s.version = 4;
    s.active_mask = 63;
    s.keyboard_devices = s.mouse_devices = s.controller_hid_devices = 1;
    s.timestamp_frequency = 10000000;
    return s;
}
IC_CAPABILITIES Get_Capabilities() {
    IC_CAPABILITIES c{};
    c.version = 1;
    c.input_mask = IC_PRODUCT_INPUT_MASK;
    c.controller_mask = IC_PRODUCT_CONTROLLER_MASK;
    return c;
}
IC_TRIAL_STATUS Get_Trial_Status() {
    IC_TRIAL_STATUS t{};
    t.state = 1;
    t.remaining_100ns = 36000000000ull;
    return t;
}
Descriptor_Result Get_descriptor(const std::string &token) {
    Descriptor_Result d;
    d.device.token = token;
    d.data = {1, 2, 3, 4};
    return d;
}
Report_Descriptor Get_Report_Descriptor(const std::string &token) {
    Report_Descriptor d;
    d.applicable = token != "fixture:4" && token != "fixture:6";
    if (d.applicable)
        d.data = {0x05, 1, 0x09, 5};
    return d;
}
template <class T> Send_Result Sent(Detail::Input_View<const T> inputs, unsigned family) {
    if (mode == 2 && ++calls == 2)
        return {0, ERROR_BUSY};
    for (auto &i : inputs) {
        seen[family][0]++;
        if constexpr (std::is_same_v<T, Keyboard_Input>)
            seen[family][1] = i.stroke.make_code;
        else if constexpr (std::is_same_v<T, Mouse_Input>)
            seen[family][1] = i.stroke.y;
        else if constexpr (std::is_same_v<T, Controller_Hid_Input> ||
                           std::is_same_v<T, Controller_XinputHid_Input>) {
            if (!i.state.axes.empty()) {
                seen[family][1] = i.state.axes[0].value;
                seen[family][2] = static_cast<long long>(i.state.axes[0].normalized * 1000);
            }
            if (!i.state.buttons.empty())
                seen[family][3] = i.state.buttons[0].pressed;
        } else if constexpr (std::is_same_v<T, Controller_GIP_Raw_Input>)
            seen[family][1] = i.input.state.left_x;
        else
            seen[family][1] = i.state.left_x;
    }
    return {inputs.size(), 0};
}
namespace Detail {
Send_Result SendAuto(Input_View<const Keyboard_Input> v) {
    return Sent(v, 1);
}
Send_Result SendAuto(Input_View<const Mouse_Input> v) {
    return Sent(v, 2);
}
Send_Result SendAuto(Input_View<const Controller_Hid_Input> v) {
    return Sent(v, 3);
}
Send_Result SendAuto(Input_View<const Controller_Xinput_Input> v) {
    return Sent(v, 4);
}
Send_Result SendAuto(Input_View<const Controller_XinputHid_Input> v) {
    return Sent(v, 5);
}
Send_Result SendAuto(Input_View<const Controller_GIP_Input> v) {
    return Sent(v, 6);
}
Send_Result SendAuto(Input_View<const Controller_GIP_Raw_Input> v) {
    return Sent(v, 6);
}
} // namespace Detail
} // namespace Interceptor
