# Release notes

## v0.1.2

Corrected package documentation. Public headers are unchanged from v0.1.1.

## v0.1.1

Added MIT licensing, repository metadata, public-header declarations, and
package export rules. Public headers are unchanged from v0.1.0.

## v0.1.0

Initial release of the allocation-free C++11 store-and-forward core:

- validated upstream-to-endpoint route translation;
- visible and applied cache images;
- typed downstream requests, validation, and completions; and
- native behavior and route-lookup performance tests.

The library is an asynchronous cached core, not a transparent Modbus proxy.
Serial I/O, queues, scheduling, retries, and completion policy remain with the
application.
