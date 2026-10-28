#!/bin/bash
# Milestone 1 test: run an OAI gNB in RF-simulator mode, attach nr-sniffer as a passive
# rfsim client, and check that the decoded MIB/SIB1 match the gNB configuration.
#
# Usage: scripts/rfsim_test.sh [OAI_DIR]
# Env:   GNB_CONF (default: OAI's ci-scripts/conf_files/gnb.sa.band78.106prb.rfsim.conf)
#        TIMEOUT  seconds to wait for SIB1 (default 90)
#        OUT_DIR  where logs and CSVs go (default: ./rfsim-test-out)
set -uo pipefail

OUT_DIR_DEFAULT="$PWD/rfsim-test-out"
source "$(dirname "${BASH_SOURCE[0]}")/rfsim_common.sh" "$@"
TIMEOUT="${TIMEOUT:-90}"
SNIFF_DIR="$OUT_DIR/sniffer"
rm -rf "$SNIFF_DIR" && mkdir -p "$SNIFF_DIR"

start_gnb
sleep 8
start_sniffer --sniffer.out_dir "$SNIFF_DIR"

echo "Waiting up to ${TIMEOUT}s for SIB1..."
for _ in $(seq 1 "$TIMEOUT"); do
  if [ -f "$SNIFF_DIR/sib1.csv" ] && [ "$(wc -l < "$SNIFF_DIR/sib1.csv")" -ge 2 ]; then
    break
  fi
  if ! kill -0 "$GNB_PID" 2>/dev/null; then echo "gNB exited early, see $OUT_DIR/gnb.log"; break; fi
  if ! kill -0 "$SNIFF_PID" 2>/dev/null; then echo "nr-sniffer exited early, see $OUT_DIR/sniffer.log"; break; fi
  sleep 1
done
sleep 3  # let a few more MIBs come in

grep "\[SNIFFER\]" "$OUT_DIR/sniffer.log" | head -20
# The passivity verdict only means something if the gNB was up and the sniffer was attached to it.
if ! kill -0 "$GNB_PID" 2>/dev/null || ! grep -q "Client connects" "$OUT_DIR/gnb.log"; then
  echo "FAIL: the gNB is not running or nr-sniffer never connected to it, see $OUT_DIR/gnb.log"
  echo "RESULT: FAIL"
  exit 1
fi
# NR gNB PHY logs "[RAPROC] ... Initiating RA procedure" on PRACH detection, MAC "initiating RA procedure".
if grep -qi "Initiating RA procedure" "$OUT_DIR/gnb.log"; then
  echo "FAIL: the gNB saw a random-access attempt, so the sniffer was not passive"
  echo "RESULT: FAIL"
  exit 1
fi
echo "PASS: the gNB saw no random-access attempt (sniffer stayed passive)"

python3 "$REPO_DIR/scripts/check_cell.py" "$TEST_CONF" "$SNIFF_DIR"
