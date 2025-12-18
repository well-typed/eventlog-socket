{-# LANGUAGE OverloadedStrings #-}

module Main (main) where

import Data.Functor ((<&>))
import Data.Machine (dropping, (~>))
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
        , test_oddball_StartHeapProfiling
        , test_oddball_StopHeapProfiling
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
        withProgram fibber $
            assertEventlogWith eventlogSocket $
                hasMatchingUserMarker ("Finished" `T.isPrefixOf`)

{- |
Test that @fibber-c-main 35@ produces a parseable eventlog.

__Note:__ This test does not support TCP sockets.
-}
test_fibberCMain :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_fibberCMain =
    testCaseForUnix "test_fibberCMain" $ \eventlogSocket -> do
        let fibber = Program "fibber-c-main" ["35"] ["-l"] eventlogSocket
        withProgram fibber $
            assertEventlogWith eventlogSocket $
                hasMatchingUserMarker ("Finished" `T.isPrefixOf`)

{- |
Test that @oddball@ produces heap profile samples.
-}
test_oddball :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball =
    testCaseFor "test_oddball" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "-i0", "--eventlog-flush-interval=1"] eventlogSocket
        withProgram oddball $
            assertEventlogWith eventlogSocket $
                hasHeapProfSampleStringWithin 50_000

{- |
Test that @--no-automatic-heap-samples@ is respected.
-}
test_oddball_NoAutomaticHeapSamples :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_NoAutomaticHeapSamples =
    testCaseFor "test_oddball_NoAutomaticHeapSamples" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "--no-automatic-heap-samples"] eventlogSocket
        withProgram oddball $
            assertEventlogWith eventlogSocket $
                hasNoHeapProfSampleStringWithin 50_000

{- |
Test that @oddball@ produces heap profile samples even after reconnecting.
-}
test_oddball_Reconnect :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_Reconnect =
    testCaseFor "test_oddball_Reconnect" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "-i0", "--eventlog-flush-interval=1"] eventlogSocket
        withProgram oddball $ do
            assertEventlogWith eventlogSocket $ hasHeapProfSampleStringWithin 50_000
            assertEventlogWith eventlogSocket $ hasHeapProfSampleStringWithin 50_000

{- |
Test that the `StartHeapProfiling` command are respected, i.e., that once the
`StartHeapProfiling` command is sent, a heap profile samples is received.
-}
test_oddball_StartHeapProfiling :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_StartHeapProfiling =
    testCaseFor "test_oddball_StartHeapProfiling" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"] eventlogSocket
        withProgram oddball $
            assertEventlogWith' eventlogSocket $ \handle ->
                hasNoHeapProfSampleStringWithin 50_000
                    &> sendCommand handle StartHeapProfiling
                    !> hasHeapProfSampleStringWithin 500_000
                    &> hasHeapProfSampleEndWithin 500_000
                    &> hasHeapProfSampleStringWithin 500_000

{- |
Test that the `StopHeapProfiling` command are respected, i.e., that once the
`StopHeapProfiling` command is sent, a heap profile samples is received.
-}
test_oddball_StopHeapProfiling :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_StopHeapProfiling =
    testCaseFor "test_oddball_StopHeapProfiling" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "--eventlog-flush-interval=1"] eventlogSocket
        withProgram oddball $
            assertEventlogWith' eventlogSocket $ \handle ->
                hasHeapProfSampleStringWithin 500_000
                    &> sendCommand handle StopHeapProfiling
                    !> dropping 100_000
                    ~> hasNoHeapProfSampleStringWithin 50_000

{- |
Test that the `RequestHeapProfile` signal is respected, i.e., that once the
`RequestHeapProfile` command is sent, a heap profile is received.
-}
test_oddball_RequestHeapProfile :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_RequestHeapProfile =
    testCaseFor "test_oddball_RequestHeapProfile" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"] eventlogSocket
        withProgram oddball $
            assertEventlogWith' eventlogSocket $ \handle ->
                hasNoHeapProfSampleStringWithin 50_000
                    &> sendCommand handle RequestHeapProfile
                    !> hasHeapProfSampleStringWithin 500_000
                    &> hasHeapProfSampleEndWithin 500_000
                    &> hasNoHeapProfSampleStringWithin 50_000

{- |
Test that the eventlog validation in `test_oddball_StartHeapProfiling`
still works if junk commands are sent beforehand.
-}
test_oddball_JunkCommand :: (HasLogger) => EventlogSocket -> Maybe TestTree
test_oddball_JunkCommand =
    testCaseFor "test_oddball_JunkCommand" $ \eventlogSocket -> do
        let oddball = Program "oddball" [] ["-l", "-hT", "-A256K", "--eventlog-flush-interval=1", "--no-automatic-heap-samples"] eventlogSocket
        withProgram oddball $
            assertEventlogWith' eventlogSocket $ \handle ->
                hasNoHeapProfSampleStringWithin 50_000
                    &> sendJunk handle "JUNK!!!!"
                    !> hasNoHeapProfSampleStringWithin 50_000
                    &> sendCommand handle RequestHeapProfile
                    !> hasHeapProfSampleStringWithin 500_000
                    &> hasHeapProfSampleEndWithin 500_000
                    &> hasNoHeapProfSampleStringWithin 50_000
