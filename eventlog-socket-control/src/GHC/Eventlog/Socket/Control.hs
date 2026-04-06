{- |
Module      : GHC.Eventlog.Socket.Control
Description : Construct @eventlog-socket@ control command messages.
Stability   : stable
Portability : portable

This modules has utilities for constructing @eventlog-socket@ control command messages.
The builtin commands are `startHeapProfiling`, `stopHeapProfiling`, and `requestHeapCensus`.
To construct custom command messages, use `userNamespace` and `userCommand`.

The t`Command` type exposes a t`Binary` instance, and can be used with the functions from
the [@binary@](https://hackage.haskell.org/package/binary) package. For convenience, this
module exposes `encodeLazy` and `encodeStrict`, which encode a command to a lazy
`BSL.ByteString` or a strict `BS.ByteString`, respectively.

For instance, the following snippet defines @sendRequestHeapCensus@,
which serialises and sends a `requestHeapCensus` message over an open eventlog socket.

@
import Network.Socket (Socket)
import Network.Socket.ByteString.Lazy (sendAll)
import GHC.Eventlog.Socket.Control (encodeLazy, requestHeapCensus)

sendRequestHeapCensus :: Socket -> IO ()
sendRequestHeapCensus eventlogSocket =
    sendAll eventlogSocket (`encodeLazy` `requestHeapCensus`)
@
-}
module GHC.Eventlog.Socket.Control (
    -- * Control Commands
    Command,
    CommandId (..),
    Namespace,
    startProfiling,
    stopProfiling,
    startHeapProfiling,
    stopHeapProfiling,
    requestHeapCensus,
    userNamespace,
    userCommand,
    encodeLazy,
    encodeStrict,

    -- ** Protocol Version
    ProtocolVersion (..),
    protocolVersion,
    minProtocolVersion,
    ProtocolVersionNotSupportedError (..),

    -- ** Namespace Errors
    NamespaceReservedError (..),
    NamespaceTooLongError (..),
) where

import Control.Exception (Exception (..), assert, throw)
import Control.Monad (replicateM, unless)
import Data.Binary (Binary (..), Get, Put, getWord8, putWord8)
import Data.Binary qualified as B (encode)
import Data.ByteString qualified as BS
import Data.ByteString.Lazy qualified as BSL
import Data.Foldable (for_, traverse_)
import Data.String (IsString (..))
import Data.Text (Text)
import Data.Text qualified as T (pack, unpack)
import Data.Text.Encoding qualified as TE (decodeUtf8, encodeUtf8)
import Data.Word (Word8)
import Text.Printf (printf)

--------------------------------------------------------------------------------
-- Magic
--------------------------------------------------------------------------------

{- |
Internal helper.

A constant that serialises to the magic byte sequence.
-}
data Magic = Magic

{- |
Internal helper.

The magic byte sequence that starts a protocol message.
-}
magicBytes :: [Word8]
magicBytes = [0xF0, 0x9E, 0x97, 0x8C]

instance Binary (WithBinary Magic) where
    put :: (WithBinary Magic) -> Put
    put (WithBinary Magic) = traverse_ putWord8 magicBytes

    get :: Get (WithBinary Magic)
    get = do
        for_ magicBytes $ \expect ->
            getWord8 >>= \actual ->
                unless (expect == actual) . fail $
                    printf "Unexpected %02x, expected %02x" actual expect
        pure $ WithBinary Magic

--------------------------------------------------------------------------------
-- Protocol Versions
--------------------------------------------------------------------------------

{- |
Version numbers for @eventlog-socket@'s control command protocol.

@since 0.1.0.0
-}
newtype ProtocolVersion = ProtocolVersion Word8
    deriving (Eq, Ord, Show)

{- |
The error thrown when deserialising a protocol message from an incompatible protocol version.

@since 0.1.0.0
-}
data ProtocolVersionNotSupportedError = ProtocolVersionNotSupportedError ProtocolVersion
    deriving (Show)

instance Exception ProtocolVersionNotSupportedError

{- |
The version of the protocol implemented by this package.

@since 0.1.0.0
-}
protocolVersion :: ProtocolVersion
protocolVersion = ProtocolVersion 0

{- |
The minimum version of the protocol supported by this package.

@since 0.1.0.0
-}
minProtocolVersion :: ProtocolVersion
minProtocolVersion = protocolVersion

{- |
Internal helper.

Check if the given protocol version is supported.
-}
isProtocolVersionSupported :: ProtocolVersion -> Bool
isProtocolVersionSupported = (>= minProtocolVersion)

instance Binary (WithBinary ProtocolVersion) where
    put :: (WithBinary ProtocolVersion) -> Put
    put (WithBinary (ProtocolVersion protocolVersionByte)) = putWord8 protocolVersionByte

    get :: Get (WithBinary ProtocolVersion)
    get = WithBinary . ProtocolVersion <$> getWord8

--------------------------------------------------------------------------------
-- Namespaces
--------------------------------------------------------------------------------

{- |
The type of control command namespaces.

Namespaces are opaque and can only be obtained using `userNamespace`.

@since 0.1.0.0
-}
data Namespace = Namespace
    { namespaceText :: !Text
    -- ^ The namespace name.
    , namespaceUtf8 :: BS.ByteString
    -- ^ Must satisfy the following invariants:
    --     prop> namespaceUtf8 == encodeUtf8 namespaceText
    --     prop> length namespaceUtf8 <= fromIntegral (maxBound :: Word8)
    }
    deriving (Eq)

{- |
__Warning__: This instance uses `userNamespace` and may throw the same errors.
-}
instance IsString Namespace where
    fromString :: String -> Namespace
    fromString = userNamespace . T.pack

instance Show Namespace where
    show :: Namespace -> String
    show = show . T.unpack . namespaceText

instance Binary (WithBinary Namespace) where
    put :: WithBinary Namespace -> Put
    put (WithBinary Namespace{namespaceUtf8 = namespaceBytes}) = do
        let namespaceNumBytes = BS.length namespaceBytes
        assert (namespaceNumBytes <= namespaceMaxBytes) $ do
            -- Put the namespace length as a Word8
            putWord8 (fromIntegral namespaceNumBytes)
            -- -- Put the namespace bytes
            for_ [0 .. namespaceNumBytes - 1] $ \i ->
                for_ (BS.indexMaybe namespaceBytes i) $ \namespaceByte ->
                    putWord8 namespaceByte

    get :: Get (WithBinary Namespace)
    get = do
        -- Get the namespace length as a Word8
        namespaceNumBytes <- fromIntegral <$> getWord8
        -- Get the namespace bytes
        namespaceBytes <- BS.pack <$> replicateM namespaceNumBytes getWord8
        -- Create a namespace
        pure . WithBinary $
            Namespace
                { namespaceText = TE.decodeUtf8 namespaceBytes
                , namespaceUtf8 = namespaceBytes
                }

{- |
Internal helper.

Construct a namespace ignoring the user namespace invariants.
-}
unsafeMakeNamespace :: Text -> Namespace
unsafeMakeNamespace namespaceText' =
    Namespace
        { namespaceText = namespaceText'
        , namespaceUtf8 = TE.encodeUtf8 namespaceText'
        }

{- |
Internal helper.

The namespace for builtin control commands.
-}
eventlogSocketNamespace :: Namespace
eventlogSocketNamespace = unsafeMakeNamespace (T.pack "eventlog-socket")

{- |
Internal helper.

Check if a namespace uses a reserved name.
-}
isNamespaceReserved :: Namespace -> Bool
isNamespaceReserved = (== eventlogSocketNamespace)

{- |
The error thrown when the name passed to `userNamespace` is reserved.

@since 0.1.0.0
-}
newtype NamespaceReservedError = NamespaceReservedError Namespace
    deriving (Show)

instance Exception NamespaceReservedError

{- |
Internal helper.

The maximum size for a namespace name.
-}
namespaceMaxBytes :: Int
namespaceMaxBytes = fromIntegral (maxBound :: Word8)

{- |
Internal helper.

Check if a namespace name is too long.
-}
isNamespaceTooLong :: Namespace -> Bool
isNamespaceTooLong Namespace{namespaceUtf8 = namespaceBytes} =
    BS.length namespaceBytes > namespaceMaxBytes

{- |
The error thrown when the name passed to `userNamespace` is longer than 255 bytes.

@since 0.1.0.0
-}
data NamespaceTooLongError = NamespaceTooLongError Namespace
    deriving (Show)

instance Exception NamespaceTooLongError

{- |
Construct a namespace for use in constructing a custom control command message.

This function may throw t`NamespaceReservedError` or t`NamespaceTooLongError`.

@since 0.1.0.0
-}
userNamespace :: Text -> Namespace
userNamespace namespaceText'
    | isNamespaceReserved namespace = throw (NamespaceReservedError namespace)
    | isNamespaceTooLong namespace = throw (NamespaceTooLongError namespace)
    | otherwise = namespace
  where
    namespace = unsafeMakeNamespace namespaceText'

--------------------------------------------------------------------------------
-- Command ID
--------------------------------------------------------------------------------

{- |
The type of control command IDs.

Command IDs must be non-zero integers between 1 and 255.

@since 0.1.0.0
-}
newtype CommandId = CommandId Word8
    deriving (Eq, Show)

-- NOTE: Take care that this definition does not drift too far from the
--       definition of the same name in @eventlog-socket@. The two live
--       in different packages and serve different purposes. Notably,
--       the definition in @eventlog-socket@ has a `Storable` instance
--       whereas this one has a `Binary` instance. However, that does not
--       mean we should be giving the user any surprises.

instance Binary (WithBinary CommandId) where
    put :: (WithBinary CommandId) -> Put
    put (WithBinary (CommandId i)) = putWord8 i

    get :: Get (WithBinary CommandId)
    get = WithBinary . CommandId <$> getWord8

--------------------------------------------------------------------------------
-- Command
--------------------------------------------------------------------------------

{- |
The type of control command messages.

Commands are opaque and can only be obtained using `userCommand` or as builtin
commands `startHeapProfiling`, `stopHeapProfiling`, and `requestHeapCensus`.

@since 0.1.0.0
-}
data Command = Command !Namespace !CommandId
    deriving (Eq, Show)

{- |
Construct a custom control command message.

@since 0.1.0.0
-}
userCommand :: Namespace -> CommandId -> Command
userCommand namespace commandId = Command namespace commandId

{- |
Encode a custom control command message to a lazy t`BSL.ByteString`.

@since 0.1.0.0
-}
encodeLazy :: Command -> BSL.ByteString
encodeLazy = B.encode

{- |
Encode a custom control command message to a strict t`BS.ByteString`.

@since 0.1.0.0
-}
encodeStrict :: Command -> BS.ByteString
encodeStrict = BSL.toStrict . encodeLazy

instance Binary Command where
    put :: Command -> Put
    put (Command namespace commandId) = do
        put $ WithBinary Magic
        put $ WithBinary protocolVersion
        put $ WithBinary namespace
        put $ WithBinary commandId

    get :: Get Command
    get = do
        WithBinary Magic <- get
        WithBinary commandProtocolVersion <- get
        unless (isProtocolVersionSupported commandProtocolVersion) $
            throw $
                ProtocolVersionNotSupportedError commandProtocolVersion
        WithBinary namespace <- get
        WithBinary commandId <- get
        pure $ Command namespace commandId

--------------------------------------------------------------------------------
-- Builtin Commands
--------------------------------------------------------------------------------

{- |
The builtin control command message that starts profiling.

See `GHC.Profiling.startProfTimer`.

@since 0.1.1.0
-}
startProfiling :: Command
startProfiling = Command eventlogSocketNamespace (CommandId 1)

{- |
The builtin control command message that stops profiling.

See `GHC.Profiling.stopProfTimer`.

@since 0.1.1.0
-}
stopProfiling :: Command
stopProfiling = Command eventlogSocketNamespace (CommandId 2)

{- |
The builtin control command message that starts heap profiling.

See `GHC.Profiling.startHeapProfTimer`.

@since 0.1.0.0
-}
startHeapProfiling :: Command
startHeapProfiling = Command eventlogSocketNamespace (CommandId 3)

{- |
The builtin control command message that stops heap profiling.

See `GHC.Profiling.stopHeapProfTimer`.

@since 0.1.0.0
-}
stopHeapProfiling :: Command
stopHeapProfiling = Command eventlogSocketNamespace (CommandId 4)

{- |
The builtin control command message that requests a heap census.

See `GHC.Profiling.requestHeapCensus`.

@since 0.1.0.0
-}
requestHeapCensus :: Command
requestHeapCensus = Command eventlogSocketNamespace (CommandId 5)

--------------------------------------------------------------------------------
-- Internal helpers
--------------------------------------------------------------------------------

{- |
Internal helper.

This wrapper hides `Binary` instances from the public API.
-}
newtype WithBinary a = WithBinary a
