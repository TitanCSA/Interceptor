#include "ViewerDecoder.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace {
constexpr uint32_t Batch = 128;
std::mutex lifecycleLock, snapshotLock;
std::condition_variable lifecycleChanged;
std::thread worker;
bool stopping = false, notificationPending = false;
HANDLE stopEvent = nullptr, changeEvent = nullptr;
VIEWER_SNAPSHOT snapshot{sizeof(VIEWER_SNAPSHOT), VIEWER_VERSION, ViewerStopped};
VIEWER_DIAGNOSTIC diagnostic{sizeof(VIEWER_DIAGNOSTIC), VIEWER_VERSION};
std::vector<VIEWER_DEVICE> published;

void NotifyLocked() noexcept {
    ++snapshot.revision;
    if (!notificationPending) {
        notificationPending = true;
        if (changeEvent)
            SetEvent(changeEvent);
    }
}
void Status(uint32_t status, DWORD error = ERROR_SUCCESS, uint32_t resetReason = 0) {
    std::lock_guard<std::mutex> lock(snapshotLock);
    snapshot.status = status;
    snapshot.win32Error = error;
    snapshot.resetReason = resetReason;
    NotifyLocked();
}
bool StopRequested() noexcept { return WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0; }
DWORD ExceptionCode() noexcept {
    try {
        throw;
    } catch (const std::system_error &e) {
        return static_cast<DWORD>(e.code().value());
    } catch (const std::bad_alloc &) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (const std::invalid_argument &) {
        return ERROR_INVALID_PARAMETER;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}
struct Device {
    VIEWER_DEVICE state{};
    std::unique_ptr<Viewer::HidDecoder> decoder;
};
struct Session {
    Interceptor::Context_Result capture;
    std::vector<Device> devices;
    std::array<std::array<int, VIEWER_MAX_DEVICES + 1>, 7> indices{};
    uint64_t received = 0, forwarded = 0;
    bool snapshotChanged = false;
    uint8_t visibleInputs = 0, visibleControllers = 0;
    VIEWER_DIAGNOSTIC operation{sizeof(VIEWER_DIAGNOSTIC), VIEWER_VERSION};
    uint32_t readyMask = 0;
    void Stage(uint32_t stage, uint32_t type = 0) noexcept {
        operation.stage = stage;
        operation.type = type;
        operation.device = 0;
        operation.requested = operation.accepted = 0;
        operation.sequence = 0;
    }
    bool Visible(uint32_t type) const noexcept {
        return type >= 1 && type <= 6 &&
               (type <= 2 ? (visibleInputs & (1u << (type - 1))) : (visibleControllers & (1u << (type - 3))));
    }
    Session() {
        for (auto &row : indices)
            row.fill(-1);
    }
    void Initialize() {
        Stage(ViewerStageEnumerate);
        capture.devices = Interceptor::Get_Devices(*capture);
        if (capture.devices.size() > VIEWER_MAX_DEVICES)
            throw std::system_error(ERROR_INVALID_DATA, std::system_category());
        devices.reserve(capture.devices.size());
        for (const auto &info : capture.devices) {
            if (!Visible(info.type))
                continue;
            if (info.type < 1 || info.type > 6 || info.id < 1 || info.id > VIEWER_MAX_DEVICES)
                throw std::system_error(ERROR_INVALID_DATA, std::system_category());
            if (indices[info.type][info.id] >= 0) {
                devices[static_cast<size_t>(indices[info.type][info.id])].state.info = info;
                continue;
            }
            Device d;
            d.state.info = info;
            d.state.hat = -1;
            // Decoder failure does not prevent transparent forwarding. It is
            // visible separately per device, without guessing report offsets.
            if (info.type == IC_TYPE_CONTROLLER_HID || info.type == IC_TYPE_CONTROLLER_XINPUTHID) {
                try {
                    auto descriptor =
                        info.type == IC_TYPE_CONTROLLER_HID
                            ? Interceptor::Get_Controller_Hid_Descriptor(*capture, info.id)
                            : Interceptor::Get_Controller_XinputHid_Descriptor(*capture, info.id);
                    d.decoder = std::make_unique<Viewer::HidDecoder>(std::move(descriptor));
                } catch (...) {
                    d.state.stateFlags |= ViewerDecodeUnsupported;
                    d.state.decodeError = ExceptionCode();
                }
            }
            indices[info.type][info.id] = static_cast<int>(devices.size());
            devices.push_back(std::move(d));
        }
        std::lock_guard<std::mutex> lock(snapshotLock);
        published.resize(devices.size());
        snapshot.deviceCount = static_cast<uint32_t>(devices.size());
        snapshot.generation = capture.generation;
        for (size_t n = 0; n < devices.size(); ++n)
            published[n] = devices[n].state;
        snapshot.status = devices.empty() ? ViewerPreparing : ViewerRunning;
        NotifyLocked();
        snapshotChanged = false;
    }
    Device *Find(uint32_t type, uint32_t id) noexcept {
        if (type > 6 || id > VIEWER_MAX_DEVICES)
            return nullptr;
        const auto index = indices[type][id];
        return index < 0 ? nullptr : &devices[static_cast<size_t>(index)];
    }
    void Publish() {
        std::lock_guard<std::mutex> lock(snapshotLock);
        snapshot.received = received;
        snapshot.forwarded = forwarded;
        for (size_t n = 0; n < devices.size(); ++n)
            published[n] = devices[n].state;
        NotifyLocked();
        snapshotChanged = false;
    }
    void PublishIfChanged() {
        if (snapshotChanged)
            Publish();
    }
    template <class T, class Getter, class Sender, class Decoder>
    void Transfer(uint32_t type, std::vector<T> &buffer, Getter get, Sender send, Decoder decode) {
        Stage(ViewerStageReceive, type);
        operation.requested = Batch;
        const auto count = get(buffer.data(), Batch);
        if (!count)
            return;
        if (Visible(type))
            received += count;
        // The original batch is sent intact FIRST. No decoding, notification,
        // UI locking, formatting, or logging precedes this synchronous send.
        Stage(ViewerStageForward, type);
        operation.requested = count;
        operation.device = Header(buffer[0]).device;
        operation.sequence = Header(buffer[0]).sequence;
        const auto result = send(buffer.data(), count);
        if (Visible(type))
            forwarded += result.accepted;
        if (result.error || result.accepted != count) {
            operation.accepted = static_cast<uint32_t>(result.accepted);
            if (result.accepted < count) {
                operation.device = Header(buffer[result.accepted]).device;
                operation.sequence = Header(buffer[result.accepted]).sequence;
            } else {
                operation.device = 0;
                operation.sequence = 0;
            }
            Publish();
            // Busy/partial/error closes capture instead of spinning or keeping
            // the physical devices captured with an unsent suffix.
            throw std::system_error(static_cast<int>(result.error ? result.error : ERROR_WRITE_FAULT),
                                    std::system_category(), "Forward original input batch");
        }
        if (!Visible(type))
            return;
        for (uint32_t n = 0; n < count; ++n) {
            const auto &input = Header(buffer[n]);
            auto *d = Find(type, input.device);
            if (!d) {
                Initialize();
                d = Find(type, input.device);
            }
            if (!d)
                throw std::system_error(ERROR_INVALID_DATA, std::system_category());
            Stage(ViewerStageDecode, type);
            operation.device = input.device;
            operation.sequence = input.sequence;
            decode(buffer[n], *d);
            Viewer::Metadata(input.sequence, input.timestamp, d->state);
        }
        snapshotChanged = true;
    }
    template <class T> static const T &Header(const T &input) { return input; }
    static const IC_CONTROLLER_GIP_INPUT &Header(const IC_CONTROLLER_GIP_RAW_INPUT &input) {
        return input.input;
    }
};
void RecordFailure(const Session &session, DWORD error, uint32_t reason,
                   const char *fallback = nullptr) noexcept {
    auto failure = session.operation;
    failure.win32Error = error;
    failure.resetReason = reason;
    failure.readyMask = session.readyMask;
    failure.generation = session.capture.generation;
    failure.received = session.received;
    failure.forwarded = session.forwarded;
    auto copy = [&](const char *message) {
        if (!message)
            return;
        const auto bytes =
            static_cast<int>(std::min<std::size_t>(std::strlen(message), std::size(failure.message) - 1));
        auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, message, bytes, failure.message,
                                         static_cast<int>(std::size(failure.message) - 1));
        if (!count)
            count = MultiByteToWideChar(CP_ACP, 0, message, bytes, failure.message,
                                        static_cast<int>(std::size(failure.message) - 1));
        failure.message[count > 0 ? count : 0] = 0;
    };
    try {
        if (fallback)
            copy(fallback);
        else
            try {
                throw;
            } catch (const std::exception &exception) {
                copy(exception.what());
            } catch (...) {
                copy("Unknown native exception");
            }
        std::lock_guard<std::mutex> lock(snapshotLock);
        diagnostic = failure;
    } catch (...) { /* Preserve the worker's original failure across diagnostic errors. */
    }
}
void Run(uint8_t mask, uint8_t controllers) noexcept {
    Session s;
    try {
        s.Stage(ViewerStageInitialize);
        const auto init = Interceptor::Init_AllDevices();
        {
            std::lock_guard<std::mutex> lock(snapshotLock);
            snapshot.initAssociated = init.associated;
            snapshot.initExisting = init.already_associated;
        }
        if (StopRequested())
            return;
        if (init.reboot_required) {
            const auto error = init.error ? init.error : ERROR_SUCCESS_REBOOT_REQUIRED;
            RecordFailure(s, error, 0, "Device associations require a Windows restart");
            Status(ViewerRebootRequired, error);
            return;
        }
        if (init.error || !init.ready) {
            const auto error = init.error ? init.error : ERROR_NOT_READY;
            const auto detail=init.error_detail.empty()?std::string("Init_AllDevices did not complete successfully"):init.error_detail;
            RecordFailure(s, error, 0, detail.c_str());
            Status(ViewerError, error);
            return;
        }
        // Create_Context checks immutable capabilities on the loaded driver.
        // SDK build switches cannot prove native support in the active SYS.
        Status(ViewerPreparing);
        s.visibleInputs = mask;
        s.visibleControllers = controllers;
        s.Stage(ViewerStageCreate);
        s.capture = Interceptor::Create_Context();
        s.Initialize();
        std::vector<Interceptor::Keyboard_Input> keyboard(Batch);
        std::vector<Interceptor::Mouse_Input> mouse(Batch);
        std::vector<Interceptor::Controller_Hid_Input> hid(Batch);
        std::vector<Interceptor::Controller_Xinput_Input> xusb(Batch);
        std::vector<Interceptor::Controller_XinputHid_Input> xih(Batch);
        std::vector<Interceptor::Controller_GIP_Input> gip(Batch);
        s.Stage(ViewerStageCapabilities);
        const bool rawGip = (Interceptor::Get_Capabilities().gip_profiles & IC_PROFILE_GIP_USB_GAMEPAD_RAW);
        std::vector<Interceptor::Controller_GIP_Raw_Input> gipRaw(rawGip ? Batch : 0);
        while (!StopRequested()) {
            s.Stage(ViewerStageWait);
            s.readyMask = 0;
            const auto ready =
                Interceptor::Wait_For_Input(*s.capture, INFINITE, stopEvent);
            s.readyMask = ready;
            if (StopRequested())
                break;
            if (!ready) {
                continue;
            }
#define TRANSFER(bit, type, buffer, Name, Decode)                                                            \
    if (ready & (bit))                                                                                       \
    s.Transfer(                                                                                              \
        type, buffer,                                                                                        \
        [&](auto *p, uint32_t count) {                                                                       \
            return Interceptor::Get_##Name##_Input(*s.capture, p, Batch, count);                             \
        },                                                                                                   \
        [&](const auto *p, size_t count) { return Interceptor::Send_Input(*s.capture, p, count); }, Decode)
            TRANSFER(IC_KEYBOARD, IC_TYPE_KEYBOARD, keyboard, Keyboard,
                     [](const auto &in, Device &d) { Viewer::DecodeKeyboard(in, d.state); });
            TRANSFER(IC_MOUSE, IC_TYPE_MOUSE, mouse, Mouse,
                     [](const auto &in, Device &d) { Viewer::DecodeMouse(in, d.state); });
            TRANSFER(IC_READY_HID, IC_TYPE_CONTROLLER_HID, hid, Controller_Hid,
                     [](const auto &in, Device &d) {
                         if (d.decoder)
                             d.decoder->Decode(in.report, in.length, d.state);
                     });
            TRANSFER(IC_READY_XUSB, IC_TYPE_CONTROLLER_XUSB, xusb, Controller_Xinput,
                     [](const auto &in, Device &d) { Viewer::DecodeNative(in.state, false, d.state); });
            TRANSFER(IC_READY_XINPUTHID, IC_TYPE_CONTROLLER_XINPUTHID, xih, Controller_XinputHid,
                     [](const auto &in, Device &d) {
                         if (d.decoder)
                             d.decoder->Decode(in.report, in.length, d.state);
                     });
            if (rawGip) {
                TRANSFER(IC_READY_GIP, IC_TYPE_CONTROLLER_GIP, gipRaw, Controller_GIP_Raw,
                         [](const auto &in, Device &d) {
                             Viewer::DecodeNative(in.input.state, true, d.state);
                             if (in.report_length > IC_GIP_INPUT_BYTES)
                                 d.state.stateFlags |= ViewerPartialDecode;
                         });
            } else {
                TRANSFER(IC_READY_GIP, IC_TYPE_CONTROLLER_GIP, gip, Controller_GIP,
                         [](const auto &in, Device &d) { Viewer::DecodeNative(in.state, true, d.state); });
            }
#undef TRANSFER
            s.PublishIfChanged();
        }
        Interceptor::Destroy_Context(s.capture);
    } catch (...) {
        const DWORD error = ExceptionCode();
        uint32_t reason = 0;
        if (s.capture) {
            try {
                reason = Interceptor::Get_Status(s.capture.context.get()).last_reset_reason;
            } catch (...) {
            }
        }
        RecordFailure(s, error, reason);
        Interceptor::Destroy_Context(s.capture);
        if (!StopRequested())
            Status(error == IC_ERROR_TRIAL_EXPIRED || reason == IC_RESET_TRIAL_EXPIRED ? ViewerTrialExpired
                   : error == ERROR_OPERATION_ABORTED                                  ? ViewerRevoked
                   : error == ERROR_NOT_SUPPORTED        ? ViewerBackendUnavailable
                   : error == ERROR_DEVICE_NOT_CONNECTED ? ViewerNoDevices
                   : error == ERROR_TIMEOUT              ? ViewerPreparationTimeout
                                                         : ViewerError,
                   error, reason);
    }
}
} // namespace

DWORD __cdecl Viewer_Start(uint8_t mask, uint8_t controllers, HANDLE event) noexcept {
    if ((!mask && !controllers) || (mask & ~IC_BOTH) || (controllers & ~IC_CONTROLLER_ALL) || !event ||
        event == INVALID_HANDLE_VALUE)
        return ERROR_INVALID_PARAMETER;
    try {
        std::lock_guard<std::mutex> lifecycle(lifecycleLock);
        if (stopping || worker.joinable())
            return ERROR_BUSY;
        HANDLE duplicated = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &duplicated, EVENT_MODIFY_STATE,
                             FALSE, 0))
            return GetLastError();
        if (!ResetEvent(duplicated)) {
            const auto error = GetLastError();
            CloseHandle(duplicated);
            return error;
        }
        const auto stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop) {
            const auto error = GetLastError();
            CloseHandle(duplicated);
            return error;
        }
        stopEvent = stop;
        {
            std::lock_guard<std::mutex> lock(snapshotLock);
            changeEvent = duplicated;
            published.clear();
            snapshot = {};
            snapshot.size = sizeof(snapshot);
            snapshot.version = VIEWER_VERSION;
            diagnostic = {};
            diagnostic.size = sizeof(diagnostic);
            diagnostic.version = VIEWER_VERSION;
            snapshot.status = ViewerStarting;
            snapshot.mask = mask;
            snapshot.controllers = controllers;
            notificationPending = false;
            NotifyLocked();
        }
        try {
            worker = std::thread(Run, mask, controllers);
        } catch (...) {
            const auto error = ExceptionCode();
            Session failure;
            failure.Stage(ViewerStageCreate);
            RecordFailure(failure, error, 0);
            Status(ViewerError, error);
            {
                std::lock_guard<std::mutex> lock(snapshotLock);
                CloseHandle(changeEvent);
                changeEvent = nullptr;
            }
            CloseHandle(stopEvent);
            stopEvent = nullptr;
            return error;
        }
        return ERROR_SUCCESS;
    } catch (...) {
        return ExceptionCode();
    }
}
void __cdecl Viewer_Stop() noexcept {
    try {
        std::thread joining;
        {
            std::unique_lock<std::mutex> lock(lifecycleLock);
            lifecycleChanged.wait(lock, [] { return !stopping; });
            stopping = true;
            if (stopEvent)
                SetEvent(stopEvent);
            joining = std::move(worker);
        }
        // No lifecycle/snapshot lock is held while waiting for SDK cleanup.
        if (joining.joinable())
            joining.join();
        {
            std::lock_guard<std::mutex> lock(snapshotLock);
            snapshot.status = ViewerStopped;
            snapshot.deviceCount = 0;
            published.clear();
            NotifyLocked();
            if (changeEvent) {
                CloseHandle(changeEvent);
                changeEvent = nullptr;
            }
        }
        {
            std::lock_guard<std::mutex> lock(lifecycleLock);
            if (stopEvent) {
                CloseHandle(stopEvent);
                stopEvent = nullptr;
            }
            stopping = false;
        }
        lifecycleChanged.notify_all();
    } catch (...) { /* C ABI; no exceptions escape. Process teardown still closes context. */
    }
}
DWORD __cdecl Viewer_ReadSnapshot(VIEWER_SNAPSHOT *header, VIEWER_DEVICE *devices, uint32_t capacity,
                                  uint32_t *required) noexcept {
    if (!header || !required || (capacity && !devices) || header->size != sizeof(VIEWER_SNAPSHOT) ||
        header->version != VIEWER_VERSION)
        return ERROR_INVALID_PARAMETER;
    try {
        std::lock_guard<std::mutex> lock(snapshotLock);
        *header = snapshot;
        *required = static_cast<uint32_t>(published.size());
        if (capacity < published.size())
            return ERROR_INSUFFICIENT_BUFFER;
        if (!published.empty())
            memcpy(devices, published.data(), published.size() * sizeof(VIEWER_DEVICE));
        notificationPending = false;
        return ERROR_SUCCESS;
    } catch (...) {
        return ExceptionCode();
    }
}
DWORD __cdecl Viewer_ReadDiagnostic(VIEWER_DIAGNOSTIC *output) noexcept {
    if (!output || output->size != sizeof(VIEWER_DIAGNOSTIC) || output->version != VIEWER_VERSION)
        return ERROR_INVALID_PARAMETER;
    try {
        std::lock_guard<std::mutex> lock(snapshotLock);
        *output = diagnostic;
        return ERROR_SUCCESS;
    } catch (...) {
        return ExceptionCode();
    }
}
