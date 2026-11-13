# .NET API

`Interceptor` implements IDisposable: New/constructor, Initialize, Info, Devices, Receive(timeout), Send(batch), Discard(batch), GetDescriptor, Inject, Cancel, Close/Dispose. Receive is synchronous; run on a worker task and call Cancel from another thread. Edit each input State or Blocked. Batch identity and field dimensions must remain intact. Stale and foreign batches are rejected; InterceptorException exposes Code and Accepted for partial retries. UInt64 preserves sequence/timestamp bits. Input mask 3, controller mask 15. Unsupported families return Windows error 50.

The own-product synthetic backend verifies edits, blocking, stale batches, partial send retries and cancellation. Production checks lazy open/close only.
