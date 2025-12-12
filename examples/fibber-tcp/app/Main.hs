{-# LANGUAGE RecordWildCards #-}

module Main where

import Control.Monad (forever)
import Data.Foldable (for_)
import GHC.Eventlog.Socket (TcpSocket (..), startTcp, wait)
import System.Environment (getArgs, lookupEnv)
import Text.Read (readMaybe)

data Mode = Finite | Infinite

parseArgs :: [String] -> (Mode, [String])
parseArgs ("--forever" : rest) = (Infinite, rest)
parseArgs args = (Finite, args)

main :: IO ()
main = do
    maybeTcpHost <- lookupEnv "GHC_EVENTLOG_TCP_HOST"
    maybeTcpPort <- lookupEnv "GHC_EVENTLOG_TCP_PORT"
    case (maybeTcpHost, readMaybe =<< maybeTcpPort) of
        (Just tcpHost, Just tcpPort) -> startTcp TcpSocket{..} >> wait
        (Just _tcpHost, Nothing) -> error "missing GHC_EVENTLOG_TCP_PORT"
        (Nothing, Just _tcpPort) -> error "missing GHC_EVENTLOG_TCP_PORT"
        (Nothing, Nothing) -> pure ()
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
