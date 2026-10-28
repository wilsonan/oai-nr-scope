#!/bin/bash
# Unit tests: no radio, no rfsim, a few seconds. Run them after every change to nr-sniffer/ or scripts/.
#   C:      nr-sniffer-unit-tests (RA tracker, known configuration, DCI schema; nr-sniffer/tests/)
#   Python: compare_dci.py and check_gnb_dci.py on synthetic logs (scripts/tests/)
#
# Usage: scripts/unit_tests.sh [OAI_DIR]
set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OAI_DIR="${1:-$REPO_DIR/oai}"
BUILD_DIR="$OAI_DIR/cmake_targets/ran_build/build"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

failures=0
echo "== C unit tests"
if ninja -C "$BUILD_DIR" nr-sniffer-unit-tests > "$SCRATCH/build.log" 2>&1; then
  "$BUILD_DIR/nr-sniffer-unit-tests" "$REPO_DIR/nr-sniffer/tests/data" "$SCRATCH" 2>&1 \
    | grep -E "^FAIL|checks|RESULT" || true
  [ "${PIPESTATUS[0]}" -eq 0 ] || failures=$((failures + 1))
else
  tail -20 "$SCRATCH/build.log"
  echo "FAIL: nr-sniffer-unit-tests does not build (run scripts/build.sh first)"
  failures=$((failures + 1))
fi

echo "== Python unit tests"
python3 -m unittest discover -s "$REPO_DIR/scripts/tests" 2>&1 | grep -E "^(FAIL|ERROR|Ran|OK|FAILED)" || true
[ "${PIPESTATUS[0]}" -eq 0 ] || failures=$((failures + 1))

echo "RESULT: $([ "$failures" -eq 0 ] && echo PASS || echo "FAIL ($failures suites)")"
exit $((failures > 0))
