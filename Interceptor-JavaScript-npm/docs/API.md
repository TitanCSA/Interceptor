# Node API

`new Interceptor()`, `initialize`, `info`, `devices`, `await receive(timeout = null)`, `send(batch)`, `discard(batch)`, `descriptor(token, report = false)`, `inject({kind,state,token,report})`, `cancel`, `close`. TypeScript declarations are bundled.

`receive` runs in a native async worker so the JS event loop remains available for cancellation. Edit each input state or blocked flag; keep the frozen batch identity and field dimensions. Stale/foreign batches are rejected. Native errors expose code and accepted for partial retries. Sequence/timestamp and session tickets use BigInt, preserving unsigned 64-bit data. Unsupported input families return error 50. Close cancels pending receive; no driver is installed by npm.

Synthetic backend checks product masks, edits, blocking, stale batches, retries and async cancellation; production checks only inert open/close. Sources are in this folder plus the product C-ffi folder.
