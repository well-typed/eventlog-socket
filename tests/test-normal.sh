#!/bin/sh
set -eu

if [ $# -lt 2 ]; then
    echo "Usage: $0 <socket-type: unix|tcp> <cabal-executable> [program-args...]" >&2
    exit 1
fi

SOCKET_TYPE="$1"
TARGET="$2"
shift 2

SOCKET_PATH=""
TCP_HOST="${EVENTLOG_TCP_HOST:-127.0.0.1}"
TCP_PORT="${EVENTLOG_TCP_PORT:-4242}"
EVENTLOG_PATH="${EVENTLOG_PATH:-/tmp/${TARGET}.eventlog}"
APP_STDOUT="${APP_STDOUT:-/tmp/${TARGET}.stdout}"

APP_PID=""
NC_PID=""

case "$SOCKET_TYPE" in
    unix)
        SOCKET_PATH="${EVENTLOG_SOCKET_PATH:-/tmp/${TARGET}_eventlog.sock}"
        export FIBBER_EVENTLOG_SOCKET="$SOCKET_PATH"
        export GHC_EVENTLOG_SOCKET="$SOCKET_PATH"
        ;;
    tcp)
        export FIBBER_EVENTLOG_TCP_HOST="$TCP_HOST"
        export FIBBER_EVENTLOG_TCP_PORT="$TCP_PORT"
        export GHC_EVENTLOG_TCP_HOST="$TCP_HOST"
        export GHC_EVENTLOG_TCP_PORT="$TCP_PORT"
        ;;
    *)
        echo "Unknown socket type: $SOCKET_TYPE (expected unix or tcp)" >&2
        exit 1
        ;;
esac

log() {
    printf '[test-normal] %s\n' "$*"
}

summarize_eventlog() {
    file="$1"
    tmp_output="$(mktemp)"
    if ghc-events show "$file" >"$tmp_output"; then
        line_count=$(wc -l <"$tmp_output")
        log "ghc-events output for $file: ${line_count} lines"
    else
        rm -f "$tmp_output"
        return 1
    fi
    rm -f "$tmp_output"
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
  if [ "$SOCKET_TYPE" = "unix" ] && [ -n "$SOCKET_PATH" ]; then
      rm -f "$SOCKET_PATH"
  fi
}

trap cleanup EXIT

wait_for_socket() {
    case "$SOCKET_TYPE" in
        unix)
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
            ;;
        tcp)
            log "Sleeping briefly to allow TCP listener on ${TCP_HOST}:${TCP_PORT} to start..."
            sleep 1
            return 0
            ;;
    esac
}

log "Building $TARGET..."
cabal build "$TARGET"

rm -f "$EVENTLOG_PATH" "$APP_STDOUT"
if [ "$SOCKET_TYPE" = "unix" ] && [ -n "$SOCKET_PATH" ]; then
    rm -f "$SOCKET_PATH"
fi

log "Launching $TARGET..."
cabal run "$TARGET" -- "$@" >"$APP_STDOUT" 2>&1 &
APP_PID=$!
log "$TARGET is running with pid $APP_PID (stdout -> $APP_STDOUT)"

wait_for_socket

log "Capturing eventlog to $EVENTLOG_PATH..."
if [ "$SOCKET_TYPE" = "unix" ]; then
    nc -U "$SOCKET_PATH" >"$EVENTLOG_PATH" &
else
    nc "$TCP_HOST" "$TCP_PORT" >"$EVENTLOG_PATH" &
fi
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
summarize_eventlog "$EVENTLOG_PATH"

log "Test completed successfully."
