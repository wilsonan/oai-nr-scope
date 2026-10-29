#!/bin/bash
# Re-record the demo video: runs scripts/rfsim_demo.sh --ues 2 under asciinema, stops the live view
# 20 s after the sniffer follows both UEs (as Ctrl-C would), shows the sniffer's log lines, then
# renders rfsim-demo.cast to rfsim-demo.gif and rfsim-demo.mp4 in this directory.
# Needs: pip install asciinema pyte pillow; ffmpeg for the MP4.
MEDIA="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$MEDIA/../.." && pwd)"
OUT="$REPO/rfsim-demo"

if [ "${1:-}" != --inner ]; then
  rm -rf "$OUT"
  asciinema rec -q --overwrite -i 2 --cols 118 --rows 32 -c "$0 --inner" "$MEDIA/rfsim-demo.cast" || exit 1
  python3 "$MEDIA/render_cast.py" "$MEDIA/rfsim-demo.cast" "$MEDIA/rfsim-demo.gif" || exit 1
  ffmpeg -loglevel error -y -i "$MEDIA/rfsim-demo.gif" -movflags faststart -pix_fmt yuv420p \
      -vf "scale=trunc(iw/2)*2:trunc(ih/2)*2" -r 10 "$MEDIA/rfsim-demo.mp4"
  exit
fi

# --- what the recording shows
cd "$REPO"
type_cmd() { printf '\033[1;32muser@host\033[0m:\033[1;34m~/oai-nr-scope\033[0m$ '; for ((i = 0; i < ${#1}; i++)); do printf '%s' "${1:i:1}"; sleep 0.04; done; sleep 0.6; echo; }
say() { printf '\033[1;33m# %s\033[0m\n' "$1"; sleep 2.5; }
clear
say "nr-sniffer demo: an OAI gNB, 2 UEs and the passive sniffer in the RF simulator"
say "the sniffer decodes the cell, catches each UE's random access, then decodes all its DCIs"
type_cmd "scripts/rfsim_demo.sh --ues 2"
# anchored pattern: an unanchored one also matches any shell whose command line contains this script
( until [ -f "$OUT/sniffer/ue.csv" ] && [ "$(grep -c follow "$OUT/sniffer/ue.csv")" -ge 2 ]; do sleep 1; done
  sleep 20; pkill -INT -f "^python3 [^ ]*/sniffer_live[.]py" ) &
scripts/rfsim_demo.sh --ues 2
sleep 3
say "what the sniffer logged while the live view was running:"
type_cmd "grep -a '\[SNIFFER\]' rfsim-demo/sniffer.log | grep -v stats"
sed 's/\x1b\[[0-9;]*m//g' "$OUT/sniffer.log" | grep -a '\[SNIFFER\]' | grep -v stats | cut -c1-118
sleep 1.5
say "(last line: the demo stops the UEs first, so the sniffer sees them go quiet and frees the shadow MAC)"
sleep 1.5
type_cmd "head -4 rfsim-demo/sniffer/dci.csv | cut -d, -f2-17"
head -4 "$OUT/sniffer/dci.csv" | cut -d, -f2-17
sleep 6
