module Main where

import Control.Monad (forever)
import Data.Foldable (for_)
import Data.Maybe (fromMaybe)
import Data.Word (Word16)
import GHC.Eventlog.Socket
import System.Environment
import Text.Read (readMaybe)

data Mode = Finite | Infinite

parseArgs :: [String] -> (Mode, [String])
parseArgs ("--forever" : rest) = (Infinite, rest)
parseArgs args = (Finite, args)

main :: IO ()
main = do
    fibberHostEnv <- lookupEnv "FIBBER_EVENTLOG_TCP_HOST"
    fibberPortEnv <- lookupEnv "FIBBER_EVENTLOG_TCP_PORT"
    let fibberHost = fromMaybe "127.0.0.1" fibberHostEnv
        fibberPort :: Word16
        fibberPort = fromMaybe 4242 (fibberPortEnv >>= readMaybe)
    startTcp TcpSocket{ tcpHost = fibberHost, tcpPort = fibberPort }
    wait
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
