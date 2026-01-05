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

    -- * Low-level API
    EventlogSocketOpts (soDebug, soBroadcast, soReuseAddr, soKeepAlive, soLinger, soOoBInline, soSndBuf, soRcvBug, soDontRoute, soSndLoWat, soSndTimeO, soRcvLoWat, soRcvTimeO),
    defaultEventlogSocketOpts,

    -- * Legacy API
    startWait,
    start,
    wait,
) where

import Control.Exception (Exception (..), throwIO)
import Control.Monad (when)
import Data.Foldable (traverse_)
import Data.Int (Int64)
import Data.Maybe (isJust)
import Data.Word (Word16)
import Foreign.C (CString, withCString)
import System.Environment (lookupEnv)
import Text.Read (readMaybe)

--------------------------------------------------------------------------------
-- High-level API
--------------------------------------------------------------------------------

{- |
A type representing the supported eventlog socket modes.
-}
data EventlogSocket
    = EventlogUnixSocket
        { unixSocketPath :: FilePath
        {- ^ Unix socket path, e.g., @"/tmp/ghc_eventlog.sock"@.

        __Warning:__ Unix socket paths are limited to 107 characters.
        -}
        }
    | EventlogTcpSocket
        { tcpHost :: String
        -- ^ TCP host or interface, e.g. @"127.0.0.1"@.
        , tcpPort :: !Word16
        -- ^ TCP port, e.g., @4242@.
        }
    deriving (Show)

{- |
Error thrown when the Unix socket path exceeds 107 characters.
-}
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
            ( \eventlogSocket ->
                lookupWaitMode >>= startWith eventlogSocket
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

This reads the `unixSocketPath` from the @GHC_EVENTLOG_UNIX_SOCKET@ environment variable.
-}
lookupEventlogUnixSocket :: IO (Maybe EventlogSocket)
lookupEventlogUnixSocket =
    lookupEnv "GHC_EVENTLOG_UNIX_SOCKET"
        >>= traverse (pure . EventlogUnixSocket)

{- |
Lookup the `TcpSocket` from the environment.

This reads the `tcpHost` from the @GHC_EVENTLOG_TCP_HOST@ and the `tcpPort` from the @GHC_EVENTLOG_TCP_PORT@ environment variable.

__Warning:__ @GHC_EVENTLOG_TCP_PORT@ must be number in the range @0-65535@.

@since 0.1.1.0
-}
lookupEventlogTcpSocket :: IO (Maybe EventlogSocket)
lookupEventlogTcpSocket =
    (,) <$> lookupEnv "GHC_EVENTLOG_TCP_HOST" <*> lookupEnv "GHC_EVENTLOG_TCP_PORT" >>= \case
        (Just tcpHost, Just tcpPortString)
            | Just tcpPort <- readMaybe tcpPortString ->
                pure . Just $ EventlogTcpSocket tcpHost tcpPort
        _otherwise -> pure Nothing

{- |
Start an @eventlog-socket@ writer using the given `EventlogSocket`.
If the second argument is `True`, the function waits for another process to connect to the eventlog socket.

@since 0.1.1.0
-}
startWith :: EventlogSocket -> Bool -> IO ()
startWith eventlogSocket shouldWait =
    case eventlogSocket of
        EventlogUnixSocket unixSocketPath -> do
            when (length unixSocketPath >= 108) $
                throwIO (UnixSocketPathTooLong unixSocketPath)
            withCString unixSocketPath $ \cUnixSocketPath ->
                c_start_unix cUnixSocketPath shouldWait
        EventlogTcpSocket tcpHost tcpPort ->
            withCString tcpHost $ \hostPtr ->
                withCString (show tcpPort) $ \portPtr ->
                    c_start_tcp hostPtr portPtr shouldWait

--------------------------------------------------------------------------------
-- Low-level API
--------------------------------------------------------------------------------

{- |
Options for Unix sockets.

Construct using `defaultEvenlogSocketOpts` and set options using field update:

- @`soDebug` :: `Maybe` `Bool`@
- @`soReuseAddr` :: `Maybe` `Bool`@
- @`soKeepAlive` :: `Maybe` `Bool`@
- @`soLinger` :: `Maybe` `Int`@
- @`soOoBInline` :: `Maybe` `Bool`@
- @`soSndBuf` :: `Maybe` `Int`@
- @`soRcvBug` :: `Maybe` `Int`@
- @`soDontRoute` :: `Maybe` `Bool`@
- @`soSndLoWat` :: `Maybe` `Int`@
- @`soSndTimeO` :: `Maybe` `Int64`@
- @`soRcvLoWat` :: `Maybe` `Int`@
- @`soRcvTimeO` :: `Maybe` `Int64`@

For a detailed description of the socket options, see [Use of Options](https://pubs.opengroup.org/onlinepubs/9699919799/).
-}
data EventlogSocketOpts = EventlogSocketOpts
    { soDebug :: !(Maybe Bool)
    -- ^ Enable recording of debugging information.
    , soBroadcast :: !(Maybe Bool)
    -- ^ Enable broadcast.
    , soReuseAddr :: !(Maybe Bool)
    -- ^ Reuse local addresses, if supported by the protocol.
    , soKeepAlive :: !(Maybe Bool)
    -- ^ Keep connections active, if supported by the protocol.
    , soLinger :: !(Maybe Int)
    -- ^ Linger time, in seconds.
    , soOoBInline :: !(Maybe Bool)
    -- ^ Leave received out-of-band data inline.
    , soSndBuf :: !(Maybe Int)
    -- ^ Send buffer size in bytes.
    , soRcvBug :: !(Maybe Int)
    -- ^ Receive buffer size in bytes.
    , soDontRoute :: !(Maybe Bool)
    {- ^ Request that outgoing messages bypass the standard routing facilities.
    The effect, if any, depends on the protocol.
    -}
    , soSndLoWat :: !(Maybe Int)
    -- ^ Send buffer "low water mark" in bytes.
    , soSndTimeO :: !(Maybe Int64)
    -- ^ Send buffer timeout in microseconds.
    , soRcvLoWat :: !(Maybe Int)
    -- ^ Receive buffer "low water mark" in bytes.
    , soRcvTimeO :: !(Maybe Int64)
    -- ^ Send buffer timeout in microseconds.
    }

{- |
Default options for Unix sockets.
-}
defaultEventlogSocketOpts :: EventlogSocketOpts
defaultEventlogSocketOpts =
    EventlogSocketOpts
        { soDebug = Nothing
        , soBroadcast = Nothing
        , soReuseAddr = Nothing
        , soKeepAlive = Nothing
        , soLinger = Nothing
        , soOoBInline = Nothing
        , soSndBuf = Nothing
        , soRcvBug = Nothing
        , soDontRoute = Nothing
        , soSndLoWat = Nothing
        , soSndTimeO = Nothing
        , soRcvLoWat = Nothing
        , soRcvTimeO = Nothing
        }

--------------------------------------------------------------------------------
-- Legacy API
--------------------------------------------------------------------------------

{- |
Start an @eventlog-socket@ writer on a Unix domain socket and wait.

@since 0.1.0.0
-}
startWait :: FilePath -> IO ()
startWait unixSocketPath =
    startWith (EventlogUnixSocket unixSocketPath) True

{- |
Start an @eventlog-socket@ writer on a Unix domain socket.

@since 0.1.0.0
-}
start :: FilePath -> IO ()
start unixSocketPath =
    startWith (EventlogUnixSocket unixSocketPath) False

{- |
Wait for another process to connect to the eventlog socket.

@since 0.1.0.0
-}
wait :: IO ()
wait = c_wait

--------------------------------------------------------------------------------
-- Foreign functions
--------------------------------------------------------------------------------

foreign import capi safe "eventlog_socket.h eventlog_socket_start_unix"
    c_start_unix :: CString -> Bool -> IO ()

foreign import capi safe "eventlog_socket.h eventlog_socket_start_tcp"
    c_start_tcp :: CString -> CString -> Bool -> IO ()

foreign import capi safe "eventlog_socket.h eventlog_socket_wait"
    c_wait :: IO ()
