{-# LANGUAGE CPP #-}

module Test.Common (
    -- * Example Programs
    Program (..),
    ProgramHandle (..),
    start,

    -- * Eventlog Validation
    EventlogSocket (..),
    isEventlogUnixSocket,
    isEventlogTcpSocket,
    EventlogAssertion (initialTimeoutS, timeoutExponent, eventlogSocket, validateEvents),
    defaultEventlogAssertion,
    assertEventlogOk,
    assertEventlogWith,
    assertEventlogWith',
    runEventlogAssertion,
    (!>),
    (&>),
    droppingFor,
    hasHeapProfSampleBeginWithinSec,
    hasHeapProfSampleBeginWithin,
    hasNoHeapProfSampleBeginWithinSec,
    hasNoHeapProfSampleBeginWithin,
    hasHeapProfSampleStringWithinSec,
    hasHeapProfSampleStringWithin,
    hasNoHeapProfSampleStringWithinSec,
    hasNoHeapProfSampleStringWithin,
    isMatchingUserMarker,
    hasMatchingUserMarkerWithinSec,
    hasMatchingUserMarkerWithin,
    hasMatchingUserMarker,
    sendCommand,
    sendJunk,

    -- * Debug
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
import Data.Maybe (fromMaybe)
import Data.Text (Text)
import Data.Word (Word16, Word64)
import GHC.Clock (getMonotonicTimeNSec)
import GHC.Eventlog.Socket (EventlogSocket (..))
import GHC.Eventlog.Socket.Control (Command (..))
import GHC.RTS.Events (Event)
import qualified GHC.RTS.Events as E
import GHC.RTS.Events.Incremental (Decoder (..), decodeEventLog)
import qualified Network.Socket as S
import System.Environment (getEnvironment, lookupEnv)
import System.Exit (ExitCode (..))
import System.FilePath (splitFileName, (</>))
import System.IO
import qualified System.IO as IO
import System.IO.Error (ioeGetErrorString, ioeGetLocation, isEOFError, isResourceVanishedError)
import System.IO.Unsafe (unsafePerformIO)
import System.Process (CreateProcess (..), ProcessHandle, StdStream (..), createProcess_, getProcessExitCode, proc, showCommandForUser, terminateProcess, waitForProcess)
import System.Process.Internals (ignoreSigPipe)
import Test.Tasty (TestName, TestTree)
import Test.Tasty.HUnit (Assertion, HasCallStack, assertFailure, testCase)
import Text.Printf (printf)

#if defined(DEBUG)
import qualified Control.Concurrent.STM as T
import GHC.RTS.Events (TimeFormat (..), ppEvent)
#endif

--------------------------------------------------------------------------------
-- Example Programs
--------------------------------------------------------------------------------

data Program = Program
    { name :: String
    , args :: [String]
    , rtsopts :: [String]
    , eventlogSocket :: EventlogSocket
    }

data ProgramHandle = ProgramHandle
    { wait :: IO ()
    , kill :: IO ()
    }

start :: (HasLogger) => Program -> IO ProgramHandle
start program = do
    debug . Info $ "Starting program " <> program.name
    let runner = do
            ghc <- fromMaybe "ghc" <$> lookupEnv "GHC"
            debug . Info $ "Building program " <> program.name <> " with " <> ghc
            inheritedEnv <- filter shouldInherit <$> getEnvironment
            let name = "cabal"
            let args = ["run", program.name, "-w" <> ghc, "--", "+RTS"] <> program.rtsopts <> ["-RTS"] <> program.args
            let extraEnv = eventlogSocketEnv program.eventlogSocket
            debug . Info . unlines . concat $
                [ ["Running " <> showCommandForUser name args]
                , [ bool "       " "  with " (i == 0) <> k <> "=" <> v
                  | (i, (k, v)) <- zip [(0 :: Int) ..] extraEnv
                  ]
                ]
            let createProcess =
                    (proc name args)
                        { env = Just (inheritedEnv <> extraEnv)
                        , std_in = CreatePipe
                        , std_out = CreatePipe
                        , std_err = CreatePipe
                        }
            (maybeHandleIn, maybeHandleOut, maybeHandleErr, processHandle) <- createProcess_ program.name createProcess
            maybeKillDebugOut <- traverse (debugHandle ProgramOut) maybeHandleOut
            maybeKillDebugErr <- traverse (debugHandle ProgramErr) maybeHandleErr
            let kill = mask_ $ do
                    debug . Info $ "Cleaning up stdout reader for " <> program.name <> "."
                    sequence_ maybeKillDebugOut
                    debug . Info $ "Cleaning up stderr reader for " <> program.name <> "."
                    sequence_ maybeKillDebugErr
                    debug . Info $ "Cleaning up process for " <> program.name <> "."
                    exitCode <- murderProcess program.name (maybeHandleIn, maybeHandleOut, maybeHandleErr, processHandle)
                    debug . Info $ "Process was killed with exit code " <> show exitCode
                    pure ()
            let wait = do
                    debug . Info $ "Waiting for " <> program.name <> " to finish."
                    bracketOnError (waitForProcess processHandle) (const kill) $ \exitCode ->
                        when (exitCode /= ExitSuccess) . assertFailure $
                            "Program " <> program.name <> " failed with exit code " <> show exitCode
            pure ProgramHandle{..}
    bracketOnError runner kill $ pure

{- |
Kill the process harder than `terminateProcess`.

This function also hides the evidence by closing the handles.
-}
murderProcess :: (HasLogger) => String -> (Maybe Handle, Maybe Handle, Maybe Handle, ProcessHandle) -> IO ExitCode
murderProcess programName (maybeStdIn, maybeStdOut, maybeStdErr, processHandle) = do
    -- Murder the process until it's dead:
    let murderLoop :: IO ExitCode
        murderLoop =
            getProcessExitCode processHandle >>= \case
                Nothing -> do
                    debug . Info $ "Trying to kill the process for " <> programName
                    terminateProcess processHandle
                    threadDelay 100_000 -- 100ms
                    murderLoop
                Just exitCode ->
                    pure exitCode
    exitCode <- murderLoop

    -- Clean up handles:
    traverse_ (ignoreSigPipe . IO.hClose) maybeStdIn
    traverse_ IO.hClose maybeStdOut
    traverse_ IO.hClose maybeStdErr

    pure exitCode

shouldInherit :: (String, String) -> Bool
shouldInherit = (`elem` ["PATH"]) . fst

eventlogSocketEnv :: EventlogSocket -> [(String, String)]
eventlogSocketEnv = \case
    EventlogUnixSocket{..} ->
        [("GHC_EVENTLOG_UNIX_SOCKET", unixSocketPath)]
    EventlogTcpSocket{..} ->
        [("GHC_EVENTLOG_TCP_HOST", tcpHost), ("GHC_EVENTLOG_TCP_PORT", show tcpPort)]

--------------------------------------------------------------------------------
-- Eventlog Validation
--------------------------------------------------------------------------------

isEventlogUnixSocket :: EventlogSocket -> Bool
isEventlogUnixSocket = \case
    EventlogUnixSocket{} -> True
    _otherwise -> False

isEventlogTcpSocket :: EventlogSocket -> Bool
isEventlogTcpSocket = \case
    EventlogTcpSocket{} -> True
    _otherwise -> False

data EventlogAssertion x
    = EventlogAssertion
    { initialTimeoutS :: Double
    , timeoutExponent :: Double
    , eventlogSocket :: EventlogSocket
    , validateEvents :: Handle -> ProcessT IO Event x
    }

defaultEventlogAssertion :: EventlogSocket -> EventlogAssertion Event
defaultEventlogAssertion eventlogSocket =
    EventlogAssertion
        { initialTimeoutS = 0.25
        , timeoutExponent = 1
        , eventlogSocket
        , validateEvents = const echo
        }

assertEventlogOk :: (HasLogger) => EventlogSocket -> Assertion
assertEventlogOk eventlogSocket =
    assertEventlogWith eventlogSocket (debugEventCounter 1_000)

assertEventlogWith :: (HasLogger) => EventlogSocket -> ProcessT IO Event x -> Assertion
assertEventlogWith eventlogSocket validateEvents =
    assertEventlogWith' eventlogSocket (const validateEvents)

assertEventlogWith' :: (HasLogger) => EventlogSocket -> (Handle -> ProcessT IO Event x) -> Assertion
assertEventlogWith' eventlogSocket validateEvents =
    runEventlogAssertion (defaultEventlogAssertion eventlogSocket){validateEvents = validateEvents}

runEventlogAssertion :: (HasLogger) => EventlogAssertion x -> Assertion
runEventlogAssertion EventlogAssertion{..} = do
    withEventlogHandle initialTimeoutS timeoutExponent eventlogSocket $ \handle ->
        runT_ $ fromHandle 1000 4096 handle ~> decodeEvent ~> debugEvents ~> validateEvents handle

withEventlogHandle ::
    (HasLogger) =>
    Double ->
    Double ->
    EventlogSocket ->
    (Handle -> IO a) ->
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
                    waitFor timeoutS
                    connectLoop (timeoutS * timeoutExponent)
                else throwIO ioe
      where
        tryConnect =
            case eventlogSocket of
                EventlogUnixSocket{..} -> do
                    bracket (S.socket S.AF_UNIX S.Stream S.defaultProtocol) S.close $ \socket -> do
                        debug . Info $ "Trying to connect to Unix socket at " <> unixSocketPath
                        S.connect socket (S.SockAddrUnix unixSocketPath)
                        debug . Info $ "Connected to Unix socket at " <> unixSocketPath
                        bracket (S.socketToHandle socket ReadWriteMode) IO.hClose $ \handle -> do
                            hSetBuffering handle NoBuffering
                            action handle
                EventlogTcpSocket{..} -> do
                    let tcpHints =
                            S.defaultHints
                                { S.addrFamily = S.AF_INET
                                , S.addrSocketType = S.Stream
                                }
                    addr <- NE.head <$> S.getAddrInfo (Just tcpHints) (Just tcpHost) (Just . show $ tcpPort)
                    let newSocket = S.socket (S.addrFamily addr) (S.addrSocketType addr) (S.addrProtocol addr)
                    let closeSocket socket = S.gracefulClose socket timeoutMSec
                    bracket newSocket closeSocket $ \socket -> do
                        debug . Info $ "Trying to connect to TCP socket at " <> show (S.addrAddress addr)
                        S.connect socket (S.addrAddress addr)
                        debug . Info $ "Connected to TCP socket at " <> show (S.addrAddress addr)
                        bracket (S.socketToHandle socket ReadWriteMode) IO.hClose $ \handle -> do
                            hSetBuffering handle NoBuffering
                            action handle
          where
            timeoutMSec = round $ timeoutS * 1e3

fromHandle ::
    (HasCallStack) =>
    Int ->
    Int ->
    Handle ->
    SourceT IO BS.ByteString
fromHandle timeoutMSec chunkSizeBytes handle = construct go
  where
    go =
        liftIO (isReady timeoutMSec handle) >>= \case
            Just ready
                | ready -> liftIO (BS.hGetSome handle chunkSizeBytes) >>= yield >> go
                | otherwise -> go
            Nothing -> pure ()

isReady :: (HasCallStack) => Int -> Handle -> IO (Maybe Bool)
isReady timeoutMSec handle = catch ready onEOF
  where
    ready =
        hWaitForInput handle timeoutMSec
            >>= pure . Just
    onEOF (e :: IOError) =
        if isEOFError e then pure Nothing else assertFailure (displayException e)

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

{- |
Consume inputs until the predicate holds, then stop.
Throw an exception if the input stream stops.
-}
anyOf :: (HasLogger) => (a -> Bool) -> (Int -> String) -> ProcessT IO a x
anyOf p msgFor = go (0 :: Int)
  where
    go count = MachineT $ pure $ Await onNext Refl onStop
      where
        onNext a = if p a then stopped else go (count + 1)
        onStop = failing (msgFor count)

{- |
Evaluate `anyOf` for the given number of seconds, then fail.
-}
anyFor :: (HasLogger) => Double -> (a -> Bool) -> String -> ProcessT IO a x
anyFor timeoutSec p msg = afterTimeoutSec timeoutSec (anyOf p $ const msg) (failing msg)

{- |
Consume inputs forever.
Throw an exception if the predicate fails to hold for any input.
-}
allOf :: (HasLogger) => (a -> Bool) -> (Int -> String) -> ProcessT IO a x
allOf p msgFor = go (0 :: Int)
  where
    go count = MachineT $ pure $ Await onNext Refl onStop
      where
        onNext a = if p a then go (count + 1) else failing (msgFor count)
        onStop = stopped

{- |
Evaluate `allFor` for the given number of seconds, then fail.
-}
allFor :: (HasLogger) => Double -> (a -> Bool) -> String -> ProcessT IO a x
allFor timeoutSec p msg = withTimeoutSec timeoutSec (allOf p $ const msg)

{- |
Immediately `assertFailure`.
-}
failing :: (HasLogger) => String -> MachineT IO k o
failing msg = MachineT $ do
    debug . Fail $ msg
    assertFailure msg

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
hasHeapProfSampleBeginWithinSec :: (HasLogger) => Double -> ProcessT IO Event x
hasHeapProfSampleBeginWithinSec timeoutSec =
    anyFor timeoutSec isHeapProfSampleBegin $
        printf "Did not find HeapProfSampleBegin within %0.2f seconds." timeoutSec

{- |
Assert that the input stream contains a `HeapProfSampleString` event within the given number of events.
-}
hasHeapProfSampleBeginWithin :: (HasLogger) => Int -> ProcessT IO Event x
hasHeapProfSampleBeginWithin count =
    taking count
        ~> anyOf
            isHeapProfSampleBegin
            (printf "Did not find HeapProfSampleBegin after %d events.")

{- |
Assert that the input stream does not contain a `HeapProfSampleString` event within the given timeout.
-}
hasNoHeapProfSampleBeginWithinSec :: (HasLogger) => Double -> ProcessT IO Event x
hasNoHeapProfSampleBeginWithinSec timeoutSec =
    anyFor timeoutSec isHeapProfSampleBegin $
        printf "Found HeapProfSampleBegin within %0.2f seconds." timeoutSec

{- |
Assert that the input stream does not contain a `HeapProfSampleString` event within the given number of events.
-}
hasNoHeapProfSampleBeginWithin :: (HasLogger) => Int -> ProcessT IO Event x
hasNoHeapProfSampleBeginWithin count =
    taking count
        ~> anyOf
            isHeapProfSampleBegin
            (printf "Found HeapProfSampleBegin after %d events.")

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
hasHeapProfSampleStringWithinSec :: (HasLogger) => Double -> ProcessT IO Event x
hasHeapProfSampleStringWithinSec timeoutSec =
    anyFor timeoutSec isHeapProfSampleString $
        printf "Did not find HeapProfSampleString within %0.2f seconds." timeoutSec

{- |
Assert that the input stream contains a `HeapProfSampleString` event within the given number of events.
-}
hasHeapProfSampleStringWithin :: (HasLogger) => Int -> ProcessT IO Event x
hasHeapProfSampleStringWithin count =
    taking count
        ~> anyOf
            isHeapProfSampleString
            (printf "Did not find HeapProfSampleString after %d events.")

{- |
Assert that the input stream does not contain a `HeapProfSampleString` event within the given timeout.
-}
hasNoHeapProfSampleStringWithinSec :: (HasLogger) => Double -> ProcessT IO Event x
hasNoHeapProfSampleStringWithinSec timeoutSec =
    allFor timeoutSec (not . isHeapProfSampleString) $
        printf "Found HeapProfSampleString within %0.2f seconds." timeoutSec

{- |
Assert that the input stream does not contain a `HeapProfSampleString` event within the given number of events.
-}
hasNoHeapProfSampleStringWithin :: (HasLogger) => Int -> ProcessT IO Event x
hasNoHeapProfSampleStringWithin count =
    taking count
        ~> allOf
            (not . isHeapProfSampleString)
            (printf "Found HeapProfSampleString after %d events.")

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
hasMatchingUserMarkerWithinSec :: (HasLogger) => (Text -> Bool) -> Double -> ProcessT IO Event x
hasMatchingUserMarkerWithinSec p timeoutSec =
    anyFor timeoutSec (isMatchingUserMarker p) $
        printf "Did not find matching UserMarker within %0.2f seconds." timeoutSec

{- |
Assert that the input stream contains a matching `UserMarker` event within the given number of events.
-}
hasMatchingUserMarkerWithin :: (HasLogger) => (Text -> Bool) -> Int -> ProcessT IO Event x
hasMatchingUserMarkerWithin p count =
    taking count
        ~> anyOf
            (isMatchingUserMarker p)
            (printf "Did not find matching UserMarker after %d events.")

{- |
Assert that the input stream contains a matching `UserMarker` event.
-}
hasMatchingUserMarker :: (HasLogger) => (Text -> Bool) -> ProcessT IO Event x
hasMatchingUserMarker p =
    anyOf
        (isMatchingUserMarker p)
        (const "Did not find matching UserMarker.")

{- |
Send the given `Command` over the `Handle`.
-}
sendCommand :: Handle -> Command -> IO ()
sendCommand handle command = do
    BSL.hPutStr handle (B.encode command)
    IO.hFlush handle

{- |
Send junk over the `Handle`.
-}
sendJunk :: Handle -> BSL.ByteString -> IO ()
sendJunk handle junk = do
    BSL.hPutStr handle junk
    IO.hFlush handle

--------------------------------------------------------------------------------
-- Print Debug Message
--------------------------------------------------------------------------------

tcpPortCounter :: MVar Word16
tcpPortCounter = unsafePerformIO (newMVar 0)

testCaseForUnix :: (HasLogger) => TestName -> (EventlogSocket -> Assertion) -> EventlogSocket -> Maybe TestTree
testCaseForUnix testName test eventlogSocket
    | isEventlogUnixSocket eventlogSocket = testCaseFor testName test eventlogSocket
    | otherwise = Nothing

testCaseFor :: (HasLogger) => TestName -> (EventlogSocket -> Assertion) -> EventlogSocket -> Maybe TestTree
testCaseFor testName test = \case
    EventlogUnixSocket{..} ->
        let testName' = testName <> "_Unix"
         in Just $ testCase testName' $ do
                debug $ Header testName'
                let (directory, fileName) = splitFileName unixSocketPath
                let unixSocketPath' = directory </> testName <> "_" <> fileName
                debug . Info $ "Using Unix socket: " <> unixSocketPath'
                test $ EventlogUnixSocket unixSocketPath'
                debug $ Footer testName'
    EventlogTcpSocket{..} ->
        let testName' = testName <> "_Tcp"
         in Just $ testCase testName' $ do
                debug $ Header testName'
                tcpPortOffset <- modifyMVar tcpPortCounter $ \currentTcpPortOffset -> do
                    let nextTcpPortOffset = currentTcpPortOffset + 1
                    pure (nextTcpPortOffset, currentTcpPortOffset)
                let tcpPort' = tcpPort + tcpPortOffset
                test $ EventlogTcpSocket tcpHost tcpPort'
                debug $ Footer testName'

type HasLogger = (?logger :: Logger, HasCallStack)

#if defined(DEBUG)
data Logger = Logger {logChan :: T.TChan Message}
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
    = Header TestName
    | Footer TestName
    | ProgramOut String
    | ProgramErr String
    | Event Event
    | Info String
    | Fail String

debug :: (HasLogger) => Message -> IO ()
#if defined(DEBUG)
debug message = do
    let logger = ?logger
    T.atomically (T.writeTChan (logChan logger) message)
#else
debug _message = do
    let _logger = ?logger
    pure ()
#endif

withLogger :: ((HasLogger) => IO ()) -> IO ()
#if defined(DEBUG)
withLogger action = do
    logger <- newLogger
    let ?logger = logger
    let runner = do
            message <- T.atomically (T.readTChan . logChan $ logger)
            IO.hPutStrLn stderr (renderMessage message)
            IO.hFlush stderr
            runner
    bracket (forkIO runner) killThread $ \_threadId -> do
        action
  where
    renderMessage :: Message -> String
    renderMessage = \case
        Header testName ->
            "-- HEADER: " <> testName <> " " <> replicate (80 - (length testName + 12)) '-'
        Footer testName ->
            "-- FOOTER: " <> testName <> " " <> replicate (80 - (length testName + 12)) '-'
        ProgramOut message ->
            "[ProgramOut] " <> message
        ProgramErr message ->
            "[ProgramErr] " <> message
        Event event ->
            "[Event] " <> ppEvent PrettyTime mempty event
        Info message ->
            "[Info] " <> message
        Fail message ->
            "[Fail] " <> message
#else
withLogger action = do
    logger <- newLogger
    let ?logger = logger
    action
#endif

debugHandle :: (HasLogger) => (String -> Message) -> Handle -> IO (IO ())
debugHandle wrapper handle = do
    let runner = do
            threadDelay 1_000 -- Wait 100ms.
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

debugEvents :: (HasLogger) => ProcessT IO Event Event
debugEvents =
    repeatedly $
        await >>= \event -> do
            liftIO (debug $ Event event)
            yield event

debugEventCounter :: (HasLogger) => Int -> ProcessT IO Event Event
debugEventCounter limit = go (0 :: Int)
  where
    go count = MachineT $ pure $ Await onNext Refl onStop
      where
        onNext event =
            MachineT $ do
                let count' = count + 1
                if (count' >= limit)
                    then do
                        liftIO (debug . Info $ "Saw " <> show count' <> " events.")
                        pure $ Yield event (go 0)
                    else do
                        pure $ Yield event (go count')
        onStop =
            MachineT $ do
                liftIO (debug . Info $ "Saw " <> show count <> " events.")
                pure Stop
