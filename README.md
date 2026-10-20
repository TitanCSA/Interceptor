# Interceptor — Windows Kernel Filter Driver for Keyboard, Mouse & Gamepad

**Microsoft-signed Windows kernel-mode input filter driver for keyboard, mouse
and HID / XInput / GIP game controllers, with a C++17 SDK.**

Intercept physical input, identify its source, change or block it, then forward
it to Windows through one API. Windows 10/11 **x64**; administrator rights required
for installation and capture. See the [supported profiles](docs/COMPATIBILITY.md)
for device and connection limits.

USB gamepad families include **DualShock 4 (DS4 / PS4), DualSense (PS5), wired
Xbox 360, Xbox One and Xbox Series X|S**, using compatible HID, XInput/XUSB or
GIP input profiles. See the [controller matrix](docs/COMPATIBILITY.md#console-gamepads)
for the required connection and profile.

**[Download](https://github.com/TitanCSA/Interceptor/releases/latest)**
· [Installation](docs/INSTALLATION.md) · [C++ API](docs/API.md)
· [Examples](ExamplesConsole/README.md)

**1.4.16.0 signed release:** complete installer, viewer, console examples and SDK
downloads are included in one release. The SDK retains temporarily unready
HID/XInputHID/XUSB collection files, and the viewer keeps exact failure details
with a copy button. Both former 33 ms display waits are removed; forwarding
still precedes display decoding. Keyboard and mouse remain supported.

Both installers now contain **Microsoft-signed driver 1.4.16.0**, including the
XUSB pending-membership and idle-start fixes. Install / reinstall to update the
kernel, then use Windows **Restart** if requested. Updating the viewer EXE alone
does not update an installed driver.

| Setup and diagnostics | Controller viewer |
| --- | --- |
| ![Interceptor installer dashboard](docs/images/installer.png) | ![Controller viewer with simulated input](docs/images/controllers.png) |

The installer is shown before installation; Windows device names follow the OS
language. The controller image is a simulated preview, not a physical-device test.

## Quick start

1. Download and run **InterceptorSetup.exe**, then accept Windows elevation.
2. Click **Install / reinstall**. If requested, use Windows **Restart**, then
   reopen the same installer to finish device associations.
3. Launch **InterceptorExampleUI.exe** and click **Observe** to view and forward
   inputs. **Stop** ends capture. Close other capture applications first.
4. Use **Diagnose** for per-device checks and a report you can copy or export.
   Checks without suitable input activity remain **unverified**.

The console installer uses the same installation engine:

```powershell
.\InterceptorSetupConsole.exe /install
.\InterceptorSetupConsole.exe /uninstall
```

Exit code `3010` means restart required; after installing, rerun `/install` after
that restart. [Installation, diagnostics and recovery](docs/INSTALLATION.md).

## C++ SDK

`Get_Input_With_Timeout()` receives a mixed input batch. Modify typed fields or
mark individual events blocked, then call `Send_Input(inputs)` to forward it.
The library is just [`Interceptor.h`](Interceptor/Interceptor.h) and
[`Interceptor.cpp`](Interceptor/Interceptor.cpp): include the header and compile
the source in your project. No prebuilt Interceptor `.lib` or DLL is required.
Sessions, send types and sizes are automatic; HID controller state is decoded
from the device descriptor.

Start with the [complete H-to-J example](ExamplesConsole/08_Clavier_Minimal.cpp)
or the [API guide](docs/API.md). Each console example contains its complete source
and uses only the two library files. Forward untouched families too: receiving a batch
consumes its input. Closing the application releases capture; physical
**Ctrl+Alt+Delete** also revokes captures.

## Demo edition

**Two hours per Windows boot**, starting with the first successful capture,
shared across all applications and devices. Time spent asleep counts; closing an
application or reinstalling does not reset it. Diagnostics that capture input
can start the allowance. After expiry, capture/injection stop and physical input
continues normally. Use Windows **Restart** for another allowance; Fast Startup
shutdown does not reset the kernel boot.

For the **full version without the demo time limit**, DM **`titancsa`** on Discord.

## Downloads

All files and checksums are in [Releases](https://github.com/TitanCSA/Interceptor/releases/latest).
The driver's Microsoft signature does not automatically sign the installer or
example EXEs.

| File | Contents |
| --- | --- |
| `InterceptorSetup.exe` | Standalone GUI installer and diagnostics |
| `InterceptorSetupConsole.exe` | Standalone `/install` and `/uninstall` installer |
| `InterceptorExampleUI.exe` | Standalone input viewer |
| `Interceptor-Examples-1.4.16.0.zip` | Eight console examples and the viewer |
| `Interceptor-SDK-1.4.16.0.zip` | C++ SDK and example source |

## Source and build

```text
Interceptor/       Standalone two-file C++ library
ExamplesConsole/   Eight console examples
ExamplesUI/        Graphical input viewer
InstallerConsole/  Console installer
InstallerUI/       GUI installer, shared package engine and recovery binaries
docs/              API, installation, compatibility and images
licenses/          Driver binary terms and attribution
```

With Visual Studio 2022 C++ and the Windows SDK installed, run
`powershell -ExecutionPolicy Bypass -File .\Build.ps1` to build everything.
Each installer can also be built alone with `InstallerUI/Build.ps1` or
`InstallerConsole/Build.ps1`. Builds reuse the pre-signed demo driver.
**Kernel driver source and
private signing material are not included.** User-mode source is [MIT](LICENSE);
the driver binary has [separate terms](licenses/BINARY-LICENSE.txt).

Interceptor independently evolves the input-interception workflow popularized by
[oblitum's Interception](https://github.com/oblitum/Interception), with its own
implementation and API. It is not affiliated with oblitum or ABI-compatible with
the original. See [attribution](licenses/NOTICE). For bug reports, include the
release, Windows build, VID/PID, connection type and a diagnostic report with
personal identifiers removed.

## Minimal example: receive, read, change, forward

Install Interceptor, restart Windows if requested, and run your application as
administrator. Add `Interceptor.h` and `Interceptor.cpp` to a C++17 project
([build instructions](docs/API.md#add-the-two-files-to-your-application)).
This example changes the physical H key into J and forwards all other input:

```cpp
#include "Interceptor.h"
#include <iostream>
#include <system_error>

int main() try {
    using namespace Interceptor;
    const auto init = Init_AllDevices();
    if (!init.ready) {
        std::cerr << "Initialization error: " << init.error
                  << ", restart required: " << init.reboot_required << '\n';
        return 1;
    }

    for (;;) {
        auto inputs = Get_Input_With_Timeout(1000);
        for (auto& input : inputs) {
            if (auto* key = input.Get<Keyboard_Input>()) {
                // Read the scan code; change H (0x23) into J (0x24).
                if (!(key->stroke.flags & (KeyE0 | KeyE1)) &&
                    key->stroke.make_code == 0x23)
                    key->stroke.make_code = 0x24;
            }
        }
        const auto sent = Send_Input(inputs);
        if (!sent) {
            std::cerr << "Send error: " << sent.error << '\n';
            return 1;
        }
    }
} catch (const std::system_error& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
```

1. **Receive:** `Get_Input_With_Timeout(1000)` waits up to 1,000 milliseconds
   for input. It returns a batch you can loop over, or an empty batch on timeout.
   Each `input` in the loop is one event; `inputs.size()` gives the event count.
   The library creates and reuses its capture automatically; no context argument
   is needed. `Get_Input_With_Timeout()` waits indefinitely instead.
2. **Read:** `input.Get<Keyboard_Input>()` returns the keyboard data, or `nullptr`
   if this entry belongs to another family. `key->stroke.make_code` is the scan
   code, not a character; `(key->stroke.flags & KeyUp) != 0` means key release.
   `input.type` gives the family; `input.device` identifies its source within that
   family. For mouse data, use `if (auto* mouse = input.Get<Mouse_Input>())` and
   read `mouse->stroke.x` / `mouse->stroke.y`.
3. **Change:** assign a new value to a field. Here both H presses and releases
   become J, while the other flags stay intact. These are physical scan-code
   positions; the resulting character depends on the Windows keyboard layout.
   Remove the `if` that changes the scan code for a simple pass-through loop.
4. **Forward:** `Send_Input(inputs)` sends the batch back to its original
   devices. Type, destination and count are automatic. Unchanged keyboard,
   mouse and controller entries are forwarded too. Without this call, received
   entries are consumed and are not forwarded automatically.

`Send_Input` returns `accepted` (entries accepted by this call) and `error`
(`0` on success). This basic example exits on an error; see the
[API guide](docs/API.md#input-fields-and-ownership) for retry handling.
Finish processing and forwarding a batch before receiving the next one; its
data is valid only while that batch exists. Close the application or press
physical **Ctrl+Alt+Delete** to release capture.
