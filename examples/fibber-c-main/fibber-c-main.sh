#!/bin/sh -e

# Set the eventlog socket
export FIBBER_EVENTLOG_SOCKET="/tmp/fibber_eventlog.sock"
FIBBER_EVENTLOG="/tmp/fibber.eventlog"

# Build fibber
cabal build fibber

# Run fibber
cabal run fibber -- 24 &
FIBBER_PID=$!

# Run NetCat
# NOTE: The purpose of 'sleep 5' is to give the fibber process
#       sufficient time to create the Unix socket.
sleep 5 && nc -U "$FIBBER_EVENTLOG_SOCKET" >"$FIBBER_EVENTLOG"

# Wait for fibber to finish
wait $FIBBER_PID

# Run ghc-events show
ghc-events show "$FIBBER_EVENTLOG"
