#!/bin/sh -e

# Usage: ./scripts/test.sh [CABAL_ARGS] -- [TEST_ARGS]
#
# NOTE: If environment variable DEBUG is defined, this script logs both the
#       output and error streams of the test suite to files and shows only
#       the error stream. Otherwise, it shows only the output stream and logs
#       only the error stream.

# Get the project root directory:
ROOT_DIR="$(CDPATH= cd -- "$(dirname "$(dirname "$(dirname -- "$0")")")" && pwd)"

# Log file for stderr.
ERR_FILE="${ROOT_DIR}/eventlog-socket-tests.err.log"

# Run test command.
if [ -n "${DEBUG+x}" ]; then
	# Log file for stdout.
	OUT_FILE="${ROOT_DIR}/eventlog-socket-tests.out.log"

	# Pipe for stderr.
	ERR_FIFO="${TMPDIR:-/tmp}/eventlog-socket-tests.err.$$"
	mkfifo "${ERR_FIFO}"
	trap 'rm "${ERR_FIFO}"' EXIT
	tee "${ERR_FILE}" <"${ERR_FIFO}" >&2 &

	# Run test suite and log debug information.
	cabal run eventlog-socket-tests --enable-tests -f+debug "$@" >"${OUT_FILE}" 2>"${ERR_FIFO}"
else
	cabal run eventlog-socket-tests --enable-tests -f+debug "$@" 2>"${ERR_FILE}"
fi
