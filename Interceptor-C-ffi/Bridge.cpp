#include "ProductSDK.h"
#include "InterceptorBridge.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <vector>
using namespace Interceptor;
namespace {
bool AllowedKind(unsigned kind) {
    if(kind==1 || kind==2) return (IC_PRODUCT_INPUT_MASK & (1u<<(kind-1))) != 0;
    if(kind>=3 && kind<=6) return (IC_PRODUCT_CONTROLLER_MASK & (1u<<(kind-3))) != 0;
    return false;
}
std::string Utf8(const wchar_t *p) {
    if (!p || !*p)
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        throw std::runtime_error("Invalid UTF-16 metadata");
    std::string s(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, p, -1, s.data(), n, nullptr, nullptr);
    s.pop_back();
    return s;
}
uint32_t Error(interceptor_error *out, DWORD code, const char *action, uint32_t accepted = 0) noexcept {
    if (out) {
        *out = {};
        out->code = code;
        out->accepted = accepted;
        if (code) {
            char text[512]{};
            FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                           code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), text, sizeof(text),
                           nullptr);
            _snprintf_s(out->message, sizeof(out->message), _TRUNCATE,
                        "%s: %s (Windows error %lu).%s", action, *text ? text : "Operation failed",
                        static_cast<unsigned long>(code),
                        (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                            ? " Install the matching Interceptor driver with Install.cmd first."
                            : "");
        }
    }
    return code;
}
[[noreturn]] void Fail(DWORD code) {
    throw std::system_error(static_cast<int>(code), std::system_category());
}
template <class F> uint32_t Guard(interceptor_error *error, const char *action, F &&f) noexcept {
    if (error)
        *error = {};
    try {
        return f();
    } catch (const std::system_error &e) {
        return Error(error, static_cast<DWORD>(e.code().value()), action);
    } catch (const std::bad_alloc &) {
        return Error(error, ERROR_NOT_ENOUGH_MEMORY, action);
    } catch (const std::invalid_argument &) {
        return Error(error, ERROR_INVALID_PARAMETER, action);
    } catch (...) {
        return Error(error, ERROR_GEN_FAILURE, action);
    }
}
using Store = std::tuple<std::vector<Keyboard_Input>, std::vector<Mouse_Input>,
                         std::vector<Controller_Hid_Input>, std::vector<Controller_Xinput_Input>,
                         std::vector<Controller_XinputHid_Input>, std::vector<Controller_GIP_Input>,
                         std::vector<Controller_GIP_Raw_Input>>;
struct Ref {
    uint32_t store, index;
    bool sent = false;
};
struct Edit {
    void *value = nullptr;
    double *normalized = nullptr;
    uint32_t number = 0;
    int64_t min = 0, max = 0, original = 0;
    uint8_t *data = nullptr;
    uint32_t length = 0;
    bool readOnly = false;
};
enum { BOOL_VALUE = 1, U8_VALUE, U16_VALUE, I16_VALUE, U32_VALUE, I32_VALUE, I64_VALUE };
template <class T> constexpr uint32_t Number() {
    if constexpr (std::is_same_v<T, bool>)
        return BOOL_VALUE;
    else if constexpr (std::is_same_v<T, uint8_t>)
        return U8_VALUE;
    else if constexpr (std::is_same_v<T, uint16_t>)
        return U16_VALUE;
    else if constexpr (std::is_same_v<T, int16_t>)
        return I16_VALUE;
    else if constexpr (std::is_same_v<T, uint32_t>)
        return U32_VALUE;
    else if constexpr (std::is_same_v<T, int32_t>)
        return I32_VALUE;
    else
        return I64_VALUE;
}
struct Session {
    HANDLE stop = nullptr;
    std::unique_ptr<Session> injection;
    bool hasCapture = false;
    std::mutex mutex;
    std::atomic<bool> closing{false};
    Store store;
    uint32_t used[7]{};
    std::vector<Ref> refs;
    std::vector<std::string> tokens;
    std::vector<interceptor_event> events;
    std::vector<interceptor_field> fields;
    std::vector<Edit> edits;
    std::vector<uint32_t> offsets;
    uint64_t serial = 0;
    interceptor_batch batch{};
    bool pending = false;
    std::vector<Device> deviceSource;
    std::vector<std::string> names, hardware;
    std::vector<interceptor_device> devices;
    interceptor_devices view{};
    std::vector<uint8_t> blob;
    Session(bool makeStop = true) {
        if (makeStop) {
            stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!stop)
                Fail(GetLastError());
        }
    }
    ~Session() {
        if (stop)
            CloseHandle(stop);
    }
    template <class T> void Capture(const Input_Event &e, uint32_t which) {
        auto &v = std::get<std::vector<T>>(store);
        auto i = used[which]++;
        if (i == v.size())
            v.emplace_back();
        v[i] = *e.Get<T>();
        refs.push_back({which, i, false});
    }
    template <class T>
    void Scalar(const char *name, T &value, uint32_t group = 0, uint32_t index = 0) {
        interceptor_field f{};
        f.name = name;
        f.value = static_cast<int64_t>(value);
        f.minimum = static_cast<int64_t>(std::numeric_limits<T>::lowest());
        f.maximum = static_cast<int64_t>(std::numeric_limits<T>::max());
        f.group = group;
        f.index = index;
        fields.push_back(f);
        edits.push_back(
            {&value, nullptr, Number<T>(), f.minimum, f.maximum, f.value, nullptr, 0, false});
    }
    void Signed16(const char *name, uint16_t &value) {
        int16_t n = 0;
        memcpy(&n, &value, 2);
        Scalar(name, n);
        edits.back().value = &value;
    }
    void Bytes(const char *name, uint8_t *data, uint32_t length, uint32_t group,
               uint32_t index = 0) {
        interceptor_field f{};
        f.name = name;
        f.data = data;
        f.length = length;
        f.group = group;
        f.index = index;
        fields.push_back(f);
        Edit e{};
        e.data = data;
        e.length = length;
        edits.push_back(e);
    }
    void Gamepad(IC_NATIVE_GAMEPAD_STATE &s, bool gip) {
        Scalar("packet", s.packet);
        fields.back().flags |= INTERCEPTOR_READ_ONLY;
        edits.back().readOnly = true;
        Scalar("buttons", s.buttons);
        Scalar("left_trigger", s.left_trigger);
        fields.back().maximum = edits.back().max = gip ? 1023 : 255;
        Scalar("right_trigger", s.right_trigger);
        fields.back().maximum = edits.back().max = gip ? 1023 : 255;
        Scalar("left_x", s.left_x);
        Scalar("left_y", s.left_y);
        Scalar("right_x", s.right_x);
        Scalar("right_y", s.right_y);
        Scalar("extra_buttons", s.extra_buttons);
    }
    void HidValue(std::vector<Hid_Value> &values, uint32_t group) {
        for (uint32_t i = 0; i < values.size(); ++i) {
            auto &v = values[i];
            interceptor_field f{};
            f.name = "value";
            f.value = v.value;
            f.minimum = v.minimum;
            f.maximum = v.maximum;
            f.physical_minimum = v.physical_minimum;
            f.physical_maximum = v.physical_maximum;
            f.normalized = v.normalized;
            f.group = group;
            f.index = i;
            f.usage_page = v.usage_page;
            f.usage = v.usage;
            f.collection = v.link_collection;
            f.report_id = v.report_id;
            f.data_index = v.data_index;
            f.element = v.element;
            f.flags = (v.normalized_valid ? INTERCEPTOR_NORMALIZED : 0u) |
                      (v.relative ? INTERCEPTOR_RELATIVE : 0u) | (v.has_null ? INTERCEPTOR_HAS_NULL : 0u) |
                      (v.is_null ? INTERCEPTOR_IS_NULL : 0u);
            f.units = v.units;
            f.units_exponent = v.units_exponent;
            f.bit_size = v.bit_size;
            fields.push_back(f);
            edits.push_back({&v.value, v.normalized_valid ? &v.normalized : nullptr, I64_VALUE,
                             v.minimum, v.maximum, v.value, nullptr, 0, false});
        }
    }
    template <class T> void Hid(T &h, interceptor_event &e) {
        e.decode_error = h.decode_error;
        e.decode_complete = h.decode_complete;
        e.parser_status = h.parser_status;
        HidValue(h.state.axes, INTERCEPTOR_AXIS);
        HidValue(h.state.triggers, INTERCEPTOR_TRIGGER);
        HidValue(h.state.hats, INTERCEPTOR_HAT);
        HidValue(h.state.values, INTERCEPTOR_VALUE);
        for (uint32_t i = 0; i < h.state.buttons.size(); ++i) {
            auto &b = h.state.buttons[i];
            Scalar("pressed", b.pressed, INTERCEPTOR_BUTTON, i);
            auto &f = fields.back();
            f.usage_page = b.usage_page;
            f.usage = b.usage;
            f.collection = b.link_collection;
            f.data_index = b.data_index;
            f.report_id = b.report_id;
            if (b.trigger)
                f.flags |= INTERCEPTOR_IS_TRIGGER;
        }
        for (uint32_t i = 0; i < h.state.binary_values.size(); ++i) {
            auto &b = h.state.binary_values[i];
            Bytes("data", b.data.data(), static_cast<uint32_t>(b.data.size()), INTERCEPTOR_BINARY, i);
            auto &f = fields.back();
            f.usage_page = b.usage_page;
            f.usage = b.usage;
            f.collection = b.link_collection;
            f.report_id = b.report_id;
            f.bit_size = b.bit_size;
            f.element = b.count;
        }
        Bytes("report", h.report, h.length, INTERCEPTOR_REPORT);
    }
    void Describe(uint32_t n) {
        auto &r = refs[n];
        auto &e = events[n];
        offsets.push_back(static_cast<uint32_t>(fields.size()));
        switch (r.store) {
        case 0: {
            auto &s = std::get<0>(store)[r.index].stroke;
            Scalar("scan_code", s.make_code);
            fields.back().maximum = edits.back().max = 255;
            Scalar("flags", s.flags);
            Scalar("information", s.extra_information);
            break;
        }
        case 1: {
            auto &s = std::get<1>(store)[r.index].stroke;
            Scalar("x", s.x);
            Scalar("y", s.y);
            Scalar("flags", s.flags);
            Scalar("buttons", s.button_flags);
            Signed16("wheel", s.button_data);
            Scalar("raw_buttons", s.raw_buttons);
            Scalar("information", s.extra_information);
            break;
        }
        case 2:
            Hid(std::get<2>(store)[r.index], e);
            break;
        case 3:
            Gamepad(std::get<3>(store)[r.index].state, false);
            break;
        case 4:
            Hid(std::get<4>(store)[r.index], e);
            break;
        case 5:
            Gamepad(std::get<5>(store)[r.index].state, true);
            break;
        case 6: {
            auto &g = std::get<6>(store)[r.index];
            Gamepad(g.input.state, true);
            Bytes("report", g.report, g.report_length, INTERCEPTOR_REPORT);
            break;
        }
        default:
            Fail(ERROR_INVALID_DATA);
        }
        e.field_count = static_cast<uint32_t>(fields.size()) - offsets.back();
    }
    void Apply() {
        for (size_t event = 0; event < events.size(); ++event) {
            if (events[event].blocked || refs[event].sent)
                continue;
            for (size_t i = offsets[event]; i < offsets[event] + events[event].field_count; ++i) {
                auto &f = fields[i];
                auto &e = edits[i];
                if (f.data != e.data || f.length != e.length)
                    Fail(ERROR_INVALID_DATA);
                if (!e.value)
                    continue;
                if (e.readOnly && f.value != e.original)
                    Fail(ERROR_INVALID_PARAMETER);
                if (f.value != e.original && (f.value < e.min || f.value > e.max))
                    Fail(ERROR_INVALID_PARAMETER);
                if (e.normalized) {
                    if (!std::isfinite(f.normalized) || f.normalized < -1 || f.normalized > 1)
                        Fail(ERROR_INVALID_PARAMETER);
                    *e.normalized = f.normalized;
                }
#define PUT(code, T)                                                                               \
    case code: {                                                                                   \
        T v = static_cast<T>(f.value);                                                             \
        memcpy(e.value, &v, sizeof(v));                                                            \
        break;                                                                                     \
    }
                switch (e.number) {
                    PUT(BOOL_VALUE, bool);
                    PUT(U8_VALUE, uint8_t);
                    PUT(U16_VALUE, uint16_t);
                    PUT(I16_VALUE, int16_t);
                    PUT(U32_VALUE, uint32_t);
                    PUT(I32_VALUE, int32_t);
                    PUT(I64_VALUE, int64_t);
                default:
                    Fail(ERROR_INVALID_DATA);
                }
#undef PUT
            }
        }
    }
    Send_Result SendRun(const Ref &r, uint32_t count) {
        switch (r.store) {
#define RUN(i)                                                                                     \
    case i: {                                                                                      \
        auto &v = std::get<i>(store);                                                              \
        return Send_Input(                                                                         \
            Detail::Input_View<const typename std::remove_reference_t<decltype(v)>::value_type>(   \
                v.data() + r.index, count));                                                       \
    }
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE
            RUN(0);
            RUN(1);
#endif
#if IC_PRODUCT_HAS_CONTROLLERS
            RUN(2);
            RUN(3);
            RUN(4);
            RUN(5);
            RUN(6);
#endif
#undef RUN
        default:
            return {0, ERROR_INVALID_DATA};
        }
    }
    void Snapshot() {
        deviceSource = Get_Devices();
        hasCapture = true;
        names.resize(deviceSource.size());
        hardware.resize(deviceSource.size());
        devices.resize(deviceSource.size());
        for (size_t i = 0; i < deviceSource.size(); ++i) {
            auto &d = deviceSource[i];
            names[i] = Utf8(reinterpret_cast<const wchar_t *>(d.name));
            hardware[i] = Utf8(reinterpret_cast<const wchar_t *>(d.hardware_id));
            devices[i] = {d.type,
                          d.id,
                          d.vid,
                          d.pid,
                          d.usage_page,
                          d.usage,
                          d.token.c_str(),
                          names[i].c_str(),
                          hardware[i].c_str()};
        }
        view = {static_cast<uint32_t>(devices.size()), 0, devices.data()};
    }
};
std::mutex registry;
std::shared_ptr<Session> current;
uint64_t currentId = 0, nextId = 0;
std::shared_ptr<Session> Find(uint64_t id) {
    std::lock_guard<std::mutex> lock(registry);
    if (!id || id != currentId || !current)
        Fail(ERROR_INVALID_HANDLE);
    return current;
}
std::unique_lock<std::mutex> Lock(Session &s) {
    std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock())
        Fail(ERROR_BUSY);
    if (s.closing)
        Fail(ERROR_OPERATION_ABORTED);
    return lock;
}
void WriteValue(const interceptor_field &from, interceptor_field &to) {
    to.value = from.value;
    to.normalized = from.normalized;
    if (from.length) {
        if (from.length != to.length || !from.data || !to.data)
            Fail(ERROR_INVALID_PARAMETER);
        memcpy(to.data, from.data, from.length);
    }
}
} // namespace
extern "C" {
uint32_t interceptor_version() {
    return 1;
}
uint32_t interceptor_sizeof(uint32_t type) {
    switch (type) {
    case 1:
        return sizeof(interceptor_field);
    case 2:
        return sizeof(interceptor_event);
    case 3:
        return sizeof(interceptor_batch);
    case 4:
        return sizeof(interceptor_device);
    case 5:
        return sizeof(interceptor_error);
    case 6:
        return sizeof(interceptor_info);
    default:
        return 0;
    }
}
uint32_t interceptor_open(uint64_t *out, interceptor_error *error) {
    if (out)
        *out = 0;
    return Guard(error, "Open Interceptor wrapper", [&] {
        if (!out)
            Fail(ERROR_INVALID_PARAMETER);
        std::lock_guard<std::mutex> lock(registry);
        if (current)
            Fail(ERROR_BUSY);
        auto s = std::make_shared<Session>();
        if (++nextId == 0)
            Fail(ERROR_ARITHMETIC_OVERFLOW);
        current = s;
        currentId = nextId;
        *out = currentId;
        return 0u;
    });
}
uint32_t interceptor_cancel(uint64_t id, interceptor_error *error) {
    return Guard(error, "Cancel input wait", [&] {
        auto s = Find(id);
        if (!SetEvent(s->stop))
            Fail(GetLastError());
        return 0u;
    });
}
uint32_t interceptor_close(uint64_t id, interceptor_error *error) {
    return Guard(error, "Close Interceptor wrapper", [&] {
        auto s = Find(id);
        if (s->closing.exchange(true))
            Fail(ERROR_BUSY);
        SetEvent(s->stop);
        std::lock_guard<std::mutex> lock(s->mutex);
        try {
            Destroy_Context();
        } catch (...) {
            std::lock_guard<std::mutex> r(registry);
            current.reset();
            currentId = 0;
            throw;
        }
        std::lock_guard<std::mutex> r(registry);
        current.reset();
        currentId = 0;
        return 0u;
    });
}
uint32_t interceptor_initialize(uint64_t id, interceptor_init *out, interceptor_error *error) {
    if (out)
        *out = {};
    return Guard(error, "Initialize installed Interceptor devices", [&] {
        auto s = Find(id);
        auto lock = Lock(*s);
        if (!out)
            Fail(ERROR_INVALID_PARAMETER);
        auto r = Init_AllDevices();
        *out = {r.ready ? 1u : 0u, r.reboot_required ? 1u : 0u,
                r.associated,      r.already_associated,
                r.skipped,         0};
        return Error(error, r.error, "Initialize installed Interceptor devices");
    });
}
uint32_t interceptor_info_get(uint64_t id, interceptor_info *out, interceptor_error *error) {
    if (out)
        *out = {};
    return Guard(error, "Read Interceptor status", [&] {
        auto s = Find(id);
        auto lock = Lock(*s);
        if (!out)
            Fail(ERROR_INVALID_PARAMETER);
        auto status = Get_Status();
        auto caps = Get_Capabilities();
        auto trial = Get_Trial_Status();
        *out = {1,
                status.active_mask ? 1u : 0u,
                trial.state,
                caps.input_mask,
                caps.controller_mask,
                static_cast<uint32_t>(s->deviceSource.size()),
                status.keyboard_count + status.mouse_count + status.controller_hid_count,
                status.last_reset_reason,
                trial.remaining_100ns / 10000,
                status.timestamp_frequency,
                status.generation};
        return 0u;
    });
}
uint32_t interceptor_devices_get(uint64_t id, const interceptor_devices **out, interceptor_error *error) {
    if (out)
        *out = nullptr;
    return Guard(error, "List captured devices", [&] {
        auto s = Find(id);
        auto lock = Lock(*s);
        if (!out)
            Fail(ERROR_INVALID_PARAMETER);
        s->Snapshot();
        *out = &s->view;
        return 0u;
    });
}
uint32_t interceptor_descriptor(uint64_t id, const char *token, uint32_t report, interceptor_blob *out,
                        interceptor_error *error) {
    if (out)
        *out = {};
    return Guard(error, "Read device descriptor", [&] {
        auto s = Find(id);
        auto lock = Lock(*s);
        if (!token || !out || report > 1)
            Fail(ERROR_INVALID_PARAMETER);
        auto d = report ? Get_Report_Descriptor(token) : Get_descriptor(token);
        s->blob = std::move(d.data);
        *out = {d.applicable ? 1u : 0u, static_cast<uint32_t>(d.format),
                static_cast<uint32_t>(s->blob.size()), 0, s->blob.data()};
        return Error(error, d.error, "Read device descriptor");
    });
}
uint32_t interceptor_receive(uint64_t id, uint32_t timeout, const interceptor_batch **out, interceptor_error *error) {
    if (out)
        *out = nullptr;
    return Guard(error, "Receive Interceptor input", [&] {
        auto s = Find(id);
        auto lock = Lock(*s);
        if (!out)
            Fail(ERROR_INVALID_PARAMETER);
        if (s->pending)
            Fail(ERROR_BUSY);
        s->batch = {++s->serial, 0, 0, nullptr};
        s->refs.clear();
        s->events.clear();
        s->tokens.clear();
        s->fields.clear();
        s->edits.clear();
        s->offsets.clear();
        std::fill(std::begin(s->used), std::end(s->used), 0u);
        auto received = Get_Input_With_Timeout(timeout, s->stop);
        s->hasCapture = true;
        for (auto &e : received) {
            switch (static_cast<uint32_t>(e.type)) {
#if IC_PRODUCT_HAS_KEYBOARD_MOUSE
            case 1:
                s->Capture<Keyboard_Input>(e, 0);
                break;
            case 2:
                s->Capture<Mouse_Input>(e, 1);
                break;
#endif
#if IC_PRODUCT_HAS_CONTROLLERS
            case 3:
                s->Capture<Controller_Hid_Input>(e, 2);
                break;
            case 4:
                s->Capture<Controller_Xinput_Input>(e, 3);
                break;
            case 5:
                s->Capture<Controller_XinputHid_Input>(e, 4);
                break;
            case 6:
                if (e.Get<Controller_GIP_Raw_Input>())
                    s->Capture<Controller_GIP_Raw_Input>(e, 6);
                else
                    s->Capture<Controller_GIP_Input>(e, 5);
                break;
#endif
            default:
                Fail(ERROR_INVALID_DATA);
            }
            interceptor_event f{};
            f.kind = static_cast<uint32_t>(e.type);
            f.device = e.device;
            f.sequence = e.sequence;
            f.timestamp = e.timestamp;
            s->events.push_back(f);
            s->tokens.push_back(e.device_info ? e.device_info->token : std::string{});
        }
        ResetEvent(s->stop);
        for (uint32_t n = 0; n < s->events.size(); ++n)
            s->Describe(n);
        for (size_t n = 0; n < s->events.size(); ++n) {
            s->events[n].token = s->tokens[n].c_str();
            s->events[n].fields = s->fields.data() + s->offsets[n];
        }
        s->batch = {s->serial, static_cast<uint32_t>(s->events.size()), 0, s->events.data()};
        s->pending = !s->events.empty();
        *out = &s->batch;
        return 0u;
    });
}
uint32_t interceptor_discard(uint64_t id, uint64_t ticket, interceptor_error *error) {
    return Guard(error, "Discard input batch", [&] {
        auto s = Find(id);
        auto lock = Lock(*s);
        if (ticket != s->batch.id)
            Fail(ERROR_INVALID_HANDLE);
        s->pending = false;
        for (auto &r : s->refs)
            r.sent = true;
        return 0u;
    });
}
uint32_t interceptor_send(uint64_t id, uint64_t ticket, interceptor_error *error) {
    return Guard(error, "Forward Interceptor input", [&] {
        auto s = Find(id);
        auto lock = Lock(*s);
        if (ticket != s->batch.id)
            Fail(ERROR_INVALID_HANDLE);
        s->Apply();
        uint32_t accepted = 0;
        for (uint32_t i = 0; i < s->refs.size();) {
            auto &r = s->refs[i];
            if (r.sent || s->events[i].blocked) {
                r.sent = true;
                ++i;
                continue;
            }
            uint32_t count = 1;
            while (i + count < s->refs.size()) {
                auto &n = s->refs[i + count];
                if (n.store != r.store || n.index != r.index + count || n.sent ||
                    s->events[i + count].blocked)
                    break;
                ++count;
            }
            auto sent = s->SendRun(r, count);
            for (uint32_t n = 0; n < sent.accepted; ++n)
                s->refs[i + n].sent = true;
            accepted += static_cast<uint32_t>(sent.accepted);
            if (sent.error || sent.accepted != count)
                return Error(error, sent.error ? sent.error : ERROR_WRITE_FAULT,
                             "Forward Interceptor input", accepted);
            i += count;
        }
        s->pending = false;
        return Error(error, 0, "Forward Interceptor input", accepted);
    });
}
uint32_t interceptor_inject(uint64_t id, const interceptor_event *inputs, uint32_t count, interceptor_error *error) {
    return Guard(error, "Inject Interceptor input", [&] {
        auto s = Find(id);
        auto lock = Lock(*s);
        if (!inputs || !count || count > 8192)
            Fail(ERROR_INVALID_PARAMETER);
        for(uint32_t n=0;n<count;++n) if(inputs[n].kind>=1 && inputs[n].kind<=6 && !AllowedKind(inputs[n].kind)) Fail(ERROR_NOT_SUPPORTED);
        if (!s->injection)
            s->injection = std::make_unique<Session>(false);
        auto &scratch = *s->injection;
        scratch.tokens.resize(count);
        for (uint32_t n = 0; n < count; ++n)
            scratch.tokens[n] = inputs[n].token ? inputs[n].token : "";
        if (s->deviceSource.empty())
            s->Snapshot();
        std::apply([](auto &...v) { (v.clear(), ...); }, scratch.store);
        scratch.refs.clear();
        scratch.events.clear();
        scratch.fields.clear();
        scratch.edits.clear();
        scratch.offsets.clear();
        std::fill(std::begin(scratch.used), std::end(scratch.used), 0u);
        for (uint32_t n = 0; n < count; ++n) {
            const auto &e = inputs[n];
            if (e.kind < 1 || e.kind > 6 || e.field_count > 65536 || (e.field_count && !e.fields))
                Fail(ERROR_INVALID_PARAMETER);
            auto d = std::find_if(
                s->deviceSource.begin(), s->deviceSource.end(), [&](const Device &device) {
                    return device.type == e.kind &&
                           (scratch.tokens[n].empty() || device.token == scratch.tokens[n]);
                });
            if (d == s->deviceSource.end()) {
                s->Snapshot();
                d = std::find_if(
                    s->deviceSource.begin(), s->deviceSource.end(), [&](const Device &device) {
                        return device.type == e.kind &&
                               (scratch.tokens[n].empty() || device.token == scratch.tokens[n]);
                    });
                if (d == s->deviceSource.end())
                    Fail(ERROR_NOT_FOUND);
            }
            uint32_t which = e.kind - 1, index = scratch.used[which]++;
            switch (which) {
            case 0: {
                auto &v = std::get<0>(scratch.store);
                v.emplace_back();
                v.back().device = d->id;
                break;
            }
            case 1: {
                auto &v = std::get<1>(scratch.store);
                v.emplace_back();
                v.back().device = d->id;
                break;
            }
            case 2: {
                auto &v = std::get<2>(scratch.store);
                v.emplace_back();
                v.back().device = d->id;
                v.back().token = d->token;
                break;
            }
            case 3: {
                auto &v = std::get<3>(scratch.store);
                v.emplace_back();
                v.back().device = d->id;
                break;
            }
            case 4: {
                auto &v = std::get<4>(scratch.store);
                v.emplace_back();
                v.back().device = d->id;
                v.back().token = d->token;
                break;
            }
            case 5: {
                auto &v = std::get<5>(scratch.store);
                v.emplace_back();
                v.back().device = d->id;
                break;
            }
            }
            if (which == 2 || which == 4) {
                const interceptor_field *report = nullptr;
                for (uint32_t f = 0; f < e.field_count; ++f)
                    if (e.fields[f].group == INTERCEPTOR_REPORT)
                        report = &e.fields[f];
                if (!report || !report->data || !report->length ||
                    report->length > IC_HID_REPORT_MAX)
                    Fail(ERROR_INVALID_PARAMETER);
                if (which == 2) {
                    auto &v = std::get<2>(scratch.store).back();
                    v.length = report->length;
                    memcpy(v.report, report->data, report->length);
                } else {
                    auto &v = std::get<4>(scratch.store).back();
                    v.length = report->length;
                    memcpy(v.report, report->data, report->length);
                }
            }
            scratch.refs.push_back({which, index, false});
            interceptor_event out{};
            out.kind = e.kind;
            scratch.events.push_back(out);
        }
        for (uint32_t n = 0; n < count; ++n)
            scratch.Describe(n);
        for (uint32_t n = 0; n < count; ++n) {
            auto &e = inputs[n];
            auto begin = scratch.offsets[n], end = begin + scratch.events[n].field_count;
            for (uint32_t f = 0; f < e.field_count; ++f) {
                auto &field = e.fields[f];
                auto found = std::find_if(scratch.fields.begin() + begin,
                                          scratch.fields.begin() + end, [&](const interceptor_field &to) {
                                              return field.group == to.group &&
                                                     field.index == to.index && field.name &&
                                                     to.name &&
                                                     std::strcmp(field.name, to.name) == 0;
                                          });
                if (found == scratch.fields.begin() + end)
                    Fail(ERROR_INVALID_PARAMETER);
                WriteValue(field, *found);
            }
        }
        scratch.Apply();
        uint32_t accepted = 0;
        for (uint32_t n = 0; n < count;) {
            uint32_t run = 1;
            while (n + run < count && scratch.refs[n + run].store == scratch.refs[n].store &&
                   scratch.refs[n + run].index == scratch.refs[n].index + run)
                ++run;
            auto r = scratch.SendRun(scratch.refs[n], run);
            accepted += static_cast<uint32_t>(r.accepted);
            if (r.error || r.accepted != run)
                return Error(error, r.error ? r.error : ERROR_WRITE_FAULT,
                             "Inject Interceptor input", accepted);
            n += run;
        }
        return Error(error, 0, "Inject Interceptor input", accepted);
    });
}
}

const char* interceptor_product_name(void){return "Interceptor";}
uint32_t interceptor_input_mask(void){return IC_PRODUCT_INPUT_MASK;}
uint32_t interceptor_controller_mask(void){return IC_PRODUCT_CONTROLLER_MASK;}
