module Main where

import Control.Monad (forever)
import Data.Foldable (for_, traverse_)
import Data.Maybe (fromMaybe)
import GHC.Eventlog.Socket (startFromEnv)
import System.Environment (getArgs, lookupEnv)

data Mode = Finite | Infinite

parseArgs :: [String] -> (Mode, [String])
parseArgs ("--forever" : rest) = (Infinite, rest)
parseArgs args = (Finite, args)

main :: IO ()
main = do
    startFromEnv
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
