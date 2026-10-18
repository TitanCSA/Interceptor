# C++ examples

Install the driver first, then run one capture example at a time as administrator.
The SDK opens its session automatically. Untouched input families are forwarded.

| Source / executable | Demonstration |
| --- | --- |
| `01_Echange_H_J` | Swap H and J, preserving presses and releases |
| `02_Ctrl_H_Hello` | Ctrl+H types `hello`, then restores modifier keys |
| `03_Inverse_Souris_Y` | Invert mouse Y |
| `04_Inverse_X_J_Boutons_HK` | Invert mouse X; J or X1/X2 holds H+K |
| `05_Clavier_Bloque_Affichage` | Display and intentionally block keyboard input |
| `06_Souris_Bloquee_Affichage` | Display and intentionally block mouse input |
| `07_Manette_HID_Inverse_X_Y` | Invert automatically decoded HID X/Y axes; optional `--verbose` samples |
| `08_Clavier_Minimal` | Minimal mixed-batch loop: H becomes J |

Examples 01–06 show a short countdown before capture. Release keys/buttons, then
focus the target application. The `hello` example uses common scan-code layouts;
it is not a universal text/IME injection mechanism.

Close the process to release capture. Physical **Ctrl+Alt+Delete** also revokes
capture and is useful when a blocking monitor suppresses ordinary shortcuts.
The demo's shared two-hour allowance starts when capture is first opened.

From the repository root, `./Build.ps1` builds the user-mode tools and examples,
using the supplied signed kernel binary without rebuilding it.
For only the SDK and minimal example:

```powershell
cmake -S sdk -B build/sdk -A x64
cmake --build build/sdk --config Release
```

[`common/Behaviors.h`](common/Behaviors.h) contains the example transformations.
[`common/Runner.cpp`](common/Runner.cpp)
handles initialization, input batches, forwarding, logging and termination.
See the [SDK guide](../../docs/API.md) and [installation guide](../../docs/INSTALLATION.md).
