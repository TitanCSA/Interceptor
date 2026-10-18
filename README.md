# Interceptor

**Windows keyboard, mouse and game controller interception — one C++17 API.**

[![Windows 10 / 11](https://img.shields.io/badge/Windows-10%20%2F%2011-0078D4)](docs/INSTALLATION.md)
[![Architecture](https://img.shields.io/badge/platform-x64-1464E8)](docs/COMPATIBILITY.md)
[![Demo](https://img.shields.io/badge/demo-2%20hours%20per%20Windows%20boot-FFE365)](#demo-edition)
[![Source license](https://img.shields.io/badge/user--mode%20source-MIT-green)](LICENSE)

Intercept physical input, inspect its source, change or block it, and forward it
to Windows. Interceptor combines keyboard and mouse capture with descriptor-driven
HID controllers, XUSB / XInput, XInputHID and GIP USB in a single driver and SDK.

This project evolves the input-interception workflow popularized by
[oblitum's Interception](https://github.com/oblitum/Interception), with its own
implementation, controller support, automatic SDK sessions and graphical tools.
It is an independent project, not affiliated with oblitum, and is not compatible
with Interception's ABI.

**[Download the latest release](https://github.com/TitanCSA/Interceptor/releases/latest)**
· [Installation](docs/INSTALLATION.md) · [C++ API](docs/API.md)
· [Compatibility](docs/COMPATIBILITY.md) · [Changelog](CHANGELOG.md)

| Setup and diagnostics | Controller viewer |
| --- | --- |
| ![Interceptor installer dashboard](docs/images/installer.png) | ![Interceptor controller viewer with simulated input](docs/images/controllers.png) |

The installer image shows the application before installation; Windows device
names follow the OS language. The controller image is a simulated viewer preview,
not a physical-device test.

## What is included

- **One input loop:** receive mixed input batches, edit typed values, call `Send_Input`.
  The library manages the capture session and infers send types and sizes.
- **Per-device information:** capture-scoped string tokens, VID/PID, descriptors
  and detailed Windows device state.
- **Decoded controllers:** dynamic HID axes, triggers, hats and buttons; native
  XUSB and GIP gamepad state. Original report data remains available.
- **Setup tools:** standalone GUI and console installers, device inventory,
  association management, quick diagnostics, report export and copy.
- **Working examples:** eight C++ console examples and an input viewer, with source.

The driver in this release is **Microsoft-signed**. This signature applies to the
driver package, not automatically to the installer or example EXEs. Windows 10/11
x64 is required; installation and capture need administrator rights. A supported
protocol family does not guarantee every device model, firmware or connection.

## Install and try

1. Download `InterceptorSetup.exe` from [Releases](https://github.com/TitanCSA/Interceptor/releases/latest).
2. Run it, accept Windows elevation and click **Install / reinstall**.
3. If requested, use Windows **Restart**, then reopen the same installer to finish
   device associations. Follow any further restart request.
4. Run the graphical example and click **Observe** to view and forward input,
   or try a console example. **Stop** ends the observation.

The console installer uses the same installation engine:

```powershell
.\InterceptorSetupConsole.exe /install
.\InterceptorSetupConsole.exe /uninstall
```

Exit code `3010` means restart required. After an installation restart, rerun
`/install` to complete pending associations. [Full instructions and recovery](docs/INSTALLATION.md).

## C++ in a few lines

Add the SDK sources to your application or link the `Interceptor::SDK` CMake target.
This example changes H into J and forwards every other input unchanged:

```cpp
#include "Interceptor.h"
#include <system_error>

int main() {
    using namespace Interceptor;
    try {
        const auto init = Init_AllDevices();
        if (!init.ready)
            return static_cast<int>(init.error ? init.error : ERROR_SUCCESS_REBOOT_REQUIRED);
        for (;;) {
            auto inputs = Get_Input_With_Timeout();
            for (auto& event : inputs)
                if (auto* key = event.Get<Keyboard_Input>())
                    if (!(key->stroke.flags & (KeyE0 | KeyE1)) && key->stroke.make_code == 0x23)
                        key->stroke.make_code = 0x24;
            const auto sent = Send_Input(inputs);
            if (!sent)
                throw std::system_error(sent.error, std::system_category());
        }
    } catch (const std::system_error& error) {
        return error.code().value() == ERROR_OPERATION_ABORTED ? 0 : error.code().value();
    }
}
```

Receiving input consumes it from the capture queue. Forward it with `Send_Input`
or intentionally block it. Closing the process releases its capture; physical
**Ctrl+Alt+Delete** also revokes captures. See [batch lifetime and error handling](docs/API.md).

## Demo edition

The driver allows **two hours per Windows boot**, starting with the first
successful capture. The allowance is shared across applications and devices;
closing an application does not reset it. Time spent asleep counts. Diagnostics
that open a capture can also start the allowance.

After expiration, capture and injection stop and physical input continues normally.
Use Windows **Restart** to begin another allowance; sleep and Fast Startup shutdown
do not reset the kernel boot. For the **full version without the demo time limit**,
send a Discord DM to **`titancsa`**.

## Downloads and source

| Item | Purpose |
| --- | --- |
| `InterceptorSetup.exe` | Standalone graphical installer and diagnostics |
| `InterceptorSetupConsole.exe` | Standalone `/install` and `/uninstall` installer |
| `InterceptorExampleUI.exe` | Standalone keyboard, mouse and controller input viewer |
| `Interceptor-Examples-1.4.15.0.zip` | Prebuilt console examples and graphical input viewer |
| `Interceptor-SDK-1.4.15.0.zip` / repository source | C++ library, shared protocol headers, examples and user-mode tools |

All downloads are on the [release page](https://github.com/TitanCSA/Interceptor/releases/latest).
Release checksums identify the published files. The repository includes **user-mode
source code**, installer scripts and the pre-signed demo driver package. **Kernel
driver source and private signing material are not published.**

Build the user-mode tools with `powershell -ExecutionPolicy Bypass -File .\Build.ps1`
from a checkout with Visual Studio 2022 C++ and the Windows SDK installed.
This reuses the signed driver package; it does not rebuild the driver.

User-mode source is covered by [MIT](LICENSE). The demo driver is a separately
licensed binary; see [BINARY-LICENSE.txt](BINARY-LICENSE.txt).
For a bug report, include the release version, Windows version, VID/PID, connection
type and the installer diagnostic report, removing personal identifiers first.

Windows input interception · keyboard filter driver · mouse filter driver · HID
gamepad · XInput · XUSB · GIP · input remapping · C++ SDK
