#!/bin/bash
# Known-configuration test in SA: an nr-uesoftmodem attaches (no core) while nr-gnb-dci-logger saves
# its CellGroupConfig from the gNB (-c). nr-sniffer starts only afterwards, so it cannot see the
# random access; it follows the UE from that CellGroupConfig file and the UE's C-RNTI
# (--sniffer.cellgroup_file, --sniffer.rnti), like NR-Scope's fixed_rnti.
#
# Usage: scripts/known_ue_test.sh [OAI_DIR]
# Env:   DURATION seconds to record after the sniffer follows the UE (default 15)
#        OUT_DIR  where logs and CSVs go (default: ./known-ue-test-out)
set -uo pipefail

OUT_DIR_DEFAULT="$PWD/known-ue-test-out"
source "$(dirname "${BASH_SOURCE[0]}")/rfsim_common.sh" "$@"
DURATION="${DURATION:-15}"
SNIFF_DIR="$OUT_DIR/sniffer"
CG_DIR="$OUT_DIR/cellgroups"
rm -rf "$SNIFF_DIR" "$CG_DIR" "$OUT_DIR/gnb_dci.csv" && mkdir -p "$SNIFF_DIR" "$CG_DIR"

fail() { echo "FAIL: $*"; echo "RESULT: FAIL"; exit 1; }

start_gnb_dci_logger -c "$CG_DIR"
start_gnb --T_stdout 2
wait_for 60 "connected to" "$OUT_DIR/gnb_dci_logger.log" "$GNB_PID" || fail "nr-gnb-dci-logger did not connect to the gNB"
wait_for 30 "Running as server" "$OUT_DIR/gnb.log" "$GNB_PID" || fail "the gNB did not start its rfsim server"
start_ue
wait_for 60 "RRC_CONNECTED reached" "$OUT_DIR/gnb.log" "$GNB_PID" || fail "the UE did not reach RRC_CONNECTED"
sleep 2

# the C-RNTI and CellGroupConfig the gNB gave the UE (last CellGroupConfig for that RNTI)
RNTI=$(sed 's/\x1b\[[0-9;]*m//g' "$OUT_DIR/gnb.log" | grep -oE "TC-RNTI [0-9a-f]{4}: initiating RA" | head -1 | awk '{ print "0x" substr($2, 1, 4) }')
[ -n "$RNTI" ] || fail "no RA procedure in the gNB log"
CG_FILE=$(awk -F, -v r="$RNTI" '$2 == r { f = $4 } END { print f }' "$CG_DIR/cellgroups.csv")
[ -n "$CG_FILE" ] || fail "nr-gnb-dci-logger saved no CellGroupConfig for $RNTI"
echo "UE C-RNTI $RNTI, CellGroupConfig $CG_FILE ($(stat -c %s "$CG_DIR/$CG_FILE") bytes)"

start_sniffer --sniffer.out_dir "$SNIFF_DIR" --sniffer.cellgroup_file "$CG_DIR/$CG_FILE" --sniffer.rnti "$((RNTI))"
wait_for 60 "following UE with C-RNTI" "$OUT_DIR/sniffer.log" "$SNIFF_PID" \
  || fail "nr-sniffer did not start following the UE, see $OUT_DIR/sniffer.log"
echo "Recording ${DURATION}s of DCIs"
sleep "$DURATION"
kill -0 "$SNIFF_PID" 2>/dev/null || echo "nr-sniffer exited early, see $OUT_DIR/sniffer.log"
cleanup
STARTED_PIDS=()

grep "\[SNIFFER\]" "$OUT_DIR/sniffer.log" | head -8
python3 "$REPO_DIR/scripts/compare_dci.py" "$OUT_DIR/gnb.log" "$OUT_DIR/gnb_dci.csv" "$SNIFF_DIR" --rnti "$RNTI"
