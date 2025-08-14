# eventlog-socket

A library to send GHC's eventlog stream over a Unix domain socket.

## Getting Started


To use the code in this repository to profile your own application, follow these steps.

### Add `eventlog-socket` as a dependency

The `eventlog-socket` package is not yet published on Hackage, so you must add it to your `cabal.project` file as a source repository package:

```cabal
source-repository-package
  type:     git
  location: https://github.com/well-typed/eventlog-socket
  tag:      LATEST_COMMIT_HASH
```

Then add `eventlog-socket` to the `build-depends` for your application:

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

For an example of an instrumented application, see [examples/fibber](examples/fibber/).

### Instrument your application from C

If you instrument your application from Haskell, the GHC RTS is started with the default file writer, and only swiches over to the socket writer once `GHC.Eventlog.Socket.start` is evaluated. This means that running your application will still create an initial eventlog file and that some events might be lost. To avoid this, you can instrument your application from C, by writing a custom C main file.

For an example of an application instrumented from a custom C main, see [examples/fibber-c-main](examples/fibber-c-main/).

## Design notes

This is a prototype to play around with the possibility of using the eventlog
for realtime profiling and performance analysis.
There are still numerous open questions:

- Access control?
- Support only Unix domain sockets or also TCP/IP?
- Do we want to support multiple consumers?
- What should happen when a consumer disconnects?
  At the moment, we pause the eventlog stream until a new consumer shows up.
  Alternatively, we could:
  - Close the socket and stop streaming.
  - Pause the program until a new consumer shows up.
  - Kill the program.

## Development

As the most code is C using following line will speedup development
considerably (change your GHC installation path accordingly):

```sh
gcc -c -Iinclude -I/opt/ghc/9.0.1/lib/ghc-9.0.1/include -o eventlog_socket.o cbits/eventlog_socket.c
gcc -c -Iinclude -I/opt/ghc/9.2.0.20210821/lib/ghc-9.2.0.20210821/include -o eventlog_socket.o cbits/eventlog_socket.c
```
