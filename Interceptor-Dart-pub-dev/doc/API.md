# Interceptor API

`Interceptor.open({String? libraryPath})` returns `Future<Interceptor>`. Always `close()` after use. Methods: `initialize()`, `info()`, `devices()`, `receive({int? timeout})`, `send(batch)`, `discard(batch)`, `cancel()`, `descriptor(token, report: false)`, `inject(kind, state, token: ..., report: ...)`.

`Batch.events` is immutable in size/order; edit each event's `Field.value`, `Field.normalized`, fixed-size binary data, or `Input.blocked`. `field(name)` selects an unambiguous state field. Stale/foreign batches are rejected. `InterceptorException` contains Windows `code` and partial `accepted`. Repeating send forwards only the unaccepted remainder. Sequence and timestamp use `BigInt` and retain unsigned 64-bit values. Unsupported input families return error 50.

`CancellationToken token = client.cancellationToken` can be sent through `Isolate.spawn`/SendPort: `token.cancel()` from that isolate interrupts the owning client's blocked native receive. It does not allocate another capture client. A closed session token throws error 6 and cannot cancel a later session. Keep normal client operations inside its creating isolate; only the token is sendable.

Construction and lazy close do not initialize devices or install a driver. Tests use the product-specific synthetic backend, including cross-isolate cancellation. Live hardware capture/injection is not part of automated validation. Native source: `../Interceptor-C-ffi`.
