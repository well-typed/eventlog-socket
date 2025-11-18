#!/bin/sh
set -eu

if [ $# -lt 1 ]; then
    echo "Usage: $0 <cabal-executable> [program-args...]" >&2
    exit 1
fi

TARGET="$1"
shift

SOCKET_PATH="${EVENTLOG_SOCKET_PATH:-/tmp/${TARGET}_reconnect.sock}"
FIRST_EVENTLOG="${FIRST_EVENTLOG:-/tmp/${TARGET}_first.eventlog}"
SECOND_EVENTLOG="${SECOND_EVENTLOG:-/tmp/${TARGET}_second.eventlog}"
APP_STDOUT="${APP_STDOUT:-/tmp/${TARGET}_reconnect.stdout}"
CAPTURE_DURATION="${RECONNECT_CAPTURE_DURATION:-2}"

export FIBBER_EVENTLOG_SOCKET="$SOCKET_PATH"
export GHC_EVENTLOG_SOCKET="$SOCKET_PATH"

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
    rm -f "$SOCKET_PATH"
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

trap report_exit EXIT

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

capture_eventlog() {
    output_file="$1"
    duration="$2"
    rm -f "$output_file"
    log "Capturing $output_file for ${duration}s..."
    nc -U "$SOCKET_PATH" >"$output_file" &
    ACTIVE_NC_PID=$!
    sleep "$duration"
    kill "$ACTIVE_NC_PID" 2>/dev/null || true
    wait "$ACTIVE_NC_PID" 2>/dev/null || true
    ACTIVE_NC_PID=""
    test -s "$output_file"
    log "Finished capture: $output_file ($(du -h "$output_file" | cut -f1))"
}

log "Building $TARGET..."
cabal build "$TARGET"

rm -f "$SOCKET_PATH" "$FIRST_EVENTLOG" "$SECOND_EVENTLOG" "$APP_STDOUT"

log "Launching $TARGET..."
cabal run "$TARGET" -- "$@" >"$APP_STDOUT" 2>&1 &
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
