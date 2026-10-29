#!/usr/bin/env python3
"""Live view of what nr-sniffer (or nr-gnb-dci-logger) decodes, refreshed every second.

Usage: sniffer_live.py <sniffer out_dir | dci CSV> [--window 1.0] [--interval 1.0] [--once]

  <sniffer out_dir>  the --sniffer.out_dir of nr-sniffer (dci.csv, mib.csv, sib1.csv, rar.csv, ue.csv)
  <dci CSV>          any CSV in the DCI schema, e.g. the gNB ground truth gnb_dci.csv
  --window S         statistics over the last S seconds (default 1)
  --once             no live view: print one summary over the whole file and exit

Per RNTI it shows the DCIs decoded (DL/UL), mean PRBs and MCS index (DL and UL), retransmissions (rv != 0), and the
scheduled throughput (transport-block bits per second, including retransmissions). Timestamps are
the sniffer's wall clock, so with the RF simulator, which runs slower or faster than real time, the
Mbit/s are per wall-clock second, not per second of air time.
"""
import argparse
import csv
import io
import os
import signal
import sys
import time


def last_row(path):
    try:
        with open(path) as f:
            rows = list(csv.DictReader(f))
        return rows[-1] if rows else None
    except OSError:
        return None


def all_rows(path):
    try:
        with open(path) as f:
            return list(csv.DictReader(f))
    except OSError:
        return []


class Tail:
    """read the rows appended to a CSV since the last call"""

    def __init__(self, path):
        self.path, self.pos, self.header, self.partial = path, 0, None, ""

    def rows(self):
        try:
            with open(self.path) as f:
                f.seek(self.pos)
                data = f.read()
                self.pos = f.tell()
        except OSError:
            return []
        data = self.partial + data
        lines = data.split("\n")
        self.partial = lines.pop()  # an incomplete last line waits for the next call
        if self.header is None and lines:
            self.header = next(csv.reader([lines.pop(0)]))
        return list(csv.DictReader(io.StringIO("\n".join(lines)), fieldnames=self.header)) if lines else []


def cell_lines(out_dir):
    mib, sib1 = last_row(os.path.join(out_dir, "mib.csv")), last_row(os.path.join(out_dir, "sib1.csv"))
    lines = []
    if mib:
        lines.append(f"Cell: PCI {mib['pci']}, SCS {mib['scs_common_khz']} kHz, CORESET#0 {mib['coreset0']}, "
                     f"SearchSpace#0 {mib['searchspace0']}, {'barred' if mib['cell_barred'] == '0' else 'not barred'}")
    if sib1:
        lines.append(f"      PLMN {sib1['mcc']}/{sib1['mnc']}, TAC {sib1['tac']}, band n{sib1['band']}, "
                     f"{sib1['carrier_bw_prb']} PRB, TDD {sib1['tdd_period']} {sib1['tdd_dl_slots']}D/{sib1['tdd_ul_slots']}U, "
                     f"PRACH config {sib1['prach_config_index']}")
    if not lines:
        lines.append("Cell: not found yet (no MIB decoded)")
    rars = all_rows(os.path.join(out_dir, "rar.csv"))
    ues = all_rows(os.path.join(out_dir, "ue.csv"))
    following = {}
    for u in ues:
        if u.get("event") == "left":
            following.pop(u["c_rnti"], None)
        else:
            following[u["c_rnti"]] = u.get("event") or "follow"
    lines.append(f"Random access: {len(rars)} RAR subPDUs decoded; following {len(following)} UEs: "
                 + (", ".join(f"{r} ({'RA' if e == 'follow' else e})" for r, e in following.items()) or "none"))
    return lines


def table(rows, seconds):
    stats = {}
    for r in rows:
        s = stats.setdefault((r["rnti"], r["rnti_type"]), {"dl": 0, "ul": 0, "dl_prb": 0, "ul_prb": 0, "dl_mcs": 0,
                                                           "ul_mcs": 0, "retx": 0, "dl_bits": 0, "ul_bits": 0,
                                                           "formats": set()})
        d = r["direction"]
        s[d] += 1
        s[d + "_prb"] += int(r["prb_len"])
        s[d + "_bits"] += int(r["tbs_bits"])
        s[d + "_mcs"] += int(r["mcs"])
        s["retx"] += r["rv"] != "0"
        s["formats"].add(r["dci_format"])
    out = [f"{'RNTI':8} {'type':5} {'formats':9} {'DL DCI':>7} {'UL DCI':>7} {'DL PRB':>7} {'UL PRB':>7} "
           f"{'DL MCS':>6} {'UL MCS':>6} {'reTx':>5} {'DL Mbit/s':>10} {'UL Mbit/s':>10}"]
    for (rnti, kind), s in sorted(stats.items(), key=lambda kv: (kv[0][1] != "c", kv[0][0])):
        mean = lambda key, d: f"{s[key] / s[d]:.1f}" if s[d] else "-"
        out.append(f"{rnti:8} {kind:5} {','.join(sorted(s['formats'])):9} {s['dl']:7} {s['ul']:7} "
                   f"{mean('dl_prb', 'dl'):>7} {mean('ul_prb', 'ul'):>7} {mean('dl_mcs', 'dl'):>6} {mean('ul_mcs', 'ul'):>6} "
                   f"{s['retx']:5} {s['dl_bits'] / seconds / 1e6:10.3f} {s['ul_bits'] / seconds / 1e6:10.3f}")
    if len(out) == 1:
        out.append("(no DCI in this window)")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--window", type=float, default=1.0)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--once", action="store_true")
    a = ap.parse_args()
    if os.path.isdir(a.path):
        out_dir, dci_path = a.path, os.path.join(a.path, "dci.csv")
    else:
        out_dir, dci_path = None, a.path

    if a.once:
        rows = all_rows(dci_path)
        if out_dir:
            print("\n".join(cell_lines(out_dir)))
        if not rows:
            print(f"no DCI in {dci_path}")
            return 1
        seconds = max((int(rows[-1]["timestamp_us"]) - int(rows[0]["timestamp_us"])) / 1e6, 1e-3)
        print(f"{len(rows)} DCIs over {seconds:.1f} s ({dci_path})")
        print("\n".join(table(rows, seconds)))
        return 0

    tail, recent = Tail(dci_path), []
    tty = sys.stdout.isatty()
    while True:
        recent += tail.rows()
        now = time.time() * 1e6
        recent = [r for r in recent if now - int(r["timestamp_us"]) <= a.window * 1e6]
        lines = [time.strftime("%H:%M:%S") + f"  {dci_path}  (last {a.window:g} s)"]
        if out_dir:
            lines += cell_lines(out_dir)
        lines += [""] + table(recent, a.window)
        if tty:
            sys.stdout.write("\033[H\033[J")
        print("\n".join(lines) + ("" if tty else "\n"), flush=True)
        time.sleep(a.interval)


if __name__ == "__main__":
    # Ctrl-C and SIGTERM end the live view, also when the shell started it with SIGINT ignored
    signal.signal(signal.SIGINT, signal.default_int_handler)
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
