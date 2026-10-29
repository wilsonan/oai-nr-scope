#!/bin/bash
# Prepare a run directory for the OAI RF simulator and print the shell variables to use it:
#   - <run dir>/gnb.conf: OAI's rfsim gNB config with the core-network addresses set to 127.0.0.1,
#     so the gNB starts (and broadcasts MIB/SIB1) without a core network
#   - the tuning arguments (-C/-r/--numerology/--band) for nr-uesoftmodem and nr-sniffer, derived
#     from that config
#
# Usage: eval "$(scripts/rfsim_conf.sh <run dir> [gNB config])"
# Sets:  RUN (run directory), B (OAI build directory with the binaries), T_DB (T-tracer database),
#        GNB_CONF_RFSIM (the gNB config to pass to -O), TUNE (tuning + rfsim client arguments),
#        DL_FREQ NRB MU BAND
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OAI_DIR="${OAI_DIR:-$REPO_DIR/oai}"
RUN="${1:?usage: rfsim_conf.sh <run dir> [gNB config]}"
SRC_CONF="${2:-$OAI_DIR/ci-scripts/conf_files/gnb.sa.band78.106prb.rfsim.conf}"
mkdir -p "$RUN" && RUN="$(cd "$RUN" && pwd)"

# The gNB runs without a core network: point its NG interface at localhost so it starts cleanly.
sed -E 's/(ipv4[[:space:]]*=[[:space:]]*")[0-9.]+(")/\1127.0.0.1\2/;
        s/(GNB_IPV4_ADDRESS_FOR_N[GU][_A-Z]*[[:space:]]*=[[:space:]]*")[0-9.]+(\/[0-9]+)?(")/\1127.0.0.1\3/' \
    "$SRC_CONF" > "$RUN/gnb.conf"

# Derive the UE/sniffer tuning arguments from the gNB config.
read -r DL_FREQ NRB MU BAND < <(python3 - "$RUN/gnb.conf" <<'PY'
import re, sys
t = re.sub(r"#.*", "", open(sys.argv[1]).read())
v = lambda k: int(re.search(r"\b" + k + r"\s*=\s*([-0-9]+)", t).group(1))
def khz(n):
    return 5 * n if n < 600000 else 3_000_000 + 15 * (n - 600000) if n < 2016667 else 24_250_080 + 60 * (n - 2016667)
mu, nrb = v("dl_subcarrierSpacing"), v("dl_carrierBandwidth")
band = v("dl_frequencyBand") if "dl_frequencyBand" in t else v("ul_frequencyBand")
center = khz(v("dl_absoluteFrequencyPointA")) + (v("dl_offstToCarrier") + nrb / 2) * 12 * (15 << mu)
print(int(center * 1000), nrb, mu, band)
PY
)

cat <<VARS
RUN='$RUN'
B='$OAI_DIR/cmake_targets/ran_build/build'
T_DB='$OAI_DIR/common/utils/T/T_messages.txt'
GNB_CONF_RFSIM='$RUN/gnb.conf'
DL_FREQ=$DL_FREQ NRB=$NRB MU=$MU BAND=$BAND
TUNE='-r $NRB --numerology $MU --band $BAND -C $DL_FREQ --rfsim --rfsimulator.[0].serveraddr 127.0.0.1'
VARS
