# Interceptor native C ABI

Direct adapter to this product's original SDK. Input mask: 3; controller mask: 15. Restrictions are enforced by the unchanged SDK header and the adapter. No driver is installed during build or lazy open/close.

Build: `cmake -S . -B build -A x64`, then `cmake --build build --config Release`. Synthetic tests: configure with `-DFAMILY_TEST_BACKEND=ON`, build, then `ctest --test-dir build -C Release`.

The C ABI provides lazy sessions, initialize, status, devices, receive/edit/send/discard, cancellation, descriptors and injection. Returned receive views are borrowed until the next receive/close; copy before concurrent operations. One native session per loaded product DLL. Errors include accepted counts for partial sends; retry forwards only unaccepted events. Native product licenses and driver prerequisites remain unchanged. Unsupported input families return ERROR_NOT_SUPPORTED. The matching driver must be installed separately by the application owner.
