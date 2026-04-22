#!/bin/sh
# run_tests.sh — execute the test binary and fail on non-zero exit.
# Invoked by `make test`. See TECH_STACK.md §6.1.
#
# The test binary is built into build/tests/run_tests by the Makefile.
# Resolve its path relative to the repository root so this script works
# whether invoked from the repo root or from the tests/ directory.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TESTS_BIN="$REPO_ROOT/build/tests/run_tests"

if [ ! -x "$TESTS_BIN" ]; then
	echo "run_tests.sh: test binary not found: $TESTS_BIN" >&2
	echo "run_tests.sh: build it first with 'make test' (or 'make dev')." >&2
	exit 1
fi

exec "$TESTS_BIN"

