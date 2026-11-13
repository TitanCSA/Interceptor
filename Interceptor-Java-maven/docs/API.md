# Interceptor Java API

`new Interceptor()` loads the bundled product DLL; `new Interceptor(Path)` chooses a compatible DLL explicitly. Always close the client or use try-with-resources.

Methods: `initialize`, `info`, `devices`, `receive()` / `receive(timeoutMs)`, `send(batch)`, `discard(batch)`, `cancel`, `descriptor(token, report)`, `inject(kind, state, token, report)`. Input scalar state is in `event.state.scalars`; HID groups contain editable Field objects. Set `event.blocked` to discard an event. Do not resize/reorder fields or reuse old/foreign batches.

`Interceptor.Error` exposes Windows `code` and partial `accepted` counts. A send retry forwards only the remaining events. Unsigned 64-bit sequence/timestamp values are represented by Java long bits: use `Long.toUnsignedString` or unsigned comparisons when necessary. `cancel()` may be called from another thread while a receive is blocked.

Supported families: keyboard, mouse, hid, xinput, xinputhid, gip. Unsupported injection returns code 50. Automated tests use a product-specific synthetic backend plus lazy production loading, not a live driver. The C ABI source is in `../Interceptor-C-ffi`.
