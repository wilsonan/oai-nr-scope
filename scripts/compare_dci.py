#!/usr/bin/env python3
"""Compare the DCIs nr-sniffer decoded for the UEs it followed with the gNB ground truth (component C).

Usage: compare_dci.py <gnb.log> <gnb_dci.csv> <sniffer out_dir> [--rnti 0x1234] [--ue-log ue.log ...]
                      [--sniffer-log sniffer.log] [--report report.md] [--report-csv per_ue.csv]

Without --rnti every UE that completed random access in the gNB log (RA-RNTI and TC-RNTI of its RA,
and a Msg4 DCI on that TC-RNTI in the ground truth) is evaluated; the sniffer must have followed each
through RAR and Msg4, and its window starts at the gNB's RAR DCI for that UE.
With --rnti (known configuration, phy-test) there is no random access to check; the window starts
when the sniffer started following (ue.csv + 100 ms), or at its first DCI for the RNTI in phy-test.
DCIs are matched on (sfn, slot, rnti, direction) with timestamps within 1 s, since SFN wraps every
10.24 s. A window ends at the last DCI either side logged. Results are printed per UE and checked
over all UEs together. Exits 0 when the sniffer stayed passive and its DCIs match.

--report writes the same verdicts plus a per-UE table (DCIs, misses, false positives, field
mismatches, DL/UL throughput scheduled by the gNB and as seen by the sniffer) as Markdown;
--report-csv writes the per-UE table as CSV. Throughput is the scheduled transport-block bits per
second of the UE's window (first to last gNB DCI), so it counts retransmissions.
"""
import argparse
import csv
import glob
import os
import re
import sys

FIELDS = ["dci_format", "rnti_type", "bwp_id", "coreset_id", "ss_id", "aggregation_level", "cce_index",
          "prb_start", "prb_len", "mcs", "mcs_table", "tbs_bits", "ndi", "rv", "harq_id"]
MATCH_WINDOW_US = 1_000_000
MAX_MISS_RATE = 0.01
MAX_FIELD_MISMATCH_RATE = 0.01

ts = lambda r: int(r["timestamp_us"])
strip = lambda t: re.sub(r"\x1b\[[0-9;]*m", "", t)


def read_csv(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def match(g, s, start, first, sniffer_end):
    """Pair gNB DCIs g with sniffer DCIs s. Returns (counted gNB DCIs, matched pairs, missed, false positives).
    first (sniffer time) is the start of a known-configuration window, applied to the pairs, or None.
    sniffer_end is when the sniffer stopped recording (its last DCI for any RNTI), so that a UE whose DCIs
    the sniffer never decoded counts as missed, not as out of the window."""
    end = min(ts(g[-1]), sniffer_end) - 100_000  # both sides stop at different times
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
    if first is not None:
        matched = [(r, c) for r, c in matched if ts(c) >= first]
        missed = [r for r in missed if ts(r) >= first]
        false_pos = [c for c in false_pos if ts(c) >= first]
        g = [r for r, _ in matched] + missed
    return g, matched, missed, false_pos


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gnb_log")
    ap.add_argument("gnb_dci")
    ap.add_argument("sniffer_dir")
    ap.add_argument("--rnti", help="C-RNTI of a UE followed with a known configuration")
    ap.add_argument("--ue-log", action="append", help="UE log(s); default: ue*.log next to gnb.log")
    ap.add_argument("--sniffer-log")
    ap.add_argument("--report", help="write a Markdown report here")
    ap.add_argument("--report-csv", help="write the per-UE table here")
    a = ap.parse_args()
    out_dir = os.path.dirname(os.path.abspath(a.gnb_log))
    ue_logs = a.ue_log or sorted(glob.glob(os.path.join(out_dir, "ue*.log")))
    sniffer_log = a.sniffer_log or os.path.join(out_dir, "sniffer.log")

    log = strip(open(a.gnb_log, errors="replace").read())
    gnb = read_csv(a.gnb_dci)
    sniff = read_csv(os.path.join(a.sniffer_dir, "dci.csv"))
    rar = read_csv(os.path.join(a.sniffer_dir, "rar.csv"))
    ue = read_csv(os.path.join(a.sniffer_dir, "ue.csv"))
    followed = {}  # C-RNTI -> the row where the sniffer started following it ("left" rows mark when it stopped)
    for r in ue:
        if r.get("event", "follow") != "left":
            followed.setdefault(r["c_rnti"], r)

    results = []
    lines = []  # every PASS/FAIL/INFO line, for the report
    per_ue = []

    def check(name, ok, detail):
        results.append(ok)
        say(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")

    def say(line):
        lines.append(line)
        print(line)

    def done():
        return finish(results, lines, per_ue, a)

    # --- the UEs to evaluate: (rnti, ra_rnti or None)
    known = a.rnti is not None
    if known:
        ues = [(f"0x{int(a.rnti, 16):04x}", None)]
        if ue:
            check("known UE", ues[0][0] in followed, f"sniffer follows C-RNTI {', '.join(followed)} "
                  "with a known configuration")
    else:
        msg4_rntis = {r["rnti"] for r in gnb if r["rnti_type"] == "tc" and r["direction"] == "dl"}
        ues = []
        for ra_hex, tc_hex in re.findall(r"UE RA-RNTI ([0-9a-f]+) TC-RNTI ([0-9a-f]+): initiating RA procedure", log):
            ue_id = (f"0x{int(tc_hex, 16):04x}", f"0x{int(ra_hex, 16):04x}")
            if ue_id[0] in msg4_rntis and ue_id not in ues:
                ues.append(ue_id)
        if not ues:
            check("gnb log", False, "no completed RA procedure in the gNB log")
            return done()
        rar_tc = {r["tc_rnti"] for r in rar}
        missing = [rnti for rnti, _ in ues if rnti not in rar_tc]
        check("RAR", not missing, f"sniffer decoded a RAR with TC-RNTI {', '.join(r for r, _ in ues)}" if not missing
              else f"no RAR with TC-RNTI {', '.join(missing)} in rar.csv ({len(rar)} RARs)")
        not_followed = [rnti for rnti, _ in ues if rnti not in followed]
        check("Msg4", not not_followed,
              f"sniffer follows C-RNTI {', '.join(followed)}, gNB TC-RNTI {', '.join(r for r, _ in ues)}"
              + (f"; not followed: {', '.join(not_followed)}" if not_followed else ""))

    # --- passivity: every PRACH the gNB detected was sent by a UE, and the gNB knows only those UEs
    gnb_prach = len(re.findall(r"\[RAPROC\] [0-9.]+ Initiating RA procedure", log))
    prach = lambda path: len(re.findall(r"PRACH \[UE \d+\] in frame\.slot", strip(open(path, errors="replace").read())))
    ue_prach = sum(prach(p) for p in ue_logs)
    sniffer_prach = prach(sniffer_log) if os.path.exists(sniffer_log) else 0
    contexts = len(re.findall(r"Create UE context", log))
    check("passive", gnb_prach == ue_prach and sniffer_prach == 0 and contexts <= max(1, len(ues)),
          f"gNB detected {gnb_prach} PRACH, the UE{'s' if len(ue_logs) > 1 else ''} sent {ue_prach}, "
          f"the sniffer sent {sniffer_prach}; gNB UE contexts {contexts}")

    # --- DCI matching, per UE
    sniffer_end = max((ts(r) for r in sniff), default=0)
    total = {"n": 0, "dl": 0, "ul": 0, "matched": [], "missed": [], "false_pos": [], "gnb_tbs": 0, "sniff_tbs": 0}
    for rnti, ra_rnti in ues:
        g = [r for r in gnb if r["rnti"] == rnti and r["rnti_type"] in ("tc", "c")]
        s = [r for r in sniff if r["rnti"] == rnti and r["rnti_type"] in ("tc", "c")]
        if known:
            if not g or not s:
                check("ground truth", False, f"{len(g)} gNB and {len(s)} sniffer DCIs for RNTI {rnti}")
                continue
            # the gNB stamps a DCI a few ms before the sniffer does (it schedules ahead), so match on a wider
            # window and apply the start, in sniffer time, to the pairs afterwards
            first = ts(followed[rnti]) + 100_000 if rnti in followed else ts(s[0])
            start = first - MATCH_WINDOW_US
            g = [r for r in g if ts(r) >= start]
        else:
            first = None
            g_rar = [r for r in gnb if r["rnti"] == ra_rnti and r["rnti_type"] == "ra"
                     and (not g or ts(r) <= ts(g[0]))]
            if not g or not g_rar:
                check("ground truth", False, f"no gNB DCI for RA-RNTI {ra_rnti} / RNTI {rnti}")
                continue
            rar_dci = g_rar[-1]  # the RAR for this UE: the last one on its RA-RNTI before its Msg4
            start = ts(rar_dci) - MATCH_WINDOW_US
            g = [rar_dci] + [r for r in g if ts(r) >= ts(rar_dci)]
            s = [r for r in sniff if r["rnti"] == ra_rnti and r["rnti_type"] == "ra"] + s
            s.sort(key=ts)
        g, matched, missed, false_pos = match(g, s, start, first, sniffer_end)
        # a RAR DCI on another UE's RA-RNTI is that UE's, not a false positive of this one
        false_pos = [c for c in false_pos if c["rnti"] == rnti]
        n = len(g)
        bad = sum(1 for r, c in matched if any(r[f] != c[f] for f in FIELDS))
        if len(ues) > 1:
            say(f"INFO UE {rnti}: {len(missed)}/{n} DCIs missed, {len(false_pos)} false positives, {bad} with a field mismatch")
        seconds = max((ts(g[-1]) - ts(g[0])) / 1e6, 1e-3) if g else 0
        mbps = lambda rows, d: sum(int(r["tbs_bits"]) for r in rows if r["direction"] == d) / seconds / 1e6 if seconds else 0
        sniff_rows = [c for _, c in matched]
        per_ue.append(dict(rnti=rnti, followed="known configuration" if known else "random access", dcis=n,
                           dl_dcis=sum(1 for r in g if r["direction"] == "dl"), missed=len(missed),
                           miss_rate_pct=round(100 * len(missed) / n, 3) if n else 0, false_positives=len(false_pos),
                           field_mismatches=bad, seconds=round(seconds, 2),
                           gnb_dl_mbps=round(mbps(g, "dl"), 4), sniffer_dl_mbps=round(mbps(sniff_rows, "dl"), 4),
                           gnb_ul_mbps=round(mbps(g, "ul"), 4), sniffer_ul_mbps=round(mbps(sniff_rows, "ul"), 4)))
        total["n"] += n
        total["dl"] += sum(1 for r in g if r["direction"] == "dl")
        total["ul"] += sum(1 for r in g if r["direction"] == "ul")
        total["matched"] += matched
        total["missed"] += missed
        total["false_pos"] += false_pos
        total["gnb_tbs"] += sum(int(r["tbs_bits"]) for r in g)
        total["sniff_tbs"] += sum(int(c["tbs_bits"]) for _, c in matched)

    # C-RNTI DCIs for an RNTI the gNB never scheduled at all
    gnb_rntis = {r["rnti"] for r in gnb}
    stray = [c for c in sniff if c["rnti_type"] == "c" and c["rnti"] not in gnb_rntis]
    n, missed, false_pos, matched = total["n"], total["missed"], total["false_pos"] + stray, total["matched"]
    if n == 0:
        check("DCI miss rate", False, "no gNB DCI to compare")
        return done()
    miss_rate = len(missed) / n
    check("DCI miss rate", miss_rate <= MAX_MISS_RATE,
          f"{len(missed)}/{n} gNB DCIs missed ({100 * miss_rate:.2f}%; {total['dl']} DL, {total['ul']} UL"
          + (f", {len(ues)} UEs" if len(ues) > 1 else "") + ")"
          + (f", first missed {missed[0]['sfn']}.{missed[0]['slot']} {missed[0]['rnti']} {missed[0]['rnti_type']} "
             f"{missed[0]['dci_format']}" if missed else ""))
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
    check("DCI fields", bool(matched) and worst / len(matched) <= MAX_FIELD_MISMATCH_RATE,
          f"{len(matched)} matched DCIs: {detail}")
    who = ues[0][0] if len(ues) == 1 else f"{len(ues)} UEs"
    gnb_tbs, sniff_tbs = total["gnb_tbs"], total["sniff_tbs"]
    say(f"INFO scheduled bits for {who}: gNB {gnb_tbs}, sniffer {sniff_tbs} "
        f"({100 * (sniff_tbs - gnb_tbs) / gnb_tbs:+.2f}%)" if gnb_tbs else "INFO no scheduled bits")
    return done()


PER_UE_COLUMNS = ["rnti", "followed", "dcis", "dl_dcis", "missed", "miss_rate_pct", "false_positives", "field_mismatches",
                  "seconds", "gnb_dl_mbps", "sniffer_dl_mbps", "gnb_ul_mbps", "sniffer_ul_mbps"]


def finish(results, lines, per_ue, a):
    failures = results.count(False)
    verdict = "PASS" if failures == 0 else f"FAIL ({failures} checks failed)"
    print("RESULT:", verdict)
    if a.report_csv:
        with open(a.report_csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=PER_UE_COLUMNS)
            w.writeheader()
            w.writerows(per_ue)
    if a.report:
        err = lambda g, s: f"{100 * (s - g) / g:+.2f}%" if g else "-"
        with open(a.report, "w") as f:
            f.write(f"# nr-sniffer evaluation\n\nResult: **{verdict}**\n\n")
            f.write(f"- gNB log: `{a.gnb_log}`\n- gNB ground truth: `{a.gnb_dci}`\n- sniffer output: `{a.sniffer_dir}`\n\n")
            f.write("## Per UE\n\n| C-RNTI | followed via | gNB DCIs | missed | miss rate | false positives | "
                    "field mismatches | seconds | DL Mbit/s gNB / sniffer (error) | UL Mbit/s gNB / sniffer (error) |\n")
            f.write("|---|---|---|---|---|---|---|---|---|---|\n")
            for u in per_ue:
                f.write(f"| {u['rnti']} | {u['followed']} | {u['dcis']} | {u['missed']} | {u['miss_rate_pct']:.2f}% | "
                        f"{u['false_positives']} | {u['field_mismatches']} | {u['seconds']} | "
                        f"{u['gnb_dl_mbps']:.3f} / {u['sniffer_dl_mbps']:.3f} ({err(u['gnb_dl_mbps'], u['sniffer_dl_mbps'])}) | "
                        f"{u['gnb_ul_mbps']:.3f} / {u['sniffer_ul_mbps']:.3f} ({err(u['gnb_ul_mbps'], u['sniffer_ul_mbps'])}) |\n")
            f.write("\n## Checks\n\n```\n" + "\n".join(lines) + f"\nRESULT: {verdict}\n```\n")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
