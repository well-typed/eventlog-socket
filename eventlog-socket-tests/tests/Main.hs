{-# LANGUAGE OverloadedStrings #-}

module Main (main) where

import qualified Data.Binary as B
import Data.ByteString.Lazy (ByteString)
import qualified Data.ByteString.Lazy as BSL
import Data.Functor ((<&>))
import Data.Machine ((~>))
import Data.Maybe (catMaybes, fromMaybe)
import qualified Data.Text as T
import GHC.Eventlog.Socket.Control (CommandId (..), requestHeapCensus, startHeapProfiling, stopHeapProfiling, userCommand, userNamespace)
import System.Environment (lookupEnv)
import System.FilePath ((</>))
import System.IO.Temp (withTempDirectory)
import Test.Common
import Test.Tasty (TestTree, defaultMain, testGroup)
import Text.Read (readMaybe)

main :: IO ()
main = do
    -- Allow the user to overwrite the TCP port:
    tcpPort <- (fromMaybe "4242" . (readMaybe =<<)) <$> lookupEnv "GHC_EVENTLOG_INET_PORT"

    -- Create:
    withLogger $ do
        -- Create temporary directory:
        withTempDirectory "/tmp" "eventlog-socket" $ \tmpDir -> do
            defaultMain . testGroup "Tests" $
                [ testGroup "Unix" . catMaybes $
                    tests <&> \test ->
                        test $ EventlogSocketUnixAddr $ tmpDir </> "ghc_eventlog.sock"
                , testGroup "Tcp" . catMaybes $
                    tests <&> \test ->
                        test $ EventlogSocketInetAddr "127.0.0.1" tcpPort
                ]
  where
    tests :: (HasLogger) => [EventlogSocketAddr -> Maybe TestTree]
    tests =
        [ test_fibber
        , test_fibberCMain
        , test_oddball
        , test_oddball_NoAutomaticHeapSamples
        , test_oddball_Reconnect
        , test_oddball_ResetOnReconnect
        , test_oddball_StartAndStopHeapProfiling
        , test_oddball_RequestHeapCensus
        , test_oddball_Junk ("\0\0", "TOASTY")
        , test_oddball_Junk ("\x01DEAD", "DORK")
        , test_customCommand
        ]

{- |
Test that @fibber 35@ produces a parseable eventlog.
-}
test_fibber :: (HasLogger) => EventlogSocketAddr -> Maybe TestTree
test_fibber =
    testCaseFor "test_fibber" $ \eventlogSocket -> do
        let fibber =
                Program
                    { name = "fibber"
                    , args = ["40"]
                    , rtsopts = ["-l-au"]
                    , eventlogSocketBuildFlags = []
                    , eventlogSocketAddr = eventlogSocket
                    }
        withProgram fibber $
            assertEventlogWith eventlogSocket $
                -- Validate that the Finished marker is seen.
                hasMatchingUserMarker ("Finished" `T.isPrefixOf`)

{- |
Test that @fibber-c-main 35@ produces a parseable eventlog.
-}
test_fibberCMain :: (HasLogger) => EventlogSocketAddr -> Maybe TestTree
test_fibberCMain =
    testCaseFor "test_fibberCMain" $ \eventlogSocket -> do
        let fibber =
                Program
                    { name = "fibber-c-main"
                    , args = ["40"]
                    , rtsopts = ["-l-au"]
                    , eventlogSocketBuildFlags = []
                    , eventlogSocketAddr = eventlogSocket
                    }
        withProgram fibber $
            assertEventlogWith eventlogSocket $
                -- Validate that the Finished marker is seen.
                hasMatchingUserMarker ("Finished" `T.isPrefixOf`)

{- |
Test that @oddball@ produces heap profile samples.
-}
test_oddball :: (HasLogger) => EventlogSocketAddr -> Maybe TestTree
test_oddball =
    testCaseFor "test_oddball" $ \eventlogSocket -> do
        let oddball =
                Program
                    { name = "oddball"
                    , args = []
                    , rtsopts = ["-l-au", "-hT", "-A256K", "-i0", "--eventlog-flush-interval=1"]
                    , eventlogSocketBuildFlags = []
                    , eventlogSocketAddr = eventlogSocket
                    }
        withProgram oddball $
            assertEventlogWith eventlogSocket $
                -- Validate that the Summing marker is seen, and that the event
                -- stream contains at least one heap profile sample.
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasHeapProfSampleString

{- |
Test that @--no-automatic-heap-samples@ is respected.
-}
test_oddball_NoAutomaticHeapSamples :: (HasLogger) => EventlogSocketAddr -> Maybe TestTree
test_oddball_NoAutomaticHeapSamples =
    testCaseFor "test_oddball_NoAutomaticHeapSamples" $ \eventlogSocket -> do
        let oddball =
                Program
                    { name = "oddball"
                    , args = []
                    , rtsopts = ["-l-au", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"]
                    , eventlogSocketBuildFlags = []
                    , eventlogSocketAddr = eventlogSocket
                    }
        withProgram oddball $
            assertEventlogWith eventlogSocket $
                -- Validate that the Summing marker is seen, and that the event
                -- stream contains no heap profile samples for two iterations.
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoHeapProfSampleString
                    ~> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))

{- |
Test that @oddball@ produces heap profile samples even after reconnecting.
-}
test_oddball_Reconnect :: (HasLogger) => EventlogSocketAddr -> Maybe TestTree
test_oddball_Reconnect =
    testCaseFor "test_oddball_Reconnect" $ \eventlogSocket -> do
        let oddball =
                Program
                    { name = "oddball"
                    , args = []
                    , rtsopts = ["-l-au", "-hT", "-A256K", "-i0", "--eventlog-flush-interval=1"]
                    , eventlogSocketBuildFlags = []
                    , eventlogSocketAddr = eventlogSocket
                    }
        withProgram oddball $ do
            -- Validate that the event stream contains at least one heap
            -- profile sample twice, connecting to the socket each time.
            assertEventlogWith eventlogSocket $ hasHeapProfSampleString
            assertEventlogWith eventlogSocket $ hasHeapProfSampleString

{- |
Test that the command parsing state is reset on reconnect.
-}
test_oddball_ResetOnReconnect :: (HasLogger) => EventlogSocketAddr -> Maybe TestTree
test_oddball_ResetOnReconnect =
    testCaseFor "test_oddball_ResetOnReconnect" $ \eventlogSocket -> do
        let oddball =
                Program
                    { name = "oddball"
                    , args = []
                    , rtsopts = ["-l", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"]
                    , eventlogSocketBuildFlags = ["+control"]
                    , eventlogSocketAddr = eventlogSocket
                    }
        withProgram oddball $ do
            -- Validate that the event stream contains at least one heap
            -- profile sample twice, connecting to the socket each time.
            let (requestHeapCensusPart1, requestHeapCensusPart2) = BSL.splitAt 6 (B.encode requestHeapCensus)
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
test_oddball_StartAndStopHeapProfiling :: (HasLogger) => EventlogSocketAddr -> Maybe TestTree
test_oddball_StartAndStopHeapProfiling =
    testCaseFor "test_oddball_StartAndStopHeapProfiling" $ \eventlogSocket -> do
        let oddball =
                Program
                    { name = "oddball"
                    , args = []
                    , rtsopts = ["-l-au", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"]
                    , eventlogSocketBuildFlags = ["+control"]
                    , eventlogSocketAddr = eventlogSocket
                    }
        withProgram oddball $
            assertEventlogWith' eventlogSocket $ \socket ->
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommand socket startHeapProfiling
                    !> (2 `times` (hasHeapProfSampleString &> hasHeapProfSampleEnd))
                    &> sendCommand socket stopHeapProfiling
                    !> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)

{- |
Test that the `RequestHeapCensus` signal is respected, i.e., that once the
`RequestHeapCensus` command is sent, a heap profile is received.
-}
test_oddball_RequestHeapCensus :: (HasLogger) => EventlogSocketAddr -> Maybe TestTree
test_oddball_RequestHeapCensus =
    testCaseFor "test_oddball_RequestHeapCensus" $ \eventlogSocket -> do
        let oddball =
                Program
                    { name = "oddball"
                    , args = []
                    , rtsopts = ["-l", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"]
                    , eventlogSocketBuildFlags = ["+control"]
                    , eventlogSocketAddr = eventlogSocket
                    }
        withProgram oddball $
            assertEventlogWith' eventlogSocket $ \socket ->
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommand socket requestHeapCensus
                    -- validate that there is exactly one heap sample, and that
                    -- afterwards there are no further samples, either in this
                    -- or the next iteration.
                    !> hasHeapProfSampleString
                    &> hasHeapProfSampleEnd -- TODO: This needs to be delimited.
                    &> hasNoHeapProfSampleString
                    ~> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))

{- |
Test that the `RequestHeapCensus` command is still respected after junk has
been sent over the control socket.
-}
test_oddball_Junk :: (HasLogger) => (ByteString, ByteString) -> EventlogSocketAddr -> Maybe TestTree
test_oddball_Junk (junkBefore, junkAfter) =
    let testName = "test_oddball_Junk[" <> show junkBefore <> "," <> show junkAfter <> "]"
     in testCaseFor testName $ \eventlogSocket -> do
            let oddball =
                    Program
                        { name = "oddball"
                        , args = []
                        , rtsopts = ["-l-au", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"]
                        , eventlogSocketBuildFlags = ["+control"]
                        , eventlogSocketAddr = eventlogSocket
                        }
            withProgram oddball $
                assertEventlogWith' eventlogSocket $ \socket ->
                    hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                        &> hasNoHeapProfSampleString
                        ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                        &> sendCommandWithJunk socket junkBefore requestHeapCensus junkAfter
                        !> hasHeapProfSampleString
                        &> hasHeapProfSampleEnd -- TODO: This needs to be delimited.
                        &> hasNoHeapProfSampleString
                        ~> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))

{- |
Test that custom commands are respected.
-}
test_customCommand :: (HasLogger) => EventlogSocketAddr -> Maybe TestTree
test_customCommand =
    testCaseFor "test_customCommand" $ \eventlogSocket -> do
        let customCommand =
                Program
                    { name = "custom-command"
                    , args = ["--forever"]
                    , rtsopts = ["-l-au", "--eventlog-flush-interval=1"]
                    , eventlogSocketBuildFlags = ["+control"]
                    , eventlogSocketAddr = eventlogSocket
                    }
        withProgram customCommand $
            assertEventlogWith' eventlogSocket $ \socket ->
                hasMatchingUserMarker ("custom workload iteration " `T.isPrefixOf`)
                    &> sendCommand socket (userCommand (userNamespace "custom-command") (CommandId 0))
                    !> hasMatchingUserMessage ("handled ping" ==)
                    &> sendCommand socket (userCommand (userNamespace "custom-command") (CommandId 1))
                    !> hasMatchingUserMessage ("handled pong" ==)
                    &> (2 `times` hasMatchingUserMarker ("custom workload iteration " `T.isPrefixOf`))
