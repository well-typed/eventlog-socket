{-|
Module      : GHC.Eventlog.Socket
Description : Haskell API for @eventlog-socket@.
Stability   : experimental
Portability : POSIX

This module exports the Haskell API for @eventlog-socket@.

To start streaming GHC eventlog events to a socket, use `startWith` with a [socket address](#t:EventlogSocketAddr) and [options](#t:EventlogSocketOpts).
For instance, the following code starts @eventlog-socket@ configured to wait for a connection and then stream events to @\/tmp\/my_app.sock@.

@
let addr = `EventlogSocketUnixAddr` "\/tmp\/my_app.sock"
let opts = `defaultEventlogSocketOpts` {`esoWait` = True}
`startWith` addr opts
@

To register custom control commands, use `registerNamespace` and `registerCommand`.
For instance, the following code registers the namespace @"ping"@ with one command at ID 1 that prints "Ping!" when called.

@
pingNamespace <- `registerNamespace` "ping"
let pingId = v`CommandId` 1
let pingHandler = putStrLn "Ping!"
`registerCommand` pingNamespace pingId pingHandler
@
-}

{-# LANGUAGE CApiFFI #-}
{-# LANGUAGE GeneralizedNewtypeDeriving #-}
{-# LANGUAGE PatternSynonyms #-}
{-# LANGUAGE TypeApplications #-}

module GHC.Eventlog.Socket (
    -- * High-level API #high_level_api#
    startWith,

    -- ** Configuration types #configuration_types#
    EventlogSocketAddr (..),
    EventlogSocketOpts (esoWait, esoSndbuf, esoLinger),
    defaultEventlogSocketOpts,

    -- ** Configuration via environment #configuration_via_environment#
    startFromEnv,
    fromEnv,
    EventlogSocketAddrError(..),

    -- ** Control commands #control_commands#
    Namespace,
    CommandId(..),
    CommandHandler,
    namespaceName,
    registerNamespace,
    registerCommand,
    EventlogSocketControlError(..),

    -- ** Low-level API #low_level_api#
    testWorkerStatus,
    testControlStatus,

    -- * Legacy API #legacy_api#
    startWait,
    start,
    wait,
) where

import Control.Exception (AssertionFailed (..), Exception (..), assert, bracket, bracket_, bracketOnError, throwIO)
import Control.Monad((<=<), when)
import Data.Foldable (traverse_)
import Data.Function ((&))
import Data.Int (Int32)
import Data.Maybe (fromMaybe)
import Data.Void (Void, vacuous)
import Data.Word (Word8, Word32)
import Foreign.C (CBool (..))
import Foreign.C.String (CString, peekCString, withCString, withCStringLen)
import Foreign.Ptr (FunPtr, freeHaskellFunPtr, Ptr, castPtr, nullPtr, plusPtr)
import Foreign.Storable (Storable (..))
import Foreign.Marshal.Alloc (alloca, allocaBytes, free)
import Foreign.Marshal.Utils (fromBool, toBool, with)
import GHC.Enum (toEnumError)
import System.IO.Unsafe (unsafePerformIO)

#include <eventlog_socket/macros.h>
#include <eventlog_socket.h>
#include <string.h>

--------------------------------------------------------------------------------
-- High-level API
--------------------------------------------------------------------------------

{- |
Start an @eventlog-socket@ writer using the given socket address and options.

@since 0.1.2.0
-}
startWith ::
    EventlogSocketAddr ->
    EventlogSocketOpts ->
    IO ()
startWith esa eso =
    allocaBytes #{size EventlogSocketStatus} $ \essPtr -> do
        withEventlogSocketAddr esa $ \esaPtr ->
            withEventlogSocketOpts eso $ \esoPtr ->
                eventlog_socket_start essPtr esaPtr esoPtr
        status <- peekEventlogSocketStatus essPtr

        -- If the status is an error, throw a user error.
        when (essStatusCode status /= EVENTLOG_SOCKET_OK) $
            vacuous $ throwEventlogSocketStatus essPtr

--------------------------------------------------------------------------------
-- Configuration types

{- |
A type representing the supported eventlog socket modes.

@since 0.1.2.0
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

[@`esoWait` :: `Bool`@ #v:esoWait#]:
Whether or not to wait for a client to connect.

[@`esoSndbuf` ~ `Foreign.C.Types.CInt`@ #v:esoSndbuf#]:
    The size of the socket send buffer.

    See the documentation for @SO_SNDBUF@ in @socket.h@.

[@`esoLinger` ~ `Foreign.C.Types.CInt`@ #v:esoLinger#]:
    The number of seconds to linger on shutdown.

    See the documentation for @SO_LINGER@ in @socket.h@.

@since 0.1.2.0
-}
data
    {-# CTYPE "eventlog_socket.h" "EventlogSocketOpts" #-}
    EventlogSocketOpts = EventlogSocketOpts
        { esoWait :: Bool
        , esoSndbuf :: Maybe #{type int}
        , esoLinger :: Maybe #{type int}
        }
    deriving (Eq, Show)

{- |
The default socket options for @eventlog-socket@.

See t`EventlogSocketOpts`.

@since 0.1.2.0
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

@since 0.1.2.0
-}
startFromEnv :: IO ()
startFromEnv = fromEnv >>= traverse_ (uncurry startWith)

{- |
Read the eventlog socket configuration from the environment.

@since 0.1.2.0
-}
fromEnv ::
    IO (Maybe (EventlogSocketAddr, EventlogSocketOpts))
fromEnv =
    allocaBytes #{size EventlogSocketAddr} $ \esaPtr ->
    allocaBytes #{size EventlogSocketOpts} $ \esoPtr -> do
        let tryGet =
                allocaBytes #{size EventlogSocketStatus} $ \essPtr -> do
                    eventlog_socket_from_env essPtr esaPtr esoPtr
                    peekEventlogSocketStatus essPtr
        let shouldFree status =
                elem (essStatusCode status) $
                    [ EVENTLOG_SOCKET_OK
                    , EVENTLOG_SOCKET_ERR_ENV_TOOLONG
                    , EVENTLOG_SOCKET_ERR_ENV_NOHOST
                    , EVENTLOG_SOCKET_ERR_ENV_NOPORT
                    ]
        let maybeFree status =
                when (shouldFree status) $ do
                    eventlog_socket_addr_free esaPtr
                    eventlog_socket_opts_free esoPtr
        let maybePeek status
                | essStatusCode status == EVENTLOG_SOCKET_OK = do
                    esa <- peekEventlogSocketAddr esaPtr
                    eso <- peekEventlogSocketOpts esoPtr
                    pure $ Just (esa, eso)
                | essStatusCode status == EVENTLOG_SOCKET_ERR_ENV_NOADDR = do
                    pure Nothing
                | essStatusCode status == EVENTLOG_SOCKET_ERR_ENV_TOOLONG = do
                    esa <- peekEventlogSocketAddr esaPtr
                    throwIO $ EventlogSocketAddrUnixPathTooLong (esaUnixPath esa)
                | essStatusCode status == EVENTLOG_SOCKET_ERR_ENV_NOHOST = do
                    esa <- peekEventlogSocketAddr esaPtr
                    throwIO $ EventlogSocketAddrInetHostMissing (esaInetPort esa)
                | essStatusCode status == EVENTLOG_SOCKET_ERR_ENV_NOPORT = do
                    esa <- peekEventlogSocketAddr esaPtr
                    throwIO $ EventlogSocketAddrInetHostMissing (esaInetHost esa)
                | otherwise =
                    vacuous $ withEventlogSocketStatus status throwEventlogSocketStatus

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
-- Errors

{- |
The type of exceptions thrown by `fromEnv`.

@since 0.1.2.0
-}
data EventlogSocketAddrError
    = EventlogSocketAddrUnixPathTooLong FilePath
      -- ^ The found Unix domain socket path was too long.
    | EventlogSocketAddrInetHostMissing String
      -- ^ No TCP/IP port number was found, but no host name was found.
    | EventlogSocketAddrInetPortMissing String
      -- ^ A TCP/IP host name was found, but no port number was found.
    deriving (Eq, Show)

instance Exception EventlogSocketAddrError where
    displayException = \case
        EventlogSocketAddrUnixPathTooLong esaUnixPath ->
            "Unix domain socket paths are limited to 107 characters. "
                <> "Found path with "
                <> show (length esaUnixPath)
                <> " characters:\n"
                <> esaUnixPath
        EventlogSocketAddrInetHostMissing esaInetPort ->
            "The port number "
                <> eventlogSocketEnvInetPort
                <> " was set to "
                <> esaInetPort
                <> ", but the host name "
                <> eventlogSocketEnvInetHost
                <> " was not set."
        EventlogSocketAddrInetPortMissing esaInetHost ->
            "The host name  "
                <> eventlogSocketEnvInetHost
                <> " was set to "
                <> esaInetHost
                <> ", but the port number "
                <> eventlogSocketEnvInetPort
                <> " was not set."

{- |
Test the current status of the worker thread. If it has failed, throw an `Control.Exception.IOException`.

@since 0.1.2.0
-}
testWorkerStatus :: IO ()
testWorkerStatus =
    throwEventlogSocketStatusAsIOException =<< workerStatus

{- |
Test the current status of the control thread. If it has failed, throw an `Control.Exception.IOException`.

@since 0.1.2.0
-}
testControlStatus :: IO ()
testControlStatus =
    throwEventlogSocketStatusAsIOException =<< controlStatus

{- |
Internal helper.

Read the current worker status.
-}
workerStatus :: IO EventlogSocketStatus
workerStatus =
    allocaBytes #{size EventlogSocketStatus} $ \essPtr -> do
        eventlog_socket_worker_status essPtr
        peekEventlogSocketStatus essPtr

{- |
Internal helper.

Read the current control status.
-}
controlStatus :: IO EventlogSocketStatus
controlStatus =
    allocaBytes #{size EventlogSocketStatus} $ \essPtr -> do
        eventlog_socket_control_status essPtr
        peekEventlogSocketStatus essPtr

{- |
Internal helper.

Throw an t`EventlogSocketStatus` as an `Control.Exception.IOException`.
-}
throwEventlogSocketStatusAsIOException :: EventlogSocketStatus -> IO ()
throwEventlogSocketStatusAsIOException ess =
    when (essStatusCode ess /= EVENTLOG_SOCKET_OK) $
        vacuous $ withEventlogSocketStatus ess throwEventlogSocketStatus

{- |
Internal helper.

Throw an t`EventlogSocketStatus` as an `Control.Exception.IOException`.

__Warning__: This function _still_ throws an error if the status code is `EVENTLOG_SOCKET_OK`.
-}
throwEventlogSocketStatus :: Ptr EventlogSocketStatus -> IO Void
throwEventlogSocketStatus essPtr = do
    strPtr <- eventlog_socket_strerror essPtr
    str <- peekNullableCString strPtr
    free strPtr
    throwIO $ userError str

--------------------------------------------------------------------------------
-- Hooks API
--------------------------------------------------------------------------------

{- |
The type of @eventlog-socket@ hooks.
-}
newtype
    {-# CTYPE "eventlog_socket.h" "EventlogSocketHook" #-}
    EventlogSocketHook = EventlogSocketHook
        { unEventlogSocketHook :: #{type EventlogSocketHook}
        }
        deriving (Eq, Show, Storable)

{- |
The hook for new connections.
-}
pattern EVENTLOG_SOCKET_HOOK_POST_START_EVENT_LOGGING :: EventlogSocketTag
pattern EVENTLOG_SOCKET_HOOK_POST_START_EVENT_LOGGING = EventlogSocketTag #{const EVENTLOG_SOCKET_HOOK_POST_START_EVENT_LOGGING}

{- |
The tag for a TCP/IP socket address.
-}
pattern EVENTLOG_SOCKET_HOOK_PRE_END_EVENT_LOGGING :: EventlogSocketTag
pattern EVENTLOG_SOCKET_HOOK_PRE_END_EVENT_LOGGING = EventlogSocketTag #{const EVENTLOG_SOCKET_HOOK_PRE_END_EVENT_LOGGING}

{-# COMPLETE
    EVENTLOG_SOCKET_HOOK_POST_START_EVENT_LOGGING,
    EVENTLOG_SOCKET_HOOK_PRE_END_EVENT_LOGGING #-}

{- |
The type of hook handlers.

The hook handler is evaluated once each time the control socket receives a request for the associated hook.

__Warning__: The hook handler /must not/ call back into the @eventlog-socket@ API.

@since 0.1.2.0
-}
type HookHandler = IO ()

--------------------------------------------------------------------------------
-- Control Commands API
--------------------------------------------------------------------------------

{- |
The type of namespaces.

Namespaces are opaque and can only be obtained using `registerNamespace`.

@since 0.1.2.0
-}
newtype
    {-# CTYPE "eventlog_socket.h" "EventlogSocketControlNamespace" #-}
    Namespace = Namespace (Ptr Namespace)

{- |
Get the `String` name for the given t`Namespace`.

@since 0.1.2.0
-}
namespaceName :: Namespace -> IO String
namespaceName (Namespace namespacePtr) = do
    peekNullableCString =<< eventlog_socket_control_strnamespace namespacePtr

{- |
The type of command IDs.

Command IDs must be non-zero integers between 1 and 255.

@since 0.1.2.0
-}
newtype
    {-# CTYPE "eventlog_socket.h" "EventlogSocketControlCommandId" #-}
    CommandId = CommandId #{type EventlogSocketControlCommandId}
    deriving (Eq, Show)

{- |
The type of command handlers.

The command handler is evaluated once each time the control socket receives a request for the associated command.

__Warning__: The command handler /must not/ call back into the @eventlog-socket@ API.

@since 0.1.2.0
-}
type CommandHandler = IO ()

{- |
Register an @eventlog-socket@ control namespace with the given name and returns an opaque t`Namespace` object.

To avoid conflicts, the namespace should use the name of the Haskell package that registers the commands.

If the size of the given name exceeds 255 bytes, this function throws v`EventlogSocketControlNamespaceTooLong`.

If a namespace is already registered under the given name, this function throws v`EventlogSocketControlNamespaceExists`.

If the binary was built without support for control commands, this function throws v`EventlogSocketControlUnsupported`.

__Warning__: Namespaces cannot be unregistered and will be kept in memory until program exit.

@since 0.1.2.0
-}
registerNamespace ::
    -- | The name for the namespace.
    String ->
    IO Namespace
registerNamespace namespace =
    alloca @(Ptr Namespace) $ \namespaceOut -> do
        -- Try to register the namespace:
        status <-
            -- Allocate space for the return status:
            allocaBytes #{size EventlogSocketStatus} $ \essPtr ->
                withCStringLen namespace $ \(namespacePtr, namespaceLen) -> do

                    -- Check the namespace length:
                    let maxNamespaceLen = fromIntegral (maxBound :: #{type uint8_t})
                    when (namespaceLen > maxNamespaceLen) $
                        throwIO $
                            EventlogSocketControlNamespaceTooLong
                                namespace
                                namespaceLen
                                maxNamespaceLen

                    -- Try to register the namespace:
                    eventlog_socket_control_register_namespace
                        essPtr
                        (fromIntegral namespaceLen)
                        namespacePtr
                        namespaceOut

                    -- Marshal the return status:
                    peekEventlogSocketStatus essPtr

        -- Handle the return status:
        case essStatusCode status of
            -- The return status is OK, marshal and return the namespace.
            EVENTLOG_SOCKET_OK -> Namespace <$> peek namespaceOut

            -- The binary was compiled without support for the control server.
            EVENTLOG_SOCKET_ERR_CTL_NOSUPPORT -> throwIO EventlogSocketControlUnsupported

            -- The namespace was already registered.
            EVENTLOG_SOCKET_ERR_CTL_EXISTS -> throwIO $ EventlogSocketControlNamespaceExists namespace

            -- The remaining errors are all system errors:
            _otherwise ->
                vacuous $ withEventlogSocketStatus status throwEventlogSocketStatus

{- |
Register an @eventlog-socket@ control command with the given ID and handler in the given namespace.

If a command is already registered under the given ID in the given namespace, this function throws v`EventlogSocketControlCommandExists`.

If the binary was built without support for control commands, this function throws v`EventlogSocketControlUnsupported`.

__Warning__: Commands cannot be unregistered and will be kept in memory until program exit.

@since 0.1.2.0
-}
registerCommand ::
    -- | The namespace.
    Namespace ->
    -- | The command ID.
    CommandId ->
    -- | The command handler.
    CommandHandler ->
    IO ()
registerCommand (Namespace namespacePtr) commandId commandHandler = do
    -- Wrap the Haskell command handler for the C API
    let c_commandHandler namespacePtr' commandId' commandDataPtr =
            assert (namespacePtr == namespacePtr') $
            assert (commandId == commandId') $
            assert (commandDataPtr == nullPtr) $
            commandHandler

    -- Allocate a C function pointer for the command handler:
    --
    -- NOTE: This function pointer is only deallocated if registration fails.
    bracketOnError (makeCommandHandlerFunPtr c_commandHandler) freeHaskellFunPtr $ \c_commandHandlerPtr -> do

        -- Try to register the command:
        status <-
            -- Allocate space for the return status:
            allocaBytes #{size EventlogSocketStatus} $ \essPtr -> do

                -- Try to register the command:
                eventlog_socket_control_register_command
                    essPtr
                    namespacePtr
                    commandId
                    c_commandHandlerPtr
                    nullPtr

                -- Marshal the return status:
                peekEventlogSocketStatus essPtr

        -- Handle the return status:
        case essStatusCode status of
            -- The return status is OK.
            EVENTLOG_SOCKET_OK -> pure ()

            -- The binary was compiled without support for the control server.
            EVENTLOG_SOCKET_ERR_CTL_NOSUPPORT ->
                throwIO EventlogSocketControlUnsupported

            -- The namespace was already registered.
            EVENTLOG_SOCKET_ERR_CTL_EXISTS -> do
                namespace <- namespaceName (Namespace namespacePtr)
                throwIO $ EventlogSocketControlCommandExists namespace commandId

            -- The remaining errors are all system errors:
            _otherwise ->
                vacuous $ withEventlogSocketStatus status throwEventlogSocketStatus

--------------------------------------------------------------------------------
-- Errors

{- |
The type of exceptions thrown by `registerNamespace` and `registerCommand`.

@since 0.1.2.0
-}
data EventlogSocketControlError
  = EventlogSocketControlNamespaceTooLong
        -- | The requested name for the namespace.
        String
        -- | The size of the requested name in bytes.
        Int
        -- | The maximum size in bytes.
        Int
  | EventlogSocketControlNamespaceExists
        -- | The requested name for the namespace.
        String
  | EventlogSocketControlCommandExists
        -- | The name for the namespace.
        String
        -- | The requested ID for the command.
        CommandId
  | EventlogSocketControlUnsupported
  deriving (Eq, Show)

instance Exception EventlogSocketControlError where
    displayException = \case
        EventlogSocketControlNamespaceTooLong namespace namespaceLen maxNamespaceLen ->
            "The name '"
                <> namespace
                <> "' was "
                <> show namespaceLen
                <> " bytes. The maximum length is "
                <> show maxNamespaceLen
                <> " bytes."
        EventlogSocketControlNamespaceExists namespace ->
            "The name '"
                <> namespace
                <> "' is already registered."
        EventlogSocketControlCommandExists namespace (CommandId commandId) ->
            "The ID "
                <> show commandId
                <> " is already registered for "
                <> namespace
                <> "."
        EventlogSocketControlUnsupported ->
            "The binary was built without support for control commands."

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
The status codes used by the @eventlog-socket@ library.
-}
newtype
    {-# CTYPE "eventlog_socket.h" "EventlogSocketStatusCode" #-}
    EventlogSocketStatusCode = EventlogSocketStatusCode
        { unEventlogSocketStatusCode :: #{type EventlogSocketStatusCode}
        }
        deriving (Eq, Show, Storable)

pattern EVENTLOG_SOCKET_OK :: EventlogSocketStatusCode
pattern EVENTLOG_SOCKET_OK = EventlogSocketStatusCode #{const EVENTLOG_SOCKET_OK}

pattern EVENTLOG_SOCKET_ERR_RTS_NOSUPPORT :: EventlogSocketStatusCode
pattern EVENTLOG_SOCKET_ERR_RTS_NOSUPPORT = EventlogSocketStatusCode #{const EVENTLOG_SOCKET_ERR_RTS_NOSUPPORT}

pattern EVENTLOG_SOCKET_ERR_RTS_FAIL :: EventlogSocketStatusCode
pattern EVENTLOG_SOCKET_ERR_RTS_FAIL = EventlogSocketStatusCode #{const EVENTLOG_SOCKET_ERR_RTS_FAIL}

pattern EVENTLOG_SOCKET_ERR_ENV_NOADDR :: EventlogSocketStatusCode
pattern EVENTLOG_SOCKET_ERR_ENV_NOADDR = EventlogSocketStatusCode #{const EVENTLOG_SOCKET_ERR_ENV_NOADDR}

pattern EVENTLOG_SOCKET_ERR_ENV_TOOLONG :: EventlogSocketStatusCode
pattern EVENTLOG_SOCKET_ERR_ENV_TOOLONG = EventlogSocketStatusCode #{const EVENTLOG_SOCKET_ERR_ENV_TOOLONG}

pattern EVENTLOG_SOCKET_ERR_ENV_NOHOST :: EventlogSocketStatusCode
pattern EVENTLOG_SOCKET_ERR_ENV_NOHOST = EventlogSocketStatusCode #{const EVENTLOG_SOCKET_ERR_ENV_NOHOST}

pattern EVENTLOG_SOCKET_ERR_ENV_NOPORT :: EventlogSocketStatusCode
pattern EVENTLOG_SOCKET_ERR_ENV_NOPORT = EventlogSocketStatusCode #{const EVENTLOG_SOCKET_ERR_ENV_NOPORT}

pattern EVENTLOG_SOCKET_ERR_CTL_NOSUPPORT :: EventlogSocketStatusCode
pattern EVENTLOG_SOCKET_ERR_CTL_NOSUPPORT = EventlogSocketStatusCode #{const EVENTLOG_SOCKET_ERR_CTL_NOSUPPORT}

pattern EVENTLOG_SOCKET_ERR_CTL_EXISTS :: EventlogSocketStatusCode
pattern EVENTLOG_SOCKET_ERR_CTL_EXISTS = EventlogSocketStatusCode #{const EVENTLOG_SOCKET_ERR_CTL_EXISTS}

pattern EVENTLOG_SOCKET_ERR_GAI :: EventlogSocketStatusCode
pattern EVENTLOG_SOCKET_ERR_GAI = EventlogSocketStatusCode #{const EVENTLOG_SOCKET_ERR_GAI}

pattern EVENTLOG_SOCKET_ERR_SYS :: EventlogSocketStatusCode
pattern EVENTLOG_SOCKET_ERR_SYS = EventlogSocketStatusCode #{const EVENTLOG_SOCKET_ERR_SYS}

{-# COMPLETE
    EVENTLOG_SOCKET_OK,
    EVENTLOG_SOCKET_ERR_RTS_NOSUPPORT,
    EVENTLOG_SOCKET_ERR_RTS_FAIL,
    EVENTLOG_SOCKET_ERR_ENV_NOADDR,
    EVENTLOG_SOCKET_ERR_ENV_TOOLONG,
    EVENTLOG_SOCKET_ERR_ENV_NOHOST,
    EVENTLOG_SOCKET_ERR_ENV_NOPORT,
    EVENTLOG_SOCKET_ERR_CTL_EXISTS,
    EVENTLOG_SOCKET_ERR_GAI,
    EVENTLOG_SOCKET_ERR_SYS #-}

{- |
The status used by the @eventlog-socket@ library.
-}
data
    {-# CTYPE "eventlog_socket.h" "EventlogSocketStatus" #-}
    EventlogSocketStatus = EventlogSocketStatus
        { essStatusCode :: !EventlogSocketStatusCode
        , essErrorCode :: !( #{type int} )
        }
    deriving (Eq, Show)

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
Marshal an t`EventlogSocketOpts` from C.
-}
peekEventlogSocketOpts ::
    Ptr EventlogSocketOpts ->
    IO EventlogSocketOpts
peekEventlogSocketOpts esoPtr = do
    esoWait <- toBool . CBool <$> #{peek EventlogSocketOpts, eso_wait} esoPtr
    esoSndbuf <- #{peek EventlogSocketOpts, eso_sndbuf} esoPtr
    esoLinger <- #{peek EventlogSocketOpts, eso_linger} esoPtr
    pure EventlogSocketOpts
        { esoWait = esoWait
        , esoSndbuf =
            if esoSndbuf <= 0 then Nothing else Just esoSndbuf
        , esoLinger =
            if esoLinger <= 0 then Nothing else Just esoLinger
        }

{- |
Marshal an t`EventlogSocketOpts` to C.
-}
withEventlogSocketOpts ::
    EventlogSocketOpts ->
    (Ptr EventlogSocketOpts -> IO a) ->
    IO a
withEventlogSocketOpts eso action =
    allocaBytes #{size EventlogSocketOpts} $ \esoPtr -> do
        #{poke EventlogSocketOpts, eso_wait} esoPtr . CBool . fromBool $ esoWait eso
        #{poke EventlogSocketOpts, eso_sndbuf} esoPtr . fromMaybe 0 $ esoSndbuf eso
        #{poke EventlogSocketOpts, eso_linger} esoPtr . fromMaybe 0 $ esoLinger eso
        action esoPtr

{- |
Marshal an t`EventlogSocketStatus` from C.
-}
peekEventlogSocketStatus ::
    Ptr EventlogSocketStatus ->
    IO EventlogSocketStatus
peekEventlogSocketStatus essPtr = do
    essStatusCode <-
        #{peek EventlogSocketStatus, ess_status_code} essPtr
    essErrorCode <-
        if essStatusCode `elem` [EVENTLOG_SOCKET_ERR_GAI, EVENTLOG_SOCKET_ERR_SYS]
            then #{peek EventlogSocketStatus, ess_error_code} essPtr
            else pure 0
    pure EventlogSocketStatus
        { essStatusCode = essStatusCode
        , essErrorCode = essErrorCode
        }

{- |
Marshal an t`EventlogSocketStatus` to C.
-}
withEventlogSocketStatus ::
    EventlogSocketStatus ->
    (Ptr EventlogSocketStatus -> IO a) ->
    IO a
withEventlogSocketStatus ess action =
    allocaBytes #{size EventlogSocketStatus} $ \essPtr -> do
        #{poke EventlogSocketStatus, ess_status_code} essPtr $ essStatusCode ess
        #{poke EventlogSocketStatus, ess_error_code} essPtr $ essErrorCode ess
        action essPtr

{- |
Variant of `peekCString` that checks for `nullPtr`.
-}
peekNullableCString :: CString -> IO String
peekNullableCString charPtr
    | charPtr == nullPtr = pure ""
    | otherwise = peekCString charPtr

--------------------------------------------------------------------------------
-- Foreign imports
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
-- eventlog_socket_addr_free

foreign import capi safe "eventlog_socket.h eventlog_socket_addr_free"
    eventlog_socket_addr_free ::
        Ptr EventlogSocketAddr ->
        IO ()

--------------------------------------------------------------------------------
-- eventlog_socket_opts_init

foreign import capi safe "eventlog_socket.h eventlog_socket_opts_init"
    eventlog_socket_opts_init ::
        Ptr EventlogSocketOpts ->
        IO ()

--------------------------------------------------------------------------------
-- eventlog_socket_opts_free

foreign import capi safe "eventlog_socket.h eventlog_socket_opts_free"
    eventlog_socket_opts_free ::
        Ptr EventlogSocketOpts ->
        IO ()

--------------------------------------------------------------------------------
-- eventlog_socket_start

foreign import capi safe "GHC/Eventlog/Socket_hsc.h eventlog_socket_wrap_start"
    eventlog_socket_start ::
        Ptr EventlogSocketStatus ->
        Ptr EventlogSocketAddr ->
        Ptr EventlogSocketOpts ->
        IO ()

#{def
  HIDDEN void eventlog_socket_wrap_start(
    EventlogSocketStatus *eventlog_socket_status,
    EventlogSocketAddr *eventlog_socket_addr,
    EventlogSocketOpts *eventlog_socket_opts
  )
  {
    const EventlogSocketStatus status = eventlog_socket_start(
      eventlog_socket_addr,
      eventlog_socket_opts
    );
    memcpy(eventlog_socket_status, &status, sizeof(EventlogSocketStatus));
  }
}

--------------------------------------------------------------------------------
-- eventlog_socket_from_env

foreign import capi safe "GHC/Eventlog/Socket_hsc.h eventlog_socket_wrap_from_env"
    eventlog_socket_from_env ::
        Ptr EventlogSocketStatus ->
        Ptr EventlogSocketAddr ->
        Ptr EventlogSocketOpts ->
        IO ()

#{def
  HIDDEN void eventlog_socket_wrap_from_env(
    EventlogSocketStatus *eventlog_socket_status,
    EventlogSocketAddr *eventlog_socket_addr,
    EventlogSocketOpts *eventlog_socket_opts
  )
  {
    const EventlogSocketStatus status = eventlog_socket_from_env(
      eventlog_socket_addr,
      eventlog_socket_opts
    );
    memcpy(eventlog_socket_status, &status, sizeof(EventlogSocketStatus));
  }
}

--------------------------------------------------------------------------------
-- eventlog_socket_wait

foreign import capi safe "eventlog_socket.h eventlog_socket_wait"
    eventlog_socket_wait :: IO ()

--------------------------------------------------------------------------------
-- eventlog_socket_strerror

foreign import capi safe "GHC/Eventlog/Socket_hsc.h eventlog_socket_wrap_strerror"
    eventlog_socket_strerror ::
        Ptr EventlogSocketStatus ->
        IO CString

#{def
  HIDDEN char *eventlog_socket_wrap_strerror(
    EventlogSocketStatus *eventlog_socket_status
  )
  {
    return eventlog_socket_strerror(*eventlog_socket_status);
  }
}

--------------------------------------------------------------------------------
-- eventlog_socket_worker_status

foreign import capi safe "GHC/Eventlog/Socket_hsc.h eventlog_socket_wrap_worker_status"
    eventlog_socket_worker_status ::
        Ptr EventlogSocketStatus ->
        IO ()

#{def
  HIDDEN void eventlog_socket_wrap_worker_status(
    EventlogSocketStatus *eventlog_socket_status_out
  )
  {
    const EventlogSocketStatus eventlog_socket_status = eventlog_socket_worker_status();
    memcpy(eventlog_socket_status_out, &eventlog_socket_status, sizeof(EventlogSocketStatus));
  }
}

--------------------------------------------------------------------------------
-- eventlog_socket_control_status

foreign import capi safe "GHC/Eventlog/Socket_hsc.h eventlog_socket_wrap_control_status"
    eventlog_socket_control_status ::
        Ptr EventlogSocketStatus ->
        IO ()

#{def
  HIDDEN void eventlog_socket_wrap_control_status(
    EventlogSocketStatus *eventlog_socket_status_out
  )
  {
    const EventlogSocketStatus eventlog_socket_status = eventlog_socket_control_status();
    memcpy(eventlog_socket_status_out, &eventlog_socket_status, sizeof(EventlogSocketStatus));
  }
}

--------------------------------------------------------------------------------
-- eventlog_socket_control_strnamespace

-- NOTE: This uses `ccall` rather than `capi` because the underlying function
--       returns a `const char*` and `ConstPtr` wasn't added until GHC 9.6.1.

foreign import ccall safe "eventlog_socket.h eventlog_socket_control_strnamespace"
    eventlog_socket_control_strnamespace ::
        Ptr Namespace ->
        IO CString

--------------------------------------------------------------------------------
-- eventlog_socket_control_register_namespace

foreign import capi safe "GHC/Eventlog/Socket_hsc.h eventlog_socket_wrap_control_register_namespace"
    eventlog_socket_control_register_namespace ::
        Ptr EventlogSocketStatus ->
        ( #{type uint8_t} ) ->
        CString ->
        Ptr (Ptr Namespace) ->
        IO ()

#{def
  HIDDEN void eventlog_socket_wrap_control_register_namespace(
    EventlogSocketStatus *eventlog_socket_status,
    uint8_t eventlog_socket_namespace_len,
    char eventlog_socket_namespace[eventlog_socket_namespace_len],
    EventlogSocketControlNamespace **eventlog_socket_namespace_out
  ) {
    const EventlogSocketStatus status = eventlog_socket_control_register_namespace(
      eventlog_socket_namespace_len,
      eventlog_socket_namespace,
      eventlog_socket_namespace_out
    );
    memcpy(eventlog_socket_status, &status, sizeof(EventlogSocketStatus));
  }
}

--------------------------------------------------------------------------------
-- eventlog_socket_control_register_command

foreign import capi safe "GHC/Eventlog/Socket_hsc.h eventlog_socket_wrap_control_register_command"
    eventlog_socket_control_register_command ::
        Ptr EventlogSocketStatus ->
        Ptr Namespace ->
        CommandId ->
        FunPtr (Ptr Namespace -> CommandId -> Ptr a -> IO ()) ->
        Ptr a ->
        IO ()

foreign import ccall "wrapper"
    makeCommandHandlerFunPtr ::
        (Ptr Namespace -> CommandId -> Ptr a -> IO ()) ->
        IO (FunPtr (Ptr Namespace -> CommandId -> Ptr a -> IO ()))

#{def
  HIDDEN void eventlog_socket_wrap_control_register_command(
    EventlogSocketStatus *eventlog_socket_status,
    EventlogSocketControlNamespace *eventlog_socket_namespace,
    EventlogSocketControlCommandId eventlog_socket_command_id,
    EventlogSocketControlCommandHandler eventlog_socket_command_handler,
    const void *eventlog_socket_command_data
  ) {
    const EventlogSocketStatus status = eventlog_socket_control_register_command(
      eventlog_socket_namespace,
      eventlog_socket_command_id,
      eventlog_socket_command_handler,
      eventlog_socket_command_data
    );
    memcpy(eventlog_socket_status, &status, sizeof(EventlogSocketStatus));
  }
}
