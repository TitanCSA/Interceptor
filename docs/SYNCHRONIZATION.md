# Combined driver synchronization

Version **1.4.17.0** combines keyboard/mouse capture with the controller fixes in
Interceptor Gamepad & Controller kernel **1.0.5.0**, SDK **1.0.4.2** and installer
revision **1.0.7.0**. Product names, service identity, device-interface identifiers
and supported-family masks remain those of the combined Interceptor product.

The public SDK is exactly `Interceptor/Interceptor.h` and `Interceptor.cpp`.
All eight console examples, the viewer and both installers use these same files.
Kernel source and private signing material are not published.

The Microsoft-signed return matches the submitted executable body and all three
INF files. Both installers embed the same verified signed driver package. Build
checks block substituting an older driver in this release.

The complete release contains both standalone installers, the standalone viewer,
the console/viewer archive, the source SDK archive and checksums. All examples use
the current SDK. Older releases are removed only after the complete new signed
release has been downloaded and verified. No previous binary is relabeled as this
version.
