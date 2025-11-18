#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTS_DIR="$ROOT_DIR/tests"

run_step() {
    local description="$1"
    shift

    printf '\n==> %s\n' "$description"
    if "$@"; then
        printf '<== %s [OK]\n' "$description"
    else
        local status=$?
        printf '<== %s [FAILED]\n' "$description"
        return "$status"
    fi
}

run_step "Test fibber (finite)" "$TESTS_DIR/test-normal.sh" fibber 30
run_step "Test fibber (forever, with reconnect)" "$TESTS_DIR/test-reconnect.sh" fibber --forever 24
run_step "Test fibber-c-main (finite)" "$TESTS_DIR/test-normal.sh" fibber-c-main 30
run_step "Test fibber-c-main (forever, with reconnect)" "$TESTS_DIR/test-reconnect.sh" fibber-c-main --forever 24

printf '\nAll requested steps completed.\n'
