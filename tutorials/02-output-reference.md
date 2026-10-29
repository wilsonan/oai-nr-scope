# Tutorial 02: output reference

This page lists everything `nr-sniffer`, `nr-gnb-dci-logger` and `compare_dci.py` write, and what it
means.

Conventions used throughout:

- **Times.** `timestamp_us` is the writing program's wall clock, in microseconds since the Unix epoch.
  `sfn.slot` is the cell's air-interface time: the system frame number (0–1023) and the slot within
  the frame (0–19 at 30 kHz).
- **RNTIs.** RNTIs are 16-bit hexadecimal values (`0x270a`). Log lines print them without the `0x`.

The examples are from the runs in [tutorial 01](01-rfsim-tutorial.md).

## Sniffer console (`sniffer.log`)

All sniffer lines start with `[SNIFFER]`. Everything else in the log is OAI's normal UE output.

### At start-up

`nr-sniffer started: out_dir=sniffer passive=1 pci=-1 mcs_table=qam256 follow_ue=1`

This echoes the main options:

- `passive=1`: never transmit.
- `pci=-1`: any cell.

`Known configuration: CellGroupConfig from cellgroups/cellgroup_a65c_0.uper (134 bytes) for C-RNTI a65c`

This appears only with `--sniffer.cellgroup_file`. It reports what the file contained, a
CellGroupConfig or an RRCReconfiguration.

### The cell

`Found cell: PCI 0, MIB SFN 490, SSB idx 0, SCS common 30 kHz, k_SSB 0, CORESET0 11, SearchSpace0 0, not barred`

The first MIB was decoded, so the sniffer is synchronised to the cell.

| Field | Meaning |
|---|---|
| `PCI` | physical cell ID |
| `SSB idx` | which SSB beam |
| `k_SSB` | subcarrier offset between the SSB and the resource grid |
| `CORESET0` / `SearchSpace0` | indexes into 38.213 tables 13-1…13-10, giving where SIB1's PDCCH is |

`SIB1 decoded: PCI 0, PLMN 208/99, TAC 1, NCI 0x000bc614e, band n78, 106 PRB @ 30 kHz, initial BWP 0+106 PRB, PRACH idx 98, TDD 5ms 7D/2U`

The sniffer now knows the cell's common configuration.

| Field | Meaning |
|---|---|
| `NCI` | 36-bit NR cell identity |
| `initial BWP start+size` | the bandwidth part every UE starts in |
| `PRACH idx` | PRACH configuration index, which gives the RA occasions |
| `TDD period nD/mU` | the TDD pattern: full downlink and uplink slots per period |

This line does not appear in phy-test, where the gNB sends no SIB1.

`Watching random access: PRACH config 98, 3 RA-RNTIs: 010b 010f 0113 (following up to 8 UEs)`

The RA-RNTIs are computed from the PRACH occasions in SIB1 (38.321 §5.1.3): one per occasion, by
symbol, slot and frequency. The sniffer now decodes every DCI on these RNTIs, which are the RARs.

### Random access and UEs

`RAR at 720.10: RAPID 41, TA 0, TC-RNTI 270a`

The sniffer decoded one subPDU of a random-access response, one line per subPDU.

| Field | Meaning |
|---|---|
| `RAPID` | the preamble the UE sent |
| `TA` | timing advance command |
| `TC-RNTI` | the temporary C-RNTI the gNB assigned. The sniffer now also looks for DCIs on this TC-RNTI, to catch Msg4 |

`Msg4 at 722.1: following UE with C-RNTI 270a (shadow MAC 1, 1 UEs followed)`

The sniffer decoded Msg4 on the TC-RNTI. Msg4 holds:

- the contention resolution identity;
- the RRCSetup with the UE's CellGroupConfig.

The UE now has that RNTI as its C-RNTI, and the sniffer follows it with its own shadow MAC instance
(1–14).

`Msg4 at … on TC-RNTI xxxx not followed (reason)`

A Msg4 was seen but not followed. The reasons:

| Reason | Meaning |
|---|---|
| `TC-RNTI not from a decoded RAR` | the sniffer missed the RAR |
| `no RRCSetup` | for example an RRCReestablishment, or a Msg4 for a UE that was already connected |
| `max_ues reached` | raise `--sniffer.max_ues` |

`SIB1 applied: following UE with C-RNTI a65c (known configuration, shadow MAC 1)`

A UE given with `--sniffer.cellgroup_file` / `--sniffer.rnti` is followed from now on. Following
starts once SIB1 is known, or immediately in phy-test.

`not following C-RNTI xxxx: max_ues reached or already followed`

A known configuration could not be added.

`UE 1bc7: no DCI for 3.0 s, no longer followed (shadow MAC 1 is free)`

The UE was idle for `--sniffer.ue_timeout` seconds. The UE probably released or lost its connection.
The shadow MAC is reused for the next UE.

### Every 2000 slots

`stats at 340.17: 2000 slots, 3054 DCIs logged, 6 UEs followed, 6.56 PDCCH configurations per slot (6.44 more merged into them)`

| Field | Meaning |
|---|---|
| `DCIs logged` | total since start |
| `PDCCH configurations per slot` | how many search-space decodings the PHY ran per slot, averaged. This is the main cost driver |
| `more merged into them` | how many additional UEs' search spaces were identical and were checked within those same decodings, by testing their RNTIs against the same candidates (`--sniffer.merge_pdcch 1`) instead of decoding again |

With 6 UEs, merging cuts about 13 decodings per slot to about 6.6.

## Sniffer output directory (`--sniffer.out_dir`)

Every file is a CSV with a header line, flushed row by row, so you can read it while the sniffer
runs.

### `dci.csv`: every decoded DCI

This is the main output. `gnb_dci.csv` has the same columns.

| Column | Meaning |
|---|---|
| `timestamp_us` | when it was logged |
| `sfn`, `slot` | the slot the DCI was sent in (PDCCH slot) |
| `rnti` | the RNTI the CRC was scrambled with |
| `rnti_type` | `c` C-RNTI; `tc` TC-RNTI (Msg4 and its retransmissions); `ra` RA-RNTI (RAR); `si` SI-RNTI (system information); `p` P-RNTI (paging) |
| `dci_format` | `1_0`, `1_1` (downlink assignment); `0_0`, `0_1` (uplink grant) |
| `direction` | `dl` or `ul` |
| `bwp_id` | the bandwidth part (0 = initial BWP) |
| `coreset_id`, `ss_id` | the CORESET and search space it was found in. 0/0 is CORESET#0 with SearchSpace#0 (SIB1). In OAI's default SA config, SS 4 is the common search space for RA and connected-mode 0_0/1_0, and SS 5 is the UE-specific search space (0_1/1_1) |
| `aggregation_level`, `cce_index` | the PDCCH candidate: size (1, 2, 4, 8, 16 CCEs) and first CCE |
| `prb_start`, `prb_len` | allocated PRBs of the PDSCH/PUSCH, relative to the BWP |
| `mcs`, `mcs_table` | MCS index and table (`qam64`, `qam256`, `qam64LowSE`) |
| `tbs_bits` | transport block size in bits, computed from the allocation (38.214 §5.1.3.2) |
| `ndi` | new data indicator. A toggle relative to the previous DCI of the same HARQ process means new data |
| `rv` | redundancy version. 0 is normally the first transmission; 2, 3, 1 are retransmissions |
| `harq_id` | HARQ process |
| `source` | `sniffer` or `gnb` |

Examples:

```
…,720,10,0x0113,ra,1_0,dl,0,0,1,4,0,0,8,0,qam64,224,0,0,0,sniffer     RAR, 8 PRB
…,722,1,0x270a,tc,1_0,dl,0,0,1,4,0,0,44,0,qam64,1256,0,0,0,sniffer    Msg4 (RRCSetup), 44 PRB
…,723,1,0x270a,c,0_0,ul,0,0,4,4,0,0,5,0,qam64,24,0,0,0,sniffer        UL grant, 5 PRB, MCS 0
```

Throughput is the sum of `tbs_bits` over a time range. Counting only `rv == 0` gives new data only.

The SI-RNTI rows differ between the two files:

- The sniffer, like any UE, decodes SIB1 once and then stops looking for SI-RNTI, so it logs one
  `si` row.
- The gNB logs every SIB1 DCI.

`compare_dci.py` only evaluates the followed UEs.

### `mib.csv`: every decoded MIB

| Column | Meaning |
|---|---|
| `timestamp_us`, `sfn` | when, and in which frame |
| `ssb_index`, `half_frame` | which SSB of the burst, and in which half frame |
| `pci` | physical cell ID |
| `scs_common_khz` | subcarrier spacing of SIB1 and Msg2/4 |
| `ssb_subcarrier_offset` | k_SSB |
| `ssb_start_subcarrier` | where the SSB sits in the carrier |
| `dmrs_typeA_pos` | PDSCH DMRS position, 2 or 3 |
| `coreset0`, `searchspace0` | indexes for SIB1's PDCCH |
| `cell_barred` | 0 = barred, 1 = not barred (ASN.1 enum) |
| `intra_freq_reselection` | 0 = allowed, 1 = not allowed |
| `fr2` | 1 for FR2 (mmWave) |
| `nb_mib_decoded` | running count |

### `sib1.csv`: every SIB1 decoded

The sniffer decodes SIB1 once in SA mode. The columns, in order:

**Identity**
- `pci`
- `mcc`, `mnc`
- `tac`
- `nr_cell_id`
- `cell_reserved_for_operator`: 0 = reserved, 1 = not reserved

**Carrier**
- `band`
- `offset_to_point_a`
- `scs_khz`
- `carrier_bw_prb`
- `offset_to_carrier`

**Initial BWP**
- `initial_dl_bwp_loc_bw`: the raw locationAndBandwidth
- `initial_dl_bwp_start_prb`, `initial_dl_bwp_size_prb`
- `initial_dl_bwp_scs_khz`

**Common search spaces**
- `searchspace_sib1`, `ra_searchspace`, `paging_searchspace`
- `nb_common_searchspaces`

**Random access**
- `prach_config_index`
- `msg1_fdm`: PRACH occasions in frequency
- `msg1_freq_start`
- `ra_response_window`: in slots

**SSB**
- `ssb_positions_in_burst`: a bitmap
- `ssb_periodicity_ms`
- `ss_pbch_block_power_dbm`

**TDD**
- `tdd_ref_scs_khz`
- `tdd_period`
- `tdd_dl_slots`, `tdd_dl_symbols`
- `tdd_ul_slots`, `tdd_ul_symbols`

**Other**
- `si_scheduling_present`: whether other SIBs are scheduled
- `nb_sib1_decoded`

`sib1_pci<N>.xml` is the full decoded SIB1 as ASN.1 XER (`--sniffer.dump_xml 1`). Open it to see
anything the CSV does not have.

### `rar.csv`: every RAR subPDU

| Column | Meaning |
|---|---|
| `sfn`, `slot` | the slot of the RAR PDSCH |
| `rapid` | the preamble ID |
| `timing_advance` | TA command, in units of 16·64·Tc / 2^μ |
| `ul_grant` | the 27-bit RAR UL grant (38.213 table 8.2-1) for Msg3, in hex |
| `tc_rnti` | the TC-RNTI assigned |

### `ue.csv`: followed UEs

One row each time the sniffer starts or stops following a UE.

| Column | Meaning |
|---|---|
| `sfn`, `slot` | when it happened |
| `c_rnti` | the UE's C-RNTI |
| `event` | `follow`: learned from RA/Msg4. `known`: from `--sniffer.cellgroup_file`. `left`: timed out |
| `contention_resolution_id` | the 48-bit identity from Msg4, the first bytes of the UE's Msg3 (`follow` only) |

## gNB ground truth (`nr-gnb-dci-logger`)

| Output | Content |
|---|---|
| `-o gnb_dci.csv` | every DCI the gNB MAC sent, same columns as `dci.csv`, `source` = `gnb`. Logged from the `GNB_MAC_DCI` T-tracer event |
| `-c DIR` | every CellGroupConfig the gNB generated for a UE (event `GNB_MAC_CELLGROUP`). Saved as `DIR/cellgroup_<rnti>_<n>.uper`, where n counts reconfigurations of the same UE, and listed in `DIR/cellgroups.csv` (`timestamp_us,rnti,length,file`). A `.uper` file can be passed directly to `--sniffer.cellgroup_file` |
| console | `connecting to 127.0.0.1:2021`, retries, then `connected to …`. Stop it with Ctrl-C |

## Evaluation (`scripts/compare_dci.py`)

```
compare_dci.py <gnb.log> <gnb_dci.csv> <sniffer out_dir> [--rnti 0x1234] [--report report.md] [--report-csv per_ue.csv]
```

**Which UEs are evaluated**
- Without `--rnti`: every UE that completed random access according to the gNB (a PRACH detection
  in `gnb.log` and a Msg4 DCI on its TC-RNTI in `gnb_dci.csv`), whether the sniffer followed it or
  not.
- With `--rnti`: that one UE, given to the sniffer by a known configuration (or phy-test).

**Which DCIs are counted**
- Random access: from the gNB's RAR DCI for that UE, so the RAR and Msg4 DCIs count too.
- Known configuration: from 100 ms after the sniffer started following (`ue.csv`).
- Either way, up to the sniffer's last logged DCI.
- DCIs are matched by `sfn`, `slot`, `rnti` and `direction`, with timestamps within 1 s of each
  other, because SFN wraps every 10.24 s.

**Checks** (each line starts with `PASS`, `FAIL` or `INFO`)

| Check | Pass condition |
|---|---|
| `RAR` | `rar.csv` has a RAR for the TC-RNTI of every UE that completed random access (SA mode) |
| `Msg4` | the sniffer followed every one of those UEs (SA mode) |
| `known UE` | the `--rnti` UE is in `ue.csv` (known-configuration mode) |
| `passive` | gNB PRACH detections = PRACH sent by the UEs (from `ue*.log`), the sniffer sent 0, and the gNB holds no extra UE context |
| `DCI miss rate` | DCIs sent but not found, at most 1% of all DCIs, over all UEs together (0 is expected on the RF simulator) |
| `false positives` | zero sniffer DCIs with no gNB DCI in the same slot, for the same RNTI and direction, including C-RNTI DCIs for an RNTI the gNB never used |
| `DCI fields` | all 15 fields equal on matched DCIs |
| `INFO scheduled bits` | the sum of `tbs_bits` on both sides, and the error |
| `RESULT` | `PASS` only if every check passed. The exit code is 0 on PASS |

**`report.md`** contains the same check lines, plus the per-UE table: C-RNTI, how it was followed,
gNB DCIs, missed, miss rate, false positives, field mismatches, seconds, and DL/UL Mbit/s for the
gNB and the sniffer with the error.

**`per_ue.csv`** has the same per-UE data:

| Column | Meaning |
|---|---|
| `rnti` | |
| `followed` | `random access` or `known configuration` |
| `dcis`, `dl_dcis` | the gNB's DCIs in the window |
| `missed`, `miss_rate_pct` | |
| `false_positives`, `field_mismatches` | |
| `seconds` | wall-clock length of the window |
| `gnb_dl_mbps`, `sniffer_dl_mbps`, `gnb_ul_mbps`, `sniffer_ul_mbps` | |

## Live view (`scripts/sniffer_live.py`)

```
sniffer_live.py <sniffer out_dir | any DCI CSV> [--window 1] [--interval 1] [--once]
```

Run it on `gnb_dci.csv` as well, to see the gNB's view side by side with the sniffer's.

The columns are explained in [tutorial 01, section 5](01-rfsim-tutorial.md#5-what-you-are-looking-at).

| Option | Effect |
|---|---|
| `--window S` | average over the last S seconds instead of 1 |
| `--once` | summarise the whole file once and exit |

## OAI's own logs, briefly

| Where | Line | Meaning |
|---|---|---|
| `gnb.log` | `UE 270a: dlsch_rounds 239/1/1/1, dlsch_errors 1, … BLER 0.00000 MCS (0) 0` | per-UE HARQ statistics every few seconds: transmissions in round 0/1/2/3, failed transport blocks, BLER, current MCS |
| `gnb.log` | `initiating RA procedure for RAPID 41` | the gNB detected a PRACH. Compare with the sniffer's `RAR` line |
| `gnb.log` | `Received RRCSetupComplete (RRC_CONNECTED reached)` | the UE is connected |
| `ueN.log` | `4-Step RA procedure succeeded. CBRA: Contention Resolution is successful.` | the UE's side of the attach |
| `ueN.log` | `State = NR_RRC_CONNECTED` | the UE's side of the attach |
| `nrMAC_stats.log`, `nrL1_stats.log` (gNB, in the run directory) | | periodic MAC and PHY statistics snapshots |
