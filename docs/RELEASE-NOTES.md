# Interceptor 1.4.15.0 — Demo

Windows 10/11 x64 input interception for **keyboards, mice and HID / XUSB /
XInputHID / GIP USB game controllers**, with one C++17 SDK.

This independent implementation evolves the workflow popularized by
[oblitum's Interception](https://github.com/oblitum/Interception). It is not affiliated
with oblitum and is not ABI-compatible with the original project.

## Downloads

| Asset | Contents |
| --- | --- |
| `InterceptorSetup.exe` | Standalone graphical installer, device inventory and diagnostics |
| `InterceptorSetupConsole.exe` | Same installation engine, using `/install` or `/uninstall` |
| `InterceptorExampleUI.exe` | Standalone graphical input viewer |
| `Interceptor-Examples-1.4.15.0.zip` | Eight console examples and the graphical input viewer |
| `Interceptor-SDK-1.4.15.0.zip` | User-mode C++ library, shared headers and example source |

The release's checksums identify the published files. The driver package is
**Microsoft-signed**; installer/example EXEs do not inherit that signature.
Kernel driver source and private signing material are not included.

## Get started

Run the GUI installer as administrator and click **Install / reinstall**, or run:

```powershell
.\InterceptorSetupConsole.exe /install
```

If a restart is required (`3010` from the console), use Windows **Restart**, then
reopen the same installer or rerun `/install` to finish pending associations.
Uninstall with the GUI's **Uninstall** button or `/uninstall`.

The SDK manages its capture automatically. Receive a batch with
`Get_Input_With_Timeout()`, change typed input fields and call `Send_Input(inputs)`.
The HID controller state exposes descriptor-driven axes, triggers, hats and buttons.
Supported protocol profiles do not guarantee every device model or connection.

## Demo limit

The first successful capture starts a shared **two-hour allowance per Windows
boot**. Time spent asleep counts; closing applications or reinstalling does not reset it. After expiration,
capture/injection stop and physical input continues normally. Use Windows **Restart**
for another allowance; sleep and Fast Startup shutdown do not reset the kernel boot.

For the **full version without the demo time limit**, DM **`titancsa`** on Discord.

User-mode source is MIT-licensed. The driver demo binary is separately licensed.
See the [README](https://github.com/TitanCSA/Interceptor#readme),
[installation guide](https://github.com/TitanCSA/Interceptor/blob/main/docs/INSTALLATION.md)
and [compatibility notes](https://github.com/TitanCSA/Interceptor/blob/main/docs/COMPATIBILITY.md).
