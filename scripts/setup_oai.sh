#!/bin/bash
# Fetch the pinned OAI release, apply the nr-sniffer patches and link nr-sniffer into the tree.
#
# Usage: scripts/setup_oai.sh [OAI_DIR]        (default: ./oai next to this repository's root)
# Env:   OAI_GIT_URL  (default: GitHub mirror of openairinterface5g)
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OAI_VERSION="$(tr -d '[:space:]' < "$REPO_DIR/OAI_VERSION")"
OAI_DIR="${1:-$REPO_DIR/oai}"
OAI_GIT_URL="${OAI_GIT_URL:-https://github.com/OPENAIRINTERFACE/openairinterface5g.git}"

if [ ! -d "$OAI_DIR/.git" ]; then
  echo "Cloning OAI $OAI_VERSION into $OAI_DIR"
  git clone --depth 1 --branch "$OAI_VERSION" "$OAI_GIT_URL" "$OAI_DIR"
else
  echo "Using existing OAI tree in $OAI_DIR"
fi

cd "$OAI_DIR"
for p in "$REPO_DIR"/oai-patches/*.patch; do
  if git apply --reverse --check "$p" 2>/dev/null; then
    echo "Already applied: $(basename "$p")"
  else
    echo "Applying: $(basename "$p")"
    git apply "$p"
  fi
done

ln -sfn "$REPO_DIR/nr-sniffer" "$OAI_DIR/nr-sniffer"
echo "OAI $OAI_VERSION ready in $OAI_DIR (nr-sniffer linked)."
