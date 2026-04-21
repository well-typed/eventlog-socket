{-# LANGUAGE CPP #-}
{-# LANGUAGE OverloadedStrings #-}

module Main (main) where

import Control.Concurrent (threadDelay)
import qualified Data.Binary as B
import Data.ByteString.Lazy (ByteString)
import qualified Data.ByteString.Lazy as BSL
import Data.Machine ((~>))
import Data.Maybe (fromMaybe)
import qualified Data.Text as T
import qualified GHC.Eventlog.Socket.Control as C
import GHC.Eventlog.Socket.Test
import System.Environment (lookupEnv)
import System.FilePath ((</>))
import System.IO.Temp (withTempDirectory)
import Test.Tasty (defaultMain, testGroup)
import Text.Read (readMaybe)

main :: IO ()
main = do
    -- Allow the user to overwrite the TCP port:
    tcpPort <- (fromMaybe "4242" . (readMaybe =<<)) <$> lookupEnv "GHC_EVENTLOG_INET_PORT"

    let basicTests :: (HasLogger) => [EventlogSocketAddr -> ProgramTest]
        basicTests =
            [ test_fibber
            , test_fibberCMain
            , test_oddball_HasHeapProfSample
            , test_oddball_NoAutomaticHeapSamples
            , test_oddball_Reconnect
            , test_oddball_HookOnReconnect
            , test_oddball_ResetOnReconnect
            , test_oddball_StartAndStopHeapProfiling
            , test_oddball_RequestHeapCensus
            , test_oddball_Junk ("\0\0", "TOASTY")
            , test_oddball_Junk ("\x01DEAD", "DORK")
            , test_customCommand
            ]

    -- Check whether or not to enable dynamic trace flag tests:
    enableDynamicTraceFlagTests <- shouldEnableDynamicTraceFlagTests

    let dynamicTraceFlagTests :: (HasLogger) => [EventlogSocketAddr -> ProgramTest]
        dynamicTraceFlagTests
            | enableDynamicTraceFlagTests =
                [ test_oddball_StartAndStopSchedulerTracing
                , test_oddball_StartAndStopGcTracing
                , test_oddball_StartAndStopNonmovingGcTracing
                , test_parfib_StartAndStopSparkSampledTracing
                , test_parfib_StartAndStopSparkFullTracing
                , test_oddball_StartAndStopUserTracing
                , test_capnDance_StartAndStopCapabilityTracing
                ]
            | otherwise = []

    -- Create logger:
    withLogger $ do
        -- Create temporary directory:
        withTempDirectory "/tmp" "eventlog-socket" $ \tmpDir -> do
            -- Base socket addresses
            let tests = basicTests <> dynamicTraceFlagTests
            let unixTests = tests <*> pure (EventlogSocketUnixAddr $ tmpDir </> "ghc_eventlog.sock")
            let inetTests = tests <*> pure (EventlogSocketInetAddr "127.0.0.1" tcpPort)
            defaultMain . testGroup "Tests" . runProgramTests $ unixTests <> inetTests

{- |
Test that @fibber 35@ produces a parseable eventlog.
-}
test_fibber :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_fibber =
    let fibber =
            Program
                { name = "fibber"
                , args = ["40"]
                , rtsopts = ["-l-au"]
                , eventlogSocketBuildFlags = []
                }
     in programTestFor "test_fibber" fibber $ \eventlogSocket -> do
            assertEventlogWith eventlogSocket $
                -- Validate that the startEventLogging hook fires.
                hasMatchingUserMarker ("HookPostStartEventLogging" `T.isPrefixOf`)
                    -- Validate that the Finished marker is seen.
                    &> hasMatchingUserMarker ("Finished" `T.isPrefixOf`)

{- |
Test that @fibber-c-main 35@ produces a parseable eventlog.
-}
test_fibberCMain :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_fibberCMain =
    let fibberCMain =
            Program
                { name = "fibber-c-main"
                , args = ["40"]
                , rtsopts = ["-l-au"]
                , eventlogSocketBuildFlags = []
                }
     in programTestFor "test_fibberCMain" fibberCMain $ \eventlogSocket -> do
            threadDelay 3_000_000
            assertEventlogWith eventlogSocket $
                -- Validate that the startEventLogging hook fires.
                hasMatchingUserMarker ("HookPostStartEventLogging" `T.isPrefixOf`)
                    -- Validate that the Initialising marker is seen.
                    &> hasMatchingUserMarker ("Initialising" `T.isPrefixOf`)
                    -- Validate that the Starting marker is seen.
                    &> hasMatchingUserMarker ("Starting" `T.isPrefixOf`)
                    -- Validate that the Finished marker is seen.
                    &> hasMatchingUserMarker ("Finished" `T.isPrefixOf`)

{- |
Test that @oddball@ produces heap profile samples.
-}
test_oddball_HasHeapProfSample :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_oddball_HasHeapProfSample =
    let oddball =
            Program
                { name = "oddball"
                , args = []
                , rtsopts = ["-l-au", "-hT", "-A256K", "-i0", "--eventlog-flush-interval=1"]
                , eventlogSocketBuildFlags = []
                }
     in programTestFor "test_oddball_HasHeapProfSample" oddball $ \eventlogSocket -> do
            assertEventlogWith eventlogSocket $
                -- Validate that the Summing marker is seen, and that the event
                -- stream contains at least one heap profile sample.
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasHeapProfSampleString

{- |
Test that @--no-automatic-heap-samples@ is respected.
-}
test_oddball_NoAutomaticHeapSamples :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_oddball_NoAutomaticHeapSamples =
    let oddball =
            Program
                { name = "oddball"
                , args = []
                , rtsopts = ["-l-au", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"]
                , eventlogSocketBuildFlags = []
                }
     in programTestFor "test_oddball_NoAutomaticHeapSamples" oddball $ \eventlogSocket -> do
            assertEventlogWith eventlogSocket $
                -- Validate that the Summing marker is seen, and that the event
                -- stream contains no heap profile samples for two iterations.
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoHeapProfSampleString
                    ~> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))

{- |
Test that @oddball@ produces the init events and heap samples on each connection.
-}
test_oddball_Reconnect :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_oddball_Reconnect =
    let oddball =
            Program
                { name = "oddball"
                , args = []
                , rtsopts = ["-l-au", "-hT", "-A256K", "-i0", "--eventlog-flush-interval=1"]
                , eventlogSocketBuildFlags = []
                }
     in programTestFor "test_oddball_Reconnect" oddball $ \eventlogSocket -> do
            -- Validate that reconnecting works and that each stream has a WallClockTime
            -- event (as proxy for the init events) and at least one heap profile sample.
            assertEventlogWith eventlogSocket $ hasWallClockTime &> hasHeapProfSampleString
            assertEventlogWith eventlogSocket $ hasWallClockTime &> hasHeapProfSampleString

{- |
Test that @oddball@ triggers the post-startEventLogging hook on reconnect.
-}
test_oddball_HookOnReconnect :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_oddball_HookOnReconnect =
    let oddball =
            Program
                { name = "oddball"
                , args = []
                , rtsopts = ["-l-au", "--eventlog-flush-interval=1"]
                , eventlogSocketBuildFlags = []
                }
     in programTestFor "test_oddball_HookOnReconnect" oddball $ \eventlogSocket -> do
            -- Validate that reconnecting works and that each stream has a WallClockTime
            -- event (as proxy for the init events) and triggers the post-startEventLogging hook.
            assertEventlogWith eventlogSocket $
                hasWallClockTime
                    &> hasMatchingUserMarker ("HookPostStartEventLogging" `T.isPrefixOf`)
            assertEventlogWith eventlogSocket $
                hasWallClockTime
                    &> hasMatchingUserMarker ("HookPostStartEventLogging" `T.isPrefixOf`)

{- |
Test that the command parsing state is reset on reconnect.
-}
test_oddball_ResetOnReconnect :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_oddball_ResetOnReconnect =
    let oddball =
            Program
                { name = "oddball"
                , args = []
                , rtsopts = ["-l", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"]
                , eventlogSocketBuildFlags = ["+control"]
                }
     in programTestFor "test_oddball_ResetOnReconnect" oddball $ \eventlogSocket -> do
            -- Validate that the event stream contains at least one heap
            -- profile sample twice, connecting to the socket each time.
            let (requestHeapCensusPart1, requestHeapCensusPart2) = BSL.splitAt 6 (B.encode C.requestHeapCensus)
            assertEventlogWith' eventlogSocket $ \socket ->
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendJunk socket requestHeapCensusPart1
                    !> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
            assertEventlogWith' eventlogSocket $ \socket ->
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendJunk socket requestHeapCensusPart2
                    !> hasNoHeapProfSampleString
                    ~> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))

{- |
Test that the `StartHeapProfiling` and `StopHeapProfiling` commands are
respected, i.e., that once the `StartHeapProfiling` command is sent, heap
profile samples are received, and once the `StopHeapProfiling` command is
sent, after some iterations, no more heap profile samples are received.
-}
test_oddball_StartAndStopHeapProfiling :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_oddball_StartAndStopHeapProfiling =
    let oddball =
            Program
                { name = "oddball"
                , args = []
                , rtsopts = ["-l-au", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"]
                , eventlogSocketBuildFlags = ["+control"]
                }
     in programTestFor "test_oddball_StartAndStopHeapProfiling" oddball $ \eventlogSocket -> do
            assertEventlogWith' eventlogSocket $ \socket ->
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommand socket C.startHeapProfiling
                    !> (2 `times` (hasHeapProfSampleString &> hasHeapProfSampleEnd))
                    &> sendCommand socket C.stopHeapProfiling
                    !> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)

{- |
Test that the `RequestHeapCensus` signal is respected, i.e., that once the
`RequestHeapCensus` command is sent, a heap profile is received.
-}
test_oddball_RequestHeapCensus :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_oddball_RequestHeapCensus =
    let oddball =
            Program
                { name = "oddball"
                , args = []
                , rtsopts = ["-l", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"]
                , eventlogSocketBuildFlags = ["+control"]
                }
     in programTestFor "test_oddball_RequestHeapCensus" oddball $ \eventlogSocket -> do
            assertEventlogWith' eventlogSocket $ \socket ->
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommand socket C.requestHeapCensus
                    -- validate that there is exactly one heap sample, and that
                    -- afterwards there are no further samples, either in this
                    -- or the next iteration.
                    !> hasHeapProfSampleString
                    &> hasHeapProfSampleEnd -- TODO: This needs to be delimited.
                    &> hasNoHeapProfSampleString
                    ~> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))

{- |
Test that the `StartSchedulerTracing` and `StopSchedulerTracing` commands are
respected, i.e., that once the `StartSchedulerTracing` command is sent,
scheduler events are received, and once the `StopSchedulerTracing` command is
sent, after some iterations, no more scheduler events are received.
-}
test_oddball_StartAndStopSchedulerTracing :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_oddball_StartAndStopSchedulerTracing =
    let oddball =
            Program
                { name = "oddball"
                , args = []
                , rtsopts = ["-l-au", "--eventlog-flush-interval=1"]
                , eventlogSocketBuildFlags = ["+control"]
                }
     in programTestFor "test_oddball_StartAndStopSchedulerTracing" oddball $ \eventlogSocket -> do
            assertEventlogWith' eventlogSocket $ \socket ->
                -- Validate that the Summing marker is seen...
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoSchedulerEvent
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommand socket C.startSchedulerTracing
                    !> hasSchedulerEvent
                    &> sendCommand socket C.stopSchedulerTracing
                    !> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))
                    &> hasNoSchedulerEvent
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)

{- |
Test that the `StartGcTracing` and `StopGcTracing` commands are
respected, i.e., that once the `StartGcTracing` command is sent,
GC events are received, and once the `StopGcTracing` command is
sent, after some iterations, no more GC events are received.
-}
test_oddball_StartAndStopGcTracing :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_oddball_StartAndStopGcTracing =
    let oddball =
            Program
                { name = "oddball"
                , args = []
                , rtsopts = ["-l-au", "--eventlog-flush-interval=1"]
                , eventlogSocketBuildFlags = ["+control"]
                }
     in programTestFor "test_oddball_StartAndStopGcTracing" oddball $ \eventlogSocket -> do
            assertEventlogWith' eventlogSocket $ \socket ->
                -- Validate that the Summing marker is seen...
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoGcEvent
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommand socket C.startGcTracing
                    !> hasGcEvent
                    &> sendCommand socket C.stopGcTracing
                    !> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))
                    &> hasNoGcEvent
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)

{- |
Test that the `StartGcTracing` and `StopNonmovingGcTracing` commands are
respected, i.e., that once the `StartNonmovingGcTracing` command is sent,
nonmoving-GC events are received, and once the `StopNonmovingGcTracing` command is
sent, after some iterations, no more nonmoving-GC events are received.
-}
test_oddball_StartAndStopNonmovingGcTracing :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_oddball_StartAndStopNonmovingGcTracing =
    let oddball =
            Program
                { name = "oddball"
                , args = []
                , rtsopts = ["-l-au", "--eventlog-flush-interval=1", "--nonmoving-gc"]
                , eventlogSocketBuildFlags = ["+control"]
                }
     in programTestFor "test_oddball_StartAndStopNonmovingGcTracing" oddball $ \eventlogSocket -> do
            assertEventlogWith' eventlogSocket $ \socket ->
                -- Validate that the Summing marker is seen...
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoNonmovingGcEvent
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommand socket C.startNonmovingGcTracing
                    !> hasNonmovingGcEvent
                    &> sendCommand socket C.stopNonmovingGcTracing
                    !> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))
                    &> hasNoNonmovingGcEvent
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)

{- |
Test that the `StartGcTracing` and `StopSparkSampledTracing` commands are
respected, i.e., that once the `StartSparkSampledTracing` command is sent,
sampled spark events are received, and once the `StopSparkSampledTracing` command is
sent, after some iterations, no more sampled spark events are received.
-}
test_parfib_StartAndStopSparkSampledTracing :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_parfib_StartAndStopSparkSampledTracing =
    let parfib =
            Program
                { name = "parfib"
                , args = ["10", "45"]
                , rtsopts = ["-l-au", "--eventlog-flush-interval=1"]
                , eventlogSocketBuildFlags = ["+control"]
                }
     in programTestFor "test_parfib_StartAndStopSparkSampledTracing" parfib $ \eventlogSocket -> do
            assertEventlogWith' eventlogSocket $ \socket ->
                -- Validate that the Fibbing marker is seen...
                hasMatchingUserMarker ("Fibbing" `T.isPrefixOf`)
                    &> hasNoSparkSampledEvent
                    ~> hasMatchingUserMarker ("Fibbing" `T.isPrefixOf`)
                    &> sendCommand socket C.startSparkSampledTracing
                    !> hasSparkSampledEvent
                    &> sendCommand socket C.stopSparkSampledTracing
                    !> (2 `times` hasMatchingUserMarker ("Fibbing" `T.isPrefixOf`))
                    &> hasNoSparkSampledEvent
                    ~> (2 `times` hasMatchingUserMarker ("Fibbing" `T.isPrefixOf`))

{- |
Test that the `StartGcTracing` and `StopSparkFullTracing` commands are
respected, i.e., that once the `StartSparkFullTracing` command is sent,
full spark events are received, and once the `StopSparkFullTracing` command is
sent, after some iterations, no more full spark events are received.
-}
test_parfib_StartAndStopSparkFullTracing :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_parfib_StartAndStopSparkFullTracing =
    let parfib =
            Program
                { name = "parfib"
                , args = ["10", "45"]
                , rtsopts = ["-l-au", "--eventlog-flush-interval=1"]
                , eventlogSocketBuildFlags = ["+control"]
                }
     in programTestFor "test_parfib_StartAndStopSparkFullTracing" parfib $ \eventlogSocket -> do
            assertEventlogWith' eventlogSocket $ \socket ->
                -- Validate that the Fibbing marker is seen...
                hasMatchingUserMarker ("Fibbing" `T.isPrefixOf`)
                    &> hasNoSparkFullEvent
                    ~> hasMatchingUserMarker ("Fibbing" `T.isPrefixOf`)
                    &> sendCommand socket C.startSparkFullTracing
                    !> hasSparkFullEvent
                    &> sendCommand socket C.stopSparkFullTracing
                    !> (2 `times` hasMatchingUserMarker ("Fibbing" `T.isPrefixOf`))
                    &> hasNoSparkFullEvent
                    ~> (2 `times` hasMatchingUserMarker ("Fibbing" `T.isPrefixOf`))

{- |
Test that the `StartUserTracing` and `StopUserTracing` commands are
respected, i.e., that once the `StartUserTracing` command is sent,
user events are received, and once the `StopUserTracing` command is
sent, after some iterations, no more user events are received.
-}
test_oddball_StartAndStopUserTracing :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_oddball_StartAndStopUserTracing =
    let oddball =
            Program
                { name = "oddball"
                , args = []
                , rtsopts = ["-l-aug", "--eventlog-flush-interval=1"]
                , eventlogSocketBuildFlags = ["+control"]
                }
     in programTestFor "test_oddball_StartAndStopUserTracing" oddball $ \eventlogSocket -> do
            assertEventlogWith' eventlogSocket $ \socket ->
                -- Validate that the Summing marker is seen...
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommand socket C.stopUserTracing
                    !> droppingFor 5.0
                    &> hasNoUserEventWithinSec 5.0
                    &> sendCommand socket C.startUserTracing
                    !> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)

{- |
Test that the `StartGcTracing` and `StopCapabilityTracing` commands are
respected, i.e., that once the `StartCapabilityTracing` command is sent,
capability events are received, and once the `StopCapabilityTracing` command is
sent, after some iterations, no more capability events are received.
-}
test_capnDance_StartAndStopCapabilityTracing :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_capnDance_StartAndStopCapabilityTracing =
    let capnDance =
            Program
                { name = "capn-dance"
                , args = []
                , rtsopts = ["-l-au", "--eventlog-flush-interval=1"]
                , eventlogSocketBuildFlags = ["+control"]
                }
     in programTestFor "test_capnDance_StartAndStopCapabilityTracing" capnDance $ \eventlogSocket -> do
            assertEventlogWith' eventlogSocket $ \socket ->
                -- Validate that the Dancing marker is seen...
                hasMatchingUserMarker ("Dancing" `T.isPrefixOf`)
                    &> hasCapabilityEvent
                    &> sendCommand socket C.stopCapabilityTracing
                    !> (2 `times` hasMatchingUserMarker ("Dancing" `T.isPrefixOf`))
                    &> hasNoCapabilityEvent
                    ~> (2 `times` hasMatchingUserMarker ("Dancing" `T.isPrefixOf`))
                    &> sendCommand socket C.startCapabilityTracing
                    !> hasCapabilityEvent

{- |
Test that the `RequestHeapCensus` command is still respected after junk has
been sent over the control socket.
-}
test_oddball_Junk :: (HasLogger) => (ByteString, ByteString) -> EventlogSocketAddr -> ProgramTest
test_oddball_Junk (junkBefore, junkAfter) =
    let oddball =
            Program
                { name = "oddball"
                , args = []
                , rtsopts = ["-l-au", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"]
                , eventlogSocketBuildFlags = ["+control"]
                }
     in programTestFor ("test_oddball_Junk[" <> show junkBefore <> "," <> show junkAfter <> "]") oddball $ \eventlogSocket -> do
            assertEventlogWith' eventlogSocket $ \socket ->
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommandWithJunk socket junkBefore C.requestHeapCensus junkAfter
                    !> hasHeapProfSampleString
                    &> hasHeapProfSampleEnd -- TODO: This needs to be delimited.
                    &> hasNoHeapProfSampleString
                    ~> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))

{- |
Test that custom commands are respected.
-}
test_customCommand :: (HasLogger) => EventlogSocketAddr -> ProgramTest
test_customCommand =
    let customCommand =
            Program
                { name = "custom-command"
                , args = ["--forever"]
                , rtsopts = ["-l-au", "--eventlog-flush-interval=1"]
                , eventlogSocketBuildFlags = ["+control"]
                }
     in programTestFor "test_customCommand" customCommand $ \eventlogSocket -> do
            assertEventlogWith' eventlogSocket $ \socket ->
                hasMatchingUserMarker ("custom workload iteration " `T.isPrefixOf`)
                    &> sendCommand socket (C.userCommand (C.userNamespace "custom-command") (C.CommandId 1))
                    !> hasMatchingUserMessage ("handled ping" ==)
                    &> sendCommand socket (C.userCommand (C.userNamespace "custom-command") (C.CommandId 2))
                    !> hasMatchingUserMessage ("handled pong" ==)
                    &> (2 `times` hasMatchingUserMarker ("custom workload iteration " `T.isPrefixOf`))

-- -}
