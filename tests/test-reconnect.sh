#!/bin/sh
set -eu

print_usage() {
    cat >&2 <<-EOF
Usage: $0 <socket-type: unix|tcp> <cabal-executable> [program-args...]
Run the reconnect test against the specified cabal executable.
EOF
}

if [ $# -lt 2 ]; then
    print_usage
    exit 1
fi

SOCKET_TYPE="$1"
TARGET="$2"
shift 2

SOCKET_PATH=""
TCP_HOST="${EVENTLOG_TCP_HOST:-127.0.0.1}"
TCP_PORT="${EVENTLOG_TCP_PORT:-4242}"
FIRST_EVENTLOG="${FIRST_EVENTLOG:-/tmp/${TARGET}_first.eventlog}"
SECOND_EVENTLOG="${SECOND_EVENTLOG:-/tmp/${TARGET}_second.eventlog}"
APP_STDOUT="${APP_STDOUT:-/tmp/${TARGET}_reconnect.stdout}"
CAPTURE_DURATION="${RECONNECT_CAPTURE_DURATION:-2}"

init_socket_env() {
    case "$SOCKET_TYPE" in
        unix)
            SOCKET_PATH="${EVENTLOG_SOCKET_PATH:-/tmp/${TARGET}_reconnect.sock}"
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

APP_PID=""
ACTIVE_NC_PID=""

log() {
    printf '[test-reconnect] %s\n' "$*"
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
    if [ -n "${ACTIVE_NC_PID:-}" ] && kill -0 "$ACTIVE_NC_PID" 2>/dev/null; then
        log "Stopping netcat (pid=$ACTIVE_NC_PID)"
        kill "$ACTIVE_NC_PID" 2>/dev/null || true
        wait "$ACTIVE_NC_PID" 2>/dev/null || true
    fi
    if [ "$SOCKET_TYPE" = "unix" ] && [ -n "$SOCKET_PATH" ]; then
        rm -f "$SOCKET_PATH"
    fi
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

capture_eventlog() {
    output_file="$1"
    duration="$2"
    rm -f "$output_file"
    log "Capturing $output_file for ${duration}s..."
    if [ "$SOCKET_TYPE" = "unix" ]; then
        nc -U "$SOCKET_PATH" >"$output_file" &
    else
        nc "$TCP_HOST" "$TCP_PORT" >"$output_file" &
    fi
    ACTIVE_NC_PID=$!
    sleep "$duration"
    kill "$ACTIVE_NC_PID" 2>/dev/null || true
    wait "$ACTIVE_NC_PID" 2>/dev/null || true
    ACTIVE_NC_PID=""
    test -s "$output_file"
    log "Finished capture: $output_file ($(du -h "$output_file" | cut -f1))"
}

report_exit() {
    status=$?
    if [ $status -ne 0 ]; then
        log "FAILED with exit code $status"
        if [ -f "$APP_STDOUT" ]; then
            log "--- fibber stdout/stderr ---"
            cat "$APP_STDOUT"
            log "--- end fibber stdout/stderr ---"
        fi
        if [ -f "$FIRST_EVENTLOG" ]; then
            log "First capture size: $(du -h "$FIRST_EVENTLOG" | cut -f1)"
        fi
        if [ -f "$SECOND_EVENTLOG" ]; then
            log "Second capture size: $(du -h "$SECOND_EVENTLOG" | cut -f1)"
        fi
    else
        log "Completed successfully."
    fi
    cleanup
    exit $status
}

init_socket_env

trap report_exit EXIT

log "Building $TARGET..."
cabal build "$TARGET"

if [ "$SOCKET_TYPE" = "unix" ] && [ -n "$SOCKET_PATH" ]; then
    rm -f "$SOCKET_PATH"
fi
rm -f "$FIRST_EVENTLOG" "$SECOND_EVENTLOG" "$APP_STDOUT"

log "Launching $TARGET..."
cabal run "$TARGET" -- "$@" +RTS --eventlog-flush-interval=1 -RTS >"$APP_STDOUT" 2>&1 &
APP_PID=$!
log "$TARGET is running with pid $APP_PID (stdout -> $APP_STDOUT)"

wait_for_socket

capture_eventlog "$FIRST_EVENTLOG" "$CAPTURE_DURATION"

if ! kill -0 "$APP_PID" 2>/dev/null; then
    log "$TARGET exited unexpectedly; see $APP_STDOUT for details."
    exit 1
fi

sleep 1

capture_eventlog "$SECOND_EVENTLOG" "$CAPTURE_DURATION"

log "Validating captured eventlogs..."
summarize_eventlog "$FIRST_EVENTLOG"
summarize_eventlog "$SECOND_EVENTLOG"

log "Reconnect test completed successfully."
