# C++ input examples

Install the driver first. Each example uses the two-file SDK and checks its own
product's driver service before capture. Run one program at a time as administrator.

## Keyboard and mouse

| Source / executable | Behavior |
| --- | --- |
| `01_Echange_H_J` | Swap H and J |
| `02_Ctrl_H_Hello` | Ctrl+H types `hello` |
| `03_Inverse_Souris_Y` | Invert mouse Y |
| `04_Inverse_X_J_Boutons_HK` | Invert mouse X; J or X1/X2 holds H+K |
| `05_Clavier_Bloque_Affichage` | Display and intentionally block keyboard input |
| `06_Souris_Bloquee_Affichage` | Display and intentionally block mouse input |
| `08_Clavier_Minimal` | Minimal mixed-batch loop: H becomes J |
| `09_Mouse_Move_Horizontal` | Send a signed relative mouse X movement through the driver |

Examples 01-06 use a countdown. Release controls, then focus the target app.
Physical Ctrl+Alt+Delete revokes capture if a blocking monitor suppresses ordinary
shortcuts. The `hello` example uses scan codes; it is not a text/IME API.

### Move the mouse using raw relative X input

Extract the examples ZIP and run `ExamplesConsole/09_Mouse_Move_Horizontal.exe`
as administrator after installing this edition's driver. When building from
source, the EXE is in `bin/Native/`. Type `300` and Enter to send X = +300, or
`-485` and Enter to send X = -485. `0` does nothing. Esc, `q` and Enter, or Ctrl+C
exits.

The value is sent directly as a relative mouse input through Interceptor:

```cpp
Interceptor::Mouse_Input input{};
input.device = mouse.id;        // A mouse returned by Get_Devices().
auto& state = input.stroke;
state.x = 300;                 // Relative X; Y, buttons and flags remain zero.
auto result = Interceptor::Send_Input(input);
```

`state` is a local reference to the SDK's `stroke` field. There is no cursor
position query, absolute coordinate conversion or screen-bound clamping. These
are raw movement counts, not a guaranteed number of screen pixels: Windows and
applications apply their own sensitivity/acceleration. The example also checks
the send result and handles queue backpressure.

Physical input is forwarded while you enter commands. The console and driver
share an event-driven loop with no idle busy polling. Run one capture app at a
time. The installed demo's one-hour allowance still applies; no driver update
is required. The compiled EXE is distributed inside the examples ZIP.

[Source](09_Mouse_Move_Horizontal.cpp)
## Controller examples

Run **one capture program at a time**, as administrator, after installing this
edition. All five programs receive **HID, XInput/XUSB, XInputHID and GIP** in the
same automatic SDK session. Other input families and untouched fields are forwarded.
The examples use physical device reports; they do not create virtual controllers.

| Source / executable | Behavior |
| --- | --- |
| `Controller_01_Invert_Vertical` | Invert every identified stick Y axis, including repeated HID Y controls |
| `Controller_02_Swap_Buttons` | Swap buttons in pairs; native A/B, X/Y, LB/RB, Back/Start, LS/RS, Up/Down, Left/Right |
| `Controller_03_Instant_Triggers` | Any positive trigger pressure becomes 100%; released remains 0% |
| `Controller_04_Deadzone_50` | 50% deadzone on each stick axis; rescale the remaining range to 0-100% |
| `Controller_05_Inverse_Exponential` | Apply a root curve to sticks; default exponent 2 (square root) |

Each `.cpp` is a complete, independent program with only `Interceptor.h` and
system/standard headers. Compile it with `Interceptor/Interceptor.cpp` and C++17;
no shared behavior header is required. Build all with:

```powershell
.\Build.ps1 -Target ExamplesConsole
```

Run the EXEs from `bin/Native/`. Each checks its own product's installed driver
service, initializes devices, receives a batch, transforms it and calls
`Send_Input(inputs)`. The loop has no sleep, FPS limit or per-report console
logging. A send error or incomplete batch stops the process with its error.
Closing the process / Ctrl+C releases capture and restores passthrough.
The shared one-hour per-boot demo allowance applies.

### HID layouts

Native XUSB/GIP sticks and triggers have defined fields. Generic HID controls
come from the descriptor, with their report and collection identities preserved.
Explicit HID trigger controls are supported. For conventional unlabelled axes,
the default `auto` layout uses X/Y for the left stick and Z/Rz for the right stick
when both exist; otherwise it uses Rx/Ry. With a Z/Rz stick, Rx/Ry are treated as
separate triggers. Repeated controls in the current report are processed too.

Some descriptors use the same usages for different purposes. Select the layout
matching the descriptor rather than a device VID/PID:

```powershell
.\Controller_01_Invert_Vertical.exe --hid-layout xy-rxry
.\Controller_03_Instant_Triggers.exe --hid-layout xy-zrz
```

`xy-rxry` means X/Y + Rx/Ry sticks, with Z/Rz as separate triggers;
`xy-zrz` means X/Y + Z/Rz sticks, with Rx/Ry triggers.
Relative/null/constant-range fields, hats, unknown usages and opaque extensions
remain unchanged. A lone Z in automatic mode is not guessed to be a trigger:
it may encode two combined triggers. Vendor encodings or stick pairs split across
reports may require adapting the clearly marked axis selection in the example.
Descriptor decoding alone cannot establish a universal stick/trigger meaning.

HID button swaps pair consecutive declared controls within the same report,
usage page and collection. An odd trailing button stays unchanged. Native Guide
and reserved bits stay unchanged. A D-pad represented as a HID hat is not a
button pair; it is preserved.

### Response curves

The deadzone is **per axis**, not radial:
`out = sign(in) * max(0, (abs(in) - 0.5) / 0.5)`.
For example, 40% becomes 0%, 75% becomes 50%, and 100% stays 100%.

```powershell
.\Controller_05_Inverse_Exponential.exe --exponent 2
```

The inverse curve is `out = sign(in) * abs(in)^(1/exponent)`, with an exponent
from 1 to 16. It compensates a game curve `sign(x) * abs(x)^exponent` only when
the exponents match and no other deadzone, acceleration or response processing
intervenes. There is no universal inverse for every game's "exponential" setting.

The former **Controller Minimal** only received and forwarded all families
unchanged. It was a developer skeleton, not a remap. These five examples replace
its EXE; the [main README](../README.md#minimal-example-receive-read-forward)
keeps the basic receive/read/forward loop.
