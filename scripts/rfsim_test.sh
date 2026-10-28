#!/bin/bash
# Milestone 1 test: run an OAI gNB in RF-simulator mode, attach nr-sniffer as a passive
# rfsim client, and check that the decoded MIB/SIB1 match the gNB configuration.
#
# Usage: scripts/rfsim_test.sh [OAI_DIR]
# Env:   GNB_CONF (default: OAI's ci-scripts/conf_files/gnb.sa.band78.106prb.rfsim.conf)
#        TIMEOUT  seconds to wait for SIB1 (default 90)
#        OUT_DIR  where logs and CSVs go (default: ./rfsim-test-out)
set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OAI_DIR="${1:-$REPO_DIR/oai}"
BUILD_DIR="$OAI_DIR/cmake_targets/ran_build/build"
GNB_CONF="${GNB_CONF:-$OAI_DIR/ci-scripts/conf_files/gnb.sa.band78.106prb.rfsim.conf}"
TIMEOUT="${TIMEOUT:-90}"
OUT_DIR="$(mkdir -p "${OUT_DIR:-$PWD/rfsim-test-out}" && cd "${OUT_DIR:-$PWD/rfsim-test-out}" && pwd)"
SNIFF_DIR="$OUT_DIR/sniffer"

rm -rf "$SNIFF_DIR" && mkdir -p "$SNIFF_DIR"

# The gNB runs without a core network: point its NG interface at localhost so it starts cleanly.
TEST_CONF="$OUT_DIR/gnb.conf"
sed -E 's/(ipv4[[:space:]]*=[[:space:]]*")[0-9.]+(")/\1127.0.0.1\2/;
        s/(GNB_IPV4_ADDRESS_FOR_N[GU][_A-Z]*[[:space:]]*=[[:space:]]*")[0-9.]+(\/[0-9]+)?(")/\1127.0.0.1\3/' \
    "$GNB_CONF" > "$TEST_CONF"

# Derive the sniffer's tuning arguments from the gNB config.
read -r DL_FREQ NRB MU BAND < <(python3 - "$TEST_CONF" <<'EOF'
import re, sys
t = re.sub(r"#.*", "", open(sys.argv[1]).read())
v = lambda k: int(re.search(r"\b" + k + r"\s*=\s*([-0-9]+)", t).group(1))
def khz(n):
    return 5 * n if n < 600000 else 3_000_000 + 15 * (n - 600000) if n < 2016667 else 24_250_080 + 60 * (n - 2016667)
mu, nrb = v("dl_subcarrierSpacing"), v("dl_carrierBandwidth")
band = v("dl_frequencyBand") if "dl_frequencyBand" in t else v("ul_frequencyBand")
center = khz(v("dl_absoluteFrequencyPointA")) + (v("dl_offstToCarrier") + nrb / 2) * 12 * (15 << mu)
print(int(center * 1000), nrb, mu, band)
EOF
)
echo "Cell under test: DL centre ${DL_FREQ} Hz, ${NRB} PRB, numerology ${MU}, band n${BAND}"

cleanup() {
  [ -n "${SNIFF_PID:-}" ] && kill "$SNIFF_PID" 2>/dev/null
  [ -n "${GNB_PID:-}" ] && kill "$GNB_PID" 2>/dev/null
  sleep 2
  [ -n "${SNIFF_PID:-}" ] && kill -9 "$SNIFF_PID" 2>/dev/null
  [ -n "${GNB_PID:-}" ] && kill -9 "$GNB_PID" 2>/dev/null
}
trap cleanup EXIT

cd "$BUILD_DIR"
./nr-softmodem -O "$TEST_CONF" --rfsim --rfsimulator.[0].serveraddr server > "$OUT_DIR/gnb.log" 2>&1 &
GNB_PID=$!
sleep 8

./nr-sniffer -r "$NRB" --numerology "$MU" --band "$BAND" -C "$DL_FREQ" \
    --rfsim --rfsimulator.[0].serveraddr 127.0.0.1 \
    --sniffer.out_dir "$SNIFF_DIR" > "$OUT_DIR/sniffer.log" 2>&1 &
SNIFF_PID=$!

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
if grep -qi "Initiating RA procedure" "$OUT_DIR/gnb.log"; then
  echo "FAIL: the gNB saw a random-access attempt, so the sniffer was not passive"
  exit 1
fi
echo "PASS: the gNB saw no random-access attempt (sniffer stayed passive)"

python3 "$REPO_DIR/scripts/check_cell.py" "$TEST_CONF" "$SNIFF_DIR"
