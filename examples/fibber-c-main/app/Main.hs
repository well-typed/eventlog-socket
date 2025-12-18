module Main where

import Control.Monad (forever)
import Data.Foldable (for_, traverse_)
import Debug.Trace (flushEventLog, traceMarkerIO)
import GHC.Eventlog.Socket (wait)
import System.Environment (getArgs, lookupEnv)

data Mode = Finite | Infinite

parseArgs :: [String] -> (Mode, [String])
parseArgs ("--forever" : rest) = (Infinite, rest)
parseArgs args = (Finite, args)

main :: IO ()
main = do
    traverse_ (const wait) =<< lookupEnv "GHC_EVENTLOG_SOCKET"
    args <- getArgs
    let (mode, fibArgs) = parseArgs args
        workload = for_ fibArgs $ \arg -> do
            traceMarkerIO $ "Starting fib " <> arg
            print $ fib (read arg)
            traceMarkerIO $ "Finished fib " <> arg
    case (mode, fibArgs) of
        (_, []) -> putStrLn "Provide at least one integer argument."
        (Finite, _) -> workload
        (Infinite, _) -> forever workload
    flushEventLog

fib :: Integer -> Integer
fib 0 = 0
fib 1 = 1
fib n = fib (n - 1) + fib (n - 2)
