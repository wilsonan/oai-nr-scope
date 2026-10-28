#!/usr/bin/env python3
"""Unit tests for scripts/compare_dci.py and scripts/check_gnb_dci.py on synthetic logs.

Run: python3 -m unittest discover -s scripts/tests   (or scripts/unit_tests.sh)
"""
import csv
import os
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = ("timestamp_us,sfn,slot,rnti,rnti_type,dci_format,direction,bwp_id,coreset_id,ss_id,aggregation_level,"
          "cce_index,prb_start,prb_len,mcs,mcs_table,tbs_bits,ndi,rv,harq_id,source").split(",")
T0 = 1_793_194_560_000_000
SLOT_US = 500  # 30 kHz
RA_RNTI, RNTI = 0x010F, 0x2B6C


def dci(n, rnti=RNTI, rnti_type="c", fmt="0_0", source="gnb", **kw):
    """n-th slot of the run (frame/slot derived, SFN wraps after 1024 frames)"""
    row = dict(timestamp_us=T0 + n * SLOT_US, sfn=(n // 20) % 1024, slot=n % 20, rnti=f"0x{rnti:04x}",
               rnti_type=rnti_type, dci_format=fmt, direction="ul" if fmt.startswith("0") else "dl", bwp_id=0,
               coreset_id=0, ss_id=4, aggregation_level=4, cce_index=0, prb_start=0, prb_len=5, mcs=0,
               mcs_table="qam64", tbs_bits=136, ndi=0, rv=0, harq_id=n % 16, source=source)
    row.update(kw)
    return row


def ue_dcis(start, count, step=10):
    """the RAR, the Msg4, then count C-RNTI DCIs alternating UL/DL every `step` slots"""
    rows = [dci(start, RA_RNTI, "ra", "1_0", prb_len=8, tbs_bits=224),
            dci(start + 20, RNTI, "tc", "1_0", ss_id=1, prb_len=44, tbs_bits=1256)]
    for i in range(count):
        rows.append(dci(start + 40 + i * step, fmt="0_0" if i % 2 else "1_0"))
    return rows


GNB_LOG = ("[NR_PHY] [RAPROC] 206.10 Initiating RA procedure with preamble 35\n"
           f"[NR_MAC] 206.10 UE RA-RNTI {RA_RNTI:04x} TC-RNTI {RNTI:04x}: initiating RA procedure for RAPID 35\n"
           "[NR_RRC] Create UE context: CU UE ID 1\n")
UE_LOG = "[PHY] PRACH [UE 0] in frame.slot 206.10, placing PRACH\n"


class Run:
    """a directory laid out like scripts/dci_test.sh output"""

    def __init__(self, gnb_rows, sniffer_rows, rar=True, ue_rows=None, gnb_log=GNB_LOG, ue_log=UE_LOG,
                 sniffer_log=""):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.dir = self.tmp.name
        os.mkdir(f"{d}/sniffer")
        self.write_csv(f"{d}/gnb_dci.csv", HEADER, gnb_rows)
        self.write_csv(f"{d}/sniffer/dci.csv", HEADER, [dict(r, source="sniffer") for r in sniffer_rows])
        self.write_csv(f"{d}/sniffer/rar.csv", ["timestamp_us", "sfn", "slot", "rapid", "timing_advance", "ul_grant",
                                                "tc_rnti"],
                       [dict(timestamp_us=T0, sfn=10, slot=10, rapid=35, timing_advance=0, ul_grant="0x0000000",
                             tc_rnti=f"0x{RNTI:04x}")] if rar else [])
        if ue_rows is None:
            ue_rows = [dict(timestamp_us=T0, sfn=11, slot=10, c_rnti=f"0x{RNTI:04x}",
                            contention_resolution_id="0x010203040506")]
        self.write_csv(f"{d}/sniffer/ue.csv", ["timestamp_us", "sfn", "slot", "c_rnti", "contention_resolution_id"],
                       ue_rows)
        for name, text in (("gnb.log", gnb_log), ("ue.log", ue_log), ("sniffer.log", sniffer_log)):
            with open(f"{d}/{name}", "w") as f:
                f.write(text)

    @staticmethod
    def write_csv(path, header, rows):
        with open(path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=header)
            w.writeheader()
            w.writerows(rows)

    def compare(self, *args):
        d = self.dir
        p = subprocess.run([sys.executable, f"{SCRIPTS}/compare_dci.py", f"{d}/gnb.log", f"{d}/gnb_dci.csv",
                            f"{d}/sniffer", *args], capture_output=True, text=True)
        return p.returncode, p.stdout

    def line(self, out, name):
        return next((l for l in out.splitlines() if f" {name}:" in l), "")


def shifted(rows, us):
    """the sniffer stamps a DCI a few ms after the gNB scheduled it"""
    return [dict(r, timestamp_us=r["timestamp_us"] + us) for r in rows]


class CompareDci(unittest.TestCase):
    def new(self, *args, **kw):
        run = Run(*args, **kw)
        self.addCleanup(run.tmp.cleanup)
        return run

    def test_perfect_match_passes(self):
        g = ue_dcis(0, 400)
        run = self.new(g, shifted(g, 3000))
        rc, out = run.compare()
        self.assertEqual(rc, 0, out)
        self.assertIn("RESULT: PASS", out)
        self.assertIn("0/", run.line(out, "DCI miss rate"))
        self.assertIn("all 15 fields equal", out)

    def test_missed_dcis_fail(self):
        g = ue_dcis(0, 400)
        s = shifted(g, 3000)
        del s[100:110]
        rc, out = self.new(g, s).compare()
        self.assertEqual(rc, 1)
        self.assertIn("FAIL DCI miss rate: 10/", out)

    def test_missed_below_one_percent_passes(self):
        g = ue_dcis(0, 400)
        s = shifted(g, 3000)
        del s[100:103]
        rc, out = self.new(g, s).compare()
        self.assertEqual(rc, 0, out)
        self.assertIn("3/", out)

    def test_fake_dci_is_a_false_positive(self):
        g = ue_dcis(0, 400)
        s = shifted(g, 3000)
        s.insert(50, dci(1003, fmt="0_0", source="sniffer", timestamp_us=s[49]["timestamp_us"] + 100))
        rc, out = self.new(g, s).compare()
        self.assertEqual(rc, 1)
        self.assertIn("FAIL false positives: 1 ", out)

    def test_wrong_field_is_reported(self):
        g = ue_dcis(0, 400)
        s = shifted(g, 3000)
        for r in s[10:20]:
            r["tbs_bits"] = 999
        rc, out = self.new(g, s).compare()
        self.assertEqual(rc, 1)
        self.assertIn("FAIL DCI fields", out)
        self.assertIn("tbs_bits 10", out)

    def test_sfn_wrap_does_not_confuse_matching(self):
        # 25 s at one DCI per 20 slots: SFN wraps twice; the same (sfn, slot) comes back every 10.24 s
        g = ue_dcis(0, 2500, step=20)
        for r in g:  # tell the wrap periods apart: a DCI paired with one from another period has another MCS
            r["mcs"] = (r["timestamp_us"] - T0) // (SLOT_US * 20 * 1024)
        s = shifted(g, 3000)
        # drop one DCI per wrap period: a matcher keyed on (sfn, slot) only would fill the gap with the
        # DCI one wrap later and see no miss
        for n in (300, 1300):
            del s[n]
        run = self.new(g, s)
        rc, out = run.compare()
        self.assertIn("2/", run.line(out, "DCI miss rate"), out)
        self.assertIn("false positives: 0 ", out)
        self.assertIn("all 15 fields equal", out)

    def test_ue_not_followed_fails(self):
        g = ue_dcis(0, 100)
        rc, out = self.new(g, [], rar=False, ue_rows=[]).compare()
        self.assertEqual(rc, 1)
        self.assertIn("FAIL RAR", out)
        self.assertIn("FAIL Msg4", out)

    def test_sniffer_prach_fails_passivity(self):
        g = ue_dcis(0, 100)
        rc, out = self.new(g, shifted(g, 3000), sniffer_log="[PHY] PRACH [UE 0] in frame.slot 300.19, placing PRACH\n",
                      gnb_log=GNB_LOG + "[NR_PHY] [RAPROC] 300.19 Initiating RA procedure with preamble 3\n").compare()
        self.assertEqual(rc, 1)
        self.assertIn("FAIL passive", out)

    def test_known_ue_window_starts_when_following(self):
        # known configuration: the sniffer only starts following at 2 s; earlier gNB DCIs are not misses,
        # and the first pair (gNB stamp before the boundary, sniffer stamp after) still matches
        g = [dci(n, fmt="1_1" if n % 2 else "0_1") for n in range(0, 8000, 5)]
        follow_us = T0 + 2_000_000
        s = [r for r in shifted(g, 3000) if r["timestamp_us"] >= follow_us - 100_000]
        ue = [dict(timestamp_us=follow_us - 100_000, sfn=-1, slot=-1, c_rnti=f"0x{RNTI:04x}",
                   contention_resolution_id="known")]
        run = self.new(g, s, rar=False, ue_rows=ue, gnb_log="", ue_log="")
        rc, out = run.compare("--rnti", f"0x{RNTI:04x}")
        self.assertEqual(rc, 0, out)
        self.assertIn("PASS known UE", out)
        self.assertIn("0/", run.line(out, "DCI miss rate"))
        self.assertIn("false positives: 0 ", out)

    def test_phytest_window_starts_at_first_sniffer_dci(self):
        g = [dci(n, rnti=0x1234, fmt="1_1" if n % 2 else "0_1") for n in range(0, 8000, 5)]
        s = shifted(g[300:], 3000)  # the sniffer synchronised late
        run = self.new(g, s, rar=False, ue_rows=[], gnb_log="", ue_log="")
        rc, out = run.compare("--rnti", "0x1234")
        self.assertEqual(rc, 0, out)
        self.assertIn("false positives: 0 ", out)


RNTI2 = 0x7A01


def two_ue_run_inputs():
    """two UEs attach 2 s apart on the same RA-RNTI; their DCIs interleave, and the first stays until the end"""
    g1 = ue_dcis(0, 900)
    g2 = [dict(r, rnti=f"0x{RNTI2:04x}") if r["rnti"] == f"0x{RNTI:04x}" else r for r in ue_dcis(4000, 400)]
    for r in g2:  # offset by one slot so the two UEs' DCIs never share a slot
        n = (r["timestamp_us"] - T0) // SLOT_US + 1
        r.update(timestamp_us=T0 + n * SLOT_US, sfn=(n // 20) % 1024, slot=n % 20)
    g = sorted(g1 + g2, key=lambda r: r["timestamp_us"])
    log = GNB_LOG + (f"[NR_PHY] [RAPROC] 406.10 Initiating RA procedure with preamble 12\n"
                     f"[NR_MAC] 406.10 UE RA-RNTI {RA_RNTI:04x} TC-RNTI {RNTI2:04x}: initiating RA procedure\n"
                     "[NR_RRC] Create UE context: CU UE ID 2\n")
    rar = [dict(timestamp_us=T0, sfn=10, slot=10, rapid=35, timing_advance=0, ul_grant="0x0000000",
                tc_rnti=f"0x{RNTI:04x}"),
           dict(timestamp_us=T0 + 2_000_000, sfn=210, slot=10, rapid=12, timing_advance=0, ul_grant="0x0000000",
                tc_rnti=f"0x{RNTI2:04x}")]
    ue = [dict(timestamp_us=T0, sfn=11, slot=10, c_rnti=f"0x{RNTI:04x}", contention_resolution_id="0x01"),
          dict(timestamp_us=T0 + 2_000_000, sfn=211, slot=10, c_rnti=f"0x{RNTI2:04x}", contention_resolution_id="0x02")]
    return g, log, rar, ue


class CompareDciMultiUe(unittest.TestCase):
    def new(self, g, s, log, rar, ue):
        run = Run(g, s, ue_rows=ue, gnb_log=log, ue_log=UE_LOG + UE_LOG)
        Run.write_csv(f"{run.dir}/sniffer/rar.csv", ["timestamp_us", "sfn", "slot", "rapid", "timing_advance",
                                                     "ul_grant", "tc_rnti"], rar)
        self.addCleanup(run.tmp.cleanup)
        return run

    def test_two_ues_followed_pass(self):
        g, log, rar, ue = two_ue_run_inputs()
        run = self.new(g, shifted(g, 3000), log, rar, ue)
        rc, out = run.compare()
        self.assertEqual(rc, 0, out)
        self.assertIn(f"INFO UE 0x{RNTI:04x}: 0/", out)
        self.assertIn(f"INFO UE 0x{RNTI2:04x}: 0/", out)
        self.assertIn("2 UEs", run.line(out, "DCI miss rate"))
        # the second UE's RAR DCI on the shared RA-RNTI is not a false positive of the first
        self.assertIn("false positives: 0 ", out)

    def test_second_ue_not_followed_fails(self):
        g, log, rar, ue = two_ue_run_inputs()
        s = [r for r in shifted(g, 3000) if r["rnti"] != f"0x{RNTI2:04x}"]
        rc, out = self.new(g, s, log, rar, ue[:1]).compare()
        self.assertEqual(rc, 1)
        self.assertIn("FAIL Msg4", out)
        self.assertIn(f"not followed: 0x{RNTI2:04x}", out)
        self.assertIn("FAIL DCI miss rate", out)

    def test_misses_add_up_over_ues(self):
        g, log, rar, ue = two_ue_run_inputs()
        s = shifted(g, 3000)
        mine = [i for i, r in enumerate(s) if r["rnti"] == f"0x{RNTI2:04x}"]
        for i in sorted(mine[100:106], reverse=True):
            del s[i]
        rc, out = self.new(g, s, log, rar, ue).compare()
        self.assertIn(f"INFO UE 0x{RNTI2:04x}: 6/", out)
        self.assertIn("6/", out.split("DCI miss rate:")[1])

    def test_stray_c_rnti_dci_is_a_false_positive(self):
        g, log, rar, ue = two_ue_run_inputs()
        s = shifted(g, 3000) + [dci(5000, rnti=0x4444, source="sniffer")]
        s.sort(key=lambda r: r["timestamp_us"])
        rc, out = self.new(g, s, log, rar, ue).compare()
        self.assertEqual(rc, 1)
        self.assertIn("FAIL false positives: 1 ", out)


class CheckGnbDci(unittest.TestCase):
    def run_check(self, rows, log):
        with tempfile.TemporaryDirectory() as d:
            Run.write_csv(f"{d}/gnb_dci.csv", HEADER, rows)
            with open(f"{d}/gnb.log", "w") as f:
                f.write(log)
            p = subprocess.run([sys.executable, f"{SCRIPTS}/check_gnb_dci.py", f"{d}/gnb.log", f"{d}/gnb_dci.csv"],
                               capture_output=True, text=True)
            return p.returncode, p.stdout

    def stats(self, dl, ul):
        return (f"UE {RNTI:04x}: dlsch_rounds {dl}/0/0/0, dlsch_errors 0\n"
                f"UE {RNTI:04x}: ulsch_rounds {ul}/0/0/0, ulsch_errors 0\n")

    def rows(self):
        rows = [dci(n, 0xFFFF, "si", "1_0") for n in range(0, 400, 40)] + ue_dcis(0, 100)
        dl = sum(1 for r in rows if r["rnti"] == f"0x{RNTI:04x}" and r["direction"] == "dl")
        ul = sum(1 for r in rows if r["rnti"] == f"0x{RNTI:04x}" and r["direction"] == "ul")
        return rows, dl, ul

    def test_consistent_log_passes(self):
        rows, dl, ul = self.rows()
        rc, out = self.run_check(rows, GNB_LOG + self.stats(dl, ul))
        self.assertEqual(rc, 0, out)

    def test_lost_dcis_fail(self):
        rows, dl, ul = self.rows()
        rc, out = self.run_check(rows[:-20], GNB_LOG + self.stats(dl, ul))
        self.assertEqual(rc, 1)
        self.assertIn("FAIL", out)

    def test_out_of_range_field_fails(self):
        rows, dl, ul = self.rows()
        rows[-1]["aggregation_level"] = 3
        rc, out = self.run_check(rows, GNB_LOG + self.stats(dl, ul))
        self.assertEqual(rc, 1)
        self.assertIn("FAIL field ranges", out)


if __name__ == "__main__":
    unittest.main()
