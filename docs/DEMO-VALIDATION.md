# Signed demo validation - 1.4.21.0

The main driver and required `TitanDemoClock.sys` have valid Microsoft Windows
Hardware Compatibility Publisher signatures. Every returned executable body and
INF matches the prepared production candidate; kernel catalog membership was
checked before integration. Kernel source and signing credentials are private.

The allowance is **3,600 seconds per product per Windows kernel boot**, shared
across applications. First capture, controller monitor subscription or raw GIP
diagnostics start the Interceptor allowance; Vdevice starts on device creation.
Status queries do not start it. Closing, unloading or reinstalling the main driver
does not renew it. Expiry releases capture and restores physical passthrough;
Vdevice removes its demo devices. Use **Windows Restart** for another allowance.

Exact production duration and non-starting status queries were checked in an
isolated Windows 11 VM. Expiry, pending-reader cancellation and rejection of new
sessions were exercised using 45-second laboratory variants. Clock and trial
stress suites passed under AddressSanitizer; the prior Driver Verifier campaign
covered the same lifecycle mechanisms. The one-hour change does not change input
routing. The signed installers are additionally checked before publication.

Upgrading from the previous two-hour clock requires a normal **Windows Restart**.
An incompatible resident clock fails closed until that restart; reopening an app
or unplugging a controller does not replace the loaded clock.

These bounded tests are not a full one-hour wall-clock soak, exhaustive physical
device testing, a zero-latency claim or proof of absence of every defect. Older
signed releases already downloaded, kernel control and external VM rollback are
outside this protection boundary.
