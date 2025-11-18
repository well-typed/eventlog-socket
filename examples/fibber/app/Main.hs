module Main where

import Control.Monad (forever)
import Data.Foldable (for_)
import Data.Maybe (fromMaybe)
import GHC.Eventlog.Socket
import System.Environment

data Mode = Finite | Infinite

parseArgs :: [String] -> (Mode, [String])
parseArgs ("--forever" : rest) = (Infinite, rest)
parseArgs args = (Finite, args)

main :: IO ()
main = do
    fibberEventlogSocket <-
        fromMaybe "/tmp/fibber_eventlog.sock"
            <$> lookupEnv "FIBBER_EVENTLOG_SOCKET"
    startWait fibberEventlogSocket
    args <- getArgs
    let (mode, fibArgs) = parseArgs args
        workload = for_ fibArgs $ \arg -> print (fib (read arg))
    case (mode, fibArgs) of
        (_, []) -> putStrLn "Provide at least one integer argument."
        (Finite, _) -> workload
        (Infinite, _) -> forever workload

fib :: Integer -> Integer
fib 0 = 0
fib 1 = 1
fib n = fib (n - 1) + fib (n - 2)
