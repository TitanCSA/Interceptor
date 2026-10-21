# Device compatibility

The Interceptor kernel-mode input driver targets **Windows 10/11 x64**. The
two-file user-mode library uses this same signed driver; combining its source
does not change device profiles. Support is based on descriptors and validated
protocol profiles rather than a list of controller brands. A family
being implemented does not mean every product, firmware or transport is supported.

| Family | Supported path | Important limits |
| --- | --- | --- |
| Keyboard | Associated HID keyboard mapper | Requires the installed mapper; scan codes are not layout-independent text |
| Mouse | Associated HID mouse mapper | Relative/absolute movement, buttons and wheels depend on the device reports |
| HID controller | Descriptor-driven game, joystick, simulation, sport and related controller collections | Declared fields can be decoded; arbitrary vendor protocols and malformed descriptors cannot be guessed |
| XUSB / XInput | Compatible wired, single-slot USB XUSB source, intercepted below the native Windows XUSB driver | Not every device accessible through the Windows XInput API uses this supported transport; no replacement HID mirror is created |
| XInputHID | Validated HID source beneath Microsoft's XInputHID translator | Native Xbox/GIP child views are not separate XInputHID sources |
| GIP USB | Compatible USB gamepad protocol with validated metadata and standard gamepad prefix | Complete input reports up to the supported 64-byte bound; unknown extension meanings remain raw |

Generic HID includes devices declaring game-controller usages across supported
Generic Desktop, Simulation, Sport and Game usage pages. Repeated usages, multiple
Report IDs and varied button/axis layouts are represented dynamically. A composite
controller can expose several HID collections and therefore several source IDs.

Some PlayStation controllers expose suitable HID collections over USB or Bluetooth;
that does not establish support for every mode, CRC scheme or vendor extension.
Native Xbox wireless/radio/Bluetooth transports are **not covered by the wired
XUSB/GIP profiles**. Rumble, force feedback, audio, firmware and proprietary feature
reports are outside the input capture/modification API.

## Console gamepads

These familiar model names map to the input paths above. Support requires a
compatible descriptor and protocol profile for the selected connection.

| Controller family | Input connection/profile |
| --- | --- |
| PlayStation 4 **DualShock 4 / DS4** | USB HID gamepad collection |
| PlayStation 5 **DualSense** | USB HID gamepad collection |
| **Xbox 360** | Wired USB XInput/XUSB, compatible single-slot parent and validated USB input profile |
| **Xbox One** | Compatible USB GIP gamepad profile |
| **Xbox Series X\|S** | Compatible USB GIP gamepad profile |

Model names do not establish support for every firmware revision, Bluetooth or
wireless adapter. The validation scope below describes the tested protocol fixtures.

## Detection and capture are different

The installer displays Windows nodes, associations and published interfaces.
Those observations alone do not prove that every input field can be intercepted.
A source may be present but not ready, outside a supported profile, waiting for a
restart or reserved by another capture application. Unsupported controller
collections retain their ordinary Windows path rather than receiving invented
protocol interpretations.

The automatic SDK session can start while some families are absent or preparing;
ready sources join as they become available. An idle device is not necessarily
missing. Read the device details and diagnostic findings instead of inferring
compatibility from a row count or a single empty input batch.

## Validation scope

Version **1.4.17.0** passes local kernel builds, static analysis, transport
and lifecycle fixtures, keyboard/mouse regression tests, and SDK tests in Release
and with AddressSanitizer. The returned Microsoft signatures and catalog membership
are verified; physical-device validation of this combined release is not claimed.
See [the current validation record](INPUT-UPDATE.md). These local
tests do not establish compatibility with every controller, application or game.

The 1.4.15 development campaign exercised six-family capture, modification,
forwarding, malformed requests, cancellation and contention in dedicated Windows
10 and 11 virtual machines with Driver Verifier. It checked 2,323,714 load packets
using HID/XUSB/GIP fixtures. Those results do not validate every physical controller
or establish physical input latency, and are not a guarantee of zero bugs.

For a useful compatibility report, include the release version, Windows build,
VID/PID, USB/Bluetooth/radio connection and the installer diagnostic text. Remove
serial numbers or personal paths before posting publicly.
