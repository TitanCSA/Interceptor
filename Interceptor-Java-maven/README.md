# Interceptor Java / Kotlin SDK

Maven: `io.github.titancsa:interceptor-sdk:1.0.0`

Requires Windows x64 and Java 22+. Launch with `--enable-native-access=ALL-UNNAMED`. Native FFM binding without JNI or external Java dependencies. The product DLL is bundled in an isolated resource path. Supported families: keyboard, mouse, hid, xinput, xinputhid, gip.

```java
import titancsa.products.interceptor.Interceptor;
try (var client = new Interceptor()) { System.out.println(client.info()); }
```

For Kotlin, use the same class with `.use { ... }`. Installing the SDK does not install or initialize a driver. Original licensing and driver requirements remain unchanged. See [API guide](docs/API.md).

## JitPack alternative

Add the Maven repository `https://jitpack.io`, then depend on `com.github.TitanCSA:Interceptor:java-sdk-v1.0.0`. Maven Central and JitPack share the same Java sources and native resources; only the build descriptor differs.
