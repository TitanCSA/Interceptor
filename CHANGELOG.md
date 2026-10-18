# Changelog

## 1.4.15.0 — Demo release

- Microsoft-signed x64 driver package for Windows 10 and 11.
- Keyboard, mouse, generic HID controller, XUSB, XInputHID and GIP USB input paths
  exposed through one C++17 SDK.
- Automatic capture session, mixed input batches and type/count inference in
  `Send_Input`.
- Dynamic descriptor-driven HID controller state, including repeated axes,
  triggers, hats, buttons, other scalar values and binary fields.
- Capture-scoped device tokens, common descriptor queries and raw HID report
  descriptor access where the Windows transport permits it.
- Standalone graphical and console installers sharing the same install,
  association, restart-continuation and uninstall engine.
- Automatic per-device diagnostic window with progress, detailed results,
  copy/export and explicit unverified findings.
- Eight console examples and the graphical input viewer, with user-mode source.
- Reduced SDK buffer allocations and queue-copy overhead; immediate input
  forwarding with separately batched display updates.
- Two-hour demo allowance shared across applications per Windows boot. Expiration
  revokes capture/injection and leaves physical input passing normally. Windows
  Restart provides a new allowance.

The kernel driver source is not part of this public repository. The published
driver is a proprietary demo binary; the user-mode library and tools have their
separate source license. For the full version without the time limit, DM
**`titancsa`** on Discord.
