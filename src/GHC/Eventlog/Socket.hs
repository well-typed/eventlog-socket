{-# LANGUAGE CApiFFI #-}

-- |
-- Stream GHC eventlog events to external processes.
module GHC.Eventlog.Socket (
    -- * Configuration types
    EventlogSocketConfig(..),
    SocketMode(..),
    UnixSocket(..),
    TcpSocket(..),
    -- * High-level APIs
    startWith,
    startUnix,
    startUnixWait,
    startTcp,
    -- * Backwards-compatible helpers
    startWait,
    start,
    wait,
) where

import Data.Word (Word16)
import Foreign.C

-- | Typed configuration for starting the eventlog socket listener.
data EventlogSocketConfig = EventlogSocketConfig
    { escMode :: SocketMode  -- ^ Which socket family to use.
    , escWait :: Bool        -- ^ Whether to block until a client connects.
    }

-- | Select the socket type to listen on.
data SocketMode
    = SocketUnix UnixSocket
    | SocketTCP TcpSocket

-- | Wrapper for Unix-domain socket paths.
newtype UnixSocket = UnixSocket { unixSocketPath :: FilePath }

-- | Configuration for TCP listeners.
data TcpSocket = TcpSocket
    { tcpHost :: String   -- ^ Host or interface to bind, e.g. \"127.0.0.1\".
    , tcpPort :: Word16   -- ^ TCP port to bind.
    }

-- | Start listening using an explicit configuration.
startWith :: EventlogSocketConfig -> IO ()
startWith cfg =
    case escMode cfg of
        SocketUnix (UnixSocket path) ->
            withCString path $ \sockPath ->
                c_start_unix sockPath (escWait cfg)
        SocketTCP tcp ->
            withCString (tcpHost tcp) $ \hostPtr ->
                withCString (show (tcpPort tcp)) $ \portPtr ->
                    c_start_tcp hostPtr portPtr (escWait cfg)

-- | Start listening on a Unix-domain socket without blocking for clients.
startUnix :: UnixSocket -> IO ()
startUnix sock = startWith EventlogSocketConfig
    { escMode = SocketUnix sock
    , escWait = False
    }

-- | Start listening on a Unix-domain socket and block until a client connects.
startUnixWait :: UnixSocket -> IO ()
startUnixWait sock = startWith EventlogSocketConfig
    { escMode = SocketUnix sock
    , escWait = True
    }

-- | Start listening on a TCP socket without blocking.
startTcp :: TcpSocket -> IO ()
startTcp tcp = startWith EventlogSocketConfig
    { escMode = SocketTCP tcp
    , escWait = False
    }

-- | Backwards-compatible wrapper for Unix sockets that blocks for clients.
startWait :: FilePath -> IO ()
startWait = startUnixWait . UnixSocket

-- | Backwards-compatible wrapper for Unix sockets that does not block.
start :: FilePath -> IO ()
start = startUnix . UnixSocket

-- | Wait (block) until a client connects.
wait :: IO ()
wait = c_wait

foreign import capi safe "eventlog_socket.h eventlog_socket_start_unix"
    c_start_unix :: CString -> Bool -> IO ()

foreign import capi safe "eventlog_socket.h eventlog_socket_start_tcp"
    c_start_tcp :: CString -> CString -> Bool -> IO ()

foreign import capi safe "eventlog_socket.h eventlog_socket_wait"
    c_wait :: IO ()
