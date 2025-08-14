module Main where

import Data.Foldable (for_)
import GHC.Eventlog.Socket (wait)
import System.Environment (getArgs)

main :: IO ()
main = do
    wait
    args <- getArgs
    for_ args $ \arg ->
        print (fib (read arg))

fib :: Integer -> Integer
fib 0 = 0
fib 1 = 1
fib n = fib (n - 1) + fib (n - 2)
