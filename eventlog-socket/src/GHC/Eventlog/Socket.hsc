{-# LANGUAGE CApiFFI #-}
{-# LANGUAGE GeneralizedNewtypeDeriving #-}
{-# LANGUAGE PatternSynonyms #-}

module GHC.Eventlog.Socket (
    -- * High-level API
    startWith,

    -- ** Configuration types
    EventlogSocketAddr (..),
    EventlogSocketOpts (..),
    defaultEventlogSocketOpts,

    -- ** Configuration via environment
    startFromEnv,
    fromEnv,

    -- ** Error types
    EventlogSocketAddrInvalid(..),

    -- * Legacy API
    startWait,
    start,
    wait,
) where

import Control.Exception (Exception (..), bracket, bracket_, throwIO)
import Control.Monad((<=<), when)
import Data.Foldable (traverse_)
import Data.Function ((&))
import Data.Int (Int32)
import Data.Maybe (fromMaybe)
import Data.Word (Word8, Word32)
import Foreign.C (CBool (..))
import Foreign.C.String (CString, peekCString, withCString)
import Foreign.Ptr (Ptr, castPtr, nullPtr, plusPtr)
import Foreign.Storable (Storable (..))
import Foreign.Marshal.Alloc (allocaBytes)
import Foreign.Marshal.Utils (fromBool, toBool, with)
import GHC.Enum (toEnumError)
import System.IO.Unsafe (unsafePerformIO)

#include <eventlog_socket.h>

--------------------------------------------------------------------------------
-- High-level API
--------------------------------------------------------------------------------

{- |
Start an @eventlog-socket@ writer using the given socket address and options.

@since 0.1.1.0
-}
startWith ::
    EventlogSocketAddr ->
    EventlogSocketOpts ->
    IO ()
startWith esa eso =
    withEventlogSocketAddr esa $ \esaPtr ->
    withEventlogSocketOpts eso $ \esoPtr ->
        eventlog_socket_start esaPtr esoPtr

--------------------------------------------------------------------------------
-- Configuration types

{- |
A type representing the supported eventlog socket modes.

@since 0.1.1.0
-}
data
    {-# CTYPE "eventlog_socket.h" "EventlogSocketAddr" #-}
    EventlogSocketAddr = EventlogSocketUnixAddr
        { esaUnixPath :: FilePath
        -- ^ Unix socket path, e.g., @"\/tmp\/ghc_eventlog.sock"@.
        --
        --         __Warning:__ Unix domain socket paths are often limited to 107 characters or less.
        }
    | EventlogSocketInetAddr
        { esaInetHost :: String
        -- ^ TCP host or interface, e.g. @"127.0.0.1"@.
        , esaInetPort :: String
        -- ^ TCP port, e.g., @"4242"@.
        }
    deriving (Eq, Show)

{- |
The socket options for @eventlog-socket@.

To construct an instance of the socket options, use `defaultEventlogSocketOpts` and the fields.
For instance:

@
myEventlogSocketOpts :: EventlogSocketOpts
myEventlogSocketOpts = defaultEventlogSocketOpts
    { esoWait = True
    }
@

The following socket options are available:

[@`esoWait` :: `Bool`@]:
    Whether or not to wait for a client to connect.

[@`esoSndbuf` ~ `Foreign.C.Types.CInt`@]:
    The size of the socket send buffer.

    See the documentation for @SO_SNDBUF@ in @socket.h@.

@since 0.1.1.0
-}
data
    {-# CTYPE "eventlog_socket.h" "EventlogSocketOpts" #-}
    EventlogSocketOpts = EventlogSocketOpts
        { esoWait :: Bool
        , esoSndbuf :: Maybe #{type int}
        }
    deriving (Eq, Show)

{- |
The default socket options for @eventlog-socket@.

See t`EventlogSocketOpts`.

@since 0.1.1.0
-}
defaultEventlogSocketOpts :: EventlogSocketOpts
defaultEventlogSocketOpts =
    unsafePerformIO $
        allocaBytes #{size EventlogSocketOpts} $ \esoPtr ->
            bracket_
                (eventlog_socket_opts_init esoPtr)
                (eventlog_socket_opts_free esoPtr)
                (peekEventlogSocketOpts esoPtr)

--------------------------------------------------------------------------------
-- Configuration via environment

{- |
Read the eventlog socket configuration from the environment.
If this succeeds, start an @eventlog-socket@ writer with that configuration.

@since 0.1.1.0
-}
startFromEnv :: IO ()
startFromEnv = fromEnv >>= traverse_ (uncurry startWith)

{- |
Read the eventlog socket configuration from the environment.

@since 0.1.1.0
-}
fromEnv ::
    IO (Maybe (EventlogSocketAddr, EventlogSocketOpts))
fromEnv =
    allocaBytes #{size EventlogSocketAddr} $ \esaPtr ->
    allocaBytes #{size EventlogSocketOpts} $ \esoPtr -> do
        let tryGet =
                eventlog_socket_from_env esaPtr esoPtr
        let shouldFree status =
                notElem status $
                    [ EVENTLOG_SOCKET_FROM_ENV_NONE
                    , EVENTLOG_SOCKET_FROM_ENV_INVAL
                    ]
        let maybeFree status =
                when (shouldFree status) $ do
                    eventlog_socket_addr_free esaPtr
                    eventlog_socket_opts_free esoPtr
        let maybePeek = \case
                EVENTLOG_SOCKET_FROM_ENV_OK -> do
                    esa <- peekEventlogSocketAddr esaPtr
                    eso <- peekEventlogSocketOpts esoPtr
                    pure $ Just (esa, eso)
                EVENTLOG_SOCKET_FROM_ENV_NONE -> do
                    pure Nothing
                EVENTLOG_SOCKET_FROM_ENV_INVAL -> do
                    error "allocaBytes returned nullPtr"
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


--------------------------------------------------------------------------------
-- Environment variables

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

{- |
The type of exceptions thrown by `fromEnv`.
-}
data EventlogSocketAddrInvalid
    = EventlogSocketAddrUnixPathTooLong FilePath
      -- ^ The found Unix domain socket path was too long.
    | EventlogSocketAddrInetHostMissing String
      -- ^ No TCP/IP port number was found, but no host name was found.
    | EventlogSocketAddrInetPortMissing String
      -- ^ A TCP/IP host name was found, but no port number was found.
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
-- Legacy API
--------------------------------------------------------------------------------

{- |
Start an @eventlog-socket@ writer on the given Unix domain socket path and wait.

@since 0.1.0.0
-}
startWait :: FilePath -> IO ()
startWait unixPath = do
    let addr = EventlogSocketUnixAddr unixPath
    let opts = defaultEventlogSocketOpts{esoWait = True}
    startWith addr opts

{- |
Start an @eventlog-socket@ writer on the given Unix domain socket path.

@since 0.1.0.0
-}
start :: FilePath -> IO ()
start unixPath = do
    let addr = EventlogSocketUnixAddr unixPath
    let opts = defaultEventlogSocketOpts{esoWait = False}
    startWith addr opts

{- |
Wait for another process to connect to the eventlog socket.

@since 0.1.0.0
-}
wait :: IO ()
wait = eventlog_socket_wait

--------------------------------------------------------------------------------
-- Low-level API
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
-- Low-level foreign types

{- |
The address family of the eventlog socket.

Used as the tag for the C tagged union @EventlogSocketAddr@.
-}
newtype
    {-# CTYPE "eventlog_socket.h" "EventlogSocketTag" #-}
    EventlogSocketTag = EventlogSocketTag
        { unEventlogSocketTag :: #{type EventlogSocketTag}
        }
        deriving (Eq, Show, Storable)

{- |
The tag for a Unix domain socket address.
-}
pattern EVENTLOG_SOCKET_UNIX :: EventlogSocketTag
pattern EVENTLOG_SOCKET_UNIX = EventlogSocketTag #{const EVENTLOG_SOCKET_UNIX}

{- |
The tag for a TCP/IP socket address.
-}
pattern EVENTLOG_SOCKET_INET :: EventlogSocketTag
pattern EVENTLOG_SOCKET_INET = EventlogSocketTag #{const EVENTLOG_SOCKET_INET}

{-# COMPLETE
    EVENTLOG_SOCKET_UNIX,
    EVENTLOG_SOCKET_INET #-}

{- |
The return status for the C function `eventlog_socket_from_env`.
-}
newtype
    {-# CTYPE "eventlog_socket.h" "EventlogSocketFromEnvStatus" #-}
    EventlogSocketFromEnvStatus = EventlogSocketFromEnvStatus
        { unEventlogSocketFromEnvStatus :: #{type EventlogSocketFromEnvStatus}
        }
        deriving (Eq, Show, Storable)

{- |
Successfully initialised the socket address and options.
-}
pattern EVENTLOG_SOCKET_FROM_ENV_OK :: EventlogSocketFromEnvStatus
pattern EVENTLOG_SOCKET_FROM_ENV_OK = EventlogSocketFromEnvStatus #{const EVENTLOG_SOCKET_FROM_ENV_OK}

{- |
Did not find any socket address.
-}
pattern EVENTLOG_SOCKET_FROM_ENV_NONE :: EventlogSocketFromEnvStatus
pattern EVENTLOG_SOCKET_FROM_ENV_NONE = EventlogSocketFromEnvStatus #{const EVENTLOG_SOCKET_FROM_ENV_NONE}

{- |
Received invalid arguments.
-}
pattern EVENTLOG_SOCKET_FROM_ENV_INVAL :: EventlogSocketFromEnvStatus
pattern EVENTLOG_SOCKET_FROM_ENV_INVAL = EventlogSocketFromEnvStatus #{const EVENTLOG_SOCKET_FROM_ENV_INVAL}

{- |
The found Unix domain socket path was too long.
-}
pattern EVENTLOG_SOCKET_FROM_ENV_UNIX_PATH_TOO_LONG :: EventlogSocketFromEnvStatus
pattern EVENTLOG_SOCKET_FROM_ENV_UNIX_PATH_TOO_LONG = EventlogSocketFromEnvStatus #{const EVENTLOG_SOCKET_FROM_ENV_UNIX_PATH_TOO_LONG}

{- |
No TCP/IP port number was found, but no host name was found.
-}
pattern EVENTLOG_SOCKET_FROM_ENV_INET_HOST_MISSING :: EventlogSocketFromEnvStatus
pattern EVENTLOG_SOCKET_FROM_ENV_INET_HOST_MISSING = EventlogSocketFromEnvStatus #{const EVENTLOG_SOCKET_FROM_ENV_INET_HOST_MISSING}

{- |
A TCP/IP host name was found, but no port number was found.
-}
pattern EVENTLOG_SOCKET_FROM_ENV_INET_PORT_MISSING :: EventlogSocketFromEnvStatus
pattern EVENTLOG_SOCKET_FROM_ENV_INET_PORT_MISSING = EventlogSocketFromEnvStatus #{const EVENTLOG_SOCKET_FROM_ENV_INET_PORT_MISSING}

{-# COMPLETE
    EVENTLOG_SOCKET_FROM_ENV_OK,
    EVENTLOG_SOCKET_FROM_ENV_NONE,
    EVENTLOG_SOCKET_FROM_ENV_INVAL,
    EVENTLOG_SOCKET_FROM_ENV_UNIX_PATH_TOO_LONG,
    EVENTLOG_SOCKET_FROM_ENV_INET_HOST_MISSING,
    EVENTLOG_SOCKET_FROM_ENV_INET_PORT_MISSING #-}

--------------------------------------------------------------------------------
-- Marshalling from foreign types

{- |
Marshal an `EventlogSocketAddr` from C.
-}
peekEventlogSocketAddr ::
    Ptr EventlogSocketAddr ->
    IO EventlogSocketAddr
peekEventlogSocketAddr esaPtr = do
    #{peek EventlogSocketAddr, esa_tag} esaPtr >>= \case
        EVENTLOG_SOCKET_UNIX -> do
            esaUnixPath <-
                peekNullableCString <=< peek
                    $ esaPtr
                    & #{ptr EventlogSocketAddr, esa_unix_addr}
                    & #{ptr EventlogSocketUnixAddr, esa_unix_path}
            pure EventlogSocketUnixAddr
                { esaUnixPath = esaUnixPath
                }
        EVENTLOG_SOCKET_INET -> do
            esaInetHost <-
                peekNullableCString <=< peek
                    $ esaPtr
                    & #{ptr EventlogSocketAddr, esa_inet_addr}
                    & #{ptr EventlogSocketInetAddr, esa_inet_host}
            esaInetPort <-
                peekNullableCString <=< peek
                    $ esaPtr
                    & #{ptr EventlogSocketAddr, esa_inet_addr}
                    & #{ptr EventlogSocketInetAddr, esa_inet_port}
            pure EventlogSocketInetAddr
                { esaInetHost = esaInetHost
                , esaInetPort = esaInetPort
                }

{- |
Marshal an `EventlogSocketAddr` to C.
-}
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

{- |
Marshal an `EventlogSocketOpts` from C.
-}
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

{- |
Marshal an `EventlogSocketOpts` to C.
-}
withEventlogSocketOpts ::
    EventlogSocketOpts ->
    (Ptr EventlogSocketOpts -> IO a) ->
    IO a
withEventlogSocketOpts eso action =
    allocaBytes #{size EventlogSocketOpts} $ \esoPtr -> do
        #{poke EventlogSocketOpts, eso_wait} esoPtr . CBool . fromBool $ esoWait eso
        #{poke EventlogSocketOpts, eso_sndbuf} esoPtr . fromMaybe 0 $ esoSndbuf eso
        action esoPtr

{- |
Variant of `peekCString` that checks for `nullPtr`.
-}
peekNullableCString :: CString -> IO String
peekNullableCString charPtr =
    if charPtr == nullPtr then pure "" else peekCString charPtr

--------------------------------------------------------------------------------
-- Foreign imports

foreign import capi safe "eventlog_socket.h eventlog_socket_addr_free"
    eventlog_socket_addr_free ::
        Ptr EventlogSocketAddr ->
        IO ()

foreign import capi safe "eventlog_socket.h eventlog_socket_opts_init"
    eventlog_socket_opts_init ::
        Ptr EventlogSocketOpts ->
        IO ()

foreign import capi safe "eventlog_socket.h eventlog_socket_opts_free"
    eventlog_socket_opts_free ::
        Ptr EventlogSocketOpts ->
        IO ()

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

foreign import capi safe "eventlog_socket.h eventlog_socket_wait"
    eventlog_socket_wait :: IO ()
