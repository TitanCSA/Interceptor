# Interceptor console examples

Install this edition with **InterceptorSetup.exe**, restart Windows if requested,
then run **one** example as administrator. Each program checks the driver before
starting and explains its action in English. Close its console or press Ctrl+C to
stop. Physical Ctrl+Alt+Delete is available if an intentional keyboard block
suppresses shortcuts; that example also exits on Escape.

Each `.cpp` is independent: `Interceptor.h`, `Interceptor.cpp`, and standard/Windows
headers are all it needs. There is no shared example runner or behavior header.

| Source / executable | What it does |
| --- | --- |
| [01_Keyboard_Swap_H_J.cpp](01_Keyboard_Swap_H_J.cpp) | Swap H and J, including key presses and releases. |
| [02_Keyboard_Ctrl_H_Hello.cpp](02_Keyboard_Ctrl_H_Hello.cpp) | Ctrl+H types hello. The shortcut H is consumed; other keys keep working. |
| [03_Mouse_Invert_Y.cpp](03_Mouse_Invert_Y.cpp) | Invert mouse Y. X, buttons, wheels and other devices stay unchanged. |
| [04_Mouse_Invert_X_Hold_HK.cpp](04_Mouse_Invert_X_Hold_HK.cpp) | Invert mouse X. Hold J or mouse X1/X2 to hold H+K instead. |
| [05_Keyboard_Block_And_Print.cpp](05_Keyboard_Block_And_Print.cpp) | Print and block keyboard input. Press Escape to stop and restore normal input. |
| [06_Mouse_Block_And_Print.cpp](06_Mouse_Block_And_Print.cpp) | Print and block mouse input. Keyboard and controller input keep working. |
| [08_Keyboard_Minimal.cpp](08_Keyboard_Minimal.cpp) | Minimal example: capture inputs, change H to J, then forward them. |
| [09_Mouse_Move_Horizontal.cpp](09_Mouse_Move_Horizontal.cpp) | Enter relative mouse X counts: 300 moves right, -485 moves left. Enter q to quit. |
| [Controller_01_Invert_Vertical.cpp](Controller_01_Invert_Vertical.cpp) | Invert every identified stick Y axis. Supports HID, XInput/XUSB, XInputHID and GIP. |
| [Controller_02_Swap_Buttons.cpp](Controller_02_Swap_Buttons.cpp) | Swap controller buttons in pairs: 1/2, 3/4, and so on. Supports all controller families. |
| [Controller_03_Instant_Triggers.cpp](Controller_03_Instant_Triggers.cpp) | Make any positive trigger pressure 100%. Supports HID, XInput/XUSB, XInputHID and GIP. |
| [Controller_04_Deadzone_50.cpp](Controller_04_Deadzone_50.cpp) | Apply a 50% deadzone per stick axis and rescale the remaining range. Supports all families. |
| [Controller_05_Inverse_Exponential.cpp](Controller_05_Inverse_Exponential.cpp) | Apply an inverse exponential stick curve (square root by default). Supports all families. |

## Receive, change, forward

The essential mouse Y operation is easy to follow:

```cpp
auto inputs = Get_Input_With_Timeout(INFINITE);
for (auto& input : inputs)
    if (auto* mouse = input.Get<Mouse_Input>())
        mouse->stroke.y = -mouse->stroke.y;
auto result = Send_Input(inputs);
```

The complete examples also handle absolute coordinates and signed endpoints,
check initialization/send failures, and forward unrelated fields/families.
`INFINITE` sleeps until input arrives; there is no polling, countdown, FPS limit,
per-packet descriptor query or manually managed context. Buffers and capture
lifetime belong to the library. Process exit closes capture in the driver.

Build from the repository root with `./Build.ps1 -Target ExamplesConsole`.
EXEs are written to `bin/Native/`. Download precompiled programs from the release's
**examples ZIP**; the **SDK ZIP** includes their complete sources.

The demo is limited to **one hour per boot**, shared across applications.

## Controller fields

All five controller examples handle HID, XInput/XUSB, XInputHID and GIP in the
same receive loop. HID uses dynamic `state.axes`, `state.triggers` and
`state.buttons`; XUSB/GIP use their native `state` fields. Only the current
report's decoded controls are edited; unknown fields and raw extensions pass through.

HID descriptors do not universally identify stick/trigger roles. Auto selects
X/Y + Z/Rz when present in the same report/collection/element, otherwise X/Y + Rx/Ry.
Use `--hid-layout xy-rxry` or `--hid-layout xy-zrz` for an ambiguous descriptor.
A lone combined-trigger Z, hats, null/relative controls and vendor data are preserved.
Button pairing stays within each report/collection/usage page; an odd last button stays unchanged.

The 50% deadzone is per axis, not radial: 40% -> 0%, 75% -> 50%, 100% -> 100%.
The inverse exponential uses `sign(x) * abs(x)^(1/exponent)`; use `--exponent 2`
(default), or a matching value from 1 to 16. It only compensates a game applying
the matching power curve, without additional deadzones/acceleration.

## Raw mouse movement

`09_Mouse_Move_Horizontal` sends `Mouse_Input` with `state.x` equal to the number
you type. These are relative input counts, not guaranteed screen pixels; sensitivity
and acceleration are applied by Windows/the game. `state.y`, buttons and flags stay
zero. The console input handle wakes `Get_Input_With_Timeout` when typing, while
physical input is forwarded; no forwarding thread, idle polling or cursor-position API
is needed. Escape or `q` followed by Enter exits.
