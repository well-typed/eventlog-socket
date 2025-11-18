# eventlog-socket

A library to send GHC's eventlog stream over a Unix domain socket.

## Getting Started


To use the code in this repository to profile your own application, follow these steps.

Add `eventlog-socket` to the `build-depends` for your application:

```cabal
executable my-app
  ...
  build-depends:
    ...
    , eventlog-socket  >=0.1.0 && <0.2
    ...
```

### Instrument your application

To instrument your application and allow the eventlog data to be streamed over a socket, all you have to do is call `GHC.Eventlog.Socket.start` with the path to your eventlog socket.

```haskell
module Main where

import           Data.Maybe (fromMaybe)
import qualified GHC.Eventlog.Socket
import           System.Environment (lookupEnv)

main :: IO ()
main = do
  putStrLn "Creating eventlog socket..."
  eventlogSocket <-
      fromMaybe "/tmp/ghc_eventlog.sock"
          <$> lookupEnv "GHC_EVENTLOG_SOCKET"
  GHC.Eventlog.Socket.start eventlogSocket
  ...
```

If you wish for your application to block until the client process connects to the eventlog socket, you can call `GHC.Eventlog.Socket.startWait`.

For Unix-domain usage, see [examples/fibber](examples/fibber/). For a TCP-based example, see [examples/fibber-tcp](examples/fibber-tcp/).

### Instrument your application from C

If you instrument your application from Haskell, the GHC RTS is started with the default file writer, and only swiches over to the socket writer once `GHC.Eventlog.Socket.start` is evaluated. This means that running your application will still create an initial eventlog file and that some events might be lost. To avoid this, you can instrument your application from C, by writing a custom C main file.

For an example of an application instrumented from a custom C main, see [examples/fibber-c-main](examples/fibber-c-main/).
The library also exposes `eventlog_socket_hs_main`, a helper that runs
`hs_init_ghc`, calls `eventlog_socket_ready`, and enters the scheduler with the
provided main closure so you don't have to reimplement the tail of `hs_main`.

## Protocol

The socket exposes a bidirectional protocol where the server streams the binary
eventlog payload (identical to what GHC writes to `*.eventlog` files) to the
client, while the client can optionally send control commands back on the same
connection. A few implementation details to be aware of when consuming the
stream:

- Each accepted connection receives the full eventlog header. The writer stops
  and restarts event logging when a new client connects so the header is always
  replayed before any events.
- The payload is the standard GHC eventlog format, so existing tooling such as
  [`ghc-events`](https://hackage.haskell.org/package/ghc-events) can parse the
  stream without additional framing.
- The writer keeps buffering events whenever the socket blocks; once the client
  disconnects the internal queue is cleared and the next client will again see
  a fresh stream that starts with the header.

### Control channel

The connection also acts as a control channel once the RTS is ready to respond
to commands. If you install the writer through `eventlog_socket_init_*` (as
shown in `examples/fibber-c-main`), make sure to call
`eventlog_socket_ready` after `hs_init_*` so that the control receiver starts
listening, or call `eventlog_socket_hs_main` which wraps those steps for you.

Control messages use a minimal framing:

```
magic: 0x47 0x43 0x54 0x4C  ("GCTL")
command id: single byte
```

Unknown commands are ignored for safety, while malformed frames leave the data
writer running so profiling continues uninterrupted. The currently supported
commands mirror the RTS heap profiling API:

| Command                          | Byte |
| -------------------------------- | ---- |
| `startHeapProfiling`             | 0x01 |
| `stopHeapProfiling`              | 0x02 |
| `requestHeapProfile` (one-shot)  | 0x03 |

For example, a simple Python client can request a sample like this:

```python
sock.sendall(b"GCTL" + bytes([0x01]))  # startHeapProfiling
...
sock.sendall(b"GCTL" + bytes([0x03]))  # requestHeapProfile
```

Garbage control traffic is ignored, so you can send commands opportunistically
without risking the eventlog feed.

# Running tests

The `./ci.sh` script can be executed to run the eventlog-socket tests.
