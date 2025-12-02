module Main where

import Control.Concurrent (threadDelay)
import GHC.Eventlog.Socket (wait)

main :: IO ()
main = do
    putStrLn "custom-command example running"
    putStrLn "client connected; emitting workload"
    loop (0 :: Int)
  where
    loop 5 = return ()
    loop n = do
        putStrLn $ "custom workload iteration " ++ show n
        threadDelay 500000
        loop (n + 1)
