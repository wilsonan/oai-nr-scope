#!/bin/bash
# nr-sniffer demo with the OAI RF simulator, in one command (see tutorials/01-rfsim-tutorial.md).
#
# Starts an OAI gNB (rfsim, T-tracer on), nr-gnb-dci-logger (ground truth), nr-sniffer and UEs, and
# shows the sniffer's live view. Press Ctrl-C to stop: everything is shut down and the evaluation
# report (sniffer vs gNB) is written to <out>/report.md.
#
# Usage: scripts/rfsim_demo.sh [--ues N] [--phytest] [--out DIR] [OAI_DIR]
#   --ues N     number of nr-uesoftmodem UEs (default 2; SA mode)
#   --phytest   phy-test mode: no random access, one preconfigured UE with continuous DL/UL traffic
#               (DCI 1_1/0_1); the sniffer uses the same reconfig.raw as the UE
#   --out DIR   run directory (default ./rfsim-demo)
# Env: GNB_ARGS extra nr-softmodem options (e.g. phy-test MCS/PRBs: "-m 20 -M 80 -t 9 -T 50")
#      SNIFFER_ARGS extra nr-sniffer options (e.g. "--sniffer.max_ues 4")
set -uo pipefail

NUM_UES=2
PHYTEST=0
OUT=""
ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --ues) NUM_UES=$2; shift 2 ;;
    --phytest) PHYTEST=1; shift ;;
    --out) OUT=$2; shift 2 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) ARGS+=("$1"); shift ;;
  esac
done
OUT_DIR="${OUT:-$PWD/rfsim-demo}"
OUT_DIR_DEFAULT="$OUT_DIR"
[ "$PHYTEST" = 1 ] && UE_CONF="${UE_CONF-}"  # the phy-test UE takes its configuration from reconfig.raw
source "$(dirname "${BASH_SOURCE[0]}")/rfsim_common.sh" "${ARGS[@]}"
SNIFF_DIR="$OUT_DIR/sniffer"
rm -rf "$SNIFF_DIR" "$OUT_DIR/cellgroups" "$OUT_DIR"/gnb_dci.csv "$OUT_DIR"/ue*.log "$OUT_DIR"/r*config.raw
mkdir -p "$SNIFF_DIR" "$OUT_DIR/cellgroups"
UE_PIDS=()

fail() { echo "FAIL: $*"; exit 1; }

echo "Run directory: $OUT_DIR"
start_gnb_dci_logger -c "$OUT_DIR/cellgroups"
if [ "$PHYTEST" = 1 ]; then
  start_gnb --phy-test --T_stdout 2 ${GNB_ARGS:-}
else
  start_gnb --T_stdout 2 ${GNB_ARGS:-}
fi
echo "gNB started (log: gnb.log); it waits for the T-tracer logger..."
wait_for 60 "connected to" "$OUT_DIR/gnb_dci_logger.log" "$GNB_PID" || fail "nr-gnb-dci-logger did not connect, see gnb_dci_logger.log"
wait_for 30 "Running as server" "$OUT_DIR/gnb.log" "$GNB_PID" || fail "the gNB did not start, see gnb.log"

RAW=(--phy-test --reconfig-file "$OUT_DIR/reconfig.raw" --rbconfig-file "$OUT_DIR/rbconfig.raw")
if [ "$PHYTEST" = 1 ]; then
  start_sniffer "${RAW[@]}" --sniffer.out_dir "$SNIFF_DIR" ${SNIFFER_ARGS:-}
  wait_for 60 "Found cell" "$OUT_DIR/sniffer.log" "$SNIFF_PID" || fail "nr-sniffer did not find the cell, see sniffer.log"
  start_ue "${RAW[@]}"
  echo "phy-test UE started (log: ue.log)"
else
  start_sniffer --sniffer.out_dir "$SNIFF_DIR" --sniffer.max_ues "$NUM_UES" ${SNIFFER_ARGS:-}
  wait_for 60 "Watching random access" "$OUT_DIR/sniffer.log" "$SNIFF_PID" || fail "nr-sniffer did not decode SIB1, see sniffer.log"
  echo "nr-sniffer is watching random access (log: sniffer.log)"
  # the UEs attach one after the other, in the background, while the live view runs
  (for n in $(seq 1 "$NUM_UES"); do sleep 3; start_ue_n "$n"; done; wait) &
  STARTER_PID=$!
fi

echo "Live view in 3 s; press Ctrl-C to stop and write the report."
sleep 3
trap 'true' INT  # Ctrl-C ends the live view, not this script
python3 "$REPO_DIR/scripts/sniffer_live.py" "$SNIFF_DIR"
trap - INT

echo "Stopping..."
# the UEs first (started by a subshell, so not in STARTED_PIDS), then the sniffer, the gNB and the logger
[ -n "${STARTER_PID:-}" ] && kill "$STARTER_PID" 2>/dev/null
for p in $(pgrep -x nr-uesoftmodem); do stop_pid "$p"; done
cleanup
STARTED_PIDS=()

if [ "$PHYTEST" = 1 ]; then
  RNTI=$(awk -F, 'NR > 1 && $5 == "c" { print $4 }' "$OUT_DIR/gnb_dci.csv" | sort | uniq -c | sort -rn | awk 'NR == 1 { print $2 }')
  python3 "$REPO_DIR/scripts/compare_dci.py" "$OUT_DIR/gnb.log" "$OUT_DIR/gnb_dci.csv" "$SNIFF_DIR" --rnti "${RNTI:-0x1234}" \
      --report "$OUT_DIR/report.md" --report-csv "$OUT_DIR/per_ue.csv"
else
  python3 "$REPO_DIR/scripts/compare_dci.py" "$OUT_DIR/gnb.log" "$OUT_DIR/gnb_dci.csv" "$SNIFF_DIR" \
      --report "$OUT_DIR/report.md" --report-csv "$OUT_DIR/per_ue.csv"
fi
echo "Report: $OUT_DIR/report.md"
