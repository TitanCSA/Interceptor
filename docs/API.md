# C++ SDK

The library targets Windows x64 and C++17. It consists of exactly two files:
[`Interceptor.h`](../Interceptor/Interceptor.h) and
[`Interceptor.cpp`](../Interceptor/Interceptor.cpp). Include the header and
compile the source once with your application. Public functions are in
`namespace Interceptor`. Install the signed kernel driver before capturing input.

## Add the two files to your application

Copy both files into your project and add `Interceptor.cpp` to its C++ sources.
There are no other local-header dependencies and no required prebuilt Interceptor
`.lib` or DLL. With MSVC, the supplied pragmas link the Windows system libraries.
For example, if you keep the `Interceptor` folder beside your `main.cpp`:

```cmake
add_executable(my_app main.cpp Interceptor/Interceptor.cpp)
target_include_directories(my_app PRIVATE Interceptor)
target_compile_features(my_app PRIVATE cxx_std_17)
target_compile_definitions(my_app PRIVATE UNICODE _UNICODE NOMINMAX)
```

Run capture applications elevated. The header alone is not an implementation:
compile `Interceptor.cpp` exactly once, rather than including it from multiple files.

To build an optional static library from a full checkout, run at the repository root:

```powershell
cmake -S . -B build/sdk -A x64 -DINTERCEPTOR_BUILD_CLIENTS=OFF
cmake --build build/sdk --config Release
```

The repository's CMake target is `Interceptor::SDK`. It is an alternative to
compiling the source directly; no kernel source or driver rebuild is involved.

## Receive, change, forward

```cpp
const auto init = Interceptor::Init_AllDevices();
if (!init.ready) {
    // init.error and init.reboot_required explain why capture should not start.
}

// After successful initialization, inside the application's input loop:
auto inputs = Interceptor::Get_Input_With_Timeout(1000, nullptr);
for (auto& event : inputs) {
    if (auto* key = event.Get<Interceptor::Keyboard_Input>()) {
        // Read or change key->stroke, preserving press/release flags.
    }
    // event.blocked = true; // Intentionally suppress this one entry.
}
const auto sent = Interceptor::Send_Input(inputs);
// Check sent.error; sent.accepted counts entries accepted by this call.
```

Use the [complete minimal example](../ExamplesConsole/08_Clavier_Minimal.cpp) for
initialization failure and exception handling.

`Get_Input_With_Timeout(timeout = INFINITE, stopEvent = nullptr)` opens the internal
session on its first call. It captures all supported families, waits on Windows
events, then returns the available input batch. `timeout` is milliseconds: `0`
checks immediately; `INFINITE` waits. An optional Windows event cancels the wait.
Timeout/cancellation returns an empty batch. Driver errors and capture revocation
throw `std::system_error`.

Receiving consumes the captured input. `Send_Input(inputs)` forwards it after your
changes; destroying a batch does not forward it. Always handle untouched families
too. FIFO order is preserved within each family; a mixed batch is not a global
chronological merge of different transports.

## Input fields and ownership

| Field | Meaning |
| --- | --- |
| `event.type` | Keyboard, mouse, HID controller, XUSB, XInputHID or GIP |
| `event.device` | Source ID, local to its family |
| `event.sequence` | Original input sequence |
| `event.timestamp` | Original QPC timestamp; frequency in `Get_Context().status.timestamp_frequency` |
| `event.device_info` | Optional pointer to cached device identity and token |
| `event.blocked` | Skip this entry when forwarding |
| `event.Get<T>()` | Typed data pointer, or `nullptr` for another family |

Types include `Keyboard_Input`, `Mouse_Input`, `Controller_Hid_Input`,
`Controller_Xinput_Input`, `Controller_XinputHid_Input`, `Controller_GIP_Input`
and `Controller_GIP_Raw_Input`. XInput here refers to the XUSB transport, not
hooking an application's `XInputGetState` call.

The noncopyable batch borrows reusable SDK buffers. Its entries, typed pointers
and identity pointers are valid until the batch is destroyed. Finish the batch
before reading again and keep it on its creating thread. A concurrent reader or
a second read while a batch is alive is rejected with `ERROR_BUSY`.

For mixed batches, forwarding tracks accepted entries: retrying the same live
batch does not resend an accepted prefix. There is no hidden indefinite retry.
A typed vector is independent; remove its accepted prefix yourself before retrying.
Fatal capture errors latch the session error; the SDK does not silently reacquire
a capture revoked by the recovery shortcut. Restart your capture application to
begin again, subject to the remaining demo allowance.

## Direct reads and sending

Each direct read returns a `std::vector` of independent typed inputs:

```cpp
auto keys = Interceptor::Get_Keyboard_Input();        // All keyboards, consume.
auto mouse = Interceptor::Get_Mouse_Input(2);        // Mouse ID 2, consume.
auto preview = Interceptor::Get_Controller_Hid_Input(0, false); // Peek all HID.
```

The same `(deviceId = 0, consume = true)` signature exists for
`Get_Controller_Xinput_Input`, `Get_Controller_XinputHid_Input`,
`Get_Controller_GIP_Input` and `Get_Controller_GIP_Raw_Input`. Unknown IDs return
an empty vector. IDs are family-local, not VID/PID or XInput player slots.
Peek leaves the entry queued; sending a peeked copy does not consume its original.
Standard and raw GIP reads share one queue.

`Send_Input` accepts a mixed batch, one batch event, a typed input, or a homogeneous
`std::vector`, `std::array` or C array. It infers type and element count. Its
`Send_Result` has `accepted` and `error`; boolean conversion means `error == 0`.
With separate reads, service all captured families to avoid retaining their inputs.

## Controller state

HID and XInputHID inputs expose dynamic `state.axes`, `state.triggers`,
`state.hats`, `state.buttons`, `state.values` and `state.binary_values`.
Repeated X/Y usages remain separate fields, identified by report, collection,
usage and data index. State describes the **current report**, not a guessed
accumulation across Report IDs.

Fields carry declared ranges and, where meaningful, normalized values. Absolute
axes normalize to `-1..1`; identified analog triggers to `0..1`. Check
`normalized_valid`, `decode_complete` and `decode_error` before using decoded
values. Modify a supported decoded value and `Send_Input` re-encodes the report.
Unknown fields and untouched bytes are preserved. Proprietary meanings cannot be
inferred from arbitrary bytes.

XUSB and GIP expose their respective native gamepad states. They are not binary
interchangeable: GIP buttons and trigger ranges differ from XInput. Raw GIP retains
the complete validated input report and its extensions; it does not expose
firmware, security or audio command injection.

## Devices and descriptors

| Function | Result |
| --- | --- |
| `Init_AllDevices()` | Silent association using installed packages: `ready`, `reboot_required`, `error`, counts and skipped-device reasons |
| `Get_Devices()` | Current device list; creates/reuses the automatic capture |
| `Get_Context()` | Snapshot: capture token, generation, active flag, devices, status, capabilities, trial and query errors |
| `Get_Device_Details(device.token)` | Exact source details, PnP identity, reader flags and last statuses |
| `Get_descriptor(device.token)` | Common identity with HID preparsed data or GIP metadata where applicable |
| `Get_Report_Descriptor(device.token)` | Raw HID report descriptor where accessible; explicitly not applicable to XUSB/GIP |
| `Get_Trial_Status()` | Trial state and remaining duration, without activating capture |

Tokens are strings unique to the source **within the capture**. Obtain them from
`Get_Context().devices`, `Get_Devices()` or `event.device_info`; do not persist them
as stable hardware serial numbers. Descriptor results expose `applicable` and
`error`: distinguish an inapplicable protocol from a failed descriptor read.

The first successful capture starts the shared two-hour demo allowance.
Expiration revokes capture and rejects injection; Windows physical input continues.
Use Windows Restart for a new allowance. [Installation and recovery](INSTALLATION.md).
