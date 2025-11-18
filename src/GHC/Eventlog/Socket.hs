{-# LANGUAGE CApiFFI #-}

-- |
-- Stream GHC eventlog events to external processes.
module GHC.Eventlog.Socket (
    startWait,
    start,
    wait,
) where

import Foreign.C
import Foreign.Ptr

-- | Start listening for eventlog connections, blocking until a client connects.
startWait :: FilePath  -- ^ File path to the unix domain socket to create.
          -> IO ()
startWait = c_start' True

-- | Start listening for eventlog connections.
start :: FilePath      -- ^ File path to the unix domain socket to create.
      -> IO ()
start = c_start' False

-- | Wait (block) until a client connects.
wait :: IO ()
wait = c_wait

c_start' :: Bool -> FilePath -> IO ()
c_start' block socketPath =
    withCString socketPath $ \socketPathCString ->
    c_start socketPathCString block

foreign import capi safe "eventlog_socket.h eventlog_socket_start_unix"
    c_start :: CString -> Bool -> IO ()

foreign import capi safe "eventlog_socket.h eventlog_socket_wait"
    c_wait :: IO ()
