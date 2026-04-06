# Revision history for eventlog-socket

## 0.1.3.0 -- 2026-03-27

- Add support for hooks that fire at certain points in the `eventlog-socket` lifecycle.

  This adds `registerHook` and `eventlog_socket_register_hook` to the Haskell and C APIs, respectively, as well as two kinds of hooks:
  - A _post-startEventLogging_ hook, which is triggered whenever event logging is started.
  - A _pre-endEventLogging_ hook, which is triggered whenever event logging is ended.

  These hooks are intended to give package writers the ability to add their own init events, which are resent event time that a new client connects. For instance, the following code registers a post-startEventLogging hook that greets every new client with a user message event:

  ```haskell
  registerHook HookPostStartEventLogging $
    traceEventIO "Hello, new user!"
  ```

  To avoid races, it is important that these hooks are registered *before* `eventlog-socket` is started.

- Add support for builtin `startProfiling` and `stopProfiling` control commands.

## 0.1.2.0 -- 2026-03-25

**Warning**: The C API exposed from this version contains breaking changes over the C API exposed from version 0.1.1.0. This is justified by the fact that the C API exposed from version 0.1.1.0 is broken and that version is deprecated and not known to be in use.

- **BREAKING**: Change `eventlog_socket_wrap_hs_main` to accept `EventlogSocketAddr` and `EventlogSocketOpts` and call `eventlog_socket_init`.
- **BREAKING**: Remove `eventlog_socket_attach_rts_config`.
- Fixed issue where `eventlog_socket_start` would ignore `eso_wait`.
- Fixed issue where `EventlogSocketWriter->writeEventLog` would drop data if there was no connection, resulting in a truncated eventlog.
- Fixed race condition between the calls to `endEventLogging` and `startEventLogging` that happened on startup and at first connection. If these restarts were interleaved, this could lead to the startup call to `endEventLogging` immediately detaching the event logger attached by the call to `startEventLogging` in the worker thread listening for incoming connections. This would cause the socket to immediately be closed and lead to a truncated eventlog. Furthermore, the startup call to `endEventLogging` happened unconditionally, which means that an event logger attached in a C main (as in `examples/fibber-c-main`) would be detached on the first connection, which would drop prior all events from the buffer. The new version does not call `endEventLogging` or `startEventLogging` on startup. This means that an event logger installed in a C main is able to capture events from before the first connection. It also means that the default event logger – which may be the file event logger – will be active until the first connection, rather than until `eventlog_socket_start` is called.
- Add `testWorkerStatus` and `testControlStatus` functions to Haskell API.
- Add `eventlog_socket_worker_status` and `eventlog_socket_control_status` to C API.

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
