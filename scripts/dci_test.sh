#!/bin/bash
# Milestone 2 test: gNB (rfsim, T-tracer) + nr-gnb-dci-logger + nr-sniffer + one nr-uesoftmodem.
# The sniffer must follow the UE through random access (RAR -> TC-RNTI -> Msg4/RRCSetup) and log
# its DCIs; scripts/compare_dci.py matches them against the gNB ground truth.
#
# Usage: scripts/dci_test.sh [OAI_DIR]
# Env:   DURATION seconds to record after the sniffer follows the UE (default 20)
#        OUT_DIR  where logs and CSVs go (default: ./dci-test-out)
set -uo pipefail

OUT_DIR_DEFAULT="$PWD/dci-test-out"
source "$(dirname "${BASH_SOURCE[0]}")/rfsim_common.sh" "$@"
DURATION="${DURATION:-20}"
SNIFF_DIR="$OUT_DIR/sniffer"
rm -rf "$SNIFF_DIR" "$OUT_DIR/gnb_dci.csv" && mkdir -p "$SNIFF_DIR"

fail() { echo "FAIL: $*"; echo "RESULT: FAIL"; exit 1; }

start_gnb_dci_logger
start_gnb --T_stdout 2
wait_for 60 "connected to" "$OUT_DIR/gnb_dci_logger.log" "$GNB_PID" || fail "nr-gnb-dci-logger did not connect to the gNB"
wait_for 30 "Running as server" "$OUT_DIR/gnb.log" "$GNB_PID" || fail "the gNB did not start its rfsim server"

start_sniffer --sniffer.out_dir "$SNIFF_DIR"
wait_for 60 "Watching random access" "$OUT_DIR/sniffer.log" "$SNIFF_PID" \
  || fail "nr-sniffer did not decode SIB1 and start watching RA, see $OUT_DIR/sniffer.log"

start_ue
echo "Waiting up to 60s for the UE to connect..."
wait_for 60 "RRC_CONNECTED reached" "$OUT_DIR/gnb.log" "$GNB_PID" || fail "the UE did not reach RRC_CONNECTED"
wait_for 10 "following UE" "$OUT_DIR/sniffer.log" "$SNIFF_PID" || echo "nr-sniffer did not report following the UE"
echo "Recording ${DURATION}s of DCIs"
sleep "$DURATION"
kill -0 "$SNIFF_PID" 2>/dev/null || echo "nr-sniffer exited early, see $OUT_DIR/sniffer.log"
cleanup
STARTED_PIDS=()

grep "\[SNIFFER\]" "$OUT_DIR/sniffer.log" | head -10
python3 "$REPO_DIR/scripts/compare_dci.py" "$OUT_DIR/gnb.log" "$OUT_DIR/gnb_dci.csv" "$SNIFF_DIR"
