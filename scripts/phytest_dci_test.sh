#!/bin/bash
# Known-configuration test with DCI 0_1/1_1: gNB in phy-test mode (rfsim, T-tracer, no RA, fixed
# DL/UL allocation every period) + nr-gnb-dci-logger + nr-uesoftmodem + nr-sniffer, the UE and the
# sniffer both loading the reconfig.raw/rbconfig.raw the gNB writes. compare_dci.py --rnti matches
# the sniffer's DCIs with the gNB ground truth.
#
# Usage: scripts/phytest_dci_test.sh [OAI_DIR]
# Env:   DURATION seconds to record after the UE is up (default 15)
#        OUT_DIR  where logs and CSVs go (default: ./phytest-dci-test-out)
set -uo pipefail

OUT_DIR_DEFAULT="$PWD/phytest-dci-test-out"
# the phy-test UE takes its configuration from reconfig.raw; with nrue.uicc.conf it hangs before connecting
UE_CONF="${UE_CONF-}"
source "$(dirname "${BASH_SOURCE[0]}")/rfsim_common.sh" "$@"
DURATION="${DURATION:-15}"
SNIFF_DIR="$OUT_DIR/sniffer"
rm -rf "$SNIFF_DIR" "$OUT_DIR"/gnb_dci.csv "$OUT_DIR"/r*config.raw && mkdir -p "$SNIFF_DIR"

fail() { echo "FAIL: $*"; echo "RESULT: FAIL"; exit 1; }
RAW=(--phy-test --reconfig-file "$OUT_DIR/reconfig.raw" --rbconfig-file "$OUT_DIR/rbconfig.raw")

start_gnb_dci_logger
start_gnb --phy-test --T_stdout 2
wait_for 60 "connected to" "$OUT_DIR/gnb_dci_logger.log" "$GNB_PID" || fail "nr-gnb-dci-logger did not connect to the gNB"
wait_for 30 "Running as server" "$OUT_DIR/gnb.log" "$GNB_PID" || fail "the gNB did not start its rfsim server"
[ -s "$OUT_DIR/reconfig.raw" ] && [ -s "$OUT_DIR/rbconfig.raw" ] || fail "the phy-test gNB wrote no reconfig.raw/rbconfig.raw"

start_sniffer "${RAW[@]}" --sniffer.out_dir "$SNIFF_DIR"
wait_for 60 "Found cell" "$OUT_DIR/sniffer.log" "$SNIFF_PID" || fail "nr-sniffer did not find the cell"
start_ue "${RAW[@]}"
wait_for 60 "Client connects" "$OUT_DIR/gnb.log" "$UE_PID" || fail "nr-uesoftmodem did not connect to the gNB, see $OUT_DIR/ue.log"
sleep 5
kill -0 "$UE_PID" 2>/dev/null || fail "nr-uesoftmodem exited, see $OUT_DIR/ue.log"
echo "Recording ${DURATION}s of DCIs"
sleep "$DURATION"
kill -0 "$SNIFF_PID" 2>/dev/null || echo "nr-sniffer exited early, see $OUT_DIR/sniffer.log"
cleanup
STARTED_PIDS=()

# phy-test schedules one preconfigured UE; its C-RNTI is the only C-RNTI in the ground truth
RNTI=$(awk -F, 'NR > 1 && $5 == "c" { print $4 }' "$OUT_DIR/gnb_dci.csv" | sort | uniq -c | sort -rn | awk 'NR == 1 { print $2 }')
[ -n "$RNTI" ] || fail "no C-RNTI DCI in $OUT_DIR/gnb_dci.csv"
echo "phy-test UE C-RNTI $RNTI; DCI formats (gNB):" $(awk -F, -v r="$RNTI" '$4 == r { print $6 }' "$OUT_DIR/gnb_dci.csv" | sort | uniq -c)
# the UE really takes part: most DL transport blocks are acknowledged in the first round (no DTX loop)
read -r first retx < <(sed 's/\x1b\[[0-9;]*m//g' "$OUT_DIR/gnb.log" | grep -oE "dlsch_rounds [0-9]+/[0-9]+" | tail -1 \
                        | awk -F'[ /]' '{ print $2, $3 }')
if [ -z "${first:-}" ] || [ "$retx" -gt $((first / 4)) ]; then
  echo "FAIL phy-test UE: gNB DL rounds ${first:-?} first / ${retx:-?} second; the UE is not acknowledging"
  echo "RESULT: FAIL"; exit 1
fi
echo "PASS phy-test UE: gNB DL rounds $first first / $retx retransmitted (the UE acknowledges)"
python3 "$REPO_DIR/scripts/compare_dci.py" "$OUT_DIR/gnb.log" "$OUT_DIR/gnb_dci.csv" "$SNIFF_DIR" --rnti "$RNTI"
