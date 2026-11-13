# Interceptor PowerShell SDK

`Install-Module Interceptor -Scope CurrentUser`

Requires PowerShell 7.4+ on Windows x64. Supported families: keyboard, mouse, hid, xinput, xinputhid, gip. The native product SDK and license/trial behavior are unchanged. No driver is installed on import or construction.

```powershell
Import-Module Interceptor
$client = Connect-InterceptorSDK
try { Get-InterceptorSDKInfo -Client $client }
finally { Disconnect-InterceptorSDK -Client $client }
```

See [API guide](docs/API.md) and `Get-Command -Module Interceptor`. The matching driver is installed separately.
