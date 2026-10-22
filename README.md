# Interceptor - Windows Kernel Filter Driver for Keyboard, Mouse and Gamepad

Capture physical input, identify its source, modify or block it, and forward it
to Windows with a **Microsoft-signed kernel filter driver** and a **two-file
C++17 SDK**. Supports keyboard, mouse and compatible HID, XInput/XUSB,
XInputHID and GIP controllers on **Windows 10 1709+ / Windows 11 x64**.

**[Download](https://github.com/TitanCSA/Interceptor/releases/latest)** |
[Installation](docs/INSTALLATION.md) | [C++ API](docs/API.md) |
[Examples](ExamplesConsole/README.md) | [Compatibility](docs/COMPATIBILITY.md)

| Setup and diagnostics | Input viewer |
| --- | --- |
| ![Installer dashboard](docs/images/installer.png) | ![Viewer with simulated controller input](docs/images/controllers.png) |

Screenshots illustrate the interface; the controller preview uses simulated input.

<a id="quick-start"></a>

## Install and use

1. Download **InterceptorSetup.exe** from this repository's latest release.
2. Run it, accept the administrator prompt and choose **Install / reinstall**.
3. If requested, use Windows **Restart**, then reopen setup to finish associations.
4. Open **InterceptorExampleUI.exe** and accept the administrator prompt.
   Observation starts automatically. Tabs select the displayed family.
   **Stop** releases capture. Run one capture app at a time.
5. Use **Diagnose** to check installation and export or copy a per-device report.

For command-line installation or removal, use the same release's console installer:

```powershell
.\InterceptorSetupConsole.exe /install
.\InterceptorSetupConsole.exe /uninstall
```

Exit code `3010` requests a restart. After installation, rerun `/install` if setup
still has pending associations. The GUI also provides **Uninstall**.
The Microsoft signature covers the driver package; installer and example EXEs
have their own signature status. [Installation and recovery](docs/INSTALLATION.md).

## How it works

The filter sits in the supported physical device's Windows input stack. Without
a capture owner, input passes through normally. An application receives input
batches, changes typed fields or marks events blocked, then sends the batch back
through the filter. Closing the application releases its capture.

| Family | Input path |
| --- | --- |
| Keyboard and mouse | Associated Windows input paths and supported HID mapper collections |
| HID controller | Descriptor-based axes, triggers, buttons, hats and other declared controls |
| XInput / XUSB | Compatible wired USB source beneath the native Windows XUSB driver |
| XInputHID | Supported HID collection beneath the Windows translator |
| GIP | Compatible USB gamepad protocol, including the standard state and Guide input |

Compatible profiles include **DualShock 4 / DS4 (PS4), DualSense (PS5), wired
Xbox 360, Xbox One and Xbox Series X|S**. Support depends on connection,
descriptor and protocol; see the [controller matrix](docs/COMPATIBILITY.md#console-gamepads).
Setup lists Windows device nodes, so one physical controller can have several rows.

## C++ SDK

Add [`Interceptor.h`](Interceptor/Interceptor.h) and
[`Interceptor.cpp`](Interceptor/Interceptor.cpp) to a Windows x64 C++17 project.
Include the header and compile the source once. No Interceptor DLL or prebuilt
LIB is required. Run capture applications as administrator.

`Get_Input_With_Timeout()` automatically creates and reuses the capture session.
It returns a mixed batch; `Send_Input(inputs)` determines the input types,
destinations and counts. HID state is decoded from the device descriptor.
Finish and forward each batch before receiving another one, including families
you do not modify. [API reference](docs/API.md).

<a id="source-and-build"></a>

## Downloads and source

The [latest release](https://github.com/TitanCSA/Interceptor/releases/latest)
contains both standalone installers, the standalone viewer, an examples ZIP,
a source SDK ZIP and **SHA256SUMS.txt**. Use matching driver, SDK and examples
from the same release. Updating the viewer alone does not update the driver.

```text
Interceptor/       Interceptor.h and Interceptor.cpp
ExamplesConsole/   Eight independent C++ examples
ExamplesUI/        Graphical input viewer
InstallerConsole/  Console installer
InstallerUI/       GUI, shared setup engine and compiled driver/recovery binaries
docs/              Installation, API, compatibility and screenshots
licenses/          Driver terms and attribution
```

With Visual Studio 2022 C++ tools, a Windows SDK and CMake available, run
`powershell -ExecutionPolicy Bypass -File .\Build.ps1` to build the user-mode tools.
The signed driver and recovery binaries are supplied; **kernel source is not
included**. User-mode source is [MIT](LICENSE); the driver has
[separate terms](licenses/BINARY-LICENSE.txt).

For controller-only capture, use [Interceptor Gamepad & Controller](https://github.com/TitanCSA/Interceptor-Gamepad-Controller).
For keyboard/mouse-only capture, use [Interceptor Keyboard & Mouse](https://github.com/TitanCSA/Interceptor-Keyboard-Mouse).
Remove an overlapping combined edition before installing a split edition.

## Demo edition

Capture and injection are available for **two hours per Windows kernel boot**,
starting with the first capture, controller monitor subscription or raw GIP
diagnostic access, and shared across applications.
Sleep time counts; closing or reinstalling does not reset the allowance.
Diagnostics that capture input can start it. At expiry, capture stops and
physical input passes through normally. Use Windows **Restart** for a new
allowance; Fast Startup shutdown does not reset the kernel boot.
For the unrestricted version, DM **`titancsa`** on Discord.

Interceptor independently evolves the input-interception workflow popularized by
[oblitum/Interception](https://github.com/oblitum/Interception), with its own
implementation and API. It is not affiliated with oblitum or ABI-compatible with
the original. [Attribution](licenses/NOTICE).

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

## Responsible use and disclaimer

This project, including its drivers, SDKs, libraries, applications, installers and examples, was created for lawful, constructive purposes such as accessibility, device customization, research, testing and software development. Illegal, malicious or unauthorized use is neither intended nor endorsed by TitanCSA or the contributors.

You are solely responsible for how you use, modify or distribute the software, for obtaining any necessary authorization, and for complying with applicable laws, third-party rights and service rules. TitanCSA and the contributors do not accept responsibility for users' actions, including illegal activity or misuse of the drivers or SDKs.

To the maximum extent permitted by applicable law, the software is provided "AS IS", without warranties of any kind, and TitanCSA and the contributors are not liable for claims, damages, data loss, system damage or other consequences arising from its use or misuse. Nothing in this notice excludes liability that cannot legally be excluded. Existing project and third-party license terms remain applicable.
