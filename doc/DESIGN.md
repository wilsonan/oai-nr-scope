# nr-sniffer design

nr-sniffer is a passive 5G SA telemetry tool built on the OpenAirInterface5G (OAI) UE receive
chain, pinned to **OAI 2026.w41** (`v3.0.0`). It is modelled on
[NR-Scope](https://github.com/PrincetonUniversity/NR-Scope) (srsRAN-based, CoNEXT 2024). It is
meant for the [ExPECA testbed](https://expeca.proj.kth.se/docs), where OAI gNBs run on USRP E320s.

NR-Scope is AGPL-3.0 and OAI uses the OAI Public License, so **no NR-Scope code is reused**.
NR-Scope is only a functional reference. Everything here is OAI code or new code.

## How it is built

```
oai-nr-scope/                       (this repository)
├── OAI_VERSION                     2026.w41
├── oai-patches/0001-*.patch        small hook patch applied to OAI
├── nr-sniffer/                     sniffer sources, linked into the OAI tree
├── scripts/setup_oai.sh            clone OAI at OAI_VERSION, apply patches, link nr-sniffer
├── nr-sniffer/tracer/              nr-gnb-dci-logger, the gNB ground-truth T-tracer client
├── scripts/build.sh                build nr-sniffer + nr-gnb-dci-logger + gNB + UE + rfsim
├── scripts/rfsim_common.sh         shared gNB/UE/sniffer start-up for the rfsim tests
├── scripts/rfsim_test.sh           M1: MIB/SIB1 against an OAI gNB in RF simulator
├── scripts/gnb_dci_test.sh         component B: gNB DCI log against the gNB's own counters
├── scripts/dci_test.sh             M2: sniffer follows one UE through RA, DCIs against component B
└── scripts/compare_dci.py          M2: DCI matching, miss rate, false positives, field agreement
```

`nr-sniffer` is a separate executable. It is compiled from the same sources as `nr-uesoftmodem`
plus `nr-sniffer/src/*.c`, and linked against the same OAI libraries. The OAI patch is
deliberately small:

| File | Change |
|------|--------|
| `common/utils/nr_sniffer_hooks.h` (new) | Weak hook declarations: `nr_sniffer_init`, `nr_sniffer_on_mib`, `nr_sniffer_on_sib1`, `nr_sniffer_is_passive`, and for M2 `nr_sniffer_on_dci`, `nr_sniffer_match_rnti`, `nr_sniffer_rnti_type`, `nr_sniffer_ra_watch_rnti`, `nr_sniffer_on_rar`, `nr_sniffer_on_contention_resolution`; `nr_sniffer_dci_t` |
| `openair2/LAYER2/NR_MAC_UE/nr_ue_procedures.c` | `nr_ue_decode_mib()` reports the decoded MIB plus PCI/SSB index. M2: `get_rnti_type()` knows tracked RA/TC-RNTIs; RA-RNTI DCIs are decoded outside an own RA window when passive; RARs go to `nr_sniffer_on_rar()`; a contention resolution CE outside the own RA procedure can adopt the UE (`nr_ra_succeeded()`); no RA is triggered on DL data while passive; each successfully processed DCI 0_0/0_1/1_0/1_1 is reported with its PDSCH/PUSCH allocation |
| `openair2/LAYER2/NR_MAC_UE/nr_ue_dci_configuration.c` | M2: monitor the RA search space for tracked RNTIs when not doing an own RA; pass SS/CORESET ids to the PHY |
| `openair2/LAYER2/NR_MAC_UE/nr_ra_procedures.c` | M2: `trigger_MAC_UE_RA()` is a no-op when passive |
| `openair1/PHY/NR_UE_TRANSPORT/dci_nr.c` | M2: a candidate is also accepted when its CRC remainder is a tracked RNTI (`nr_sniffer_match_rnti`); SS/CORESET ids go into the DCI indication |
| `openair1/SCHED_NR_UE/fapi_nr_ue_l1.c` | M2: when passive, UL config (PUSCH, PUCCH, PRACH, SRS) is dropped before it reaches the PHY |
| `nfapi/.../fapi_nr_ue_interface.h` | M2: `ss_id`/`coreset_id` in the UE DCI config and indication structs |
| `openair2/RRC/NR_UE/rrc_UE.c` | SIB1 reported before processing; random access is not started in passive mode |
| `executables/nr-uesoftmodem.c` | Calls `nr_sniffer_init()` so `--sniffer.*` options are registered |
| `CMakeLists.txt` | `add_subdirectory(nr-sniffer)` if the directory exists (binary dir `nr-sniffer-build`, executable next to `nr-softmodem`) |
| `common/utils/T/T_messages.txt` | New `GNB_MAC_DCI` T event (component B) |
| `openair2/LAYER2/NR_MAC_gNB/*` | `nr_mac_trace_dci()` emits `GNB_MAC_DCI` after each `fill_dci_pdu_rel15()` call (SIB/OSI, paging, Msg2, Msg3 retransmission, Msg4, C-RNTI DL and UL); `nr_cell_sched_t` remembers the slot being scheduled. A no-op unless the gNB runs with `--T_stdout 0/2` |
| `radio/rfsimulator/simulator.cpp` | rfsim server falls back to IPv4 when the kernel has no IPv6 (cloud containers); not sniffer-specific |

Because the hooks are weak symbols, they are NULL in every other OAI binary. `nr-uesoftmodem`
and `nr-softmodem` behave exactly as before (apart from the rfsim IPv4 fallback, which only
changes anything on hosts where the IPv6 bind used to fail), so the patch could be upstreamed.

## Data flow (milestone 1)

```
radio (rfsim / USRP E320 via UHD)
   │  time-domain IQ
   ▼
nrUE PHY  ── nr_initial_sync.c (PSS/SSS → PCI), nr_pbch.c (PBCH → MIB bits)
   │  FAPI SSB PDU
   ▼
NR_IF_Module.c handle_bcch_bch() → RRC MIB decode → MAC nr_ue_decode_mib() ──► nr_sniffer_on_mib()  → mib.csv
   │  MAC schedules Type0-PDCCH (SI-RNTI, CORESET#0 / SearchSpace#0)
   ▼
PHY dci_nr.c (PDCCH blind decode) → nr_dlsch_* (PDSCH/LDPC) → MAC → RRC
   └─ nr_rrc_ue_decode_NR_BCCH_DL_SCH_Message() ─────────────────────► nr_sniffer_on_sib1() → sib1.csv, sib1_pci<N>.xml
                                                  └─ passive: can_start_ra = false (no PRACH, ever)
```

## Data flow (milestone 2: following one UE)

The sniffer is a "shadow UE": instead of a second decoding path, OAI's own UE MAC/RRC is pointed at
another UE's RNTIs. The UE then decodes that UE's DCIs (and PDSCHs) as if they were its own. Nothing
is transmitted.

```
SIB1 ──► nr_sniffer_ra_configure(): RA-RNTIs of every PRACH occasion (prach-ConfigurationIndex, msg1-FDM)
          e.g. config 98, 30 kHz: 010b 010f 0113
MAC ue_dci_configuration(): RA search space monitored every occasion, ra_rnti = first tracked RNTI
PHY dci_nr.c: polar decode once per candidate; CRC remainder == any tracked RA-/TC-RNTI → accepted
   │                                         (24-bit remainder: false match ~K/2^24 per decode)
   ├─ DCI 1_0 on RA-RNTI ─► OAI schedules the RAR PDSCH ─► nr_sniffer_on_rar(): every subPDU's
   │                                                        TC-RNTI is tracked (rar.csv)
   ├─ DCI 1_0 on TC-RNTI ─► Msg4 PDSCH ─► MAC CE "UE contention resolution id"
   │                                       └─ nr_sniffer_on_contention_resolution(): adopt this TC-RNTI
   │                                          → nr_ra_succeeded(): C-RNTI := TC-RNTI, MAC UE_CONNECTED (ue.csv)
   │                                     └─ CCCH SDU = RRCSetup → OAI RRC applies masterCellGroup to this
   │                                        MAC: dedicated BWP, CORESETs, search spaces, PDSCH/PUSCH config
   └─ from then on OAI's normal C-RNTI path: USS hashing, DMRS scrambling, DCI sizes, TDA, TBS are the
      followed UE's ─► report_dci_to_sniffer() after every DCI 0_0/0_1/1_0/1_1 ─► dci.csv
UL: fapi_nr_ue_l1.c drops all UL config when passive (no PRACH, PUSCH, PUCCH, SRS), so the
    RRCSetupComplete, HARQ-ACKs, SRs and Msg3s that OAI's UE stack prepares are never sent.
```

Limits (M2): one UE, the first whose Msg4 is decoded. The sniffer must be running before that UE's
random access. A UE that is already connected can only be followed with a known configuration (M3).

## NR-Scope feature map

| NR-Scope feature | OAI building block (2026.w41) | Status / work needed |
|---|---|---|
| Cell search, MIB | `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c`, `nr_pbch.c`; MAC `nr_ue_decode_mib()` | **Done (M1)**, reused as is |
| Scan of all GSCN/SSB points (`nrscan`) | `nr_initial_sync.c` searches one frequency | M4: loop over the band's GSCN raster and re-tune between attempts |
| SIB1 decoding | MAC `ue_dci_configuration()` Type0 CSS; `dci_nr.c`; `nr_dlsch_decoding.c`; RRC BCCH decoder | **Done (M1)** |
| Other SIBs | `nr_decode_SI()` in `rrc_UE.c` already schedules SI windows | M3: add a hook in `nr_decode_SI()` |
| RACH tracking (RAR / TC-RNTI) | MAC `nr_ra_procedures.c` handles RA-RNTI/TC-RNTI only for *our own* preamble | **Done (M2)**: RA-RNTIs of all PRACH occasions from SIB1, RA search space always monitored, every RAR subPDU's TC-RNTI tracked (`nr_sniffer_ra.c`) |
| Msg4 / RRCSetup decoding | `rrc_UE.c` RRCSetup handling, CellGroupConfig parsing in `config_ue.c` | **Done (M2)** for one UE: OAI decodes Msg4 on the tracked TC-RNTI and its RRC applies the RRCSetup to the sniffer's MAC. M3: keep a per-RNTI copy of `NR_CellGroupConfig_t` for several UEs |
| DCI decoding for many UEs | `dci_nr.c: nr_dci_decoding_procedure()` checks the CRC against **one** RNTI per search-space PDU | M2: tracked-RNTI acceptance for RA/TC-RNTIs, one followed C-RNTI. M3: **main refactor**, see below |
| DCI → PRB/MCS/TBS per UE | MAC `nr_extract_dci_info()`, `nr_ue_process_dci_indication_pdu()`; TBS via `nr_compute_tbs()` | **Done (M2)** for the followed UE, by OAI's own DCI processing (`report_dci_to_sniffer()`) |
| 64QAM vs 256QAM MCS table | `pdsch-Config.mcs-Table` in the CellGroupConfig | Config option `--sniffer.mcs_table` (already parsed) |
| Known RRCReconfiguration (NR-Scope `fixed_rnti`) | RRCReconfiguration is ciphered | M3: `--sniffer.cellgroup_file` loads a CellGroupConfig (UPER/XER). On ExPECA it can be exported from our own gNB |
| Worker pool / circular slot buffer | nrUE already processes slots with an actor/thread pool (`executables/nr-ue.c`) | M3: give DCI search for tracked RNTIs its own actor queue so it can lag behind real time |
| Multiple USRPs / cells | one nrUE instance per process | Run one `nr-sniffer` per radio, each with its own `out_dir` |
| CSV logs | — | **Done**: MIB/SIB1 (M1), `dci.csv`, `rar.csv`, `ue.csv` (M2); schema below |
| BigQuery upload | — | Out of scope |
| PDCCH constellation GUI | OAI's own `nr_phy_scope` / imscope already show PDCCH | Reused via `-d` / `--imscope` |

## Main refactor: multi-RNTI DCI decoding

M2 implements step 2 below for the RA search space only, and follows one UE as "this" UE (above).
Steps 1 and 3 are M3: several UEs need several sets of dedicated configuration, so step 3 will have
to extract DCI fields per RNTI instead of reusing the single MAC instance's configuration.

`nr_dci_decoding_procedure()` (`openair1/PHY/NR_UE_TRANSPORT/dci_nr.c`) loops over the search-space
PDUs that the MAC configured for this slot. For each candidate it polar-decodes the payload and
accepts it only if the CRC residue equals that PDU's RNTI (`crc == n_rnti`).

For a sniffer the useful fact is that, after polar decoding, the CRC residue **is** the RNTI the gNB
scrambled with. So the plan is:

1. Have the MAC configure *one* search-space PDU per (CORESET, search space, aggregation level,
   DCI size) for the slot, not one per RNTI. UE-specific search-space positions depend on the RNTI
   (hashing in TS 38.213 §10.1), so the candidate set is the union over tracked RNTIs plus the
   common search spaces.
2. Decode each candidate once. Look the residue up in the tracked-RNTI table (hash map). Accept it
   if found, or if it is a common RNTI (SI/P/RA). Optionally keep "unknown RNTI" hits with a high
   polar metric as candidates for new UEs that were missed during RACH.
3. Report the DCI to a sniffer-specific MAC path. It extracts fields with that RNTI's
   CellGroupConfig, logs a row, and **does not** schedule PDSCH/PUSCH for the sniffer's own UE.

This keeps one PDCCH channel estimate and LLR computation per slot, which is what made NR-Scope fast
enough for 100 MHz cells.

## Ground truth from the gNB (component B)

OAI's T-tracer (`common/utils/T`) already emits `GNB_MAC_DL`/`GNB_MAC_UL` (rnti, frame, slot,
MCS, TBS) and the MAC PDU events. None of them has the DCI format, aggregation level, CCE, PRBs,
NDI, RV or HARQ process, so the patch adds one event:

```
GNB_MAC_DCI: frame, slot, rnti, rnti_type, dci_format, bwp_id, coreset_id, ss_id, aggregation_level,
             cce_index, prb_start, prb_len, mcs, mcs_table, tbs_bits, ndi, rv, harq_pid
```

It is emitted by `nr_mac_trace_dci()` (`gNB_scheduler_primitives.c`) right after each
`fill_dci_pdu_rel15()` call, so it carries exactly what goes into the DCI payload, plus the PRBs,
MCS table and TBS of the PDSCH/PUSCH PDU it schedules. `frame`/`slot` are the PDCCH slot (what the
sniffer sees), not the PDSCH/PUSCH slot. `prb_start` is relative to the BWP start. `rnti_type`
and `dci_format` are OAI's `nr_rnti_type_t` / `nr_dci_format_t`. The phy-test scheduler is not
instrumented.

`nr-gnb-dci-logger` (`nr-sniffer/tracer/`) is a T-tracer client modelled on OAI's `tracer/csv.c`.
It turns on only `GNB_MAC_DCI` and writes `gnb_dci.csv` in the shared DCI schema with
`source=gnb`. `nr_sniffer_dci.c`, which also builds into nr-sniffer, defines that schema. Usage:

```bash
./nr-softmodem -O gnb.conf --rfsim ... --T_stdout 2         # waits for a tracer before starting
./nr-gnb-dci-logger -d ../../../common/utils/T/T_messages.txt -o gnb_dci.csv
```

`scripts/gnb_dci_test.sh` runs gNB + logger + one `nr-uesoftmodem` (no core, see README) and checks
the log against the gNB's own view. It expects SI-RNTI DCIs, the RAR DCI on the RA-RNTI and the Msg4
DCI on the TC-RNTI that the gNB logs for the RA procedure. Per-UE DL and UL DCI counts must equal
the `dlsch_rounds`/`ulsch_rounds` totals the gNB prints, with a small allowance for DCIs sent after
its last statistics print. In the cloud runs they matched exactly (e.g. 83 DL / 791 UL).

## Output schemas

`mib.csv` (one row for the first MIB of a cell and whenever its content changes):

```
timestamp_us,sfn,ssb_index,half_frame,pci,scs_common_khz,ssb_subcarrier_offset,ssb_start_subcarrier,
dmrs_typeA_pos,coreset0,searchspace0,cell_barred,intra_freq_reselection,fr2,nb_mib_decoded
```

`sib1.csv` (first SIB1 and every content change; the full SIB1 is also written as `sib1_pci<N>.xml`):

```
timestamp_us,sfn,slot,pci,mcc,mnc,tac,nr_cell_id,cell_reserved_for_operator,band,offset_to_point_a,
scs_khz,carrier_bw_prb,offset_to_carrier,initial_dl_bwp_loc_bw,initial_dl_bwp_start_prb,
initial_dl_bwp_size_prb,initial_dl_bwp_scs_khz,searchspace_sib1,ra_searchspace,paging_searchspace,
nb_common_searchspaces,prach_config_index,msg1_fdm,msg1_freq_start,ra_response_window,
ssb_positions_in_burst,ssb_periodicity_ms,ss_pbch_block_power_dbm,tdd_ref_scs_khz,tdd_period,
tdd_dl_slots,tdd_dl_symbols,tdd_ul_slots,tdd_ul_symbols,si_scheduling_present,nb_sib1_decoded
```

`rar.csv` (M2, one row per RAR subPDU): `timestamp_us,sfn,slot,rapid,timing_advance,ul_grant,tc_rnti`

`ue.csv` (M2, the UE being followed): `timestamp_us,sfn,slot,c_rnti,contention_resolution_id`

`dci.csv` (M2) and `gnb_dci.csv` (component B) share one schema (`nr-sniffer/src/nr_sniffer_dci.h`).
`rnti` is hex, `rnti_type` is one of `c,cs,tc,p,si,ra,msgb,mcs-c`, `dci_format` is e.g. `1_0`,
`direction` is `dl`/`ul`, `mcs_table` is `qam64,qam256,qam64LowSE,tp-qam64,tp-qam64LowSE`,
`prb_start` is relative to the BWP, `tbs_bits` is the transport block size, and `source` is
`sniffer` or `gnb`:

```
timestamp_us,sfn,slot,rnti,rnti_type,dci_format,direction,bwp_id,coreset_id,ss_id,aggregation_level,
cce_index,prb_start,prb_len,mcs,mcs_table,tbs_bits,ndi,rv,harq_id,source
```

## Passive operation

- `--sniffer.passive 1` (default) stops the UE stack from starting random access, so no PRACH is ever
  sent. Since M2 every UL configuration (PUSCH, PUCCH, PRACH, SRS) is also dropped in
  `fapi_nr_ue_l1.c` before it reaches the PHY, because a followed UE makes OAI's UE stack want to
  send RRCSetupComplete, HARQ-ACKs, SRs and so on. `rfsim_test.sh` checks the gNB log for any RA
  attempt. `dci_test.sh` checks that the gNB detected exactly as many PRACHs as the real UE sent,
  that the sniffer sent none, and that the gNB created a single UE context.
- In RF-simulator mode the UE still sends zero-valued samples back to the gNB, because rfsim needs
  them to keep the two sides in time sync. They carry no energy.
- **On a USRP (ExPECA E320) the TX chain must be disabled outright.** This is M4 work: open the
  device RX-only, or force TX gain to 0 and never call the write path. Until then, don't connect a
  TX antenna when running on hardware.

## Configuration

All options can go on the command line (`--sniffer.<name> value`) or in a `sniffer = { ... };`
section of the `-O` config file. See `nr-sniffer/conf/nr-sniffer.conf`.

| Option | Default | Meaning |
|---|---|---|
| `out_dir` | `./sniffer-logs` | Directory for CSV/XML output |
| `passive` | `1` | `0` lets the UE attach normally (useful for debugging with a SIM) |
| `pci` | `-1` | Only log this PCI |
| `mcs_table` | `qam256` | MCS table assumed with a known configuration (M3). In M2 the MCS table comes from the decoded RRCSetup (`pdsch-Config.mcs-Table`) |
| `dump_xml` | `1` | Write each new SIB1 as XER |
| `follow_ue` | `1` | Follow the first UE seen in random access and log its DCIs; `0` is M1 behaviour (MIB/SIB1 only) |

Tuning uses the standard nrUE options: `-C <DL centre Hz> -r <PRBs> --numerology <mu> --band <n> [--ssb <start subcarrier>]`.

## Milestone 2 results and limits

`scripts/dci_test.sh` (gNB + `nr-gnb-dci-logger` + sniffer + one `nr-uesoftmodem`, no core) in the
cloud VM, three runs of 10-20 s. The sniffer decoded the UE's RAR and Msg4 and followed it. It missed
0 of the 298 / 554 / 575 gNB DCIs for the UE, had 0 false positives, and summed exactly the same
scheduled bits as the gNB. All 15 compared fields were equal except, in one run, one `ss_id` (below).

- **Only DCI 0_0/1_0 are tested.** Without a core, the gNB never sends RRCReconfiguration, so it keeps
  the UE on common search space 4 (CORESET 0). Search space 5 with DCI 0_1/1_1 is in the RRCSetup, and
  the sniffer monitors it, but the gNB moves the UE there only after RRCReconfigurationComplete. The
  0_1/1_1 path (`report_dci_to_sniffer()` in `nr_ue_process_dci_dl_11/ul_01`) still needs a run with
  a core (ExPECA) or with a known configuration (M3).
- **RRCReconfiguration** (ciphered once a core is attached) can change the DCI 0_1/1_1 sizes, for
  example through antenna ports or the CSI request field. The sniffer keeps the RRCSetup
  configuration, so with a core it may lose the UE after reconfiguration until M3's
  `cellgroup_file` gives it the reconfigured CellGroupConfig.
- **Search-space attribution:** right after Msg4 the gNB can already use SS 4, which the RRCSetup
  defines, while the sniffer's RRC has not yet applied the RRCSetup. The sniffer then finds the same PDCCH candidate (same
  CORESET 0 CCEs) through its fallback CSS 1 and logs `ss_id` 1. The DCI itself is identical.

## ExPECA notes

- ExPECA's SDR nodes are USRP **E320**s driven over the network from containers on the testbed's
  Kubernetes workers. Build with `EXTRA_TARGETS=oai_usrpdevif` and run with
  `--usrp-args "type=e3xx,addr=<sdr ip>"`.
- The testbed is in a shielded underground hall, so the only cells visible are the ones
  experimenters run. The sniffer should be pointed at a gNB you reserved, with matching `-C`/`-r`.
- Because the gNB is ours, the gNB config gives everything the encrypted RRCReconfiguration
  would hide (M3, `cellgroup_file`), and T-tracer gives exact ground truth. This is a big advantage
  over sniffing commercial cells.
