#include "InterceptorBridge.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <node_api.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
namespace {
std::mutex gate;
const interceptor_batch *live = nullptr;
uint64_t owner = 0;
struct Environment {
    uint64_t session = 0;
};
std::unique_lock<std::mutex> TryGate() {
    std::unique_lock<std::mutex> l(gate, std::try_to_lock);
    if (!l.owns_lock())
        throw std::runtime_error("Interceptor is busy in another operation");
    return l;
}
const char *kinds[] = {"", "keyboard", "mouse", "hid", "xinput", "xinputhid", "gip"};
const char *groups[] = {"state",  "axes",    "triggers",      "hats",
                        "values", "buttons", "binary_values", "report"};
void N(napi_status status) {
    if (status != napi_ok)
        throw std::runtime_error("Invalid JavaScript argument or allocation failure");
}
napi_value Undefined(napi_env e) {
    napi_value v;
    N(napi_get_undefined(e, &v));
    return v;
}
napi_value Object(napi_env e) {
    napi_value v;
    N(napi_create_object(e, &v));
    return v;
}
napi_value Array(napi_env e, size_t n = 0) {
    napi_value v;
    N(napi_create_array_with_length(e, n, &v));
    return v;
}
napi_value Num(napi_env e, double n) {
    napi_value v;
    N(napi_create_double(e, n, &v));
    return v;
}
napi_value Big(napi_env e, uint64_t n) {
    napi_value v;
    N(napi_create_bigint_uint64(e, n, &v));
    return v;
}
napi_value Int(napi_env e, int64_t n) {
    if (n >= -9007199254740991ll && n <= 9007199254740991ll)
        return Num(e, static_cast<double>(n));
    napi_value v;
    N(napi_create_bigint_int64(e, n, &v));
    return v;
}
napi_value Bool(napi_env e, bool b) {
    napi_value v;
    N(napi_get_boolean(e, b, &v));
    return v;
}
napi_value Text(napi_env e, const char *s) {
    napi_value v;
    N(napi_create_string_utf8(e, s ? s : "", NAPI_AUTO_LENGTH, &v));
    return v;
}
void Set(napi_env e, napi_value o, const char *k, napi_value v) {
    N(napi_set_named_property(e, o, k, v));
}
napi_value Get(napi_env e, napi_value o, const char *k) {
    napi_value v;
    N(napi_get_named_property(e, o, k, &v));
    return v;
}
std::string String(napi_env e, napi_value v) {
    size_t n = 0;
    N(napi_get_value_string_utf8(e, v, nullptr, 0, &n));
    std::string s(n + 1, 0);
    N(napi_get_value_string_utf8(e, v, s.data(), s.size(), &n));
    s.resize(n);
    return s;
}
uint64_t Handle(napi_env e, napi_value v) {
    uint64_t n = 0;
    bool loss = false;
    N(napi_get_value_bigint_uint64(e, v, &n, &loss));
    if (!loss)
        throw std::runtime_error("Invalid session/ticket");
    return n;
}
int64_t Integer(napi_env e, napi_value v) {
    napi_valuetype t;
    N(napi_typeof(e, v, &t));
    if (t == napi_bigint) {
        int64_t n = 0;
        bool loss = false;
        N(napi_get_value_bigint_int64(e, v, &n, &loss));
        if (!loss)
            throw std::runtime_error("Integer outside int64 range");
        return n;
    }
    double n;
    N(napi_get_value_double(e, v, &n));
    if (!std::isfinite(n) || std::floor(n) != n || std::abs(n) > 9007199254740991.0)
        throw std::runtime_error("Expected a safe integer or BigInt");
    return static_cast<int64_t>(n);
}
bool Boolean(napi_env e, napi_value v) {
    bool b;
    N(napi_get_value_bool(e, v, &b));
    return b;
}
void Check(napi_env env, const interceptor_error &error) {
    if (!error.code)
        return;
    napi_value message = Text(env, error.message), ex;
    N(napi_create_error(env, nullptr, message, &ex));
    Set(env, ex, "code", Num(env, error.code));
    Set(env, ex, "accepted", Num(env, error.accepted));
    N(napi_throw(env, ex));
    throw std::runtime_error("native error");
}
template <class F> napi_value Guard(napi_env e, F f) {
    try {
        return f();
    } catch (const std::exception &x) {
        bool pending = false;
        napi_is_exception_pending(e, &pending);
        if (!pending)
            napi_throw_error(e, nullptr, x.what());
        return nullptr;
    }
}
std::vector<napi_value> Args(napi_env e, napi_callback_info i, size_t count) {
    std::vector<napi_value> a(count);
    size_t n = count;
    N(napi_get_cb_info(e, i, &n, a.data(), nullptr, nullptr));
    if (n != count)
        throw std::runtime_error("Invalid argument count");
    return a;
}
napi_value Buffer(napi_env e, const uint8_t *data, uint32_t n) {
    napi_value v;
    N(napi_create_buffer_copy(e, n, data, nullptr, &v));
    return v;
}
struct Field {
    interceptor_field f{};
    std::string name;
    std::vector<uint8_t> data;
};
struct Event {
    interceptor_event e{};
    std::string token;
    std::vector<Field> fields;
};
struct Work {
    napi_env env = nullptr;
    napi_async_work work = nullptr;
    napi_deferred promise = nullptr;
    uint64_t session = 0, ticket = 0;
    uint32_t timeout = 0;
    interceptor_error error{};
    std::vector<Event> events;
};
void Execute(napi_env, void *opaque) {
    auto &w = *static_cast<Work *>(opaque);
    try {
        std::lock_guard<std::mutex> lock(gate);
        const interceptor_batch *b = nullptr;
        interceptor_receive(w.session, w.timeout, &b, &w.error);
        if (w.error.code)
            return;
        owner = w.session;
        live = b;
        w.ticket = b->id;
        w.events.resize(b->count);
        for (uint32_t n = 0; n < b->count; ++n) {
            auto &out = w.events[n];
            out.e = b->events[n];
            out.token = out.e.token ? out.e.token : "";
            out.fields.resize(out.e.field_count);
            for (uint32_t k = 0; k < out.e.field_count; ++k) {
                auto &f = out.fields[k];
                f.f = out.e.fields[k];
                f.name = f.f.name ? f.f.name : "";
                if (f.f.length)
                    f.data.assign(f.f.data, f.f.data + f.f.length);
            }
        }
    } catch (const std::exception &ex) {
        w.error.code = ERROR_GEN_FAILURE;
        strncpy_s(w.error.message, ex.what(), _TRUNCATE);
    }
}
napi_value Convert(napi_env env, const Work &w) {
    auto batch = Object(env), items = Array(env, w.events.size());
    Set(env, batch, "ticket", Big(env, w.ticket));
    Set(env, batch, "events", items);
    for (uint32_t n = 0; n < w.events.size(); ++n) {
        auto &src = w.events[n];
        auto out = Object(env), state = Object(env);
        Set(env, out, "kind", Text(env, kinds[src.e.kind]));
        Set(env, out, "token", Text(env, src.token.c_str()));
        Set(env, out, "sequence", Big(env, src.e.sequence));
        Set(env, out, "timestamp", Big(env, src.e.timestamp));
        Set(env, out, "blocked", Bool(env, false));
        Set(env, out, "decode_error", Num(env, src.e.decode_error));
        Set(env, out, "decode_complete", Bool(env, src.e.decode_complete != 0));
        Set(env, out, "parser_status", Num(env, src.e.parser_status));
        Set(env, out, "state", state);
        Set(env, out, "report", Buffer(env, nullptr, 0));
        for (auto &field : src.fields) {
            auto &f = field.f;
            if (f.group == 0)
                Set(env, state, field.name.c_str(), Int(env, f.value));
            else if (f.group == 7)
                Set(env, out, "report", Buffer(env, field.data.data(), f.length));
            else {
                bool has = false;
                N(napi_has_named_property(env, state, groups[f.group], &has));
                auto list = has ? Get(env, state, groups[f.group]) : Array(env);
                if (!has)
                    Set(env, state, groups[f.group], list);
                auto o = Object(env);
                Set(env, o, "value", Int(env, f.value));
                Set(env, o, "minimum", Int(env, f.minimum));
                Set(env, o, "maximum", Int(env, f.maximum));
                Set(env, o, "physical_minimum", Int(env, f.physical_minimum));
                Set(env, o, "physical_maximum", Int(env, f.physical_maximum));
                Set(env, o, "normalized", Num(env, f.normalized));
                Set(env, o, "normalized_valid", Bool(env, (f.flags & 1) != 0));
                Set(env, o, "pressed", Bool(env, f.value != 0));
#define META(name) Set(env, o, #name, Num(env, f.name))
                META(usage_page);
                META(usage);
                META(collection);
                META(report_id);
                META(data_index);
                META(element);
                META(flags);
                META(units);
                META(units_exponent);
                META(bit_size);
#undef META
                Set(env, o, "data", Buffer(env, field.data.data(), f.length));
                N(napi_set_element(env, list, f.index, o));
            }
        }
        N(napi_set_element(env, items, n, out));
    }
    return batch;
}
void Complete(napi_env env, napi_status status, void *opaque) {
    auto *w = static_cast<Work *>(opaque);
    try {
        if (status != napi_ok && !w->error.code) {
            w->error.code = ERROR_CANCELLED;
            strcpy_s(w->error.message, "Input wait cancelled");
        }
        if (w->error.code) {
            auto message = Text(env, w->error.message);
            napi_value error;
            N(napi_create_error(env, nullptr, message, &error));
            Set(env, error, "code", Num(env, w->error.code));
            Set(env, error, "accepted", Num(env, w->error.accepted));
            N(napi_reject_deferred(env, w->promise, error));
        } else
            N(napi_resolve_deferred(env, w->promise, Convert(env, *w)));
    } catch (const std::exception &ex) {
        bool pending = false;
        napi_is_exception_pending(env, &pending);
        napi_value error = nullptr;
        if (pending)
            napi_get_and_clear_last_exception(env, &error);
        else {
            napi_value msg = nullptr;
            if (napi_create_string_utf8(env, ex.what(), NAPI_AUTO_LENGTH, &msg) == napi_ok)
                napi_create_error(env, nullptr, msg, &error);
        }
        if (error)
            napi_reject_deferred(env, w->promise, error);
    }
    napi_delete_async_work(env, w->work);
    delete w;
}
napi_value Open(napi_env e, napi_callback_info) {
    return Guard(e, [&] {
        auto lock = TryGate();
        uint64_t id = 0;
        interceptor_error err{};
        interceptor_open(&id, &err);
        Check(e, err);
        Environment *context = nullptr;
        N(napi_get_instance_data(e, reinterpret_cast<void **>(&context)));
        context->session = id;
        owner = id;
        live = nullptr;
        return Big(e, id);
    });
}
napi_value Close(napi_env e, napi_callback_info i) {
    return Guard(e, [&] {
        auto a = Args(e, i, 1);
        auto id = Handle(e, a[0]);
        interceptor_error err{};
        interceptor_cancel(id, &err);
        std::lock_guard<std::mutex> lock(gate);
        interceptor_close(id, &err);
        if (owner == id) {
            owner = 0;
            live = nullptr;
        }
        Environment *context = nullptr;
        N(napi_get_instance_data(e, reinterpret_cast<void **>(&context)));
        context->session = 0;
        Check(e, err);
        return Undefined(e);
    });
}
napi_value Cancel(napi_env e, napi_callback_info i) {
    return Guard(e, [&] {
        auto a = Args(e, i, 1);
        interceptor_error err{};
        interceptor_cancel(Handle(e, a[0]), &err);
        Check(e, err);
        return Undefined(e);
    });
}
napi_value Receive(napi_env e, napi_callback_info i) {
    return Guard(e, [&] {
        auto a = Args(e, i, 2);
        auto *w = new Work;
        w->env = e;
        w->session = Handle(e, a[0]);
        w->timeout = static_cast<uint32_t>(Integer(e, a[1]));
        napi_value promise;
        try {
            N(napi_create_promise(e, &w->promise, &promise));
            auto resource = Text(e, "Interceptor.receive");
            N(napi_create_async_work(e, nullptr, resource, Execute, Complete, w, &w->work));
            N(napi_queue_async_work(e, w->work));
        } catch (...) {
            if (w->work)
                napi_delete_async_work(e, w->work);
            delete w;
            throw;
        }
        return promise;
    });
}
void CopyBytes(napi_env env, napi_value value, interceptor_field &f) {
    bool buffer = false;
    N(napi_is_buffer(env, value, &buffer));
    void *data = nullptr;
    size_t length = 0;
    if (buffer)
        N(napi_get_buffer_info(env, value, &data, &length));
    else {
        napi_typedarray_type type;
        napi_value array;
        size_t offset;
        N(napi_get_typedarray_info(env, value, &type, &length, &data, &array, &offset));
        if (type != napi_uint8_array)
            throw std::runtime_error("Expected Uint8Array");
    }
    if (length != f.length)
        throw std::runtime_error("Input data length is immutable");
    if (length)
        memcpy(f.data, data, length);
}
napi_value Send(napi_env e, napi_callback_info i) {
    return Guard(e, [&] {
        auto a = Args(e, i, 3);
        auto id = Handle(e, a[0]), ticket = Handle(e, a[1]);
        auto lock = TryGate();
        if (owner != id || !live || live->id != ticket)
            throw std::runtime_error("Stale or foreign batch");
        uint32_t count = 0;
        N(napi_get_array_length(e, a[2], &count));
        if (count != live->count)
            throw std::runtime_error("Do not resize an input batch");
        for (uint32_t n = 0; n < count; ++n) {
            napi_value input;
            N(napi_get_element(e, a[2], n, &input));
            auto &event = live->events[n];
            auto state = Get(e, input, "state");
            event.blocked = Boolean(e, Get(e, input, "blocked"));
            if (event.blocked)
                continue;
            for (uint32_t k = 0; k < event.field_count; ++k) {
                auto &f = event.fields[k];
                if (f.group == 0)
                    f.value = Integer(e, Get(e, state, f.name));
                else if (f.group == 7)
                    CopyBytes(e, Get(e, input, "report"), f);
                else {
                    auto list = Get(e, state, groups[f.group]);
                    napi_value item;
                    N(napi_get_element(e, list, f.index, &item));
                    f.value = f.group == 5 ? Boolean(e, Get(e, item, "pressed"))
                                           : Integer(e, Get(e, item, "value"));
                    if (f.flags & 1) {
                        double value;
                        N(napi_get_value_double(e, Get(e, item, "normalized"), &value));
                        f.normalized = value;
                    }
                    if (f.length)
                        CopyBytes(e, Get(e, item, "data"), f);
                }
            }
        }
        interceptor_error err{};
        interceptor_send(id, ticket, &err);
        Check(e, err);
        return Num(e, err.accepted);
    });
}
napi_value Discard(napi_env e, napi_callback_info i) {
    return Guard(e, [&] {
        auto a = Args(e, i, 2);
        auto lock = TryGate();
        interceptor_error err{};
        interceptor_discard(Handle(e, a[0]), Handle(e, a[1]), &err);
        Check(e, err);
        return Undefined(e);
    });
}
napi_value Initialize(napi_env e, napi_callback_info i) {
    return Guard(e, [&] {
        auto a = Args(e, i, 1);
        auto lock = TryGate();
        interceptor_init out{};
        interceptor_error err{};
        interceptor_initialize(Handle(e, a[0]), &out, &err);
        Check(e, err);
        auto result = Object(e);
        Set(e, result, "ready", Bool(e, out.ready != 0));
        Set(e, result, "reboot_required", Bool(e, out.reboot_required != 0));
        Set(e, result, "associated", Num(e, out.associated));
        Set(e, result, "already_associated", Num(e, out.already_associated));
        Set(e, result, "skipped", Num(e, out.skipped));
        return result;
    });
}
napi_value Info(napi_env e, napi_callback_info i) {
    return Guard(e, [&] {
        auto a = Args(e, i, 1);
        auto lock = TryGate();
        interceptor_info out{};
        interceptor_error err{};
        interceptor_info_get(Handle(e, a[0]), &out, &err);
        Check(e, err);
        auto r = Object(e);
#define INFO(name) Set(e, r, #name, Num(e, static_cast<double>(out.name)))
        INFO(installed);
        INFO(active);
        INFO(trial_state);
        INFO(input_mask);
        INFO(controller_mask);
        INFO(device_count);
        INFO(queued);
        INFO(last_reset);
        INFO(remaining_ms);
        INFO(timestamp_frequency);
        INFO(generation);
#undef INFO
        return r;
    });
}
napi_value Devices(napi_env e, napi_callback_info i) {
    return Guard(e, [&] {
        auto a = Args(e, i, 1);
        auto lock = TryGate();
        interceptor_error err{};
        const interceptor_devices *out = nullptr;
        interceptor_devices_get(Handle(e, a[0]), &out, &err);
        Check(e, err);
        auto result = Array(e, out->count);
        for (uint32_t n = 0; n < out->count; ++n) {
            auto &d = out->items[n];
            auto o = Object(e);
            Set(e, o, "kind", Text(e, kinds[d.kind]));
            Set(e, o, "token", Text(e, d.token));
            Set(e, o, "name", Text(e, d.name));
            Set(e, o, "hardware_id", Text(e, d.hardware_id));
            Set(e, o, "id", Num(e, d.id));
            Set(e, o, "vid", Num(e, d.vid));
            Set(e, o, "pid", Num(e, d.pid));
            Set(e, o, "usage_page", Num(e, d.usage_page));
            Set(e, o, "usage", Num(e, d.usage));
            N(napi_set_element(e, result, n, o));
        }
        return result;
    });
}
napi_value Descriptor(napi_env e, napi_callback_info i) {
    return Guard(e, [&] {
        auto a = Args(e, i, 3);
        auto lock = TryGate();
        interceptor_error err{};
        interceptor_blob out{};
        auto token = String(e, a[1]);
        interceptor_descriptor(Handle(e, a[0]), token.c_str(), Boolean(e, a[2]), &out, &err);
        Check(e, err);
        auto result = Object(e);
        Set(e, result, "applicable", Bool(e, out.applicable != 0));
        Set(e, result, "format", Num(e, out.format));
        Set(e, result, "data", Buffer(e, out.data, out.length));
        return result;
    });
}
napi_value Inject(napi_env e, napi_callback_info i) {
    return Guard(e, [&] {
        auto a = Args(e, i, 2);
        auto lock = TryGate();
        auto kind = String(e, Get(e, a[1], "kind"));
        auto found = std::find_if(kinds + 1, kinds + 7, [&](const char *k) { return kind == k; });
        if (found == kinds + 7)
            throw std::runtime_error("Unknown input kind");
        interceptor_event input{};
        input.kind = static_cast<uint32_t>(found - kinds);
        bool has = false;
        N(napi_has_named_property(e, a[1], "token", &has));
        auto token = has ? String(e, Get(e, a[1], "token")) : std::string{};
        input.token = token.c_str();
        auto state = Get(e, a[1], "state");
        napi_value keys;
        N(napi_get_property_names(e, state, &keys));
        uint32_t count = 0;
        N(napi_get_array_length(e, keys, &count));
        std::vector<std::string> names(count);
        std::vector<interceptor_field> fields(count);
        for (uint32_t n = 0; n < count; ++n) {
            napi_value key, value;
            N(napi_get_element(e, keys, n, &key));
            names[n] = String(e, key);
            N(napi_get_property(e, state, key, &value));
            fields[n].name = names[n].c_str();
            fields[n].value = Integer(e, value);
        }
        N(napi_has_named_property(e, a[1], "report", &has));
        if (has) {
            auto report = Get(e, a[1], "report");
            bool buffer = false;
            N(napi_is_buffer(e, report, &buffer));
            if (!buffer)
                throw std::runtime_error("Synthetic report must be a Buffer");
            void *data = nullptr;
            size_t length = 0;
            N(napi_get_buffer_info(e, report, &data, &length));
            interceptor_field f{};
            f.name = "report";
            f.group = 7;
            f.data = static_cast<uint8_t *>(data);
            f.length = static_cast<uint32_t>(length);
            fields.push_back(f);
        }
        input.fields = fields.data();
        input.field_count = static_cast<uint32_t>(fields.size());
        interceptor_error err{};
        interceptor_inject(Handle(e, a[0]), &input, 1, &err);
        Check(e, err);
        return Num(e, err.accepted);
    });
}
void Cleanup(void *data) {
    auto *context = static_cast<Environment *>(data);
    if (context->session) {
        interceptor_error err{};
        interceptor_cancel(context->session, &err);
        std::lock_guard<std::mutex> lock(gate);
        interceptor_close(context->session, &err);
        if (owner == context->session) {
            owner = 0;
            live = nullptr;
        }
    }
    delete context;
}
#ifdef ICB_TEST_BACKEND
napi_value FixtureMode(napi_env e, napi_callback_info i) {
    return Guard(e, [&] {
        auto a = Args(e, i, 1);
        using Mode = void (*)(unsigned);
        auto fn = reinterpret_cast<Mode>(
            GetProcAddress(GetModuleHandleW(L"InterceptorSDKBridge.dll"), "interceptor_test_mode"));
        if (!fn)
            throw std::runtime_error("Not a fixture DLL");
        fn(static_cast<unsigned>(Integer(e, a[0])));
        return Undefined(e);
    });
}
#endif
napi_value Init(napi_env e, napi_value exports) {
    return Guard(e, [&] {
#define FN(name, fn) {name, nullptr, fn, nullptr, nullptr, nullptr, napi_default, nullptr}
        napi_property_descriptor methods[] = {FN("open", Open),
                                              FN("close", Close),
                                              FN("cancel", Cancel),
                                              FN("receive", Receive),
                                              FN("send", Send),
                                              FN("discard", Discard),
                                              FN("initialize", Initialize),
                                              FN("info", Info),
                                              FN("devices", Devices),
                                              FN("descriptor", Descriptor),
                                              FN("inject", Inject)};
#undef FN
        N(napi_define_properties(e, exports, sizeof(methods) / sizeof(methods[0]), methods));
#ifdef ICB_TEST_BACKEND
        napi_value fn;
        N(napi_create_function(e, "_fixtureMode", NAPI_AUTO_LENGTH, FixtureMode, nullptr, &fn));
        Set(e, exports, "_fixtureMode", fn);
#endif
        auto *context = new Environment;
        N(napi_set_instance_data(e, context, nullptr, nullptr));
        N(napi_add_env_cleanup_hook(e, Cleanup, context));
        return exports;
    });
}
} // namespace
NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
