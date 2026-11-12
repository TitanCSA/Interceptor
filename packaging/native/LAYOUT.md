# Native SDK

This directory contains the shared native SDK source and the CMake, Conan, vcpkg and NuGet metadata. Registry-specific metadata with differing contents is preserved under `registry-metadata/`.

Build with `cmake -S packaging/native -B build/native -A x64`, then `cmake --build build/native --config Release`. No kernel driver is installed.

Existing release archives and tags remain immutable. NuGet publication remains pending account verification.
