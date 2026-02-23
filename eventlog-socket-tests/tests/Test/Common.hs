{-# LANGUAGE CPP #-}

module Test.Common (
    -- * Example Programs
    Program (..),
    HasProgramInfo,
    ProgramInfo,
    ProgramHandle (..),
    withProgram,

    -- * Eventlog Validation
    EventlogSocketAddr (..),
    isEventlogSocketUnixAddr,
    isEventlogSocketInetAddr,
    EventlogAssertion (initialTimeoutS, timeoutExponent, eventlogSocket, validateEvents),
    defaultEventlogAssertion,
    assertEventlogOk,
    assertEventlogWith,
    assertEventlogWith',
    runEventlogAssertion,
    (!>),
    (&>),
    times,
    anyOf,
    allOf,
    droppingFor,
    isHeapProfSampleBegin,
    hasHeapProfSampleBeginWithinSec,
    hasHeapProfSampleBeginWithin,
    hasHeapProfSampleBegin,
    hasNoHeapProfSampleBeginWithinSec,
    hasNoHeapProfSampleBeginWithin,
    hasNoHeapProfSampleBegin,
    isHeapProfSampleEnd,
    hasHeapProfSampleEndWithinSec,
    hasHeapProfSampleEndWithin,
    hasHeapProfSampleEnd,
    hasNoHeapProfSampleEndWithinSec,
    hasNoHeapProfSampleEndWithin,
    hasNoHeapProfSampleEnd,
    isHeapProfSampleString,
    hasHeapProfSampleStringWithinSec,
    hasHeapProfSampleStringWithin,
    hasHeapProfSampleString,
    hasNoHeapProfSampleStringWithinSec,
    hasNoHeapProfSampleStringWithin,
    hasNoHeapProfSampleString,
    isMatchingUserMarker,
    hasMatchingUserMarkerWithinSec,
    hasMatchingUserMarkerWithin,
    hasMatchingUserMarker,
    isMatchingUserMessage,
    hasMatchingUserMessageWithinSec,
    hasMatchingUserMessageWithin,
    hasMatchingUserMessage,
    failing,
    debugging,
    sendCommand,
    sendJunk,
    sendCommandWithJunk,

    -- * Debug
    HasTestInfo,
    TestInfo,
    testCaseFor,
    testCaseForUnix,
    HasLogger,
    Logger,
    withLogger,
    Message (..),
    debug,
    debugEvents,
) where

import Control.Concurrent (forkIO, killThread, threadDelay)
import Control.Concurrent.MVar (MVar, modifyMVar, newMVar)
import Control.Exception (Exception (..), bracket, bracketOnError, catch, mask_, throwIO)
import Control.Monad (when)
import Control.Monad.IO.Class (MonadIO (..))
import qualified Data.Binary as B
import Data.Bool (bool)
import qualified Data.ByteString as BS
import qualified Data.ByteString.Lazy as BSL
import Data.Foldable (traverse_)
import qualified Data.List as L
import qualified Data.List.NonEmpty as NE
import Data.Machine
import Data.Maybe (fromMaybe, isNothing)
import Data.Text (Text)
import Data.Word (Word16, Word64)
import GHC.Clock (getMonotonicTimeNSec)
import GHC.Eventlog.Socket (EventlogSocketAddr (..))
import GHC.Eventlog.Socket.Control (Command)
import GHC.RTS.Events (Event)
import qualified GHC.RTS.Events as E
import GHC.RTS.Events.Incremental (Decoder (..), decodeEventLog)
import Network.Socket (Socket)
import qualified Network.Socket as S
import qualified Network.Socket.ByteString as SB
import System.Environment (getEnvironment, lookupEnv)
import System.Exit (ExitCode (..))
import System.FilePath (splitFileName, (</>))
import System.IO
import qualified System.IO as IO
import System.IO.Error (ioeGetErrorString, ioeGetLocation, isEOFError, isResourceVanishedError)
import System.IO.Unsafe (unsafePerformIO)
import System.Process (CreateProcess (..), Pid, ProcessHandle, StdStream (..), createProcess_, getPid, getProcessExitCode, proc, readProcessWithExitCode, showCommandForUser, terminateProcess, waitForProcess)
import System.Process.Internals (ignoreSigPipe)
import Test.Tasty (TestName, TestTree)
import Test.Tasty.HUnit (Assertion, HasCallStack, assertFailure, testCase)
import Text.Printf (printf)

#if defined(DEBUG)
import qualified Control.Concurrent.STM as T
import GHC.RTS.Events (TimeFormat (..), ppEvent)
import System.Posix.Types (CPid (..))
#endif

--------------------------------------------------------------------------------
-- Example Programs
--------------------------------------------------------------------------------

data Program = Program
    { name :: String
    , args :: [String]
    , rtsopts :: [String]
    , eventlogSocketBuildFlags :: [String]
    , eventlogSocketAddr :: EventlogSocketAddr
    }

type HasProgramInfo = (?programInfo :: ProgramInfo)

data ProgramInfo = ProgramInfo
    { program :: Program
    , programPidIO :: IO (Maybe Pid)
    , programPid :: Maybe Pid
    }

data ProgramHandle = ProgramHandle
    { wait :: IO ()
    , kill :: IO ()
    , info :: ProgramInfo
    }

withProgram :: (HasLogger, HasTestInfo) => Program -> ((HasProgramInfo) => IO ()) -> IO ()
withProgram program action =
    bracket (start program) kill $ \ProgramHandle{..} ->
        let ?programInfo = info in action

defaultBuildFlags :: [String]
#if defined(DEBUG)
defaultBuildFlags = ["+debug"]
#else
defaultBuildFlags = []
#endif

start :: (HasLogger, HasTestInfo) => Program -> IO ProgramHandle
start program = do
    debugInfo $ "Starting program " <> program.name
    let runner = do
            ghc <- fromMaybe "ghc" <$> lookupEnv "GHC"
            cabal <- fromMaybe "cabal" <$> lookupEnv "CABAL"

            -- Build program
            debugInfo $ "Build program " <> program.name <> " with " <> ghc <> " and " <> cabal
            let eventlogSocketBuildFlags = program.eventlogSocketBuildFlags <> defaultBuildFlags
            let constraintArgs
                    | null eventlogSocketBuildFlags = []
                    | otherwise = ["--constraint", "eventlog-socket " <> unwords eventlogSocketBuildFlags]
            let buildDir = "dist-newstyle/" <> L.intercalate "-" ("test-haskell" : eventlogSocketBuildFlags)
            let buildArgs = ["build", program.name, "-w" <> ghc, "--builddir", buildDir] <> constraintArgs
            debugInfo (showCommandForUser cabal buildArgs)
            (buildExit, buildOut, buildErr) <- readProcessWithExitCode cabal buildArgs ""
            when (buildExit /= ExitSuccess) $ do
                debugFail . unlines $ ["Failed to build program", buildOut, buildErr]
                throwIO buildExit

            -- Find binary for program
            debugInfo $ "Find binary for program " <> program.name
            let findArgs = ["list-bin", program.name, "-w" <> ghc, "--builddir", buildDir]
            debugInfo (showCommandForUser cabal findArgs)
            (findExit, findOut, findErr) <- readProcessWithExitCode cabal findArgs ""
            when (findExit /= ExitSuccess) $ do
                debugFail . unlines $ ["Failed to find binary for program ", findOut, findErr]
                throwIO findExit
            let maybeProgramBin = fmap fst . L.uncons . lines $ findOut
            programBin <- flip (`maybe` pure) maybeProgramBin $ do
                debugFail . unlines $ ["Failed to find binary for program ", findOut, findErr]
                throwIO findExit

            -- Start program
            debugInfo $ "Launching program " <> program.name
            inheritedEnv <- filter shouldInherit <$> getEnvironment
            let programArgs = program.args <> ["+RTS"] <> program.rtsopts <> ["-RTS"]
            let extraEnv = eventlogSocketEnv program.eventlogSocketAddr
            debugInfo . unlines . concat $
                [ [showCommandForUser programBin programArgs]
                , [ bool "       " "  with " (i == 0) <> k <> "=" <> v
                  | (i, (k, v)) <- zip [(1 :: Int) ..] extraEnv
                  ]
                ]
            -- Create the process:
            let createProcess =
                    (proc programBin programArgs)
                        { env = Just (inheritedEnv <> extraEnv)
                        , std_in = CreatePipe
                        , std_out = CreatePipe
                        , std_err = CreatePipe
                        }
            (maybeHandleIn, maybeHandleOut, maybeHandleErr, processHandle) <- createProcess_ program.name createProcess
            -- Create the ProgramInfo:
            let programPidIO = getPid processHandle
            programPid <- programPidIO
            let info = ProgramInfo{..}
            let ?programInfo = info
            debugInfo $ "Launched"
            -- Start loggers for stderr and stdout:
            maybeKillDebugOut <- traverse (debugHandle $ ProgramOut info) maybeHandleOut
            maybeKillDebugErr <- traverse (debugHandle $ ProgramErr info) maybeHandleErr
            let kill = mask_ $ do
                    debugInfo $ "Cleaning up stdout reader for " <> program.name <> "."
                    sequence_ maybeKillDebugOut
                    debugInfo $ "Cleaning up stderr reader for " <> program.name <> "."
                    sequence_ maybeKillDebugErr
                    debugInfo $ "Cleaning up process for " <> program.name <> "."
                    exitCode <- murderProcess program.name (maybeHandleIn, maybeHandleOut, maybeHandleErr, processHandle)
                    debugInfo $ "Process was killed with exit code " <> show exitCode
                    pure ()
            let wait = do
                    debugInfo $ "Waiting for " <> program.name <> " to finish."
                    bracketOnError (waitForProcess processHandle) (const kill) $ \exitCode ->
                        when (exitCode /= ExitSuccess) . assertFailure $
                            "Program " <> program.name <> " failed with exit code " <> show exitCode
            pure ProgramHandle{..}
    bracketOnError runner kill $ pure

{- |
Kill the process harder than `terminateProcess`.

This function also hides the evidence by closing the handles.
-}
murderProcess :: (HasLogger, HasTestInfo) => String -> (Maybe Handle, Maybe Handle, Maybe Handle, ProcessHandle) -> IO ExitCode
murderProcess programName (maybeStdIn, maybeStdOut, maybeStdErr, processHandle) = do
    -- Murder the process until it's dead:
    let murderLoop :: Int -> IO ExitCode
        murderLoop n
            | n > 0 =
                getProcessExitCode processHandle >>= \case
                    Nothing -> do
                        debugInfo $ "Trying to kill the process for " <> programName
                        terminateProcess processHandle
                        threadDelay 100_000 -- 100ms
                        murderLoop (n - 1)
                    Just exitCode ->
                        pure exitCode
            | otherwise =
                pure $ ExitFailure 1
    exitCode <- murderLoop 100 -- 100 * 100ms = 10_000ms = 10s
    debugInfo $ "Killed the process for " <> programName

    -- Clean up handles:
    traverse_ (ignoreSigPipe . IO.hClose) maybeStdIn
    traverse_ IO.hClose maybeStdOut
    traverse_ IO.hClose maybeStdErr

    pure exitCode

shouldInherit :: (String, String) -> Bool
shouldInherit = (`elem` ["PATH"]) . fst

eventlogSocketEnv :: EventlogSocketAddr -> [(String, String)]
eventlogSocketEnv = \case
    EventlogSocketUnixAddr{..} ->
        [("GHC_EVENTLOG_UNIX_PATH", esaUnixPath)]
    EventlogSocketInetAddr{..} ->
        [("GHC_EVENTLOG_INET_HOST", esaInetHost), ("GHC_EVENTLOG_INET_PORT", esaInetPort)]

--------------------------------------------------------------------------------
-- Eventlog Validation
--------------------------------------------------------------------------------

isEventlogSocketUnixAddr :: EventlogSocketAddr -> Bool
isEventlogSocketUnixAddr = \case
    EventlogSocketUnixAddr{} -> True
    _otherwise -> False

isEventlogSocketInetAddr :: EventlogSocketAddr -> Bool
isEventlogSocketInetAddr = \case
    EventlogSocketInetAddr{} -> True
    _otherwise -> False

data EventlogAssertion x
    = EventlogAssertion
    { initialTimeoutS :: Double
    , timeoutExponent :: Double
    , eventlogSocket :: EventlogSocketAddr
    , validateEvents :: Socket -> ProcessT IO Event x
    }

defaultEventlogAssertion :: EventlogSocketAddr -> EventlogAssertion Event
defaultEventlogAssertion eventlogSocket =
    EventlogAssertion
        { initialTimeoutS = 0.25
        , timeoutExponent = 1
        , eventlogSocket
        , validateEvents = const echo
        }

assertEventlogOk ::
    (HasLogger, HasTestInfo, HasProgramInfo) =>
    EventlogSocketAddr ->
    Assertion
assertEventlogOk eventlogSocket =
    assertEventlogWith eventlogSocket (debugEventCounter 1_000)

assertEventlogWith ::
    (HasLogger, HasTestInfo, HasProgramInfo) =>
    EventlogSocketAddr ->
    (ProcessT IO Event x) ->
    Assertion
assertEventlogWith eventlogSocket validateEvents =
    assertEventlogWith' eventlogSocket (const validateEvents)

assertEventlogWith' ::
    (HasLogger, HasTestInfo, HasProgramInfo) =>
    EventlogSocketAddr ->
    (Socket -> ProcessT IO Event x) ->
    Assertion
assertEventlogWith' eventlogSocket validateEvents =
    runEventlogAssertion (defaultEventlogAssertion eventlogSocket){validateEvents = validateEvents}

runEventlogAssertion ::
    (HasLogger, HasTestInfo, HasProgramInfo) =>
    EventlogAssertion x ->
    Assertion
runEventlogAssertion EventlogAssertion{..} =
    withEventlogHandle initialTimeoutS timeoutExponent eventlogSocket $ \socket ->
        runT_ $ fromSocket 1000 4096 socket ~> decodeEvent ~> debugEvents ~> validateEvents socket

withEventlogHandle ::
    (HasLogger, HasTestInfo, HasProgramInfo) =>
    Double ->
    Double ->
    EventlogSocketAddr ->
    (Socket -> IO a) ->
    IO a
withEventlogHandle initialTimeoutS timeoutExponent eventlogSocket action =
    connectLoop initialTimeoutS
  where
    waitFor timeoutS = threadDelay timeoutMicro
      where
        timeoutMicro = round $ timeoutS * 1e6

    isDoesNotExistError :: IOError -> Bool
    isDoesNotExistError ioe =
        "Network.Socket.connect" `L.isPrefixOf` ioeGetLocation ioe
            && ioeGetErrorString ioe == "does not exist"

    connectLoop timeoutS = do
        catch @IOError tryConnect $ \ioe ->
            if isDoesNotExistError ioe || isResourceVanishedError ioe
                then do
                    -- Log the error:
                    debugInfo $ "Caught IOError: " <> displayException ioe

                    -- Check if program is still running:
                    maybePid <- ?programInfo.programPidIO
                    when (isNothing maybePid) $
                        error $
                            "Program " <> ?programInfo.program.name <> " has terminated."

                    -- Wait before retrying:
                    waitFor timeoutS

                    -- Retry connection:
                    connectLoop (timeoutS * timeoutExponent)
                else throwIO ioe
      where
        tryConnect = do
            let timeoutMSec = round $ timeoutS * 1e3
            case eventlogSocket of
                EventlogSocketUnixAddr{..} -> do
                    let newSocket = S.socket S.AF_UNIX S.Stream S.defaultProtocol
                    let closeSocket socket = S.close socket
                    bracket newSocket closeSocket $ \socket -> do
                        debugInfo $ "Trying to connect to Unix socket at " <> esaUnixPath
                        S.connect socket (S.SockAddrUnix esaUnixPath)
                        debugInfo $ "Connected to Unix socket at " <> esaUnixPath
                        action socket
                EventlogSocketInetAddr{..} -> do
                    let addrInfo =
                            S.defaultHints
                                { S.addrFamily = S.AF_INET
                                , S.addrSocketType = S.Stream
                                }
                    addr <- NE.head <$> S.getAddrInfo (Just addrInfo) (Just esaInetHost) (Just $ esaInetPort)
                    let newSocket = S.socket (S.addrFamily addr) (S.addrSocketType addr) (S.addrProtocol addr)
                    let closeSocket socket = S.gracefulClose socket timeoutMSec
                    bracket newSocket closeSocket $ \socket -> do
                        debugInfo $ "Trying to connect to TCP socket at " <> show (S.addrAddress addr)
                        S.connect socket (S.addrAddress addr)
                        debugInfo $ "Connected to TCP socket at " <> show (S.addrAddress addr)
                        action socket

fromSocket ::
    Int ->
    Int ->
    Socket ->
    SourceT IO BS.ByteString
fromSocket _timeoutMSec chunkSizeBytes socket = construct go
  where
    go = do
        chunk <- liftIO (SB.recv socket chunkSizeBytes)
        yield chunk
        go

isReady ::
    (HasLogger, HasTestInfo) =>
    Int ->
    Handle ->
    IO (Maybe Bool)
isReady timeoutMSec handle = catch ready onEOF
  where
    ready =
        hWaitForInput handle timeoutMSec
            >>= pure . Just
    onEOF (e :: IOError) =
        if isEOFError e
            then do
                debugInfo $ displayException e
                pure Nothing
            else assertFailure (displayException e)

decodeEvent :: ProcessT IO BS.ByteString Event
decodeEvent = construct $ loop decodeEventLog
  where
    loop Done{} = pure ()
    loop (Consume k) = await >>= \chunk -> loop (k chunk)
    loop (Produce a d') = yield a >> loop d'
    loop (Error _ err) = liftIO $ assertFailure err

{- |
Evaluate the machine until the given number of seconds has passed, then stop.
-}
withTimeoutSec :: Double -> MachineT IO k o -> MachineT IO k o
withTimeoutSec timeoutSec m = afterTimeoutSec timeoutSec m stopped

{- |
Evaluate the first machine until the given number of seconds has passed, then continue as the second machine.
-}
afterTimeoutSec :: Double -> MachineT IO k o -> MachineT IO k o -> MachineT IO k o
afterTimeoutSec timeoutSec m n = MachineT $ do
    startTimeNano <- getMonotonicTimeNSec
    let timeoutNano = round $ 1e9 * timeoutSec
    let endTimeNSec = startTimeNano + timeoutNano
    runMachineT $ afterTimeNSec endTimeNSec m n

{- |
Evaluate the first machine until the given time, then continue as the second machine.
-}
afterTimeNSec :: Word64 -> MachineT IO k o -> MachineT IO k o -> MachineT IO k o
afterTimeNSec timeNSec = go
  where
    go m n = MachineT $ do
        getMonotonicTimeNSec >>= \case
            currentTimeNSec
                | currentTimeNSec >= timeNSec -> runMachineT n
                | otherwise ->
                    runMachineT m >>= \case
                        Stop ->
                            pure $ Stop
                        Yield o m' ->
                            pure $ Yield o (go m' n)
                        Await onNext k onStop ->
                            pure $ Await (\t -> go (onNext t) n) k (go onStop n)

infixl 8 !>

{- |
Evaluate a monadic action before running the machine.
-}
(!>) :: (Monad m) => m () -> MachineT m k o -> MachineT m k o
(!>) m n = MachineT $ m >> runMachineT n

infixl 7 &>

{- |
Evaluate the first machine until it stops, then continue as the second machine.
-}
(&>) :: (Monad m) => MachineT m k o -> MachineT m k o -> MachineT m k o
(&>) m n =
    MachineT $ do
        runMachineT m >>= \case
            Stop ->
                runMachineT n
            Yield o m' ->
                pure $ Yield o (m' &> n)
            Await onNext k onStop ->
                pure $ Await (\t -> onNext t &> n) k (onStop &> n)

times :: (Monad m) => Int -> MachineT m k o -> MachineT m k o
times count m = foldr (&>) stopped (replicate count m)

{- |
Consume inputs until the predicate holds, then stop.
Throw an exception if the input stream stops.
-}
anyOf :: (HasLogger, HasTestInfo) => (a -> Bool) -> (Int -> String) -> (Int -> String) -> ProcessT IO a a
anyOf p onSuccess onFailure = go (0 :: Int)
  where
    go count = MachineT $ pure $ Await onNext Refl onStop
      where
        onNext a = if p a then debugging (onSuccess count) else MachineT $ pure $ Yield a (go (count + 1))
        onStop = failing (onFailure count)

{- |
Evaluate `anyOf` for the given number of seconds, then fail.
-}
anyFor :: (HasLogger, HasTestInfo) => Double -> (a -> Bool) -> String -> String -> ProcessT IO a a
anyFor timeoutSec p onSuccess onFailure =
    afterTimeoutSec timeoutSec (anyOf p (const onSuccess) (const onFailure)) (failing onFailure)

{- |
Consume inputs forever.
Throw an exception if the predicate fails to hold for any input.
-}
allOf :: (HasLogger, HasTestInfo) => (a -> Bool) -> (Int -> String) -> (Int -> String) -> ProcessT IO a a
allOf p onSuccess onFailure = go (0 :: Int)
  where
    go count = MachineT $ pure $ Await onNext Refl onStop
      where
        onNext a = if p a then MachineT $ pure $ Yield a (go (count + 1)) else failing (onFailure count)
        onStop = debugging (onSuccess count)

{- |
Evaluate `allFor` for the given number of seconds, then fail.
-}
allFor :: (HasLogger, HasTestInfo) => Double -> (a -> Bool) -> String -> String -> ProcessT IO a a
allFor timeoutSec p onSuccess onFailure =
    withTimeoutSec timeoutSec (allOf p (const onSuccess) (const onFailure))

{- |
Immediately `assertFailure`.
-}
failing :: (HasLogger, HasTestInfo) => String -> MachineT IO k o
failing msg = MachineT $ do
    debugFail $ msg
    assertFailure msg

{- |
Write a debug messages and stop.
-}
debugging :: (HasLogger, HasTestInfo) => String -> MachineT IO k o
debugging message = MachineT $ debugInfo message >> pure Stop

{- |
Drop all inputs.
-}
droppingForever :: (Monad m) => ProcessT m a x
droppingForever = repeatedly $ await >>= const (pure ())

{- |
Drop all inputs for the given number of seconds.
-}
droppingFor :: Double -> ProcessT IO a x
droppingFor timeoutSec = withTimeoutSec timeoutSec droppingForever

{- |
Test if an `Event` is a `HeapProfSampleBegin` event.
-}
isHeapProfSampleBegin :: Event -> Bool
isHeapProfSampleBegin ev
    | E.HeapProfSampleBegin{} <- E.evSpec ev = True
    | otherwise = False

{- |
Assert that the input stream contains a `HeapProfSampleString` event within the given timeout.
-}
hasHeapProfSampleBeginWithinSec :: (HasLogger, HasTestInfo) => Double -> ProcessT IO Event Event
hasHeapProfSampleBeginWithinSec timeoutSec =
    anyFor timeoutSec isHeapProfSampleBegin onSuccess onFailure
  where
    onSuccess = printf "Found HeapProfSampleBegin within %0.2f seconds." timeoutSec
    onFailure = printf "Did not find HeapProfSampleBegin within %0.2f seconds." timeoutSec

{- |
Assert that the input stream contains a `HeapProfSampleString` event within the given number of events.
-}
hasHeapProfSampleBeginWithin :: (HasLogger, HasTestInfo) => Int -> ProcessT IO Event Event
hasHeapProfSampleBeginWithin count = taking count ~> hasHeapProfSampleBegin

{- |
Assert that the input stream contains a `HeapProfSampleString` event.
-}
hasHeapProfSampleBegin :: (HasLogger, HasTestInfo) => ProcessT IO Event Event
hasHeapProfSampleBegin =
    anyOf isHeapProfSampleBegin onSuccess onFailure
  where
    onSuccess = printf "Found HeapProfSampleBegin after %d events."
    onFailure = printf "Did not find HeapProfSampleBegin after %d events."

{- |
Assert that the input stream does not contain a `HeapProfSampleString` event within the given timeout.
-}
hasNoHeapProfSampleBeginWithinSec :: (HasLogger, HasTestInfo) => Double -> ProcessT IO Event Event
hasNoHeapProfSampleBeginWithinSec timeoutSec =
    anyFor timeoutSec isHeapProfSampleBegin onSuccess onFailure
  where
    onSuccess = printf "Did not find HeapProfSampleBegin within %0.2f seconds." timeoutSec
    onFailure = printf "Found HeapProfSampleBegin within %0.2f seconds." timeoutSec

{- |
Assert that the input stream does not contain a `HeapProfSampleString` event within the given number of events.
-}
hasNoHeapProfSampleBeginWithin :: (HasLogger, HasTestInfo) => Int -> ProcessT IO Event Event
hasNoHeapProfSampleBeginWithin count = taking count ~> hasNoHeapProfSampleBegin

{- |
Assert that the input stream does not contain a `HeapProfSampleString` event.
-}
hasNoHeapProfSampleBegin :: (HasLogger, HasTestInfo) => ProcessT IO Event Event
hasNoHeapProfSampleBegin =
    anyOf isHeapProfSampleBegin onSuccess onFailure
  where
    onSuccess = printf "Did not find HeapProfSampleBegin after %d events."
    onFailure = printf "Found HeapProfSampleBegin after %d events."

{- |
Test if an `Event` is a `HeapProfSampleEnd` event.
-}
isHeapProfSampleEnd :: Event -> Bool
isHeapProfSampleEnd ev
    | E.HeapProfSampleEnd{} <- E.evSpec ev = True
    | otherwise = False

{- |
Assert that the input stream contains a `HeapProfSampleEnd` event within the given timeout.
-}
hasHeapProfSampleEndWithinSec :: (HasLogger, HasTestInfo) => Double -> ProcessT IO Event Event
hasHeapProfSampleEndWithinSec timeoutSec =
    anyFor timeoutSec isHeapProfSampleEnd onSuccess onFailure
  where
    onSuccess = printf "Found HeapProfSampleEnd within %0.2f seconds." timeoutSec
    onFailure = printf "Did not find HeapProfSampleEnd within %0.2f seconds." timeoutSec

{- |
Assert that the input stream contains a `HeapProfSampleEnd` event within the given number of events.
-}
hasHeapProfSampleEndWithin :: (HasLogger, HasTestInfo) => Int -> ProcessT IO Event Event
hasHeapProfSampleEndWithin count = taking count ~> hasHeapProfSampleEnd

{- |
Assert that the input stream contains a `HeapProfSampleEnd` event.
-}
hasHeapProfSampleEnd :: (HasLogger, HasTestInfo) => ProcessT IO Event Event
hasHeapProfSampleEnd =
    anyOf isHeapProfSampleEnd onSuccess onFailure
  where
    onSuccess = printf "Found HeapProfSampleEnd after %d events."
    onFailure = printf "Did not find HeapProfSampleEnd after %d events."

{- |
Assert that the input stream does not contain a `HeapProfSampleEnd` event within the given timeout.
-}
hasNoHeapProfSampleEndWithinSec :: (HasLogger, HasTestInfo) => Double -> ProcessT IO Event Event
hasNoHeapProfSampleEndWithinSec timeoutSec =
    anyFor timeoutSec isHeapProfSampleEnd onSuccess onFailure
  where
    onSuccess = printf "Did not find HeapProfSampleEnd within %0.2f seconds." timeoutSec
    onFailure = printf "Found HeapProfSampleEnd within %0.2f seconds." timeoutSec

{- |
Assert that the input stream does not contain a `HeapProfSampleEnd` event within the given number of events.
-}
hasNoHeapProfSampleEndWithin :: (HasLogger, HasTestInfo) => Int -> ProcessT IO Event Event
hasNoHeapProfSampleEndWithin count = taking count ~> hasNoHeapProfSampleEnd

{- |
Assert that the input stream does not contain a `HeapProfSampleEnd` event.
-}
hasNoHeapProfSampleEnd :: (HasLogger, HasTestInfo) => ProcessT IO Event Event
hasNoHeapProfSampleEnd =
    anyOf isHeapProfSampleEnd onSuccess onFailure
  where
    onSuccess = printf "Did not find HeapProfSampleEnd after %d events."
    onFailure = printf "Found HeapProfSampleEnd after %d events."

{- |
Test if an `Event` is a `HeapProfSampleString` event.
-}
isHeapProfSampleString :: Event -> Bool
isHeapProfSampleString ev
    | E.HeapProfSampleString{} <- E.evSpec ev = True
    | otherwise = False

{- |
Assert that the input stream contains a `HeapProfSampleString` event within the given timeout.
-}
hasHeapProfSampleStringWithinSec :: (HasLogger, HasTestInfo) => Double -> ProcessT IO Event Event
hasHeapProfSampleStringWithinSec timeoutSec =
    anyFor timeoutSec isHeapProfSampleString onSuccess onFailure
  where
    onSuccess = printf "Found HeapProfSampleString within %0.2f seconds." timeoutSec
    onFailure = printf "Did not find HeapProfSampleString within %0.2f seconds." timeoutSec

{- |
Assert that the input stream contains a `HeapProfSampleString` event within the given number of events.
-}
hasHeapProfSampleStringWithin :: (HasLogger, HasTestInfo) => Int -> ProcessT IO Event Event
hasHeapProfSampleStringWithin count = taking count ~> hasHeapProfSampleString

{- |
Assert that the input stream contains a `HeapProfSampleString` event.
-}
hasHeapProfSampleString :: (HasLogger, HasTestInfo) => ProcessT IO Event Event
hasHeapProfSampleString =
    anyOf isHeapProfSampleString onSuccess onFailure
  where
    onSuccess = printf "Found HeapProfSampleString after %d events."
    onFailure = printf "Did not find HeapProfSampleString after %d events."

{- |
Assert that the input stream does not contain a `HeapProfSampleString` event within the given timeout.
-}
hasNoHeapProfSampleStringWithinSec :: (HasLogger, HasTestInfo) => Double -> ProcessT IO Event Event
hasNoHeapProfSampleStringWithinSec timeoutSec =
    allFor timeoutSec (not . isHeapProfSampleString) onSuccess onFailure
  where
    onSuccess = printf "Did not find HeapProfSampleString within %0.2f seconds." timeoutSec
    onFailure = printf "Found HeapProfSampleString within %0.2f seconds." timeoutSec

{- |
Assert that the input stream does not contain a `HeapProfSampleString` event within the given number of events.
-}
hasNoHeapProfSampleStringWithin :: (HasLogger, HasTestInfo) => Int -> ProcessT IO Event Event
hasNoHeapProfSampleStringWithin count =
    taking count ~> hasNoHeapProfSampleString

{- |
Assert that the input stream does not contain a `HeapProfSampleString` event.
-}
hasNoHeapProfSampleString :: (HasLogger, HasTestInfo) => ProcessT IO Event Event
hasNoHeapProfSampleString =
    allOf (not . isHeapProfSampleString) onSuccess onFailure
  where
    onSuccess = printf "Did not find HeapProfSampleString after %d events."
    onFailure = printf "Found HeapProfSampleString after %d events."

{- |
Test if an `Event` is a `UserMarker` whose message satisfies the given predicate.
-}
isMatchingUserMarker :: (Text -> Bool) -> Event -> Bool
isMatchingUserMarker p ev
    | E.UserMarker{markername} <- E.evSpec ev = p markername
    | otherwise = False

{- |
Assert that the input stream contains a matching `UserMarker` event within the given timeout.
-}
hasMatchingUserMarkerWithinSec :: (HasLogger, HasTestInfo) => (Text -> Bool) -> Double -> ProcessT IO Event Event
hasMatchingUserMarkerWithinSec p timeoutSec =
    anyFor timeoutSec (isMatchingUserMarker p) onSuccess onFailure
  where
    onSuccess = printf "Found matching UserMarker within %0.2f seconds." timeoutSec
    onFailure = printf "Did not find matching UserMarker within %0.2f seconds." timeoutSec

{- |
Assert that the input stream contains a matching `UserMarker` event within the given number of events.
-}
hasMatchingUserMarkerWithin :: (HasLogger, HasTestInfo) => (Text -> Bool) -> Int -> ProcessT IO Event Event
hasMatchingUserMarkerWithin p count = taking count ~> hasMatchingUserMarker p

{- |
Assert that the input stream contains a matching `UserMarker` event.
-}
hasMatchingUserMarker :: (HasLogger, HasTestInfo) => (Text -> Bool) -> ProcessT IO Event Event
hasMatchingUserMarker p =
    anyOf (isMatchingUserMarker p) onSuccess onFailure
  where
    onSuccess = printf "Found matching UserMarker after %d events."
    onFailure = printf "Did not find matching UserMarker after %d events."

{- |
Test if an `Event` is a `UserMessage` whose message satisfies the given predicate.
-}
isMatchingUserMessage :: (Text -> Bool) -> Event -> Bool
isMatchingUserMessage p ev
    | E.UserMessage{msg} <- E.evSpec ev = p msg
    | otherwise = False

{- |
Assert that the input stream contains a matching `UserMessage` event within the given timeout.
-}
hasMatchingUserMessageWithinSec :: (HasLogger, HasTestInfo) => (Text -> Bool) -> Double -> ProcessT IO Event Event
hasMatchingUserMessageWithinSec p timeoutSec =
    anyFor timeoutSec (isMatchingUserMessage p) onSuccess onFailure
  where
    onSuccess = printf "Found matching UserMessage within %0.2f seconds." timeoutSec
    onFailure = printf "Did not find matching UserMessage within %0.2f seconds." timeoutSec

{- |
Assert that the input stream contains a matching `UserMessage` event within the given number of events.
-}
hasMatchingUserMessageWithin :: (HasLogger, HasTestInfo) => (Text -> Bool) -> Int -> ProcessT IO Event Event
hasMatchingUserMessageWithin p count = taking count ~> hasMatchingUserMessage p

{- |
Assert that the input stream contains a matching `UserMessage` event.
-}
hasMatchingUserMessage :: (HasLogger, HasTestInfo) => (Text -> Bool) -> ProcessT IO Event Event
hasMatchingUserMessage p =
    anyOf (isMatchingUserMessage p) onSuccess onFailure
  where
    onSuccess = printf "Found matching UserMessage after %d events."
    onFailure = printf "Did not find matching UserMessage after %d events."

{- |
Send the given `Command` over the `Socket`.
-}
sendCommand :: (HasLogger, HasTestInfo) => Socket -> Command -> IO ()
sendCommand socket command = do
    let commandBytes = B.encode command
    debugInfo $ "Sending control command: " <> show commandBytes
    bytesSent <- SB.send socket (BSL.toStrict commandBytes)
    debugInfo $ "Sent " <> show bytesSent <> " bytes."

{- |
Send junk over the `Socket`.
-}
sendJunk :: (HasLogger, HasTestInfo) => Socket -> BSL.ByteString -> IO ()
sendJunk socket junkBytes = do
    debugInfo $ "Sending junk: " <> show junkBytes
    bytesSent <- SB.send socket (BSL.toStrict junkBytes)
    debugInfo $ "Sent " <> show bytesSent <> " bytes."

{- |
Send the given `Command` over the `Socket`.
-}
sendCommandWithJunk :: (HasLogger, HasTestInfo) => Socket -> BSL.ByteString -> Command -> BSL.ByteString -> IO ()
sendCommandWithJunk socket junkBefore command junkAfter = do
    let commandWithJunkBytes = junkBefore <> B.encode command <> junkAfter
    debugInfo $ "Sending control command with junk " <> show commandWithJunkBytes
    bytesSent <- SB.send socket (BSL.toStrict commandWithJunkBytes)
    debugInfo $ "Sent " <> show bytesSent <> " bytes."

--------------------------------------------------------------------------------
-- Print Debug Message
--------------------------------------------------------------------------------

type HasTestInfo = (?testInfo :: TestInfo)

data TestInfo = TestInfo
    { testName :: TestName
    }

inetPortCounter :: MVar Word16
inetPortCounter = unsafePerformIO (newMVar 0)

testCaseForUnix ::
    (HasLogger) =>
    TestName ->
    ((HasTestInfo) => EventlogSocketAddr -> Assertion) ->
    EventlogSocketAddr ->
    Maybe TestTree
testCaseForUnix testName test eventlogSocket
    | isEventlogSocketUnixAddr eventlogSocket = testCaseFor testName test eventlogSocket
    | otherwise = Nothing

testCaseFor ::
    (HasLogger) =>
    TestName ->
    ((HasTestInfo) => EventlogSocketAddr -> Assertion) ->
    EventlogSocketAddr ->
    Maybe TestTree
testCaseFor testName test = \case
    EventlogSocketUnixAddr{..} ->
        let testName' = testName <> "_Unix"
         in let ?testInfo = TestInfo{testName = testName'}
             in Just $ testCase testName' $ do
                    debug Header
                    let (directory, fileName) = splitFileName esaUnixPath
                    let unixPath' = directory </> testName <> "_" <> fileName
                    debugInfo $ "Using Unix socket: " <> unixPath'
                    test $ EventlogSocketUnixAddr unixPath'
                    debug Footer
    EventlogSocketInetAddr{..} ->
        let testName' = testName <> "_Tcp"
         in let ?testInfo = TestInfo{testName = testName'}
             in Just $ testCase testName' $ do
                    debug Header
                    esaInetPortOffset <- modifyMVar inetPortCounter $ \currentTcpPortOffset -> do
                        let nextTcpPortOffset = currentTcpPortOffset + 1
                        pure (nextTcpPortOffset, currentTcpPortOffset)
                    let inetPort' = show (read esaInetPort + esaInetPortOffset)
                    test $ EventlogSocketInetAddr esaInetHost inetPort'
                    debug Footer

type HasLogger = (?logger :: Logger, HasCallStack)

#if defined(DEBUG)
data Logger = Logger {logChan :: T.TChan (TestInfo, Message)}
#else
data Logger = Logger
#endif

newLogger :: IO Logger
#if defined(DEBUG)
newLogger = Logger <$> T.newTChanIO
#else
newLogger = pure Logger
#endif

data Message
    = Header
    | Footer
    | ProgramOut ProgramInfo String
    | ProgramErr ProgramInfo String
    | Event Event
    | Info String
    | Fail String

debug :: (HasLogger, HasTestInfo) => Message -> IO ()
#if defined(DEBUG)
#if !defined(DEBUG_EVENTS)
debug Event{} = pure () -- Discard events
#endif
debug message = do
    T.atomically (T.writeTChan (logChan ?logger) (?testInfo, message))
#else
debug _message = do
    let _logger = ?logger
    let _testInfo = ?testInfo
    pure ()
#endif

debugInfo :: (HasLogger, HasTestInfo) => String -> IO ()
debugInfo message = debug (Info message)

debugFail :: (HasLogger, HasTestInfo) => String -> IO ()
debugFail message = debug (Fail message)

withLogger :: ((HasLogger) => IO ()) -> IO ()
#if defined(DEBUG)
withLogger action = do
    logger <- newLogger
    let ?logger = logger
    let runner = do
            (testInfo, message) <- T.atomically (T.readTChan . logChan $ logger)
            IO.hPutStrLn stderr (renderMessage testInfo message)
            -- IO.hFlush stderr
            runner
    bracket (forkIO runner) killThread $ \_threadId -> do
        action
  where
    renderMessage :: TestInfo -> Message -> String
    renderMessage TestInfo{..} = \case
        Header ->
            "-- HEADER: " <> testName <> " " <> replicate (80 - (length testName + 12)) '-'
        Footer ->
            "-- FOOTER: " <> testName <> " " <> replicate (80 - (length testName + 12)) '-'
        ProgramOut info message ->
            "[" <> testName <> ", " <> prettyProgramInfo info <> "] stdout: " <> message
        ProgramErr info message ->
            "[" <> testName <> ", " <> prettyProgramInfo info <> "] stderr: " <> message
        Event event ->
            "[" <> testName <> "] event: " <> ppEvent PrettyTime mempty event
        Info message ->
            "[" <> testName <> "] info: " <> message
        Fail message ->
            "[" <> testName <> "] fail: " <> message

    prettyProgramInfo :: ProgramInfo -> String
    prettyProgramInfo ProgramInfo{..} = program.name <> " (" <> prettyPidInfo programPid <> ")"

    prettyPidInfo :: Maybe Pid -> String
    prettyPidInfo = maybe "terminated" (\(CPid pid) -> "pid=" <> show pid)
#else
withLogger action = do
    logger <- newLogger
    let ?logger = logger
    action
#endif

debugHandle :: (HasLogger, HasTestInfo) => (String -> Message) -> Handle -> IO (IO ())
debugHandle wrapper handle = do
    let runner = do
            threadDelay 30 -- Wait 30mcg.
            isReady 1_000 handle >>= \case
                Just ready
                    | ready -> do
                        line <- IO.hGetLine handle
                        debug . wrapper $ line
                        runner
                    | otherwise -> runner
                Nothing -> pure ()
    threadId <- forkIO runner
    pure $ killThread threadId

debugEvents :: (HasLogger, HasTestInfo) => ProcessT IO Event Event
debugEvents =
    repeatedly $
        await >>= \event -> do
            liftIO (debug $ Event event)
            yield event

debugEventCounter :: (HasLogger, HasTestInfo) => Int -> ProcessT IO Event Event
debugEventCounter limit = go (1 :: Int)
  where
    go count = MachineT $ pure $ Await onNext Refl onStop
      where
        onNext event =
            MachineT $ do
                let count' = count + 1
                if (count' >= limit)
                    then do
                        liftIO (debugInfo $ "Saw " <> show count' <> " events.")
                        pure $ Yield event (go 0)
                    else do
                        pure $ Yield event (go count')
        onStop =
            MachineT $ do
                liftIO (debugInfo $ "Saw " <> show count <> " events.")
                pure Stop
