{- |
Stream GHC eventlog events to external processes.
-}
module GHC.Eventlog.Socket (
    -- * High-level API
    startWith,
    startFromEnv,

    -- ** Configuration types
    EventlogSocketAddr (..),
    EventlogSocketOpts (..),
    defaultEventlogSocketOpts,

    -- ** Configuration via environment
    fromEnv,

    -- * Legacy API
    startWait,
    start,
    wait,
) where

import Control.Exception (Exception (..), throwIO)
import Control.Monad (when)
import Data.Foldable (traverse_)
import Data.Maybe (isJust)
import Data.Word (Word16)
import Foreign.C (CString, withCString)
import GHC.Eventlog.Socket.CApi
import System.Environment (lookupEnv)
import Text.Read (readMaybe)

--------------------------------------------------------------------------------
-- High-level API
--------------------------------------------------------------------------------

{- |
Start an @eventlog-socket@ writer using the given socket address and options.

@since 0.1.1.0
-}
startWith :: EventlogSocketAddr -> EventlogSocketOpts -> IO ()
startWith = eventlogSocketStartWith

{- |
Read the eventlog socket configuration from the environment.
If this succeeds, start an @eventlog-socket@ writer with that configuration.

@since 0.1.1.0
-}
startFromEnv :: IO ()
startFromEnv = fromEnv >>= traverse_ (uncurry startWith)

{- |
Read the eventlog socket configuration from the environment.

@since 0.1.1.0
-}
fromEnv :: IO (Maybe (EventlogSocketAddr, EventlogSocketOpts))
fromEnv = eventlogSocketFromEnv

--------------------------------------------------------------------------------
-- Legacy API
--------------------------------------------------------------------------------

{- |
Start an @eventlog-socket@ writer on the given Unix domain socket path and wait.

@since 0.1.0.0
-}
startWait :: FilePath -> IO ()
startWait unixPath = do
    let addr = EventlogSocketUnixAddr unixPath
    let opts = defaultEventlogSocketOpts{esoWait = True}
    startWith addr opts

{- |
Start an @eventlog-socket@ writer on the given Unix domain socket path.

@since 0.1.0.0
-}
start :: FilePath -> IO ()
start unixPath = do
    let addr = EventlogSocketUnixAddr unixPath
    let opts = defaultEventlogSocketOpts{esoWait = False}
    startWith addr opts

{- |
Wait for another process to connect to the eventlog socket.

@since 0.1.0.0
-}
wait :: IO ()
wait = eventlogSocketWait
