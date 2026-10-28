#!/usr/bin/env python3
"""Compare the MIB/SIB1 decoded by nr-sniffer with the gNB configuration file.

Usage: check_cell.py <gnb.conf> <sniffer out_dir>
Exits 0 when every checked field matches, 1 otherwise.
"""
import csv
import re
import sys


def conf_value(text, key):
    m = re.search(r"\b" + re.escape(key) + r"\s*=\s*\(?\s*([-0-9a-fA-Fx]+)L?", text)
    if not m:
        raise KeyError(key)
    return int(m.group(1), 0)


def arfcn_to_khz(n):
    """NR-ARFCN to frequency in kHz (TS 38.104 5.4.2.1)."""
    if n < 600000:
        return 5 * n
    if n < 2016667:
        return 3_000_000 + 15 * (n - 600000)
    return 24_250_080 + 60 * (n - 2016667)


def last_row(path):
    with open(path) as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise RuntimeError(f"{path} has no rows")
    return rows[-1]


def main():
    conf_path, out_dir = sys.argv[1], sys.argv[2]
    text = re.sub(r"#.*", "", open(conf_path).read())

    scs = 15 << conf_value(text, "dl_subcarrierSpacing")
    ssb_khz = arfcn_to_khz(conf_value(text, "absoluteFrequencySSB"))
    point_a_khz = arfcn_to_khz(conf_value(text, "dl_absoluteFrequencyPointA"))
    # SSB is 20 PRBs centred on absoluteFrequencySSB; offsetToPointA is in 15 kHz PRBs for FR1
    offset_to_point_a = (ssb_khz - 120 * scs - point_a_khz) // 180
    plmn = re.search(r"mcc\s*=\s*(\d+)\s*;\s*mnc\s*=\s*(\d+)", text)

    expected_mib = {
        "pci": conf_value(text, "physCellId"),
        "scs_common_khz": 15 << conf_value(text, "initialDLBWPsubcarrierSpacing"),
        "coreset0": conf_value(text, "initialDLBWPcontrolResourceSetZero"),
        "searchspace0": conf_value(text, "initialDLBWPsearchSpaceZero"),
        "cell_barred": 1,  # notBarred
    }
    tdd_periods = ["0.5ms", "0.625ms", "1ms", "1.25ms", "2ms", "2.5ms", "5ms", "10ms"]
    expected_sib1 = {
        "pci": conf_value(text, "physCellId"),
        "mcc": int(plmn.group(1)),
        "mnc": int(plmn.group(2)),
        "tac": conf_value(text, "tracking_area_code"),
        "nr_cell_id": conf_value(text, "nr_cellid"),
        "band": conf_value(text, "dl_frequencyBand") if "dl_frequencyBand" in text
        else conf_value(text, "ul_frequencyBand"),
        "offset_to_point_a": offset_to_point_a,
        "scs_khz": scs,
        "carrier_bw_prb": conf_value(text, "dl_carrierBandwidth"),
        "offset_to_carrier": conf_value(text, "dl_offstToCarrier"),
        "initial_dl_bwp_loc_bw": conf_value(text, "initialDLBWPlocationAndBandwidth"),
        "prach_config_index": conf_value(text, "prach_ConfigurationIndex"),
        "msg1_fdm": 1 << conf_value(text, "prach_msg1_FDM"),
        "msg1_freq_start": conf_value(text, "prach_msg1_FrequencyStart"),
        "ssb_periodicity_ms": [5, 10, 20, 40, 80, 160][conf_value(text, "ssb_periodicityServingCell")],
        "ss_pbch_block_power_dbm": conf_value(text, "ssPBCH_BlockPower"),
        "tdd_period": tdd_periods[conf_value(text, "dl_UL_TransmissionPeriodicity")],
        "tdd_dl_slots": conf_value(text, "nrofDownlinkSlots"),
        "tdd_dl_symbols": conf_value(text, "nrofDownlinkSymbols"),
        "tdd_ul_slots": conf_value(text, "nrofUplinkSlots"),
        "tdd_ul_symbols": conf_value(text, "nrofUplinkSymbols"),
    }

    failures = 0
    for name, expected, row in (("MIB", expected_mib, last_row(f"{out_dir}/mib.csv")),
                                ("SIB1", expected_sib1, last_row(f"{out_dir}/sib1.csv"))):
        for key, want in expected.items():
            got = row[key]
            try:
                ok = int(got, 0) == want
            except ValueError:
                ok = got == str(want)
            print(f"{'PASS' if ok else 'FAIL'} {name}.{key}: decoded={got} expected={want}")
            failures += not ok
    print("RESULT:", "PASS" if failures == 0 else f"FAIL ({failures} mismatches)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
