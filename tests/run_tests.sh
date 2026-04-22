#!/bin/sh
# run_tests.sh — execute the test binary and fail on non-zero exit.
# Invoked by `make test`. See TECH_STACK.md §6.1.

set -e

TESTS_BIN="$(dirname "$0")/run_tests"

if [ ! -x "$TESTS_BIN" ]; then
	echo "run_tests.sh: test binary not found: $TESTS_BIN" >&2
	exit 1
fi

exec "$TESTS_BIN"

