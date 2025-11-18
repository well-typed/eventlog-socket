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

For an example of an instrumented application, see [examples/fibber](examples/fibber/).

### Instrument your application from C

If you instrument your application from Haskell, the GHC RTS is started with the default file writer, and only swiches over to the socket writer once `GHC.Eventlog.Socket.start` is evaluated. This means that running your application will still create an initial eventlog file and that some events might be lost. To avoid this, you can instrument your application from C, by writing a custom C main file.

For an example of an application instrumented from a custom C main, see [examples/fibber-c-main](examples/fibber-c-main/).

# Running tests

The `./ci.sh` script can be executed to run the eventlog-socket tests.
