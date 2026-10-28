#!/usr/bin/env python3
"""Check the gNB ground-truth DCI log (nr-gnb-dci-logger) against the gNB's own log.

Usage: check_gnb_dci.py <gnb.log> <gnb_dci.csv>
Exits 0 when every check passes, 1 otherwise.
"""
import csv
import re
import sys

HEADER = ("timestamp_us,sfn,slot,rnti,rnti_type,dci_format,direction,bwp_id,coreset_id,ss_id,aggregation_level,"
          "cce_index,prb_start,prb_len,mcs,mcs_table,tbs_bits,ndi,rv,harq_id,source").split(",")


def main():
    log_path, csv_path = sys.argv[1], sys.argv[2]
    log = re.sub(r"\x1b\[[0-9;]*m", "", open(log_path, errors="replace").read())
    with open(csv_path) as f:
        reader = csv.DictReader(f)
        header = reader.fieldnames
        rows = list(reader)

    results = []

    def check(name, ok, detail):
        results.append(ok)
        print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")

    check("header", header == HEADER, "matches the shared DCI schema" if header == HEADER else f"got {header}")
    if not rows:
        check("rows", False, "no DCI logged")
        return 1

    m = re.search(r"UE RA-RNTI ([0-9a-f]+) TC-RNTI ([0-9a-f]+): initiating RA procedure", log)
    if not m:
        check("gnb log", False, "no RA procedure in the gNB log")
        return 1
    ra_rnti, tc_rnti = int(m.group(1), 16), int(m.group(2), 16)
    by = lambda **kw: [r for r in rows if all(r[k] == v for k, v in kw.items())]
    hexr = lambda v: f"0x{v:04x}"

    si = by(rnti_type="si")
    check("SI-RNTI", len(si) > 0 and all(r["rnti"] == "0xffff" and r["dci_format"] == "1_0" for r in si),
          f"{len(si)} SIB DCIs, all 1_0 on 0xffff")
    ra = by(rnti=hexr(ra_rnti), rnti_type="ra", dci_format="1_0")
    check("RAR", len(ra) >= 1, f"{len(ra)} DCI 1_0 on RA-RNTI {hexr(ra_rnti)} (gNB log: RA-RNTI {ra_rnti:04x})")
    msg4 = by(rnti=hexr(tc_rnti), rnti_type="tc", direction="dl")
    check("Msg4", len(msg4) >= 1, f"{len(msg4)} DL DCI on TC-RNTI {hexr(tc_rnti)}")

    ue = [r for r in rows if r["rnti"] == hexr(tc_rnti) and r["rnti_type"] in ("tc", "c")]
    for direction, key in (("dl", "dlsch_rounds"), ("ul", "ulsch_rounds")):
        stats = re.findall(r"UE %04x: %s (\d+)/(\d+)/(\d+)/(\d+)" % (tc_rnti, key), log)
        if not stats:
            check(f"{direction} count", False, f"no {key} statistics for UE {tc_rnti:04x} in the gNB log")
            continue
        gnb = sum(int(x) for x in stats[-1])
        logged = sum(1 for r in ue if r["direction"] == direction)
        # gNB statistics are printed periodically, so the log may hold a few DCIs sent after the last print
        ok = gnb <= logged <= gnb * 1.05 + 25
        check(f"{direction} count", ok, f"{logged} {direction.upper()} DCIs logged for {tc_rnti:04x}, "
              f"gNB last reported {gnb} {key} ({'+'.join(stats[-1])})")

    bad = [r for r in rows if not (0 <= int(r["sfn"]) < 1024 and 0 <= int(r["slot"]) < 20
                                   and int(r["aggregation_level"]) in (1, 2, 4, 8, 16)
                                   and 0 <= int(r["mcs"]) < 32 and int(r["prb_len"]) > 0
                                   and int(r["prb_start"]) + int(r["prb_len"]) <= 275 and int(r["tbs_bits"]) > 0
                                   and r["source"] == "gnb")]
    check("field ranges", not bad, f"{len(rows)} rows" + (f", first bad row {bad[0]}" if bad else " in range"))

    failures = results.count(False)
    print("RESULT:", "PASS" if failures == 0 else f"FAIL ({failures} checks failed)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
