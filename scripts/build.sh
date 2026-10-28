#!/bin/bash
# Build nr-sniffer plus the OAI gNB and UE used for RF-simulator testing.
#
# Usage: scripts/build.sh [OAI_DIR]
# Env:   JOBS (default: nproc), EXTRA_TARGETS (e.g. "oai_usrpdevif" for USRP/E320 support)
# Run scripts/setup_oai.sh first, and install OAI's dependencies once with
#   cd $OAI_DIR/cmake_targets && ./build_oai -I
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OAI_DIR="${1:-$REPO_DIR/oai}"
JOBS="${JOBS:-$(nproc)}"
BUILD_DIR="$OAI_DIR/cmake_targets/ran_build/build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [ ! -f build.ninja ]; then
  cmake ../../.. -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi
ninja -j"$JOBS" nr-sniffer nr-gnb-dci-logger nr-softmodem nr-uesoftmodem \
  params_libconfig coding rfsimulator dfts params_yaml ${EXTRA_TARGETS:-}
echo "Built: $BUILD_DIR/nr-sniffer $BUILD_DIR/nr-gnb-dci-logger"
