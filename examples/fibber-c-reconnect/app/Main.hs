module Main where

import Control.Concurrent (threadDelay)
import Control.Exception (evaluate)
import Control.Monad (forever, void)
import Data.Foldable (for_)
import System.Environment (getArgs)

main :: IO ()
main = do
    args <- getArgs
    let fibArgs =
            if null args
                then [30]
                else map read args
    runForever fibArgs

runForever :: [Integer] -> IO ()
runForever fibArgs =
    forever $ do
        for_ fibArgs $ \arg ->
            print $ (fib arg)
        threadDelay 100000

fib :: Integer -> Integer
fib 0 = 0
fib 1 = 1
fib n = fib (n - 1) + fib (n - 2)
