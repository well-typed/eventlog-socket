module GHC.Eventlog.Socket.Control (
    -- * Protocol Version
    ProtocolVersion (..),
    protocolVersion,
    minProtocolVersion,
    ProtocolVersionNotSupportedError (..),

    -- * Namespace
    Namespace,
    userNamespace,
    NamespaceReservedError (..),
    NamespaceTooLongError (..),

    -- * Command
    Command,
    CommandId (..),
    userCommand,
    startHeapProfiling,
    stopHeapProfiling,
    requestHeapCensus,
) where

import Control.Exception (Exception (..), assert, throw)
import Control.Monad (replicateM, unless)
import Data.Binary (Binary (..), Get, Put, getWord8, putWord8)
import Data.ByteString (ByteString)
import Data.ByteString qualified as BS
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

data Magic = Magic

magicBytes :: [Word8]
magicBytes = [0xF0, 0x9E, 0x97, 0x8C]

instance Binary Magic where
    put :: Magic -> Put
    put Magic = traverse_ putWord8 magicBytes

    get :: Get Magic
    get = do
        for_ magicBytes $ \expect ->
            getWord8 >>= \actual ->
                unless (expect == actual) . fail $
                    printf "Unexpected %02x, expected %02x" actual expect
        pure Magic

--------------------------------------------------------------------------------
-- Protocol Version
--------------------------------------------------------------------------------

newtype ProtocolVersion = ProtocolVersion {protocolVersionByte :: Word8}
    deriving (Eq, Ord, Show)

data ProtocolVersionNotSupportedError = ProtocolVersionNotSupportedError ProtocolVersion
    deriving (Show)

instance Exception ProtocolVersionNotSupportedError

protocolVersion :: ProtocolVersion
protocolVersion = ProtocolVersion{protocolVersionByte = 0}

minProtocolVersion :: ProtocolVersion
minProtocolVersion = protocolVersion

isProtocolVersionSupported :: ProtocolVersion -> Bool
isProtocolVersionSupported = (>= minProtocolVersion)

instance Binary ProtocolVersion where
    put :: ProtocolVersion -> Put
    put = putWord8 . protocolVersionByte

    get :: Get ProtocolVersion
    get = ProtocolVersion <$> getWord8

--------------------------------------------------------------------------------
-- Protocol Version
--------------------------------------------------------------------------------

data Namespace = Namespace
    { namespaceText :: !Text
    , namespaceUtf8 :: ByteString
    -- ^ Must satisfy the following invariants:
    --     prop> namespaceUtf8 == encodeUtf8 namespaceText
    --     prop> length namespaceUtf8 <= fromIntegral (maxBound :: Word8)
    }
    deriving (Eq)

instance IsString Namespace where
    fromString :: String -> Namespace
    fromString = userNamespace . T.pack

instance Show Namespace where
    show :: Namespace -> String
    show = show . T.unpack . namespaceText

instance Binary Namespace where
    put :: Namespace -> Put
    put Namespace{namespaceUtf8 = namespaceBytes} = do
        let namespaceNumBytes = BS.length namespaceBytes
        assert (namespaceNumBytes <= namespaceMaxBytes) $ do
            -- Put the namespace length as a Word8
            putWord8 (fromIntegral namespaceNumBytes)
            -- -- Put the namespace bytes
            for_ [0 .. namespaceNumBytes - 1] $ \i ->
                for_ (BS.indexMaybe namespaceBytes i) $ \namespaceByte ->
                    putWord8 namespaceByte

    get :: Get Namespace
    get = do
        -- Get the namespace length as a Word8
        namespaceNumBytes <- fromIntegral <$> getWord8
        -- Get the namespace bytes
        namespaceBytes <- BS.pack <$> replicateM namespaceNumBytes getWord8
        -- Create a namespace
        pure
            Namespace
                { namespaceText = TE.decodeUtf8 namespaceBytes
                , namespaceUtf8 = namespaceBytes
                }

unsafeMakeNamespace :: Text -> Namespace
unsafeMakeNamespace namespaceText' =
    Namespace
        { namespaceText = namespaceText'
        , namespaceUtf8 = TE.encodeUtf8 namespaceText'
        }

eventlogSocketNamespace :: Namespace
eventlogSocketNamespace = unsafeMakeNamespace (T.pack "eventlog-socket")

isNamespaceReserved :: Namespace -> Bool
isNamespaceReserved = (== eventlogSocketNamespace)

newtype NamespaceReservedError = NamespaceReservedError Namespace
    deriving (Show)

instance Exception NamespaceReservedError

namespaceMaxBytes :: Int
namespaceMaxBytes = fromIntegral (maxBound :: Word8)

isNamespaceTooLong :: Namespace -> Bool
isNamespaceTooLong Namespace{namespaceUtf8 = namespaceBytes} =
    BS.length namespaceBytes > namespaceMaxBytes

data NamespaceTooLongError = NamespaceTooLongError Namespace
    deriving (Show)

instance Exception NamespaceTooLongError

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

newtype CommandId = CommandId {unCommandId :: Word8}
    deriving (Eq, Show)

instance Binary CommandId where
    put :: CommandId -> Put
    put = putWord8 . unCommandId

    get :: Get CommandId
    get = CommandId <$> getWord8

--------------------------------------------------------------------------------
-- Command
--------------------------------------------------------------------------------

userCommand :: Namespace -> CommandId -> Command
userCommand namespace commandId
    | isNamespaceReserved namespace = throw (NamespaceReservedError namespace)
    | isNamespaceTooLong namespace = throw (NamespaceTooLongError namespace)
    | otherwise = Command namespace commandId

data Command = Command !Namespace !CommandId
    deriving (Eq, Show)

instance Binary Command where
    put :: Command -> Put
    put (Command namespace commandId) = do
        put Magic
        put protocolVersion
        put namespace
        put commandId

    get :: Get Command
    get = do
        Magic <- get
        commandProtocolVersion <- get
        unless (isProtocolVersionSupported commandProtocolVersion) $
            throw $
                ProtocolVersionNotSupportedError commandProtocolVersion
        namespace <- get
        commandId <- get
        pure $ Command namespace commandId

--------------------------------------------------------------------------------
-- Builtin Commands
--------------------------------------------------------------------------------

startHeapProfiling :: Command
startHeapProfiling = Command eventlogSocketNamespace (CommandId 0)

stopHeapProfiling :: Command
stopHeapProfiling = Command eventlogSocketNamespace (CommandId 1)

requestHeapCensus :: Command
requestHeapCensus = Command eventlogSocketNamespace (CommandId 2)
