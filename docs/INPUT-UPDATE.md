# Input update 1.4.17.0

This release synchronizes the combined Interceptor product with the current
Interceptor Gamepad & Controller fixes, retaining keyboard and mouse support.

- XUSB input is intercepted at the USB layer beneath the native Windows driver.
  Windows retains its native controller views; a replacement HID mirror is not
  needed. Association migration removes only this product's obsolete bindings.
- HID polling reads the current physical state, including valid replies whose
  completion reports a zero Information count. Capture, reset and device removal
  preserve ownership and bounded request lifetimes.
- GIP keeps native protocol, Guide and fragmented metadata traffic unchanged
  when no capture owns the source. Closing an unused session does not alter it.
- Passive controller IN/OUT telemetry uses a separate bounded observer path.
  Observation alone does not claim input or start the two-hour capture allowance.
- The SDK keeps preparing sources available for later activation. Idle-start,
  cancellation, hotplug and detailed viewer failure reporting use the current
  controller implementation across supported families.
- Installers handle absent class-filter properties, product-owned migrations and
  an existing classless control endpoint. The classic keyboard/mouse mapper setup
  and recovery binaries remain included.

The two-hour per-boot demo limit is unchanged. The public library remains two
files, with automatic capture ownership and source tokens.

## Validation

Local tests for this release passed:

| Check | Scope |
| --- | --- |
| WDK Release and static-analysis builds | Combined kernel, all six input families |
| SDK Release and AddressSanitizer | 1,191,185 checks in each run; 100,000 idle receives and 10,000 pending-source lifecycles |
| Product policy | 105,858 checks covering family masks, ownership, hotplug and demo expiry |
| Transport and lifecycle fixtures | XUSB USB, HID polling/ownership and GIP passthrough; regression mutations rejected |
| Keyboard and mouse | Production mapper lifetimes, mouse report modes and input-buffer ownership |
| Passive monitor | AddressSanitizer and concurrent subscription/drain/close tests |
| User-mode clients | All eight console examples, viewer self-tests and diagnostic-worker self-test |
| Installers | GUI and console built from the same SDK and matching Microsoft-signed payload |
| Submission package | Three INFs, catalogs, symbols and extracted CAB hashes validated locally |
| Microsoft-signed return | Submitted executable body and all three INFs match; 12 kernel-policy signature/catalog checks pass |
| Dedicated Windows 11 VM | Final GUI install/completion and console reinstall/uninstall; exact active signed binary/API after normal restarts; clean removal, unchanged certificates and boot configuration |

Fixtures model OS boundaries and do not replace physical-device testing. No host
driver was installed or opened in this campaign. No physical latency measurement
or universal joy.cpl/browser/game compatibility claim is made for this release.

## Signing and installation

Both installers contain the verified **Microsoft-signed 1.4.17.0 driver**. The
complete release also includes its matching SDK, viewer and console examples.
Install/reinstall and use Windows Restart if requested; replacing only the viewer
cannot update the installed kernel.

See [synchronization and release contents](SYNCHRONIZATION.md).
