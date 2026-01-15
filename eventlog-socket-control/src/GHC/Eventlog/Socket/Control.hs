{-# LANGUAGE PatternSynonyms #-}

module GHC.Eventlog.Socket.Control (
    Namespace,
    userNamespace,
    CommandId (..),
    Command (StartHeapProfiling, StopHeapProfiling, RequestHeapProfile),
    userCommand,
) where

import Control.Exception (Exception (displayException), throw)
import Control.Monad (unless, when)
import Data.Binary (Binary (..), Get, Put, getWord8, putWord8)
import Data.Foldable (traverse_)
import Data.Word (Word8)
import Text.Printf (printf)
import Prelude hiding (getChar)

userCommand :: Namespace -> CommandId -> Command
userCommand namespace commandId
    | namespace == BuiltinNamespace = throw CannotUseBuiltinNamespace
    | otherwise = Command namespace commandId

newtype Namespace = Namespace {unNamespace :: Word8}
    deriving (Eq, Show)

pattern BuiltinNamespace :: Namespace
pattern BuiltinNamespace = Namespace 0

userNamespace :: Word8 -> Namespace
userNamespace namespaceId
    | namespace == BuiltinNamespace = throw CannotUseBuiltinNamespace
    | otherwise = namespace
  where
    namespace = Namespace namespaceId

data CannotUseBuiltinNamespace = CannotUseBuiltinNamespace
    deriving (Show)

instance Exception CannotUseBuiltinNamespace

newtype CommandId = CommandId {unCommandId :: Word8}
    deriving (Eq, Show)

nextBuiltinCommandId :: CommandId
nextBuiltinCommandId = CommandId 3

data UnknownBuiltinCommandId = UnknownBuiltinCommandId CommandId
    deriving (Show)

instance Exception UnknownBuiltinCommandId

data Command = Command !Namespace !CommandId
    deriving (Eq, Show)

pattern StartHeapProfiling :: Command
pattern StartHeapProfiling = Command BuiltinNamespace (CommandId 0)

pattern StopHeapProfiling :: Command
pattern StopHeapProfiling = Command BuiltinNamespace (CommandId 1)

pattern RequestHeapProfile :: Command
pattern RequestHeapProfile = Command BuiltinNamespace (CommandId 2)

instance Binary Command where
    put :: Command -> Put
    put (Command namespace commandId) = do
        putList "GCTL"
        putNamespace namespace
        putCommandId commandId

    get :: Get Command
    get = do
        getString "GCTL"
        namespace <- getNamespace
        commandId <- getCommandId
        when (namespace == BuiltinNamespace) $
            when (unCommandId commandId >= unCommandId nextBuiltinCommandId) $
                fail . displayException $
                    UnknownBuiltinCommandId commandId
        pure $ Command namespace commandId

--------------------------------------------------------------------------------
-- Internal helpers.
--------------------------------------------------------------------------------

putNamespace :: Namespace -> Put
putNamespace = putWord8 . unNamespace

getNamespace :: Get Namespace
getNamespace = Namespace <$> getWord8

putCommandId :: CommandId -> Put
putCommandId = putWord8 . unCommandId

getCommandId :: Get CommandId
getCommandId = CommandId <$> getWord8

getString :: String -> Get ()
getString = traverse_ getChar

getChar :: Char -> Get ()
getChar c =
    get >>= \c' ->
        unless (c == c') . fail $
            printf "Unexpected %02x, expected %02x" c' c
