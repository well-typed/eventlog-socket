{-# LANGUAGE ViewPatterns #-}

module Main where

import Control.Concurrent (threadDelay)
import Debug.Trace (flushEventLog, traceEventIO, traceMarkerIO)
import GHC.Eventlog.Socket (CommandId (..), registerCommand, registerNamespace, startFromEnv, testControlStatus, testWorkerStatus)
import System.Environment (getArgs)
import Text.Read (readMaybe)

main :: IO ()
main = do
    -- Register custom commands
    myNamespace <- registerNamespace "custom-command"
    registerCommand myNamespace (CommandId 1) (traceEventIO "handled ping")
    registerCommand myNamespace (CommandId 2) (traceEventIO "handled pong")

    -- Start eventlog-socket from the environment
    startFromEnv

    -- Do stuff
    maybeEnd <- parseArgs <$> getArgs
    putStrLn . maybe "looping forever" (\n -> "looping " <> show n <> " times") $ maybeEnd
    loopFromTo 0 maybeEnd

    -- Poll for asynchronous errors
    testWorkerStatus
    testControlStatus
  where
    loopFromTo n maybeEnd
        | Just m <- maybeEnd, n >= m = pure ()
        | otherwise = do
            traceMarkerIO $ "custom workload iteration " ++ show n
            threadDelay 500000
            flushEventLog

            -- Poll for asynchronous errors
            testWorkerStatus
            testControlStatus
            loopFromTo (n + 1) maybeEnd

parseArgs :: [String] -> Maybe Int
parseArgs ("--forever" : _xs) = Nothing
parseArgs ((readMaybe -> Just n) : _xs) = Just n
parseArgs _xs = Just 5
