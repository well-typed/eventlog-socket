{-# LANGUAGE CApiFFI #-}
{-# LANGUAGE GeneralizedNewtypeDeriving #-}
{-# LANGUAGE PatternSynonyms #-}

module GHC.Eventlog.Socket.CApi (
    -- * Core API
    eventlogSocketStartWith,
    eventlogSocketWait,

    -- * Configuration types
    EventlogSocketAddr (..),
    peekEventlogSocketAddr,
    withEventlogSocketAddr,
    EventlogSocketOpts (esoWait, esoSndbuf),
    peekEventlogSocketOpts,
    withEventlogSocketOpts,
    defaultEventlogSocketOpts,

    -- * Configuration via environment
    eventlogSocketFromEnv,
    eventlogSocketEnvUnixPath,
    eventlogSocketEnvInetHost,
    eventlogSocketEnvInetPort,
    eventlogSocketEnvWait,

    -- * Errors
    EventlogSocketAddrInvalid(..),
    ) where

import Control.Exception (Exception (..), bracket, bracket_, throwIO)
import Control.Monad((<=<), when)
import Data.Function ((&))
import Data.Int (Int32)
import Data.Maybe (fromMaybe)
import Data.Word (Word8, Word32)
import Foreign.C (CBool (..))
import Foreign.C.String (CString, peekCString, withCString)
import Foreign.Ptr (Ptr, castPtr, plusPtr)
import Foreign.Storable (Storable (..))
import Foreign.Marshal.Alloc (allocaBytes)
import Foreign.Marshal.Utils (fromBool, toBool, with)
import GHC.Enum (toEnumError)
import System.IO.Unsafe (unsafePerformIO)

#include <eventlog_socket.h>

--------------------------------------------------------------------------------
-- High-level API
--------------------------------------------------------------------------------

eventlogSocketStartWith ::
    EventlogSocketAddr ->
    EventlogSocketOpts ->
    IO ()
eventlogSocketStartWith esa eso =
    withEventlogSocketAddr esa $ \esaPtr ->
    withEventlogSocketOpts eso $ \esoPtr ->
        eventlog_socket_start esaPtr esoPtr

--------------------------------------------------------------------------------
-- Legacy API
--------------------------------------------------------------------------------

eventlogSocketWait :: IO ()
eventlogSocketWait = eventlog_socket_wait

foreign import capi safe "eventlog_socket.h eventlog_socket_wait"
    eventlog_socket_wait :: IO ()

--------------------------------------------------------------------------------
-- Configuration types
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
-- EventlogSocketTag

newtype
    {-# CTYPE "eventlog_socket.h" "EventlogSocketTag" #-}
    EventlogSocketTag = EventlogSocketTag
        { unEventlogSocketTag :: #{type EventlogSocketTag}
        }
        deriving (Eq, Show, Storable)

pattern EVENTLOG_SOCKET_UNIX :: EventlogSocketTag
pattern EVENTLOG_SOCKET_UNIX = EventlogSocketTag #{const EVENTLOG_SOCKET_UNIX}

pattern EVENTLOG_SOCKET_INET :: EventlogSocketTag
pattern EVENTLOG_SOCKET_INET = EventlogSocketTag #{const EVENTLOG_SOCKET_INET}

{-# COMPLETE
    EVENTLOG_SOCKET_UNIX,
    EVENTLOG_SOCKET_INET #-}

--------------------------------------------------------------------------------
-- EventlogSocketAddr

{- |
A type representing the supported eventlog socket modes.
-}
data
    {-# CTYPE "eventlog_socket.h" "EventlogSocketAddr" #-}
    EventlogSocketAddr = EventlogSocketUnixAddr
        { esaUnixPath :: FilePath
        -- ^ Unix socket path, e.g., @"/tmp/ghc_eventlog.sock"@.
        --
        --         __Warning:__ Unix socket paths are limited to 107 characters.
        }
    | EventlogSocketInetAddr
        { esaInetHost :: String
        -- ^ TCP host or interface, e.g. @"127.0.0.1"@.
        , esaInetPort :: String
        -- ^ TCP port, e.g., @"4242"@.
        }
    deriving (Eq, Show)

peekEventlogSocketAddr ::
    Ptr EventlogSocketAddr ->
    IO EventlogSocketAddr
peekEventlogSocketAddr esaPtr =
    #{peek EventlogSocketAddr, esa_tag} esaPtr >>= \case
        EVENTLOG_SOCKET_UNIX -> do
            esaUnixPath <-
                peekCString <=< peek
                    $ esaPtr
                    & #{ptr EventlogSocketAddr, esa_unix_addr}
                    & #{ptr EventlogSocketUnixAddr, esa_unix_path}
            pure EventlogSocketUnixAddr
                { esaUnixPath = esaUnixPath
                }
        EVENTLOG_SOCKET_INET -> do
            esaInetHost <-
                peekCString <=< peek
                    $ esaPtr
                    & #{ptr EventlogSocketAddr, esa_inet_addr}
                    & #{ptr EventlogSocketInetAddr, esa_inet_host}
            esaInetPort <-
                peekCString <=< peek
                    $ esaPtr
                    & #{ptr EventlogSocketAddr, esa_inet_addr}
                    & #{ptr EventlogSocketInetAddr, esa_inet_port}
            pure EventlogSocketInetAddr
                { esaInetHost = esaInetHost
                , esaInetPort = esaInetPort
                }

withEventlogSocketAddr ::
    EventlogSocketAddr ->
    (Ptr EventlogSocketAddr -> IO a) ->
    IO a
withEventlogSocketAddr esa action =
    case esa of
        EventlogSocketUnixAddr{esaUnixPath = esaUnixPath} ->
            allocaBytes #{size EventlogSocketAddr} $ \esaPtr -> do
                #{poke EventlogSocketAddr, esa_tag} esaPtr EVENTLOG_SOCKET_UNIX
                withCString esaUnixPath $ \esaUnixPathCString -> do
                    flip poke esaUnixPathCString
                        $ esaPtr
                        & #{ptr EventlogSocketAddr, esa_unix_addr}
                        & #{ptr EventlogSocketUnixAddr, esa_unix_path}
                    action esaPtr
        EventlogSocketInetAddr{esaInetHost = esaInetHost, esaInetPort = esaInetPort} ->
            allocaBytes #{size EventlogSocketAddr} $ \esaPtr -> do
                #{poke EventlogSocketAddr, esa_tag} esaPtr EVENTLOG_SOCKET_INET
                withCString esaInetHost $ \esaInetHostCString -> do
                    flip poke esaInetHostCString
                        $ esaPtr
                        & #{ptr EventlogSocketAddr, esa_inet_addr}
                        & #{ptr EventlogSocketInetAddr, esa_inet_host}
                    withCString esaInetPort $ \esaInetPortCString -> do
                        flip poke esaInetPortCString
                            $ esaPtr
                            & #{ptr EventlogSocketAddr, esa_inet_addr}
                            & #{ptr EventlogSocketInetAddr, esa_inet_port}
                        action esaPtr

foreign import capi safe "eventlog_socket.h eventlog_socket_addr_free"
    eventlog_socket_addr_free ::
        Ptr EventlogSocketAddr ->
        IO ()

--------------------------------------------------------------------------------
-- EventlogSocketOpts

data
    {-# CTYPE "eventlog_socket.h" "EventlogSocketOpts" #-}
    EventlogSocketOpts = EventlogSocketOpts
        { esoWait :: Bool
        , esoSndbuf :: Maybe #{type int}
        }
    deriving (Eq, Show)

peekEventlogSocketOpts ::
    Ptr EventlogSocketOpts ->
    IO EventlogSocketOpts
peekEventlogSocketOpts esoPtr = do
    esoWait <- toBool . CBool <$> #{peek EventlogSocketOpts, eso_wait} esoPtr
    esoSndbuf <- #{peek EventlogSocketOpts, eso_sndbuf} esoPtr
    pure EventlogSocketOpts
        { esoWait = esoWait
        , esoSndbuf =
            if esoSndbuf <= 0 then Nothing else Just esoSndbuf
        }

withEventlogSocketOpts ::
    EventlogSocketOpts ->
    (Ptr EventlogSocketOpts -> IO a) ->
    IO a
withEventlogSocketOpts eso action =
    allocaBytes #{size EventlogSocketOpts} $ \esoPtr -> do
        #{poke EventlogSocketOpts, eso_wait} esoPtr . CBool . fromBool $ esoWait eso
        #{poke EventlogSocketOpts, eso_sndbuf} esoPtr . fromMaybe 0 $ esoSndbuf eso
        action esoPtr

defaultEventlogSocketOpts :: EventlogSocketOpts
defaultEventlogSocketOpts =
    unsafePerformIO $
        allocaBytes #{size EventlogSocketOpts} $ \esoPtr ->
            bracket_
                (eventlog_socket_opts_init esoPtr)
                (eventlog_socket_opts_free esoPtr)
                (peekEventlogSocketOpts esoPtr)

foreign import capi safe "eventlog_socket.h eventlog_socket_opts_init"
    eventlog_socket_opts_init ::
        Ptr EventlogSocketOpts ->
        IO ()

foreign import capi safe "eventlog_socket.h eventlog_socket_opts_free"
    eventlog_socket_opts_free ::
        Ptr EventlogSocketOpts ->
        IO ()

--------------------------------------------------------------------------------
-- Configuration via environment
--------------------------------------------------------------------------------

eventlogSocketFromEnv ::
    IO (Maybe (EventlogSocketAddr, EventlogSocketOpts))
eventlogSocketFromEnv =
    allocaBytes #{size EventlogSocketAddr} $ \esaPtr ->
    allocaBytes #{size EventlogSocketOpts} $ \esoPtr -> do
        let tryGet =
                eventlog_socket_from_env esaPtr esoPtr
        let maybeFree status =
                when (status /= EVENTLOG_SOCKET_FROM_ENV_INVAL) $ do
                    eventlog_socket_addr_free esaPtr
                    eventlog_socket_opts_free esoPtr
        let maybePeek = \case
                EVENTLOG_SOCKET_FROM_ENV_OK -> do
                    esa <- peekEventlogSocketAddr esaPtr
                    eso <- peekEventlogSocketOpts esoPtr
                    pure $ Just (esa, eso)
                EVENTLOG_SOCKET_FROM_ENV_UNIX_PATH_TOO_LONG -> do
                    esa <- peekEventlogSocketAddr esaPtr
                    throwIO $ EventlogSocketAddrUnixPathTooLong (esaUnixPath esa)
                EVENTLOG_SOCKET_FROM_ENV_INET_HOST_MISSING -> do
                    esa  <- peekEventlogSocketAddr esaPtr
                    throwIO $ EventlogSocketAddrInetHostMissing (esaInetPort esa)
                EVENTLOG_SOCKET_FROM_ENV_INET_PORT_MISSING -> do
                    esa  <- peekEventlogSocketAddr esaPtr
                    throwIO $ EventlogSocketAddrInetPortMissing (esaInetHost esa)
        bracket tryGet maybeFree maybePeek

newtype
    {-# CTYPE "eventlog_socket.h" "EventlogSocketFromEnvStatus" #-}
    EventlogSocketFromEnvStatus = EventlogSocketFromEnvStatus
        { unEventlogSocketFromEnvStatus :: #{type EventlogSocketFromEnvStatus}
        }
        deriving (Eq, Show, Storable)

pattern EVENTLOG_SOCKET_FROM_ENV_OK :: EventlogSocketFromEnvStatus
pattern EVENTLOG_SOCKET_FROM_ENV_OK = EventlogSocketFromEnvStatus #{const EVENTLOG_SOCKET_FROM_ENV_OK}

pattern EVENTLOG_SOCKET_FROM_ENV_INVAL :: EventlogSocketFromEnvStatus
pattern EVENTLOG_SOCKET_FROM_ENV_INVAL = EventlogSocketFromEnvStatus #{const EVENTLOG_SOCKET_FROM_ENV_INVAL}

pattern EVENTLOG_SOCKET_FROM_ENV_UNIX_PATH_TOO_LONG :: EventlogSocketFromEnvStatus
pattern EVENTLOG_SOCKET_FROM_ENV_UNIX_PATH_TOO_LONG = EventlogSocketFromEnvStatus #{const EVENTLOG_SOCKET_FROM_ENV_UNIX_PATH_TOO_LONG}

pattern EVENTLOG_SOCKET_FROM_ENV_INET_HOST_MISSING :: EventlogSocketFromEnvStatus
pattern EVENTLOG_SOCKET_FROM_ENV_INET_HOST_MISSING = EventlogSocketFromEnvStatus #{const EVENTLOG_SOCKET_FROM_ENV_INET_HOST_MISSING}

pattern EVENTLOG_SOCKET_FROM_ENV_INET_PORT_MISSING :: EventlogSocketFromEnvStatus
pattern EVENTLOG_SOCKET_FROM_ENV_INET_PORT_MISSING = EventlogSocketFromEnvStatus #{const EVENTLOG_SOCKET_FROM_ENV_INET_PORT_MISSING}

{-# COMPLETE
    EVENTLOG_SOCKET_FROM_ENV_OK,
    EVENTLOG_SOCKET_FROM_ENV_INVAL,
    EVENTLOG_SOCKET_FROM_ENV_UNIX_PATH_TOO_LONG,
    EVENTLOG_SOCKET_FROM_ENV_INET_HOST_MISSING,
    EVENTLOG_SOCKET_FROM_ENV_INET_PORT_MISSING #-}

--------------------------------------------------------------------------------
-- Environment variables
--------------------------------------------------------------------------------

eventlogSocketEnvUnixPath :: String
eventlogSocketEnvUnixPath = #{const_str EVENTLOG_SOCKET_ENV_UNIX_PATH}

eventlogSocketEnvInetHost :: String
eventlogSocketEnvInetHost = #{const_str EVENTLOG_SOCKET_ENV_INET_HOST}

eventlogSocketEnvInetPort :: String
eventlogSocketEnvInetPort = #{const_str EVENTLOG_SOCKET_ENV_INET_PORT}

eventlogSocketEnvWait :: String
eventlogSocketEnvWait = #{const_str EVENTLOG_SOCKET_ENV_WAIT}

--------------------------------------------------------------------------------
-- Error types
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
-- EventlogSocketAddrInvalid

data EventlogSocketAddrInvalid
    = EventlogSocketAddrUnixPathTooLong FilePath
    | EventlogSocketAddrInetHostMissing String
    | EventlogSocketAddrInetPortMissing String
    deriving (Eq, Show)

instance Exception EventlogSocketAddrInvalid where
    displayException (EventlogSocketAddrUnixPathTooLong esaUnixPath) =
        "Unix domain socket paths are limited to 107 characters. "
            <> "Found path with "
            <> show (length esaUnixPath)
            <> " characters:\n"
            <> esaUnixPath
    displayException (EventlogSocketAddrInetHostMissing esaInetPort) =
        "The port number "
            <> eventlogSocketEnvInetPort
            <> " was set to "
            <> esaInetPort
            <> ", but the host name "
            <> eventlogSocketEnvInetHost
            <> " was not set."
    displayException (EventlogSocketAddrInetPortMissing esaInetHost) =
        "The host name  "
            <> eventlogSocketEnvInetHost
            <> " was set to "
            <> esaInetHost
            <> ", but the port number "
            <> eventlogSocketEnvInetPort
            <> " was not set."

--------------------------------------------------------------------------------
-- Foreign imports
--------------------------------------------------------------------------------

foreign import capi safe "eventlog_socket.h eventlog_socket_start"
    eventlog_socket_start ::
        Ptr EventlogSocketAddr ->
        Ptr EventlogSocketOpts ->
        IO ()

foreign import capi safe "eventlog_socket.h eventlog_socket_from_env"
    eventlog_socket_from_env ::
        Ptr EventlogSocketAddr ->
        Ptr EventlogSocketOpts ->
        IO EventlogSocketFromEnvStatus
