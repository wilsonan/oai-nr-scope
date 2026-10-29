#!/bin/bash
# Shared plumbing for the RF-simulator tests. Source it after setting OUT_DIR_DEFAULT:
#
#   OUT_DIR_DEFAULT=$PWD/some-test-out; source "$(dirname "$0")/rfsim_common.sh" "$@"
#
# Every process runs with OUT_DIR as its working directory (the phy-test gNB writes reconfig.raw there).
# Provides: REPO_DIR OAI_DIR BUILD_DIR OUT_DIR TEST_CONF DL_FREQ NRB MU BAND T_DB,
#           start_gnb [args], start_ue [args], start_ue_n N [args], start_sniffer [args], start_gnb_dci_logger,
#           stop_pid PID, and an EXIT trap that stops everything started here.
# Env:      GNB_CONF (default: OAI's ci-scripts/conf_files/gnb.sa.band78.106prb.rfsim.conf)
#           OUT_DIR  (default: OUT_DIR_DEFAULT)
#           SNIFFER_WRAP command prefix for nr-sniffer, e.g. "gdb -batch -ex run -ex bt --args"

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OAI_DIR="${1:-$REPO_DIR/oai}"
BUILD_DIR="$OAI_DIR/cmake_targets/ran_build/build"
GNB_CONF="${GNB_CONF:-$OAI_DIR/ci-scripts/conf_files/gnb.sa.band78.106prb.rfsim.conf}"
UE_CONF="${UE_CONF-$OAI_DIR/ci-scripts/conf_files/nrue.uicc.conf}"  # UE_CONF="": no -O
T_DB="$OAI_DIR/common/utils/T/T_messages.txt"
OUT_DIR="${OUT_DIR:-$OUT_DIR_DEFAULT}"
mkdir -p "$OUT_DIR" && OUT_DIR="$(cd "$OUT_DIR" && pwd)"

# A gNB left over from an earlier run keeps the rfsim and T-tracer ports busy. nr-softmodem's forked
# helper process outlives it by a few seconds, so give a just-finished test time to wind down.
others_running() { pgrep -x nr-softmodem > /dev/null || pgrep -x nr-uesoftmodem > /dev/null || pgrep -x nr-sniffer > /dev/null; }
for _ in $(seq 1 15); do others_running || break; sleep 1; done
if others_running; then
  echo "FAIL: nr-softmodem, nr-uesoftmodem or nr-sniffer is already running; stop it first"
  echo "RESULT: FAIL"
  exit 1
fi

# The gNB runs without a core network: point its NG interface at localhost so it starts cleanly.
TEST_CONF="$OUT_DIR/gnb.conf"
sed -E 's/(ipv4[[:space:]]*=[[:space:]]*")[0-9.]+(")/\1127.0.0.1\2/;
        s/(GNB_IPV4_ADDRESS_FOR_N[GU][_A-Z]*[[:space:]]*=[[:space:]]*")[0-9.]+(\/[0-9]+)?(")/\1127.0.0.1\3/' \
    "$GNB_CONF" > "$TEST_CONF"

# Derive the UE/sniffer tuning arguments from the gNB config.
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
TUNING=(-r "$NRB" --numerology "$MU" --band "$BAND" -C "$DL_FREQ" --rfsim --rfsimulator.[0].serveraddr 127.0.0.1)

STARTED_PIDS=()
stop_pid() {
  kill "$1" 2>/dev/null
  for _ in $(seq 1 10); do kill -0 "$1" 2>/dev/null || return 0; sleep 1; done
  kill -9 "$1" 2>/dev/null
}
cleanup() {
  # reverse start order: clients first, the gNB, then the T-tracer logger (so it gets the last events)
  for ((i = ${#STARTED_PIDS[@]} - 1; i >= 0; i--)); do stop_pid "${STARTED_PIDS[i]}"; done
}
trap cleanup EXIT

# start_gnb_dci_logger [logger args], e.g. -c DIR to also save each UE's CellGroupConfig
start_gnb_dci_logger() {
  "$BUILD_DIR/nr-gnb-dci-logger" -d "$T_DB" -o "$OUT_DIR/gnb_dci.csv" "$@" 2> "$OUT_DIR/gnb_dci_logger.log" &
  LOGGER_PID=$!
  STARTED_PIDS+=("$LOGGER_PID")
}

start_gnb() {
  (cd "$OUT_DIR" && exec "$BUILD_DIR/nr-softmodem" -O "$TEST_CONF" --rfsim --rfsimulator.[0].serveraddr server "$@") \
      > "$OUT_DIR/gnb.log" 2>&1 &
  GNB_PID=$!
  STARTED_PIDS+=("$GNB_PID")
}

start_ue() {
  (cd "$OUT_DIR" && exec "$BUILD_DIR/nr-uesoftmodem" ${UE_CONF:+-O "$UE_CONF"} "${TUNING[@]}" --uicc0.imsi 208990100001100 "$@") \
      > "$OUT_DIR/ue.log" 2>&1 &
  UE_PID=$!
  STARTED_PIDS+=("$UE_PID")
}

# start_ue_n N [args]: UE number N (its own rfsim connection, IMSI and log ue<N>.log), for multi-UE tests
start_ue_n() {
  local n=$1
  shift
  (cd "$OUT_DIR" && exec "$BUILD_DIR/nr-uesoftmodem" ${UE_CONF:+-O "$UE_CONF"} "${TUNING[@]}" \
       --uicc0.imsi "$(printf '2089901000011%02d' "$n")" "$@") > "$OUT_DIR/ue$n.log" 2>&1 &
  UE_PIDS+=($!)
  STARTED_PIDS+=($!)
}

start_sniffer() {
  # SNIFFER_WRAP, e.g. "gdb -batch -ex run -ex bt --args", runs nr-sniffer under a debugger
  (cd "$OUT_DIR" && exec ${SNIFFER_WRAP:-} "$BUILD_DIR/nr-sniffer" "${TUNING[@]}" "$@") > "$OUT_DIR/sniffer.log" 2>&1 &
  SNIFF_PID=$!
  STARTED_PIDS+=("$SNIFF_PID")
}

# Wait up to $1 seconds for "$2" (a grep -E pattern) to appear in file $3; fails early if pid $4 died.
wait_for() {
  local timeout=$1 pattern=$2 file=$3 pid=${4:-}
  for _ in $(seq 1 "$timeout"); do
    grep -qE "$pattern" "$file" 2>/dev/null && return 0
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then return 1; fi
    sleep 1
  done
  return 1
}
