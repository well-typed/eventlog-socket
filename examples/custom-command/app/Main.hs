{-# LANGUAGE ViewPatterns #-}

module Main where

import Control.Concurrent (threadDelay)
import Debug.Trace (flushEventLog, traceEventIO, traceMarkerIO)
import GHC.Eventlog.Socket (wait)
import System.Environment (getArgs)
import Text.Read (readMaybe)

main :: IO ()
main = do
    traceEventIO "custom-command example running"
    traceEventIO "client connected; emitting workload"
    maybeEnd <- parseArgs <$> getArgs
    putStrLn . maybe "looping forever" (\n -> "looping " <> show n <> " times") $ maybeEnd
    loopFromTo 0 maybeEnd
  where
    loopFromTo n maybeEnd
        | Just m <- maybeEnd, n >= m = pure ()
        | otherwise = do
            traceMarkerIO $ "custom workload iteration " ++ show n
            threadDelay 500000
            flushEventLog
            loopFromTo (n + 1) maybeEnd

parseArgs :: [String] -> Maybe Int
parseArgs ("--forever" : _xs) = Nothing
parseArgs ((readMaybe -> Just n) : _xs) = Just n
parseArgs _xs = Just 5
