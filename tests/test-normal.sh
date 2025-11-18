#!/bin/sh
set -eu

print_usage() {
    cat >&2 <<-EOF
Usage: $0 <socket-type: unix|tcp> [normal|reconnect] <cabal-executable> [program-args...]
Run the specified cabal executable and capture its eventlog via socket.
EOF
}

if [ $# -lt 2 ]; then
    print_usage
    exit 1
fi

SOCKET_TYPE="$1"
shift

MODE="normal"
case "${1-}" in
    normal|reconnect)
        MODE="$1"
        shift
        ;;
esac

if [ $# -lt 1 ]; then
    print_usage
    exit 1
fi

TARGET="$1"
shift

SOCKET_PATH=""
TCP_HOST="${EVENTLOG_TCP_HOST:-127.0.0.1}"
TCP_PORT="${EVENTLOG_TCP_PORT:-4242}"
EVENTLOG_PATH="${EVENTLOG_PATH:-/tmp/${TARGET}.eventlog}"
FIRST_EVENTLOG="${FIRST_EVENTLOG:-/tmp/${TARGET}_first.eventlog}"
SECOND_EVENTLOG="${SECOND_EVENTLOG:-/tmp/${TARGET}_second.eventlog}"
CAPTURE_DURATION="${RECONNECT_CAPTURE_DURATION:-2}"
APP_STDOUT="${APP_STDOUT:-/tmp/${TARGET}.stdout}"

APP_PID=""
NC_PID=""
APP_STATUS=0

init_socket_env() {
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
}

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

start_nc_stream() {
    output_file="$1"
    if [ "$SOCKET_TYPE" = "unix" ]; then
        nc -U "$SOCKET_PATH" >"$output_file" &
    else
        nc "$TCP_HOST" "$TCP_PORT" >"$output_file" &
    fi
    NC_PID=$!
    log "netcat is running with pid $NC_PID"
}

stop_nc_stream() {
    if [ -n "${NC_PID:-}" ] && kill -0 "$NC_PID" 2>/dev/null; then
        kill "$NC_PID" 2>/dev/null || true
        wait "$NC_PID" 2>/dev/null || true
    fi
    NC_PID=""
}

capture_for_duration() {
    output_file="$1"
    duration="$2"
    rm -f "$output_file"
    log "Capturing $output_file for ${duration}s..."
    start_nc_stream "$output_file"
    sleep "$duration"
    stop_nc_stream
    test -s "$output_file"
    log "Finished capture: $output_file ($(du -h "$output_file" | cut -f1))"
}

launch_target() {
    log "Launching $TARGET..."
    if [ "$MODE" = "reconnect" ]; then
        cabal run "$TARGET" -- "$@" +RTS --eventlog-flush-interval=1 -RTS >"$APP_STDOUT" 2>&1 &
    else
        cabal run "$TARGET" -- "$@" >"$APP_STDOUT" 2>&1 &
    fi
    APP_PID=$!
    log "$TARGET is running with pid $APP_PID (stdout -> $APP_STDOUT)"
}

run_normal_mode() {
    log "Capturing eventlog to $EVENTLOG_PATH..."
    start_nc_stream "$EVENTLOG_PATH"

    APP_STATUS=0
    if ! wait "$APP_PID"; then
        APP_STATUS=$?
        log "$TARGET exited with status $APP_STATUS"
    fi

    if ! wait "$NC_PID"; then
        log "netcat exited with non-zero status"
    fi
    NC_PID=""

    if [ $APP_STATUS -ne 0 ]; then
        if [ -f "$APP_STDOUT" ]; then
            log "--- application output ---"
            cat "$APP_STDOUT"
            log "--- end application output ---"
        fi
        exit $APP_STATUS
    fi

    test -s "$EVENTLOG_PATH"
    log "Eventlog captured at $EVENTLOG_PATH"
    summarize_eventlog "$EVENTLOG_PATH"

    log "Test completed successfully."
}

run_reconnect_mode() {
    capture_for_duration "$FIRST_EVENTLOG" "$CAPTURE_DURATION"

    if ! kill -0 "$APP_PID" 2>/dev/null; then
        log "$TARGET exited unexpectedly; see $APP_STDOUT for details."
        exit 1
    fi

    sleep 1

    capture_for_duration "$SECOND_EVENTLOG" "$CAPTURE_DURATION"

    log "Validating captured eventlogs..."
    summarize_eventlog "$FIRST_EVENTLOG"
    summarize_eventlog "$SECOND_EVENTLOG"

    log "Reconnect test completed successfully."
}

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

init_socket_env
log "Building $TARGET..."
cabal build "$TARGET"

rm -f "$APP_STDOUT"
if [ "$MODE" = "reconnect" ]; then
    rm -f "$FIRST_EVENTLOG" "$SECOND_EVENTLOG"
else
    rm -f "$EVENTLOG_PATH"
fi
if [ "$SOCKET_TYPE" = "unix" ] && [ -n "$SOCKET_PATH" ]; then
    rm -f "$SOCKET_PATH"
fi

launch_target "$@"

wait_for_socket

if [ "$MODE" = "reconnect" ]; then
    run_reconnect_mode
else
    run_normal_mode
fi
