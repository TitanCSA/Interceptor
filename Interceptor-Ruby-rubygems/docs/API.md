# Interceptor Ruby API

`Interceptor::Interceptor.open(library_path: nil) { |client| ... }`; or construct explicitly and call `close`. `initialize_devices`, `info`, `devices`, `receive(timeout: nil)`, `send(batch)`, `discard(batch)`, `cancel`, `descriptor(token, report: false)`, `inject(kind, state = {}, token: nil, report: nil)` expose the native C ABI.

Edit input fields through `event.field(name).value` or the unambiguous descriptor metadata in `event.fields`; set `event.blocked = true` to drop an input. Do not resize reports/fields or reuse stale/foreign batches. Partial failures expose `NativeError#code` and `#accepted`; retry sends only the remaining inputs. Sequence/timestamp use Ruby integers preserving unsigned 64-bit values. Cancellation works from another Ruby thread.

Supported families: keyboard, mouse, hid, xinput, xinputhid, gip. Unsupported injection raises Windows error 50. Driver installation and real capture are separate from package installation; original trial/license checks remain intact. The package was tested with its own synthetic backend and lazy production open/close. Native adapter source: `../Interceptor-C-ffi`.
