# Interceptor Go SDK

`go get github.com/TitanCSA/Interceptor/Interceptor-Go-modules@v1.0.0`

Windows x64 and Go 1.22+. Pure syscall FFI without cgo or external modules; the product DLL is embedded and extracted to a SHA-256 checked user cache. Supported families: keyboard, mouse, hid, xinput, xinputhid, gip.

```go
import sdk "github.com/TitanCSA/Interceptor/Interceptor-Go-modules"
client, err := sdk.New()
if err != nil { panic(err) }
defer client.Close()
```

Methods: `Initialize`, `Info`, `Devices`, `Receive(timeoutMs)` (-1 infinite), `Send`, `Discard`, `Cancel`, `Descriptor`, `Inject`. Batch `Inputs` expose mutable scalar `State`, HID fields and `Blocked`. Do not reuse stale/foreign batches or resize reports. Partial errors expose `Code` and `Accepted`; send retries only remaining events. Sequence/timestamp are uint64. Cancel may interrupt Receive from another goroutine. Driver and original licensing remain separate and unchanged. Native DLL paths may be overridden explicitly with `New(path)`.
