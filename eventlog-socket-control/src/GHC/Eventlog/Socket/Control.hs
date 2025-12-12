module GHC.Eventlog.Socket.Control (
    Command (..),
) where

import Control.Monad (unless)
import Data.Binary (Binary (..), Get, Put, getWord8, putWord8)
import Data.Foldable (traverse_)
import Text.Printf (printf)
import Prelude hiding (getChar)

data Command
    = StartHeapProfiling
    | StopHeapProfiling
    | RequestHeapProfile
    deriving (Eq, Show)

instance Binary Command where
    put :: Command -> Put
    put command = do
        putList "GCTL"
        case command of
            StartHeapProfiling -> putWord8 1
            StopHeapProfiling -> putWord8 2
            RequestHeapProfile -> putWord8 3

    get :: Get Command
    get = do
        getString "GCTL"
        getWord8 >>= \case
            1 -> pure StartHeapProfiling
            2 -> pure StopHeapProfiling
            3 -> pure RequestHeapProfile
            b -> fail $ printf "Unexpected %02x" b

--------------------------------------------------------------------------------
-- Internal helpers.
--------------------------------------------------------------------------------

getString :: String -> Get ()
getString = traverse_ getChar

getChar :: Char -> Get ()
getChar c =
    get >>= \c' ->
        unless (c == c') . fail $
            printf "Unexpected %02x, expected %02x" c' c
