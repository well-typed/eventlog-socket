#!/bin/sh
set -eu

if [ $# -lt 1 ]; then
    echo "Usage: $0 <cabal-executable> [program-args...]" >&2
    exit 1
fi

TARGET="$1"
shift

SOCKET_PATH="${EVENTLOG_SOCKET_PATH:-/tmp/${TARGET}_eventlog.sock}"
EVENTLOG_PATH="${EVENTLOG_PATH:-/tmp/${TARGET}.eventlog}"
APP_STDOUT="${APP_STDOUT:-/tmp/${TARGET}.stdout}"

APP_PID=""
NC_PID=""

# Support both the Haskell and C examples by exporting both env vars.
export FIBBER_EVENTLOG_SOCKET="$SOCKET_PATH"
export GHC_EVENTLOG_SOCKET="$SOCKET_PATH"

log() {
    printf '[test-normal] %s\n' "$*"
}

cleanup() {
  if [ -n "${APP_PID:-}" ] && kill -0 "$APP_PID" 2>/dev/null; then
    log "Terminating $TARGET (pid=$APP_PID)"
    kill "$APP_PID" 2>/dev/null || true
    wait "$APP_PID" 2>/dev/null || true
  fi
  if [ -n "${NC_PID:-}" ] && kill -0 "$NC_PID" 2>/dev/null; then
    log "Stopping netcat (pid=$NC_PID)"
    kill "$NC_PID" 2>/dev/null || true
    wait "$NC_PID" 2>/dev/null || true
  fi
  rm -f "$SOCKET_PATH"
}

trap cleanup EXIT

wait_for_socket() {
    log "Waiting for socket $SOCKET_PATH"
    for _ in $(seq 1 40); do
        if [ -S "$SOCKET_PATH" ]; then
            log "Socket ready."
            return 0
        fi
        sleep 0.25
    done
    log "Timed out waiting for $SOCKET_PATH"
    return 1
}

log "Building $TARGET..."
cabal build "$TARGET"

rm -f "$SOCKET_PATH" "$EVENTLOG_PATH" "$APP_STDOUT"

log "Launching $TARGET..."
cabal run "$TARGET" -- "$@" >"$APP_STDOUT" 2>&1 &
APP_PID=$!
log "$TARGET is running with pid $APP_PID (stdout -> $APP_STDOUT)"

wait_for_socket

log "Capturing eventlog to $EVENTLOG_PATH..."
nc -U "$SOCKET_PATH" >"$EVENTLOG_PATH" &
NC_PID=$!
log "netcat is running with pid $NC_PID"

APP_STATUS=0
if ! wait "$APP_PID"; then
    APP_STATUS=$?
    log "$TARGET exited with status $APP_STATUS"
fi

if ! wait "$NC_PID"; then
    log "netcat exited with non-zero status"
fi

if [ $APP_STATUS -ne 0 ]; then
    if [ -f "$APP_STDOUT" ]; then
        log "--- application output ---"
        cat "$APP_STDOUT"
        log "--- end application output ---"
    fi
    exit $APP_STATUS
fi

test -s "$EVENTLOG_PATH"
log "Eventlog captured at $EVENTLOG_PATH; running ghc-events..."
ghc-events show "$EVENTLOG_PATH"

log "Test completed successfully."
