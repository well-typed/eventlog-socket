{-# LANGUAGE NumericUnderscores #-}
module Main where

import Control.Concurrent (setNumCapabilities, threadDelay)
import Data.Foldable (for_)
import GHC.Conc (getNumProcessors)
import GHC.Eventlog.Socket (startFromEnv)
import System.IO (hPutStrLn, stderr)
import System.Mem (performMajorGC)
import Debug.Trace (traceMarkerIO)

main :: IO ()
main = do
    -- Start eventlog-socket:
    startFromEnv
    -- Start dancing:
    numProcessors <- getNumProcessors
    for_ (cycle [1 .. numProcessors]) $ \n -> do
        traceMarkerIO $ "Dancing to number " <> show n
        putStrLn $ "Setting number of capabilities to " <> show n
        setNumCapabilities n
        threadDelay 1000_000
