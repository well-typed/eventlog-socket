{-# LANGUAGE OverloadedStrings #-}

module Main (main) where

import Data.Functor ((<&>))
import Data.Machine ((~>))
import Data.Maybe (catMaybes, fromMaybe)
import qualified Data.Text as T
import GHC.Eventlog.Socket.Control (Command (..), CommandId (..), userCommand, userNamespace)
import System.Environment (lookupEnv)
import System.FilePath ((</>))
import System.IO.Temp (withTempDirectory)
import Test.Common
import Test.Tasty (TestTree, defaultMain, testGroup)
import Text.Read (readMaybe)

main :: IO ()
main = do
    -- Allow the user to overwrite the TCP port:
    tcpPort <- (fromMaybe 4242 . (readMaybe =<<)) <$> lookupEnv "GHC_EVENTLOG_TCP_PORT"

    -- Create:
    withLogger $ do
        -- Create temporary directory:
        withTempDirectory "/tmp" "eventlog-socket" $ \tmpDir -> do
            defaultMain . testGroup "Tests" $
                [ testGroup "Unix" . catMaybes $
                    tests <&> \test ->
                        test $ EventlogUnixSocket $ tmpDir </> "ghc_eventlog.sock"
                , testGroup "Tcp" . catMaybes $
                    tests <&> \test ->
                        test $ EventlogTcpSocket "127.0.0.1" tcpPort
                ]
  where
    tests :: (HasLogger) => [EventlogSocket -> Maybe TestTree]
    tests =
        [ test_fibber
        , test_fibberCMain
        , test_oddball
        , test_oddball_NoAutomaticHeapSamples
        , test_oddball_Reconnect
        , test_oddball_StartAndStopHeapProfiling
        , test_oddball_RequestHeapProfile
        , test_oddball_AfterJunk
        , test_oddball_AlignedJunk
        , test_oddball_MisalignedJunk
        , test_customCommand
        ]

{- |
Test that @fibber 35@ produces a parseable eventlog.
-}
test_fibber :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_fibber =
    testCaseFor "test_fibber" $ \eventlogSocket -> do
        let fibber = Program "fibber" ["35"] ["-l-au"] eventlogSocket
        withProgram fibber $
            assertEventlogWith eventlogSocket $
                -- Validate that the Finished marker is seen.
                hasMatchingUserMarker ("Finished" `T.isPrefixOf`)

{- |
Test that @fibber-c-main 35@ produces a parseable eventlog.

__Note:__ This test does not support TCP sockets.
-}
test_fibberCMain :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_fibberCMain =
    testCaseForUnix "test_fibberCMain" $ \eventlogSocket -> do
        let fibber = Program "fibber-c-main" ["35"] ["-l-au"] eventlogSocket
        withProgram fibber $
            assertEventlogWith eventlogSocket $
                -- Validate that the Finished marker is seen.
                hasMatchingUserMarker ("Finished" `T.isPrefixOf`)

{- |
Test that @oddball@ produces heap profile samples.
-}
test_oddball :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball =
    testCaseFor "test_oddball" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l-au", "-hT", "-A256K", "-i0", "--eventlog-flush-interval=1"] eventlogSocket
        withProgram oddball $
            assertEventlogWith eventlogSocket $
                -- Validate that the Summing marker is seen, and that the event
                -- stream contains at least one heap profile sample.
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasHeapProfSampleString

{- |
Test that @--no-automatic-heap-samples@ is respected.
-}
test_oddball_NoAutomaticHeapSamples :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_NoAutomaticHeapSamples =
    testCaseFor "test_oddball_NoAutomaticHeapSamples" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l-au", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"] eventlogSocket
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
test_oddball_Reconnect :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_Reconnect =
    testCaseFor "test_oddball_Reconnect" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l-au", "-hT", "-A256K", "-i0", "--eventlog-flush-interval=1"] eventlogSocket
        withProgram oddball $ do
            -- Validate that the event stream contains at least one heap
            -- profile sample twice, connecting to the socket each time.
            assertEventlogWith eventlogSocket $ hasHeapProfSampleString
            assertEventlogWith eventlogSocket $ hasHeapProfSampleString

{- |
Test that the `StartHeapProfiling` and `StopHeapProfiling` commands are
respected, i.e., that once the `StartHeapProfiling` command is sent, heap
profile samples are received, and once the `StopHeapProfiling` command is
sent, after some iterations, no more heap profile samples are received.
-}
test_oddball_StartAndStopHeapProfiling :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_StartAndStopHeapProfiling =
    testCaseFor "test_oddball_StartAndStopHeapProfiling" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l-au", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"] eventlogSocket
        withProgram oddball $
            assertEventlogWith' eventlogSocket $ \handle ->
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommand handle StartHeapProfiling
                    !> (2 `times` (hasHeapProfSampleString &> hasHeapProfSampleEnd))
                    &> sendCommand handle StopHeapProfiling
                    !> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)

{- |
Test that the `RequestHeapProfile` signal is respected, i.e., that once the
`RequestHeapProfile` command is sent, a heap profile is received.
-}
test_oddball_RequestHeapProfile :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_RequestHeapProfile =
    testCaseFor "test_oddball_RequestHeapProfile" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l-au", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"] eventlogSocket
        withProgram oddball $
            assertEventlogWith' eventlogSocket $ \handle ->
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommand handle RequestHeapProfile
                    -- validate that there is exactly one heap sample, and that
                    -- afterwards there are no further samples, either in this
                    -- or the next iteration.
                    !> hasHeapProfSampleString
                    &> hasHeapProfSampleEnd -- TODO: This needs to be delimited.
                    &> hasNoHeapProfSampleString
                    ~> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))

{- |
Test that the `RequestHeapProfile` command is still respected after junk has
been sent over the control socket.
-}
test_oddball_AfterJunk :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_AfterJunk =
    testCaseFor "test_oddball_AfterJunk" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l-au", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"] eventlogSocket
        withProgram oddball $
            assertEventlogWith' eventlogSocket $ \handle ->
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendJunk handle "JUNK!!!!"
                    !> hasNoHeapProfSampleString
                    -- Validate that there are no heap profile samples in the
                    -- remainder of this iteration AND the next iteration.
                    ~> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))
                    &> sendCommand handle RequestHeapProfile
                    !> hasHeapProfSampleString
                    &> hasHeapProfSampleEnd -- TODO: This needs to be delimited.
                    &> hasNoHeapProfSampleString
                    ~> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))

{- |
Test that the `RequestHeapProfile` command is still respected after junk has
been sent over the control socket.

TODO: This test confirms a misfeature, which should be fixed.
-}
test_oddball_AlignedJunk :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_AlignedJunk =
    testCaseFor "test_oddball_AlignedJunk" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l-au", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"] eventlogSocket
        withProgram oddball $
            assertEventlogWith' eventlogSocket $ \handle ->
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommandWithJunk handle "FLORPIES" RequestHeapProfile "BLEHBLES"
                    !> hasHeapProfSampleString
                    &> hasHeapProfSampleEnd -- TODO: This needs to be delimited.
                    &> hasNoHeapProfSampleString
                    ~> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))

{- |
Test that the `RequestHeapProfile` command is still respected after junk has
been sent over the control socket.

TODO: This test confirms a misfeature, which should be fixed.
-}
test_oddball_MisalignedJunk :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_MisalignedJunk =
    testCaseFor "test_oddball_MisalignedJunk" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l-au", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"] eventlogSocket
        withProgram oddball $
            assertEventlogWith' eventlogSocket $ \handle ->
                hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> hasNoHeapProfSampleString
                    ~> hasMatchingUserMarker ("Summing" `T.isPrefixOf`)
                    &> sendCommandWithJunk handle "FLORP" RequestHeapProfile "BLERP"
                    !> hasNoHeapProfSampleString
                    ~> (2 `times` hasMatchingUserMarker ("Summing" `T.isPrefixOf`))

{- |
Test that custom commands are respected.
-}
test_customCommand :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_customCommand =
    testCaseFor "test_customCommand" $ \eventlogSocket -> do
        let customCommand = Program "custom-command" ["--forever"] ["-l-au", "--eventlog-flush-interval=1"] eventlogSocket
        withProgram customCommand $
            assertEventlogWith' eventlogSocket $ \handle ->
                hasMatchingUserMarker ("custom workload iteration " `T.isPrefixOf`)
                    &> sendCommand handle (userCommand (userNamespace 1) (CommandId 0))
                    !> hasMatchingUserMessage ("custom command handled" ==)
                    &> (2 `times` hasMatchingUserMarker ("custom workload iteration " `T.isPrefixOf`))
