# Rust API

`Interceptor::new`, `initialize`, `info`, `devices`, `receive(Option<u32>)`, `send(&mut Batch)`, `discard(&Batch)`, `descriptor`, `inject`, `cancel_handle`, `close`. Drop closes the session. A CancelHandle can be sent to a worker thread; call cancel to interrupt receive.

Edit `batch.inputs[n].state` scalars/axes/report fields or `blocked` before send. Keep field dimensions intact. Stale and foreign batches are rejected. Error has code/message/accepted for partial-send retries. Sequence/timestamp use u64. Unsupported families return Windows error 50. Input mask 3, controller mask 15.

Synthetic contract tests exercise product masks, edits, blocking, unsigned values, stale/partial sends and cancellation. Production validation only opens/closes an inert session.
