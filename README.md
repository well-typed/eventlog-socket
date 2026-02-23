# eventlog-socket

The `eventlog-socket` package supports streaming the GHC eventlog over Unix domain and TCP/IP sockets.
It streams the GHC eventlog in the [standard binary format](https://downloads.haskell.org/ghc/latest/docs/users_guide/eventlog-formats.html), identical to the contents of a binary `.eventlog` file, which can be parsed using [ghc-events](https://hackage.haskell.org/package/ghc-events). Whenever a new client connects, the event header is repeated, which guarantees that the client receives important events such as [`RTS_IDENTIFIER`](https://downloads.haskell.org/ghc/latest/docs/users_guide/eventlog-formats.html#event-type-RTS_IDENTIFIER) and [`WALL_CLOCK_TIME`](https://downloads.haskell.org/ghc/latest/docs/users_guide/eventlog-formats.html#event-type-WALL_CLOCK_TIME).

## Getting Started

To use the code in this repository to profile your own application, follow these steps.

### Add `eventlog-socket` as a dependency {#getting-started--build-depends}

Add `eventlog-socket` to the `build-depends` section for your executable.

```cabal
executable my-app
  ...
  build-depends:
    ...
    , eventlog-socket  >=0.1 && <0.2
    ...
```

### Instrument your application from Haskell {#getting-started--instrument-from-haskell}

If you want to stream the GHC eventlog over a Unix domain socket, all you have to do is call the `start` function from `GHC.Eventlog.Socket` with the path you'd like it to use for the socket.

```haskell
module Main where

import           Data.Foldable (for_)
import qualified GHC.Eventlog.Socket
import           System.Environment (lookupEnv)

main :: IO ()
main = do
  -- Start eventlog-socket on GHC_EVENTLOG_UNIX_PATH, if set:
  maybeUnixPath <- lookupEnv "GHC_EVENTLOG_UNIX_PATH"
  for_ maybeUnixPath $ \unixPath -> do
    putStrLn "Start eventlog-socket on " <> unixPath
    GHC.Eventlog.Socket.start unixPath

  -- The rest of your application:
  ...
```

If you also want your application to block until the client process connects to the Unix domain socket, you can use `startWait`.

For more detailed configuration options, including TCP/IP sockets, you should use the `startWith` function, which takes a socket address and socket options as its arguments.

> [!NOTE]
> On most platforms, Unix domain socket paths are limited to 107 characters or less.

> [!NOTE]
> <a name="default-writer"></a>If you instrument your application from Haskell, the GHC RTS will start with the default eventlog writer, which is the file writer. This means that your application will write the first few events to a file called `my-app.eventlog`. Usually, this is not an issue, as all initialization events are repeated every time a new client connects to the socket. However, it may give you problems if your application is stored on a read-only filesystem, such as the Nix store. To change the file path, you can use the `-ol` RTS option. To disable the file writer entirely, use the `--null-eventlog-writer` RTS option. If it's important that you capture *all* of the GHC eventlog, you must [instrument your application from C](#getting-started--instrument-from-c), so that the GHC RTS is started with the `eventlog-socket` writer.

### Instrument your application from C {#getting-started--instrument-from-c}

The `eventlog-socket` package installs a C header file, `eventlog_socket.h`, which enables you to instrument your application from a C main function. There are two reasons you may want to instrument your application from C:

1. You want to capture *all* of the GHC eventlog. If your application is instrumented from Haskell, it loses the first few events. See the note [above](#default-writer).

2. You want to register custom commands for use with the control command protocol. Custom commands are currently only supported from C. For details, see [control commands](#control-commands).

For an example of an application instrumented from a custom C main, see [`examples/fibber-c-main`](examples/fibber-c-main/).

For an example of an application instrumented with custom control commands, see [`examples/custom-command`](examples/custom-command/).

### Configure your application to enable the eventlog {#getting-started--configure}

To enable the eventlog, you must pass the `-l` RTS option. This can be done either at compile time or at runtime:

- To set the flag at compile time, add the following to the `ghc-options` section for your executable. See [Setting RTS options at compile time](https://downloads.haskell.org/ghc/latest/docs/users_guide/runtime_control.html#rts-opts-compile-time).

  ```cabal
  executable my-app
    ghc-options:
      ...
      -with-rtsopts="-l"
      ...
  ```

- To set the flag at runtime, call you application with explicit RTS options. This requires that the application is compiled with the `-rtsopts` GHC option. See [Setting RTS options on the command line](https://downloads.haskell.org/ghc/latest/docs/users_guide/runtime_control.html#setting-rts-options-on-the-command-line).

  ```sh
  ./my-app +RTS -l -RTS
  ```

## Control Commands {#control-commands}

When compiled with the `+control` feature flag, the eventlog socket supports *control commands*. These are messages that can be *written to* the eventlog socket by the client to control the RTS or execute custom control commands.

The `eventlog-socket` package provides three built-in commands:

1. The `startHeapProfiling` control command starts heap profiling.
2. The `stopHeapProfiling` control command stops heap profiling.
3. The `requestHeapCensus` control command requests a single heap profile.

The heap profiling commands require that the RTS is initialised with one of the `-h` options. See [RTS options for heap profiling](https://downloads.haskell.org/ghc/latest/docs/users_guide/profiling.html#rts-options-heap-prof). If any heap profiling option is passed, heap profiling is started by default. To avoid this, pass the `--no-automatic-heap-samples` to the RTS *in addition to* the selected `-h` option, e.g., `./my-app +RTS -hT --no-automatic-heap-samples -RTS`.

### Control Command Protocol {#control-commands--protocol}

The [`eventlog-socket-control`](eventlog-socket-control/) package provides all you need to create control commands to write to the eventlog socket.

A control command message starts with the magic byte sequence `0xF0` `0x9E` `0x97` `0x8C`, followed by the protocol version, followed by the namespace as a length-prefixed string, followed by the command ID byte. The following is an an ENBF grammar for control command messages:

```ebnf
(* control command protocol version 0 *)

message          = magic protocol-version namespace-len namespace command-id;
magic            = "0xF0" "0x9E" "0x97" "0x8C";
protocol-version = "0x00";
namespace-len    = byte;   (* must be between 0-255 *)
namespace        = {byte}; (* must be exactly namespace-len bytes *)
command-id       = byte;   (* commands are assigned numeric IDs *)
```

The built-in commands live in the `"eventlog-socket"` namespace and are numbered in order starting at 0.

1. The message for `startHeapProfiling` is `\xF0\x9E\x97\x8C\x00\x15eventlog-socket\x00`.
2. The message for `stopHeapProfiling` is `\xF0\x9E\x97\x8C\x00\x15eventlog-socket\x01`.
3. The message for `requestHeapCensus` is `\xF0\x9E\x97\x8C\x00\x15eventlog-socket\x02`.

For example, a simple Python client using the [`socket`](https://docs.python.org/3/library/socket.html) library could request a heap census as follows:

```python
def request_heap_census(sock):
  sock.sendall(b"\xF0\x9E\x97\x8C\x00\x15eventlog-socket\x02")
```

Any unknown control commands or incorrectly formatted messages are ignored, so you can send control commands without worry for crashing your application.

## Contributing

The `scripts` directory contains various scripts for contributors to this repository.

The `./scripts/test-haskell.sh` script runs the Haskell test suite, which is located in [`eventlog-socket-tests`](eventlog-socket-tests/).

The `./scripts/test-python.sh` scripts runs the Python test suite, which is located in [`eventlog-socket/tests`](eventlog-socket/tests/).
This test suite is not fully portable, and is primarily intended to be run on Linux machines.

The `./scripts/pre-commit.sh` script runs the pre-commit hooks, which contains various formatters and linters.

The `./scripts/build-doxygen.sh` and `./scripts/build-haddock.sh` scripts build the documentation for the C and Haskell APIs.
