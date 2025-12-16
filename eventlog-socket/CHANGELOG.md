# Revision history for eventlog-socket

## 0.1.1.0 -- 2025-12-15

* Added high-level API (`startWith` and `EventlogSocket`).
* Added support for TCP sockets (using `EventlogTcpSocket`).
* Added support for reading the configuration from environment variables (`startFromEnv`, `lookupEventlogSocket`, `lookupEventlogUnixSocket`, `lookupEventlogTcpSocket`, and `lookupWaitMode`).
* **FIX**: Test Unix domain socket path length (must be <108 characters).

## 0.1.0.0 -- 2023-04-14

* First version. Released on an unsuspecting world.
