# Interceptor cmdlets

Cmdlet prefix: `InterceptorSDK`. Commands: `Connect`, `Initialize`, `Get...Info`, `Get...Device`, `Receive...Input`, `Send...Batch`, `Remove...Batch`, `Stop...Wait`, `Get...Descriptor`, `Send...Input`, `Disconnect`. Discover exact names using `Get-Command -Module Interceptor`; each command includes help.

Receive returns a single strongly typed Batch object. Edit an input's scalar `State` fields or decoded HID fields, then forward the batch. `Blocked` drops one input. Do not resize or reuse stale/foreign batches. Sequence/timestamp use UInt64. Partial send exceptions expose Windows Code and Accepted counts; retry sends only remaining events. Cancellation may be called from another thread. Supported families: keyboard, mouse, hid, xinput, xinputhid, gip. Original native C ABI and SDK sources remain in the parent repository.

Automated validation uses a product-specific synthetic DLL and lazy production open/close; no live input was captured.
