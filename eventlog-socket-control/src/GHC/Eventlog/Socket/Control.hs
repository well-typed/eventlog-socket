module GHC.Eventlog.Socket.Control (
    Namespace,
    userNamespace,
    CommandId (..),
    Command,
    startHeapProfiling,
    stopHeapProfiling,
    requestHeapProfile,
    userCommand,
) where

import Control.Exception (Exception (displayException), assert, throw)
import Control.Monad (replicateM, unless, when)
import Data.Binary (Binary (..), Get, Put, getWord8, putWord8)
import Data.ByteString (ByteString)
import qualified Data.ByteString as BS
import Data.Foldable (for_, traverse_)
import Data.String (IsString (..))
import Data.Text (Text)
import qualified Data.Text as T (pack, unpack)
import qualified Data.Text.Encoding as TE (decodeUtf8, encodeUtf8)
import Data.Word (Word8)
import Text.Printf (printf)
import Prelude hiding (getChar, length)

userCommand :: Namespace -> CommandId -> Command
userCommand namespace commandId
    | isBuiltinNamespace namespace = throw CannotUseBuiltinNamespace
    | isNamespaceTooLong namespace = throw (NamespaceTooLong namespace)
    | otherwise = Command namespace commandId

data Namespace = Namespace
    { namespaceText :: !Text
    , namespaceUtf8 :: ByteString
    -- ^ Must satisfy the following invariants:
    --     prop> namespaceUtf8 == encodeUtf8 namespaceText
    --     prop> length namespaceUtf8 <= fromIntegral (maxBound :: Word8)
    }
    deriving (Eq)

instance Show Namespace where
    show :: Namespace -> String
    show = show . T.unpack . namespaceText

unsafeMakeNamespace :: Text -> Namespace
unsafeMakeNamespace namespaceText' =
    Namespace
        { namespaceText = namespaceText'
        , namespaceUtf8 = TE.encodeUtf8 namespaceText'
        }

eventlogSocketNamespace :: Namespace
eventlogSocketNamespace = unsafeMakeNamespace (T.pack "eventlog-socket")

isBuiltinNamespace :: Namespace -> Bool
isBuiltinNamespace = (== eventlogSocketNamespace)

userNamespace :: Text -> Namespace
userNamespace (unsafeMakeNamespace -> namespace)
    | isBuiltinNamespace namespace = throw CannotUseBuiltinNamespace
    | isNamespaceTooLong namespace = throw (NamespaceTooLong namespace)
    | otherwise = namespace

instance IsString Namespace where
    fromString :: String -> Namespace
    fromString = userNamespace . T.pack

data CannotUseBuiltinNamespace = CannotUseBuiltinNamespace
    deriving (Show)

instance Exception CannotUseBuiltinNamespace

namespaceMaxBytes :: Int
namespaceMaxBytes = fromIntegral (maxBound :: Word8)

isNamespaceTooLong :: Namespace -> Bool
isNamespaceTooLong (namespaceUtf8 -> namespaceBytes) =
    BS.length namespaceBytes > namespaceMaxBytes

data NamespaceTooLong = NamespaceTooLong Namespace
    deriving (Show)

instance Exception NamespaceTooLong

newtype CommandId = CommandId {unCommandId :: Word8}
    deriving (Eq, Show)

nextBuiltinCommandId :: CommandId
nextBuiltinCommandId = CommandId 3

data UnknownBuiltinCommandId = UnknownBuiltinCommandId CommandId
    deriving (Show)

instance Exception UnknownBuiltinCommandId

data Command = Command !Namespace !CommandId
    deriving (Eq, Show)

startHeapProfiling :: Command
startHeapProfiling = Command eventlogSocketNamespace (CommandId 0)

stopHeapProfiling :: Command
stopHeapProfiling = Command eventlogSocketNamespace (CommandId 1)

requestHeapProfile :: Command
requestHeapProfile = Command eventlogSocketNamespace (CommandId 2)

instance Binary Command where
    put :: Command -> Put
    put (Command namespace commandId) = do
        putControlMagic
        putNamespace namespace
        putCommandId commandId

    get :: Get Command
    get = do
        getControlMagic
        namespace <- getNamespace
        commandId <- getCommandId
        when (namespace == eventlogSocketNamespace) $
            when (unCommandId commandId >= unCommandId nextBuiltinCommandId) $
                fail . displayException $
                    UnknownBuiltinCommandId commandId
        pure $ Command namespace commandId

--------------------------------------------------------------------------------
-- Internal helpers.
--------------------------------------------------------------------------------

controlMagic :: [Word8]
controlMagic = [0xF0, 0x9E, 0x97, 0x8C]

putControlMagic :: Put
putControlMagic = traverse_ putWord8 controlMagic

getControlMagic :: Get ()
getControlMagic =
    for_ controlMagic $ \expect ->
        getWord8 >>= \actual ->
            unless (expect == actual) . fail $
                printf "Unexpected %02x, expected %02x" actual expect

putNamespace :: Namespace -> Put
putNamespace (namespaceUtf8 -> namespaceBytes) = do
    let namespaceNumBytes = BS.length namespaceBytes
    assert (namespaceNumBytes <= namespaceMaxBytes) $ do
        -- Put the namespace length as a Word8
        putWord8 (fromIntegral namespaceNumBytes)
        -- -- Put the namespace bytes
        for_ [0 .. namespaceNumBytes - 1] $ \i ->
            for_ (BS.indexMaybe namespaceBytes i) $ \namespaceByte ->
                putWord8 namespaceByte

getNamespace :: Get Namespace
getNamespace = do
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

putCommandId :: CommandId -> Put
putCommandId = putWord8 . unCommandId

getCommandId :: Get CommandId
getCommandId = CommandId <$> getWord8
