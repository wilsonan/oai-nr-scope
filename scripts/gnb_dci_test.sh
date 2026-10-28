#!/bin/bash
# Component B test: run an OAI gNB (rfsim, T-tracer on) with nr-gnb-dci-logger and one
# nr-uesoftmodem, and check the logged DCIs against what the gNB itself reports.
#
# Usage: scripts/gnb_dci_test.sh [OAI_DIR]
# Env:   DURATION seconds to record after the UE is connected (default 20)
#        OUT_DIR  where logs and CSVs go (default: ./gnb-dci-test-out)
set -uo pipefail

OUT_DIR_DEFAULT="$PWD/gnb-dci-test-out"
source "$(dirname "${BASH_SOURCE[0]}")/rfsim_common.sh" "$@"
DURATION="${DURATION:-20}"
rm -f "$OUT_DIR/gnb_dci.csv"

# The logger retries until the gNB's T-tracer port is up; the gNB waits for it before starting.
start_gnb_dci_logger
start_gnb --T_stdout 2
if ! wait_for 60 "connected to" "$OUT_DIR/gnb_dci_logger.log" "$GNB_PID"; then
  echo "FAIL: nr-gnb-dci-logger did not connect to the gNB, see $OUT_DIR/gnb_dci_logger.log"
  echo "RESULT: FAIL"; exit 1
fi
wait_for 30 "Running as server" "$OUT_DIR/gnb.log" "$GNB_PID"
start_ue
echo "Waiting up to 60s for the UE to connect..."
if ! wait_for 60 "RRC_CONNECTED reached" "$OUT_DIR/gnb.log" "$GNB_PID"; then
  echo "FAIL: the UE did not reach RRC_CONNECTED, see $OUT_DIR/gnb.log and $OUT_DIR/ue.log"
  echo "RESULT: FAIL"; exit 1
fi
echo "UE connected; recording ${DURATION}s of DCIs"
sleep "$DURATION"
cleanup
STARTED_PIDS=()

python3 "$REPO_DIR/scripts/check_gnb_dci.py" "$OUT_DIR/gnb.log" "$OUT_DIR/gnb_dci.csv"
