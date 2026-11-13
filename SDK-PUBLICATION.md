# SDK publication status

Current source layout and registry coverage. A GitHub download is distinct from publication on a central registry.

| Target | Status | Package / documentation |
| --- | --- | --- |
| CPP/vcpkg | published_custom_registry | interceptor |
| CPP/conan | public_recipe_builds_locally |  |
| CPP/nuget | paused_pending_owner_email_confirmation | Interceptor.Native |
| NET/nuget | binding_not_implemented |  |
| Python/pypi | binding_not_implemented |  |
| Python/conda-forge | binding_not_implemented |  |
| Rust/cargo | binding_not_implemented |  |
| JavaScript/npm | binding_not_implemented |  |
| Java/maven | binding_not_implemented |  |
| Java/jitpack | binding_not_implemented |  |
| Go/modules | binding_not_implemented |  |
| Go/pkg-go-dev | binding_not_implemented |  |
| PowerShell/gallery | binding_not_implemented |  |
| Dart/pub-dev | prepared_source_native_verified | `interceptor` [Source](Interceptor-Dart-pub-dev/) |
| Ruby/rubygems | binding_not_implemented |  |
| PHP/packagist | binding_not_implemented |  |

Native SDK source and CMake/Conan/vcpkg/NuGet recipes share [`packaging/native`](packaging/native/LAYOUT.md). Previous release archives and tags remain unchanged.

Application and driver prerequisites and original licensing remain those documented by each SDK. No driver installer is bundled with these source packages.
