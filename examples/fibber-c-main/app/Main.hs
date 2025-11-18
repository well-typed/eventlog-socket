module Main where

import Control.Monad (forever)
import Data.Foldable (for_)
import GHC.Eventlog.Socket (wait)
import System.Environment (getArgs)

data Mode = Finite | Infinite

parseArgs :: [String] -> (Mode, [String])
parseArgs ("--forever" : rest) = (Infinite, rest)
parseArgs args = (Finite, args)

main :: IO ()
main = do
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
