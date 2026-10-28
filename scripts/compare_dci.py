#!/usr/bin/env python3
"""Compare the DCIs nr-sniffer decoded for the UE it followed with the gNB ground truth (component C, first cut).

Usage: compare_dci.py <gnb.log> <gnb_dci.csv> <sniffer out_dir> [--ue-log ue.log] [--sniffer-log sniffer.log]

The UE is identified from the gNB log (RA-RNTI and TC-RNTI of its random access). DCIs are matched on
(sfn, slot, rnti, direction) with timestamps within 1 s, since SFN wraps every 10.24 s. Only the
window in which both logs were recording counts: from the gNB's RAR DCI for the UE to the last DCI
either side logged. Exits 0 when the sniffer followed the UE, stayed passive, and its DCIs match.
"""
import argparse
import csv
import os
import re
import sys

FIELDS = ["dci_format", "rnti_type", "bwp_id", "coreset_id", "ss_id", "aggregation_level", "cce_index",
          "prb_start", "prb_len", "mcs", "mcs_table", "tbs_bits", "ndi", "rv", "harq_id"]
MATCH_WINDOW_US = 1_000_000
MAX_MISS_RATE = 0.01
MAX_FIELD_MISMATCH_RATE = 0.01


def read_csv(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gnb_log")
    ap.add_argument("gnb_dci")
    ap.add_argument("sniffer_dir")
    ap.add_argument("--ue-log")
    ap.add_argument("--sniffer-log")
    a = ap.parse_args()
    out_dir = os.path.dirname(os.path.abspath(a.gnb_log))
    ue_log = a.ue_log or os.path.join(out_dir, "ue.log")
    sniffer_log = a.sniffer_log or os.path.join(out_dir, "sniffer.log")

    strip = lambda t: re.sub(r"\x1b\[[0-9;]*m", "", t)
    log = strip(open(a.gnb_log, errors="replace").read())
    gnb = read_csv(a.gnb_dci)
    sniff = read_csv(os.path.join(a.sniffer_dir, "dci.csv"))
    rar = read_csv(os.path.join(a.sniffer_dir, "rar.csv"))
    ue = read_csv(os.path.join(a.sniffer_dir, "ue.csv"))

    results = []

    def check(name, ok, detail):
        results.append(ok)
        print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")

    m = re.search(r"UE RA-RNTI ([0-9a-f]+) TC-RNTI ([0-9a-f]+): initiating RA procedure", log)
    if not m:
        check("gnb log", False, "no RA procedure in the gNB log")
        return finish(results)
    ra_rnti, rnti = f"0x{int(m.group(1), 16):04x}", f"0x{int(m.group(2), 16):04x}"

    # --- random access tracking
    check("RAR", any(r["tc_rnti"] == rnti for r in rar),
          f"sniffer decoded a RAR with TC-RNTI {rnti}" if any(r["tc_rnti"] == rnti for r in rar)
          else f"no RAR with TC-RNTI {rnti} in rar.csv ({len(rar)} RARs)")
    followed = ue[0]["c_rnti"] if ue else None
    check("Msg4", followed == rnti, f"sniffer follows C-RNTI {followed}, gNB TC-RNTI {rnti}")

    # --- passivity: every PRACH the gNB detected was sent by the UE, and the gNB knows only one UE
    gnb_prach = len(re.findall(r"\[RAPROC\] [0-9.]+ Initiating RA procedure", log))
    ue_prach = len(re.findall(r"PRACH \[UE \d+\] in frame\.slot", strip(open(ue_log, errors="replace").read())))
    sniffer_prach = len(re.findall(r"PRACH \[UE \d+\] in frame\.slot", strip(open(sniffer_log, errors="replace").read())))
    contexts = len(re.findall(r"Create UE context", log))
    check("passive", gnb_prach == ue_prach and sniffer_prach == 0 and contexts == 1,
          f"gNB detected {gnb_prach} PRACH, the UE sent {ue_prach}, the sniffer sent {sniffer_prach}; "
          f"gNB UE contexts {contexts}")

    # --- DCI matching for the followed UE
    ts = lambda r: int(r["timestamp_us"])
    mine = lambda rows: [r for r in rows if r["rnti"] in (rnti, ra_rnti) and r["rnti_type"] in ("ra", "tc", "c")
                         and (r["rnti"] == rnti or r["rnti_type"] == "ra")]
    g, s = mine(gnb), mine(sniff)
    g_rar = [r for r in g if r["rnti"] == ra_rnti]
    if not g or not g_rar:
        check("ground truth", False, f"no gNB DCI for RA-RNTI {ra_rnti} / RNTI {rnti}")
        return finish(results)
    start = ts(g_rar[0]) - MATCH_WINDOW_US
    g = [r for r in g if ts(r) >= ts(g_rar[0]) and (r["rnti"] == rnti or r is g_rar[0])]
    end = min(ts(g[-1]), ts(s[-1]) if s else 0) - 100_000  # both sides stop at different times
    g = [r for r in g if ts(r) <= end]
    s = [r for r in s if start <= ts(r) <= end + MATCH_WINDOW_US]

    by_key = {}
    for r in s:
        by_key.setdefault((r["sfn"], r["slot"], r["rnti"], r["direction"]), []).append(r)
    matched, missed, used = [], [], set()
    for r in g:
        cands = [c for c in by_key.get((r["sfn"], r["slot"], r["rnti"], r["direction"]), [])
                 if id(c) not in used and abs(ts(c) - ts(r)) <= MATCH_WINDOW_US]
        if cands:
            c = min(cands, key=lambda c: abs(ts(c) - ts(r)))
            used.add(id(c))
            matched.append((r, c))
        else:
            missed.append(r)
    false_pos = [c for c in s if id(c) not in used and ts(c) <= end]

    n = len(g)
    per_dir = {d: sum(1 for r in g if r["direction"] == d) for d in ("dl", "ul")}
    miss_rate = len(missed) / n
    check("DCI miss rate", miss_rate <= MAX_MISS_RATE,
          f"{len(missed)}/{n} gNB DCIs missed ({100 * miss_rate:.2f}%; {per_dir['dl']} DL, {per_dir['ul']} UL)"
          + (f", first missed {missed[0]['sfn']}.{missed[0]['slot']} {missed[0]['rnti_type']} {missed[0]['dci_format']}"
             if missed else ""))
    check("false positives", len(false_pos) == 0, f"{len(false_pos)} sniffer DCIs with no gNB DCI"
          + (f", first {false_pos[0]['sfn']}.{false_pos[0]['slot']} {false_pos[0]['rnti']}" if false_pos else ""))
    bad_fields = {}
    for r, c in matched:
        for f in FIELDS:
            if r[f] != c[f]:
                bad_fields.setdefault(f, []).append((r, c))
    worst = max((len(v) for v in bad_fields.values()), default=0)
    detail = ", ".join(f"{f} {len(v)} (e.g. gNB {v[0][0][f]} sniffer {v[0][1][f]} at {v[0][0]['sfn']}.{v[0][0]['slot']})"
                       for f, v in bad_fields.items()) or f"all {len(FIELDS)} fields equal"
    check("DCI fields", matched and worst / len(matched) <= MAX_FIELD_MISMATCH_RATE,
          f"{len(matched)} matched DCIs: {detail}")
    gnb_tbs = sum(int(r["tbs_bits"]) for r in g)
    sniff_tbs = sum(int(c["tbs_bits"]) for _, c in matched)
    print(f"INFO scheduled bits for {rnti}: gNB {gnb_tbs}, sniffer {sniff_tbs} "
          f"({100 * (sniff_tbs - gnb_tbs) / gnb_tbs:+.2f}%)" if gnb_tbs else "INFO no scheduled bits")
    return finish(results)


def finish(results):
    failures = results.count(False)
    print("RESULT:", "PASS" if failures == 0 else f"FAIL ({failures} checks failed)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
