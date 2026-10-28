#!/bin/bash
# Milestone 3 test: gNB (rfsim, T-tracer) + nr-gnb-dci-logger + nr-sniffer + NUM_UES nr-uesoftmodem
# instances attaching one after the other. The sniffer must follow every UE through RAR and Msg4,
# each in its own shadow MAC instance, and log their DCIs; compare_dci.py checks every UE against
# the gNB ground truth and the total.
#
# Usage: scripts/multi_ue_test.sh [OAI_DIR]
# Env:   NUM_UES  number of UEs (default 3)
#        DURATION seconds to record after the last UE is connected (default 20)
#        OUT_DIR  where logs and CSVs go (default: ./multi-ue-test-out)
set -uo pipefail

OUT_DIR_DEFAULT="$PWD/multi-ue-test-out"
source "$(dirname "${BASH_SOURCE[0]}")/rfsim_common.sh" "$@"
NUM_UES="${NUM_UES:-3}"
DURATION="${DURATION:-20}"
SNIFF_DIR="$OUT_DIR/sniffer"
rm -rf "$SNIFF_DIR" "$OUT_DIR"/gnb_dci.csv "$OUT_DIR"/ue*.log && mkdir -p "$SNIFF_DIR"
UE_PIDS=()

fail() { echo "FAIL: $*"; echo "RESULT: FAIL"; exit 1; }
connected() { sed 's/\x1b\[[0-9;]*m//g' "$OUT_DIR/gnb.log" | grep -c "RRC_CONNECTED reached"; }

start_gnb_dci_logger
start_gnb --T_stdout 2
wait_for 60 "connected to" "$OUT_DIR/gnb_dci_logger.log" "$GNB_PID" || fail "nr-gnb-dci-logger did not connect to the gNB"
wait_for 30 "Running as server" "$OUT_DIR/gnb.log" "$GNB_PID" || fail "the gNB did not start its rfsim server"
start_sniffer --sniffer.out_dir "$SNIFF_DIR" --sniffer.max_ues "$NUM_UES"
wait_for 60 "Watching random access" "$OUT_DIR/sniffer.log" "$SNIFF_PID" \
  || fail "nr-sniffer did not decode SIB1 and start watching RA, see $OUT_DIR/sniffer.log"

for n in $(seq 1 "$NUM_UES"); do
  start_ue_n "$n"
  echo "UE $n started, waiting for it to connect..."
  for _ in $(seq 1 60); do [ "$(connected)" -ge "$n" ] && break; sleep 1; done
  [ "$(connected)" -ge "$n" ] || fail "UE $n did not reach RRC_CONNECTED, see $OUT_DIR/ue$n.log"
  kill -0 "$SNIFF_PID" 2>/dev/null || fail "nr-sniffer exited, see $OUT_DIR/sniffer.log"
  sleep 2
done
echo "$NUM_UES UEs connected; recording ${DURATION}s of DCIs"
sleep "$DURATION"
kill -0 "$SNIFF_PID" 2>/dev/null || echo "nr-sniffer exited early, see $OUT_DIR/sniffer.log"
cleanup
STARTED_PIDS=()

grep -E "\[SNIFFER\] (RAR|Msg4)" "$OUT_DIR/sniffer.log" | head -20
python3 "$REPO_DIR/scripts/compare_dci.py" "$OUT_DIR/gnb.log" "$OUT_DIR/gnb_dci.csv" "$SNIFF_DIR"
