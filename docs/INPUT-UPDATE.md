# Input update 1.4.16.0

The complete signed release includes the GUI and console installers, viewer,
console examples, source SDK and checksums in one place. Both installers bundle
Microsoft-signed kernel **1.4.16.0**. Install / reinstall, then use Windows
**Restart** if requested. The earlier client-only release bundled kernel 1.4.15.0.

The SDK keeps open HID, XInputHID and XUSB HID-view source files when their
SOURCE query temporarily returns Windows error 21. The all-family context
reserves the streams and the driver decides when each source can actually join.
Other ready sources continue working. No source polling, capture restart or
fake input is introduced. Native-parent bootstrap failures, unsupported
mandatory mirrors, access errors and trial expiry still retain their cause.

The viewer now preserves the failed operation, actual family/device/sequence
when known, Win32 and reset codes, forwarding totals and partial sends, with
a copyable diagnostic. Both 33 ms publication/display waits are removed.
Latest snapshots are displayed on available WPF frames with one pending UI
notification. There is no fixed application FPS cap or idle rendering loop.
Keyboard and mouse remain available alongside all four controller families.

The additional XUSB kernel fixes keep pending parents transparent until their
mandatory mirror is ready, initialize cached physical state on the persistent
System reader and validate its ordinary input wait before reporting readiness.
These changes are included in signed kernel 1.4.16.0; replacing user-mode files
cannot apply them to an older driver. No unsigned SYS or submission CAB is public.

Offline validation: actual native source opening and context-attempt fixtures
pass 90,112 checks across 10,000 lifecycles in Release and AddressSanitizer;
the previous implementation is rejected. Actual viewer transfer/diagnostic
fixtures pass 120,291 checks in both builds. The managed frame scheduler passes
60,019 checks, and all six families render offscreen. Kernel pending-source
and bootstrap fixtures pass 180,139 and 760,764 checks, respectively, including
10,000 cycles each in Release and AddressSanitizer. The production kernel
compiles with warnings as errors and passes static analysis; the CAB passes
local INF/catalog/symbol/content checks. These are software checks, not live
certification of every controller, end-to-end latency or display FPS.

Signed package verification confirms that all three returned INF files match
the submission and all three signed SYS images contain the submitted executable
body. Twelve Windows kernel-policy signature/catalog checks pass, including the
shared SYS against each catalog. Both installer EXEs embed the same verified
20-file runtime package and pass their offline integrity/self-tests. The demo
allowance remains two hours per Windows boot.
