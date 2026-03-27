module Main where

import Control.Concurrent
import Control.Monad
import Data.Maybe
import Debug.Trace
import GHC.Eventlog.Socket
import System.Environment
import System.IO (hPutStrLn, stderr)
import System.Mem (performMajorGC)
import System.Random

main :: IO ()
main = do
    -- Register hooks:
    registerHook HookPostStartEventLogging $
        traceMarkerIO "HookPostStartEventLogging fired."
    registerHook HookPreEndEventLogging $
        hPutStrLn stderr "HookPreEndEventLogging fired."
    -- Start eventlog-socket:
    startFromEnv
    -- Start oddball:
    _ <- forever $ threadDelay 3000000 >> doRandom
    pure ()

-- | Generate a random length stream of random numbers and sum them (poorly)
doRandom :: IO ()
doRandom = do
    g <- newStdGen
    n <- randomRIO (1000, 10000000)
    traceMarkerIO $ "Summing " ++ show n ++ " numbers"
    putStrLn $ "Generating " ++ show n ++ " random numbers"
    let stream = randomRs @Integer (-1000, 1000) g
        result = foldr (+) 00 $ take n stream
    putStrLn $ "Sum: " ++ show result
    performMajorGC

    -- Poll for asynchronous errors
    testWorkerStatus
    testControlStatus

    threadDelay 30
