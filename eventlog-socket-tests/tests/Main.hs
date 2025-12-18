{-# LANGUAGE OverloadedStrings #-}

module Main (main) where

import Data.Functor ((<&>))
import Data.Maybe (catMaybes, fromMaybe)
import qualified Data.Text as T
import GHC.Eventlog.Socket.Control (Command (..))
import System.Environment (lookupEnv)
import System.FilePath ((</>))
import System.IO.Temp (withTempDirectory)
import Test.Common
import Test.Tasty (DependencyType (AllFinish), TestTree, defaultMain, sequentialTestGroup, testGroup)
import Text.Read (readMaybe)

main :: IO ()
main = do
    -- Allow the user to overwrite the TCP port:
    tcpPort <- (fromMaybe 4242 . (readMaybe =<<)) <$> lookupEnv "GHC_EVENTLOG_TCP_PORT"

    -- Create:
    withLogger $ do
        -- Create temporary directory:
        withTempDirectory "/tmp" "eventlog-socket" $ \tmpDir -> do
            -- Log inputs:
            debug . Info $ "Testing with temporary directory " <> tmpDir

            defaultMain . testGroup "Tests" $
                [ sequentialTestGroup "Unix" AllFinish . catMaybes $
                    tests <&> \test ->
                        test $ EventlogUnixSocket $ tmpDir </> "ghc_eventlog.sock"
                , sequentialTestGroup "Tcp" AllFinish . catMaybes $
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
        , test_oddball_JunkCommand
        ]

{- |
Test that @fibber 35@ produces a parseable eventlog.
-}
test_fibber :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_fibber =
    testCaseFor "test_fibber" $ \eventlogSocket -> do
        let fibber = Program "fibber" ["35"] ["-l"] eventlogSocket
        ProgramHandle{..} <- start fibber
        assertEventlogWith eventlogSocket $ hasMatchingUserMarker ("Finished" `T.isPrefixOf`)
        wait

{- |
Test that @fibber-c-main 35@ produces a parseable eventlog.

__Note:__ This test does not support TCP sockets.
-}
test_fibberCMain :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_fibberCMain =
    testCaseForUnix "test_fibberCMain" $ \eventlogSocket -> do
        let fibber = Program "fibber-c-main" ["35"] ["-l"] eventlogSocket
        ProgramHandle{..} <- start fibber
        assertEventlogWith eventlogSocket $ hasMatchingUserMarker ("Finished" `T.isPrefixOf`)
        wait

{- |
Test that @oddball@ produces heap profile samples.
-}
test_oddball :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball =
    testCaseFor "test_oddball" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "-i0", "--eventlog-flush-interval=1"] eventlogSocket
        ProgramHandle{..} <- start oddball
        assertEventlogWith eventlogSocket $ hasHeapProfSampleStringWithin 50_000
        kill

{- |
Test that @--no-automatic-heap-samples@ is respected.
-}
test_oddball_NoAutomaticHeapSamples :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_NoAutomaticHeapSamples =
    testCaseForUnix "test_oddball_NoAutomaticHeapSamples" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "--no-automatic-heap-samples"] eventlogSocket
        ProgramHandle{..} <- start oddball
        assertEventlogWith eventlogSocket $ hasNoHeapProfSampleStringWithin 50_000
        kill

{- |
Test that @oddball@ produces heap profile samples even after reconnecting.
-}
test_oddball_Reconnect :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_Reconnect =
    testCaseFor "test_oddball_Reconnect" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "-i0", "--eventlog-flush-interval=1"] eventlogSocket
        ProgramHandle{..} <- start oddball
        assertEventlogWith eventlogSocket $ hasHeapProfSampleStringWithin 50_000
        assertEventlogWith eventlogSocket $ hasHeapProfSampleStringWithin 50_000
        kill

{- |
Test that the `StartHeapProfiling` and `StopHeapProfiling` signals are
respected, i.e., that once the `StartHeapProfiling` control signal is sent,
multiple heap profiles are received, and that once the `StopHeapProfiling`
control signal is sent, no further heap profiles are received.
-}
test_oddball_StartAndStopHeapProfiling :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_StartAndStopHeapProfiling =
    testCaseFor "test_oddball_StartAndStopHeapProfiling" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"] eventlogSocket
        ProgramHandle{..} <- start oddball
        assertEventlogWith' eventlogSocket $ \handle ->
            hasNoHeapProfSampleStringWithin 50_000
                &> sendCommand handle StartHeapProfiling
                !> hasHeapProfSampleStringWithin 500_000
        kill

{- |
Test that the eventlog validation in `test_oddball_StartAndStopHeapProfiling`
still works if junk control signals are sent beforehand.
-}
test_oddball_JunkCommand :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_JunkCommand =
    testCaseFor "test_oddball_JunkCommand" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"] eventlogSocket
        ProgramHandle{..} <- start oddball
        assertEventlogWith' eventlogSocket $ \handle ->
            hasNoHeapProfSampleStringWithin 50_000
                &> sendJunk handle "JUNK!!!!"
                !> hasNoHeapProfSampleStringWithin 50_000
                &> sendCommand handle StartHeapProfiling
                !> hasHeapProfSampleStringWithin 500_000
        kill

{- |
Test that the `RequestHeapProfile` signal is respected, i.e., that once the
`RequestHeapProfile` control signal is sent, a heap profile is received.
-}
test_oddball_RequestHeapProfile :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_RequestHeapProfile =
    testCaseFor "test_oddball_RequestHeapProfile" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"] eventlogSocket
        ProgramHandle{..} <- start oddball
        assertEventlogWith' eventlogSocket $ \handle ->
            hasNoHeapProfSampleStringWithin 50_000
                &> sendCommand handle RequestHeapProfile
                !> hasHeapProfSampleStringWithin 500_000
        kill
