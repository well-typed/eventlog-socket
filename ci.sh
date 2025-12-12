#!/bin/sh

ROOT_DIR="$(CDPATH= cd -- "$(dirname "$(dirname -- "$0")")" && pwd)"

(cd "${ROOT_DIR}/eventlog-socket" && python3 -m tests.run_tests "$@")
