{-# LANGUAGE CApiFFI #-}

{- |
Stream GHC eventlog events to external processes.
-}
module GHC.Eventlog.Socket (
    -- * High-level API
    EventlogSocket (..),
    startWith,
    startFromEnv,
    lookupEventlogSocket,
    lookupEventlogUnixSocket,
    lookupEventlogTcpSocket,
    lookupWaitMode,

    -- * Legacy API
    startWait,
    start,
    wait,
) where

import Control.Exception (Exception (..), throwIO)
import Control.Monad (when)
import Data.Foldable (traverse_)
import Data.Maybe (isJust)
import Data.Word (Word16)
import Foreign.C (CString, withCString)
import System.Environment (lookupEnv)
import Text.Read (readMaybe)

{- |
A type representing the supported eventlog socket modes.
-}
data EventlogSocket
    = EventlogUnixSocket
        { unixPath :: FilePath
        -- ^ Unix socket path, e.g., @"/tmp/ghc_eventlog.sock"@.
        --
        --         __Warning:__ Unix socket paths are limited to 107 characters.
        }
    | EventlogTcpSocket
        { inetHost :: String
        -- ^ TCP host or interface, e.g. @"127.0.0.1"@.
        , inetPort :: !Word16
        -- ^ TCP port, e.g., @4242@.
        }
    deriving (Show)

newtype UnixSocketPathTooLong = UnixSocketPathTooLong FilePath
    deriving (Show)

instance Exception UnixSocketPathTooLong where
    displayException (UnixSocketPathTooLong unixSocketPath) =
        "Unix domain socket paths are limited to 107 characters. \
        \Found path with "
            <> show (length unixSocketPath)
            <> " characters:\n"
            <> unixSocketPath

{- |
Read the `EventlogSocket` and wait mode from the environment.
If this succeeds, start an @eventlog-socket@ writer with that configuration.
See `lookupEventlogSocket` and `lookupWaitMode`.

@since 0.1.1.0
-}
startFromEnv :: IO ()
startFromEnv =
    lookupEventlogSocket
        >>= traverse_
            ( \eventlogSocket -> do
                startWith eventlogSocket
                lookupWaitMode >>= \waitMode ->
                    when waitMode wait
            )

{- |
Determine whether or not the eventlog socket should wait,
based on whether the @GHC_EVENTLOG_WAIT@ environment variable is set.
-}
lookupWaitMode :: IO Bool
lookupWaitMode = isJust <$> lookupEnv "GHC_EVENTLOG_WAIT"

{- |
Lookup the `EventlogSocket` from the environment.

This uses `lookupEventlogUnixSocket` and `lookupEventlogTcpSocket` with a bias towards Unix sockets.
-}
lookupEventlogSocket :: IO (Maybe EventlogSocket)
lookupEventlogSocket =
    lookupEventlogUnixSocket
        >>= maybe lookupEventlogTcpSocket (pure . Just)

{- |
Lookup the `UnixSocket` from the environment.

This reads the `unixSocketPath` from the @GHC_EVENTLOG_UNIX_PATH@ environment variable.
-}
lookupEventlogUnixSocket :: IO (Maybe EventlogSocket)
lookupEventlogUnixSocket =
    lookupEnv "GHC_EVENTLOG_UNIX_PATH"
        >>= traverse (pure . EventlogUnixSocket)

{- |
Lookup the `TcpSocket` from the environment.

This reads the host name from the @GHC_EVENTLOG_INET_HOST@ and the port number from the @GHC_EVENTLOG_INET_PORT@ environment variable.

__Warning:__ @GHC_EVENTLOG_INET_PORT@ must be number in the range @0-65535@.

@since 0.1.1.0
-}
lookupEventlogTcpSocket :: IO (Maybe EventlogSocket)
lookupEventlogTcpSocket =
    (,) <$> lookupEnv "GHC_EVENTLOG_INET_HOST" <*> lookupEnv "GHC_EVENTLOG_INET_PORT" >>= \case
        (Just tcpHost, Just tcpPortString)
            | Just tcpPort <- readMaybe tcpPortString ->
                pure . Just $ EventlogTcpSocket tcpHost tcpPort
        _otherwise -> pure Nothing

{- |
Start an @eventlog-socket@ writer using the given `EventlogSocket`.
If the second argument is `True`, the function waits for another process to connect to the eventlog socket.

@since 0.1.1.0
-}
startWith :: EventlogSocket -> IO ()
startWith = \case
    EventlogUnixSocket unixSocketPath -> do
        when (length unixSocketPath >= 108) $
            throwIO (UnixSocketPathTooLong unixSocketPath)
        withCString unixSocketPath $ \cUnixSocketPath ->
            eventlog_socket_start_unix cUnixSocketPath
    EventlogTcpSocket tcpHost tcpPort ->
        withCString tcpHost $ \hostPtr ->
            withCString (show tcpPort) $ \portPtr ->
                eventlog_socket_start_inet hostPtr portPtr

{- |
Start an @eventlog-socket@ writer on a Unix domain socket and wait.

@since 0.1.0.0
-}
startWait :: FilePath -> IO ()
startWait unixSocketPath =
    startWith (EventlogUnixSocket unixSocketPath) >> wait

{- |
Start an @eventlog-socket@ writer on a Unix domain socket.

@since 0.1.0.0
-}
start :: FilePath -> IO ()
start unixSocketPath =
    startWith (EventlogUnixSocket unixSocketPath)

{- |
Wait for another process to connect to the eventlog socket.

@since 0.1.0.0
-}
wait :: IO ()
wait = eventlog_socket_wait

foreign import capi safe "eventlog_socket.h eventlog_socket_start_unix"
    eventlog_socket_start_unix :: CString -> IO ()

foreign import capi safe "eventlog_socket.h eventlog_socket_start_inet"
    eventlog_socket_start_inet :: CString -> CString -> IO ()

foreign import capi safe "eventlog_socket.h eventlog_socket_wait"
    eventlog_socket_wait :: IO ()
