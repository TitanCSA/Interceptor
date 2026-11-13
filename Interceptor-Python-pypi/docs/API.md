# Interceptor API

Create `Client(library=None)`, use it as a context manager, or call `close()`. The native library is bundled with the wheel. A custom library path or `INTERCEPTOR_NATIVE_PATH` may select a compatible product DLL.

Methods: `initialize()`, `info()`, `devices()`, `receive(timeout=None)`, `send(batch)`, `discard(batch)`, `cancel()`, `descriptor(token, report=False)`, `inject(kind, token=None, report=None, **state)`, `close()`.

`receive` returns an owned Python batch view over the current native receive storage. Edit `input.state`, HID field values/normalized values, fixed-length report bytes, or `input.blocked`, then `send`. Do not retain a batch across another receive/close, resize it or pass it to another client. Duplicate sends forward only the remaining unaccepted inputs. Error exceptions contain Windows `code` and partial `accepted` counts. Cancellation may be called from another thread. Sequence and timestamp use Python integers, preserving unsigned 64-bit values.

Supported input families: keyboard, mouse, hid, xinput, xinputhid, gip. Unsupported injection returns Windows error 50. `initialize` uses already installed driver packages and may require elevation; it is never called by construction or import. Live capture/injection requires the matching product driver. The automated tests use a synthetic backend and lazy production open/close only.

Native C ABI source is in `../Interceptor-C-ffi`; product C++ source in `../packaging/native` (or the organized local CPP SDK folder).
