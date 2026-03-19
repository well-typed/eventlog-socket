{-# LANGUAGE CPP #-}

module Main where

import Control.Monad (forever)
import Data.Foldable (for_, traverse_)
import Data.Maybe (fromMaybe)
import Debug.Trace (flushEventLog, traceMarkerIO)
import GHC.Eventlog.Socket (startFromEnv)
import System.Environment (getArgs, lookupEnv)

#if MIN_VERSION_base(4,20,0)
import System.Mem (performBlockingMajorGC)
#else
import System.Mem (performMajorGC)
#endif

data Mode = Finite | Infinite

parseArgs :: [String] -> (Mode, [String])
parseArgs ("--forever" : rest) = (Infinite, rest)
parseArgs args = (Finite, args)

performPreferablyBlockingMajorGC :: IO ()
#if MIN_VERSION_base(4,20,0)
performPreferablyBlockingMajorGC = performBlockingMajorGC
#else
performPreferablyBlockingMajorGC = performMajorGC
#endif

main :: IO ()
main = do
    startFromEnv
    args <- getArgs
    let (mode, fibArgs) = parseArgs args
        workload = for_ fibArgs $ \arg -> do
            traceMarkerIO $ "Starting fib " <> arg
            print $ fib (read arg)
            traceMarkerIO $ "Finished fib " <> arg
            performPreferablyBlockingMajorGC
    case (mode, fibArgs) of
        (_, []) -> putStrLn "Provide at least one integer argument."
        (Finite, _) -> workload
        (Infinite, _) -> forever workload
    flushEventLog

fib :: Integer -> Integer
fib 0 = 0
fib 1 = 1
fib n = fib (n - 1) + fib (n - 2)
