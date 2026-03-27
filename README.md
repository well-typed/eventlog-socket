# eventlog-socket

The `eventlog-socket` package supports streaming the GHC eventlog over Unix domain and TCP/IP sockets.
It streams the GHC eventlog in the [standard binary format](https://downloads.haskell.org/ghc/latest/docs/users_guide/eventlog-formats.html), identical to the contents of a binary `.eventlog` file, which can be parsed using [ghc-events](https://hackage.haskell.org/package/ghc-events). Whenever a new client connects, the event header is repeated, which guarantees that the client receives important events such as [`RTS_IDENTIFIER`](https://downloads.haskell.org/ghc/latest/docs/users_guide/eventlog-formats.html#event-type-RTS_IDENTIFIER) and [`WALL_CLOCK_TIME`](https://downloads.haskell.org/ghc/latest/docs/users_guide/eventlog-formats.html#event-type-WALL_CLOCK_TIME).

When compiled with `+control` the `eventlog-socket` package also exposes a control command server over the socket, which receives and executes builtin or pre-registered commands, which allows you to dynamically enable, e.g., heap profiling from the external monitoring process. The `eventlog-socket-control` package provides utilities for constructing messages for the the control command protocol used by this feature.

## Getting Started

To use the code in this repository to profile your own application, follow these steps.

### Add `eventlog-socket` as a dependency

Add `eventlog-socket` to the `build-depends` section for your executable.

```cabal
executable my-app
  ...
  build-depends:
    ...
    , eventlog-socket  >=0.1 && <0.2
    ...
```

### Instrument your application from Haskell

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
> <a name="default-writer"></a>If you instrument your application from Haskell, the GHC RTS will start with the default eventlog writer, which is the file writer. This means that your application will write the first few events to a file called `my-app.eventlog`. Usually, this is not an issue, as all initialization events are repeated every time a new client connects to the socket. However, it may give you problems if your application is stored on a read-only filesystem, such as the Nix store. To change the file path, you can use the `-ol` RTS option. To disable the file writer entirely, use the `--null-eventlog-writer` RTS option. If it's important that you capture *all* of the GHC eventlog, you must [instrument your application from C](#instrument-your-application-from-c), so that the GHC RTS is started with the `eventlog-socket` writer.

### Instrument your application from C

The `eventlog-socket` package installs a C header file, `eventlog_socket.h`, which enables you to instrument your application from a C main function. There is one reason you may want to instrument your application from C, which is that you want to capture *all* of the GHC eventlog. If your application is instrumented from Haskell, it loses the first few events. See the note [above](#default-writer).

For an example of an application instrumented from a custom C main, see [`examples/fibber-c-main`](examples/fibber-c-main/).

### Configure your application to enable the eventlog

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

## Lifecycle Hooks

Every time a new client connects, the GHC RTS resends the eventlog header and initialisation events.
If you develop a package that communicates over the eventlog, you may find that your package *also* needs to resent certain initialisation events.
The `eventlog-socket` package provides lifecycle hooks for this purpose.
There are two supported hooks:

1. The *post-startEventLogging* hook triggers *after* each time event logging is started.
2. The *pre-endEventLogging* hook triggers *before* each time event logging is ended.

To register a handler for lifecycle hook, use the `registerHook` function.
For instance, the following snippet registers a hook that sends a user message event every time event logging is started.

```haskell
module Main where

import Data.Foldable (for_)
import Debug.Trace (traceEventIO)
import GHC.Eventlog.Socket (Hook (..), registerHook, start)
import System.Environment (lookupEnv)

main :: IO ()
main = do
  -- Register the greeting hook:
  registerHook HookPostStartEventLogging $
    traceEventIO "Hello, new user!"

  -- Start eventlog-socket on GHC_EVENTLOG_UNIX_PATH, if set:
  maybeUnixPath <- lookupEnv "GHC_EVENTLOG_UNIX_PATH"
  for_ maybeUnixPath $ \unixPath -> do
    putStrLn "Start eventlog-socket on " <> unixPath
    start unixPath

  -- The rest of your application:
  ...
```

> [!CAUTION]
> To avoid races, it is important that lifecycle hooks are registered *before* `eventlog-socket` is started.

> [!NOTE]
> The post-startEventLogging and pre-endEventLogging lifecycle hooks usually correspond to clients connecting and disconnecting, respectively.
> The only exception is if your application is instrumented from a C main, in which case the *first* post-startEventLogging hook triggers immediately after the GHC RTS is initialised.
> In this scenario, the first client *does not* trigger the post-startEventLogging hook, but subsequent clients *do*.

For an example of an application that registers lifecycle hooks using the Haskell API, see [`examples/fibber`](examples/fibber/).

For an example of an application that registers lifecycle hooks using the C API, see [`examples/fibber-c-main`](examples/fibber-c-main/).

## Control Commands

When compiled with the `+control` feature flag, the eventlog socket supports *control commands*. These are messages that can be *written to* the eventlog socket by the client to control the RTS or execute custom control commands.

The `eventlog-socket` package provides three built-in commands:

1. The `startHeapProfiling` control command starts heap profiling.
2. The `stopHeapProfiling` control command stops heap profiling.
3. The `requestHeapCensus` control command requests a single heap profile.

The heap profiling commands require that the RTS is initialised with one of the `-h` options. See [RTS options for heap profiling](https://downloads.haskell.org/ghc/latest/docs/users_guide/profiling.html#rts-options-heap-prof). If any heap profiling option is passed, heap profiling is started by default. To avoid this, pass the `--no-automatic-heap-samples` to the RTS *in addition to* the selected `-h` option, e.g., `./my-app +RTS -hT --no-automatic-heap-samples -RTS`.

The [`eventlog-socket-control`](eventlog-socket-control/) package provides utilities for creating control command protocol messages to write to the eventlog socket.
If you want to send control commands from a different programming language, see [Control Command Protocol](#control-command-protocol) for the specification of the binary control command protocol.

### Control Command Protocol

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

The built-in commands live in the `"eventlog-socket"` namespace and are numbered in order starting at 3.

1. The message for `startHeapProfiling` is `\xF0\x9E\x97\x8C\x00\x15eventlog-socket\x03`.
2. The message for `stopHeapProfiling` is `\xF0\x9E\x97\x8C\x00\x15eventlog-socket\x04`.
3. The message for `requestHeapCensus` is `\xF0\x9E\x97\x8C\x00\x15eventlog-socket\x05`.

For example, a simple Python client using the [`socket`](https://docs.python.org/3/library/socket.html) library could request a heap census as follows:

```python
def request_heap_census(sock):
  sock.sendall(b"\xF0\x9E\x97\x8C\x00\x15eventlog-socket\x05")
```

Any unknown control commands or incorrectly formatted messages are ignored, so you can send control commands without worry for crashing your application.

### Custom Control Commands

The `eventlog-socket` package supports custom control commands.
Custom control commands can be registered via both the Haskell and the C API.
Registration is a two-step process:

1.  Register a namespace. This must be a string of between 1 and 255 bytes. By convention, this should be your Haskell package name.
2.  Register each command. This must be a number between 1 and 255. By convention, this should be the first non-zero number that's still available for this namespace.

To register a namespace, use the `registerNamespace` function.
To register a custom command handler, use the `registerCommand` function.
For instance, the following snippet registers two custom commands under the `"greeters"` namespace which greet C and Haskell, respectively.

```haskell
module Main where

import Data.Foldable (for_)
import Debug.Trace (traceEventIO)
import GHC.Eventlog.Socket (Hook (..), registerHook, start)
import System.Environment (lookupEnv)

greeter :: String -> IO ()
greeter name =
  putStrLn $ "Hello, " <> name <> "!"

main :: IO ()
main = do
  -- Register the custom command handlers:
  myNamespace <- registerNamespace "greeters"
  registerCommand myNamespace (CommandId 1) (greeter "C")
  registerCommand myNamespace (CommandId 2) (greeter "Haskell")

  -- Start eventlog-socket on GHC_EVENTLOG_UNIX_PATH, if set:
  maybeUnixPath <- lookupEnv "GHC_EVENTLOG_UNIX_PATH"
  for_ maybeUnixPath $ \unixPath -> do
    putStrLn "Start eventlog-socket on " <> unixPath
    start unixPath

  -- The rest of your application:
  ...
```

> [!CAUTION]
> To avoid races, it is important that custom commands are registered *before* `eventlog-socket` is started.

For an example of an application instrumented with custom control commands using the Haskell API, see [`examples/custom-command`](examples/custom-command/).

For an example of an application instrumented with custom control commands using the C API, see [`examples/custom-command-c-main`](examples/custom-command-c-main/).
