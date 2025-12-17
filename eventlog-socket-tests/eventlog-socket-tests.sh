#!/bin/sh -e

# Get the script directory
DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)

# Create the temporary directory
TMPDIR=$(mktemp -d) || exit
trap 'rm -rf "$TMPDIR"' EXIT INT TERM HUP

# Create the screen pipe for eventlog-socket-tests
EVENTLOG_SOCKET_TESTS_FIFO="$TMPDIR/eventlog-socket-tests.fifo"
mkfifo "$EVENTLOG_SOCKET_TESTS_FIFO" || exit

# Store arguments in variable
EVENTLOG_SOCKET_TESTS_COMMAND="
cabal test eventlog-socket-tests -f+debug $@
"

# Create the screen conf file
SCREEN_CONF="$TMPDIR/screen.conf"
cat > "$SCREEN_CONF" << 'EOF' || exit
split -v
focus right
screen -t 'stderr' sh -c 'tty > "$EVENTLOG_SOCKET_TESTS_FIFO"; read done < "$EVENTLOG_SOCKET_TESTS_FIFO"'
focus left
screen -t 'stdout' sh -c 'trap "screen -X quit" INT; read tty < "$EVENTLOG_SOCKET_TESTS_FIFO"; eval "$EVENTLOG_SOCKET_TESTS_COMMAND" 2> "$tty"; echo "[Control exited with status $?, press enter to exit]"; read prompt; echo done > "$EVENTLOG_SOCKET_TESTS_FIFO"'
EOF

# Start screen
# shellcheck disable=SC2090
export EVENTLOG_SOCKET_TESTS_FIFO
export EVENTLOG_SOCKET_TESTS_COMMAND
screen -mc "$SCREEN_CONF"
