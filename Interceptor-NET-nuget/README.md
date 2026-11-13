# Interceptor .NET SDK

.NET 8+ Windows x64. Package ID: `Interceptor.SDK`; Installation: `dotnet add package Interceptor.SDK --version 1.0.0`.

```csharp
using TitanCSA.Products.Interceptor;
using var client = new Interceptor();
var info = client.GetInfo();
```

Native DLL bundled as runtime asset. No driver installation or capture on construction. [API](docs/API.md). Original licensing/trial prerequisites remain unchanged.
