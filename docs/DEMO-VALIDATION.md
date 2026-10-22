# Signed demo validation - 1.4.20.0

This release includes the Microsoft-signed main driver and `TitanDemoClock.sys`.
The returned executable bodies and INF files match the production candidates
validated in an isolated Windows 11 VM. Catalog membership and Microsoft kernel
signatures were checked for all returned packages. No kernel source is distributed.

The four-product validation campaign covered 1,278,238 offline assertions with
AddressSanitizer, Driver Verifier, expiry, reload/reinstall persistence, administrator
and SYSTEM access rejection, and concurrent API requests. Vdevice additionally
completed 1,000 rapid create/delete cycles and 100 early owner-close cycles with
zero remaining driver-attributed tracked allocations in those tests.

The allowance remains 7,200 seconds per product per Windows kernel boot. First
capture starts it; controller monitor subscription and raw GIP diagnostics also
count as use. Vdevice starts on virtual-device creation. Metadata/status queries
do not start it. At expiry, protected operations stop; physical Interceptor input
returns to passthrough. Windows Restart provides a new allowance.

These bounded tests do not establish zero latency, universal hardware support,
absence of all possible defects, or protection against arbitrary kernel control,
external VM rollback, or older signed driver versions already distributed.
