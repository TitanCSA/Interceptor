# Validation scope - 1.4.22.0

The embedded driver and required demo clock carry valid Microsoft Windows
Hardware Compatibility Publisher signatures. Their executable bodies and INF
files match the frozen production candidates. Kernel catalog membership is
verified for every package, including the shared primary driver image.

The SDK, viewer and installers are rebuilt from this release's public sources.
Installer and viewer self-tests perform no installation or capture. The input
changes have passed extracted production-code regression, memory and stress
tests; AddressSanitizer checks those user-mode fixtures, not a running kernel.
An isolated Windows 10 VM exercised the combined candidate's keyboard, mouse
and generic HID sends. These bounded results are not physical device-to-game
latency measurements or proof of universal hardware compatibility.

An independent native Windows XUSB/ViGEm removal fault (Verifier C9/7) reproduced
with Interceptor absent. The unsafe virtual-XUSB benchmark scenario is disabled;
its removal crash is not claimed fixed by this driver. No zero-crash guarantee
or zero-latency claim is made.

The demo remains 3,600 seconds per product per Windows kernel boot, shared by
applications. The existing trial clock policy and expiry behavior are unchanged.
Use Windows Restart after setup requests it. Kernel source and signing material
are not distributed in this user-mode source SDK.
