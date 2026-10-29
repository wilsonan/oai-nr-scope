#!/bin/bash
# Milestone 3 test: a followed UE that leaves frees its shadow MAC instance for the next UE.
# nr-sniffer runs with --sniffer.max_ues 1 --sniffer.ue_timeout 3. UE 1 attaches and is followed;
# UE 1 is then stopped, the gNB stops scheduling it (UL failure), and 3 s later the sniffer stops
# following it. UE 2 attaches and must be followed on the same shadow MAC instance.
#
# Usage: scripts/ue_reuse_test.sh [OAI_DIR]
# Env:   DURATION seconds to record with each UE connected (default 10)
#        OUT_DIR  where logs and CSVs go (default: ./ue-reuse-test-out)
set -uo pipefail

OUT_DIR_DEFAULT="$PWD/ue-reuse-test-out"
source "$(dirname "${BASH_SOURCE[0]}")/rfsim_common.sh" "$@"
DURATION="${DURATION:-10}"
SNIFF_DIR="$OUT_DIR/sniffer"
rm -rf "$SNIFF_DIR" "$OUT_DIR"/gnb_dci.csv "$OUT_DIR"/ue*.log && mkdir -p "$SNIFF_DIR"
UE_PIDS=()

fail() { echo "FAIL: $*"; echo "RESULT: FAIL"; exit 1; }
connected() { sed 's/\x1b\[[0-9;]*m//g' "$OUT_DIR/gnb.log" | grep -c "RRC_CONNECTED reached"; }

start_gnb_dci_logger
start_gnb --T_stdout 2
wait_for 60 "connected to" "$OUT_DIR/gnb_dci_logger.log" "$GNB_PID" || fail "nr-gnb-dci-logger did not connect to the gNB"
wait_for 30 "Running as server" "$OUT_DIR/gnb.log" "$GNB_PID" || fail "the gNB did not start its rfsim server"
start_sniffer --sniffer.out_dir "$SNIFF_DIR" --sniffer.max_ues 1 --sniffer.ue_timeout 3
wait_for 60 "Watching random access" "$OUT_DIR/sniffer.log" "$SNIFF_PID" || fail "nr-sniffer did not start watching RA"

start_ue_n 1
wait_for 60 "following UE with C-RNTI" "$OUT_DIR/sniffer.log" "$SNIFF_PID" || fail "UE 1 was not followed"
sleep "$DURATION"
echo "Stopping UE 1"
stop_pid "${UE_PIDS[0]}"
wait_for 90 "no longer followed" "$OUT_DIR/sniffer.log" "$SNIFF_PID" || fail "the sniffer kept following the UE that left"

start_ue_n 2
for _ in $(seq 1 60); do [ "$(connected)" -ge 2 ] && break; sleep 1; done
[ "$(connected)" -ge 2 ] || fail "UE 2 did not reach RRC_CONNECTED"
sleep 2
sed 's/\x1b\[[0-9;]*m//g' "$OUT_DIR/sniffer.log" | grep -q "following UE with C-RNTI .*(shadow MAC 1, 1 UEs followed)" \
  && [ "$(grep -c 'following UE with C-RNTI' "$OUT_DIR/sniffer.log")" -ge 2 ] \
  || fail "UE 2 was not followed on the freed shadow MAC instance 1"
echo "PASS reuse: UE 2 is followed on shadow MAC instance 1"
sleep "$DURATION"
cleanup
STARTED_PIDS=()

grep -E "\[SNIFFER\] (Msg4|UE )" "$OUT_DIR/sniffer.log" | sed 's/\x1b\[[0-9;]*m//g'
python3 "$REPO_DIR/scripts/compare_dci.py" "$OUT_DIR/gnb.log" "$OUT_DIR/gnb_dci.csv" "$SNIFF_DIR"
