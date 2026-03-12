# Revision history for eventlog-socket

## 0.1.1.1 -- 2026-03-12

- Fixed issue where `eventlog_socket_start` would ignore `eso_wait`.
- Fixed race condition between the calls to `endEventLogging` and `startEventLogging` that happened on startup and at first connection. If these restarts were interleaved, this could lead to the startup call to `endEventLogging` immediately detaching the event logger attached by the call to `startEventLogging` in the worker thread listening for incoming connections. This would cause the socket to immediately be closed and lead to a truncated eventlog. Furthermore, the startup call to `endEventLogging` happened unconditionally, which means that an event logger attached in a C main (as in `examples/fibber-c-main`) would be detached on the first connection, which would drop prior all events from the buffer. The new version does not call `endEventLogging` or `startEventLogging` on startup. This means that an event logger installed in a C main is able to capture events from before the first connection. It also means that the default event logger – which may be the file event logger – will be active until the first connection, rather than until `eventlog_socket_start` is called.

## 0.1.1.0 -- 2026-02-23

- Added high-level API (`startWith` with `EventlogSocketAddr` and `EventlogSocketOpts`).
- Added support for TCP sockets (via `EventlogSocketAddr`).
- Added support for setting the send buffer size (via `EventlogSocketOpts`).
- Added support for reading the configuration from environment variables (`fromEnv` and `startFromEnv`).
- Added public C API.
- Added support for control commands (if compiled with `+control`).
- Added support for custom control commands.

## 0.1.0.0 -- 2023-04-14

- First version. Released on an unsuspecting world.
