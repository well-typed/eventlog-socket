# Revision history for eventlog-socket

## 0.1.1.0 -- 2026-02-23

* Added high-level API (`startWith` with `EventlogSocketAddr` and `EventlogSocketOpts`).
* Added support for TCP sockets (via `EventlogSocketAddr`).
* Added support for setting the send buffer size (via `EventlogSocketOpts`).
* Added support for reading the configuration from environment variables (`fromEnv` and `startFromEnv`).
* Added public C API.
* Added support for control commands (if compiled with `+control`).
* Added support for custom control commands via C API.

## 0.1.0.0 -- 2023-04-14

* First version. Released on an unsuspecting world.
