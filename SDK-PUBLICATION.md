# SDK publication status

Current source layout and registry coverage. A GitHub download is distinct from publication on a central registry.

| Target | Status | Package / documentation |
| --- | --- | --- |
| CPP/vcpkg | published_custom_registry | interceptor |
| CPP/conan | published_custom_source_index | [interceptor](https://github.com/TitanCSA/sdk-packages/tree/089bc73095aa8eda89bacbe10d0b9e58394b0207/conan-index) [Source](packaging/native/) |
| CPP/nuget | paused_pending_owner_email_confirmation | Interceptor.Native |
| NET/nuget | built_verified_publication_on_hold_by_owner | Interceptor.SDK [Source](Interceptor-NET-nuget/) |
| Python/pypi | built_fixture_and_production_verified | interceptor-sdk [Source](Interceptor-Python-pypi/) |
| Python/conda-forge | custom_channel_install_verified_requires_conda_forge_review | [interceptor-sdk](https://raw.githubusercontent.com/TitanCSA/sdk-packages/089bc73095aa8eda89bacbe10d0b9e58394b0207/conda-channel) [Source](packaging/conda/) |
| Rust/cargo | published_install_verified | [interceptor-sdk](https://crates.io/crates/interceptor-sdk) [Source](Interceptor-Rust-cargo/) |
| JavaScript/npm | awaiting_browser_publication_authentication | [interceptor-sdk](https://www.npmjs.com/package/interceptor-sdk) [Source](Interceptor-JavaScript-npm/) |
| Java/maven | published_install_verified | [interceptor-sdk](https://central.sonatype.com/artifact/io.github.titancsa/interceptor-sdk/1.0.0) [Source](Interceptor-Java-maven/) |
| Java/jitpack | published_install_verified | [interceptor-sdk](https://jitpack.io/#TitanCSA/Interceptor/java-sdk-v1.0.0) [Source](Interceptor-Java-maven/) |
| Go/modules | published_install_verified | [interceptor](https://pkg.go.dev/github.com/TitanCSA/Interceptor/Interceptor-Go-modules@v1.0.0) [Source](Interceptor-Go-modules/) |
| Go/pkg-go-dev | indexed_verified | [Documentation](https://pkg.go.dev/github.com/TitanCSA/Interceptor/Interceptor-Go-modules@v1.0.0) |
| PowerShell/gallery | published_install_verified | [Interceptor](https://www.powershellgallery.com/packages/Interceptor/1.0.0) [Source](Interceptor-PowerShell-gallery/) |
| Dart/pub-dev | published_install_verified | [interceptor](https://pub.dev/packages/interceptor) [Source](Interceptor-Dart-pub-dev/) |
| Ruby/rubygems | published_install_verified | [interceptor-sdk](https://rubygems.org/gems/interceptor-sdk) [Source](Interceptor-Ruby-rubygems/) |
| PHP/packagist | composer_source_and_tag_published | [titancsa/interceptor](https://packagist.org/packages/titancsa/interceptor) |

Native SDK source and CMake/Conan/vcpkg/NuGet recipes share [`packaging/native`](packaging/native/LAYOUT.md). Previous release archives and tags remain unchanged.

Application and driver prerequisites and original licensing remain those documented by each SDK. No driver installer is bundled with these source packages.
