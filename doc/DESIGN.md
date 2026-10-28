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
├── scripts/build.sh                build nr-sniffer + gNB + UE + rfsim
└── scripts/rfsim_test.sh           end-to-end test against an OAI gNB in RF simulator
```

`nr-sniffer` is a separate executable. It is compiled from the same sources as `nr-uesoftmodem`
plus `nr-sniffer/src/*.c`, and linked against the same OAI libraries. The OAI patch is
deliberately small:

| File | Change |
|------|--------|
| `common/utils/nr_sniffer_hooks.h` (new) | Weak hook declarations: `nr_sniffer_init`, `nr_sniffer_on_mib`, `nr_sniffer_on_sib1`, `nr_sniffer_is_passive` |
| `openair2/LAYER2/NR_MAC_UE/nr_ue_procedures.c` | `nr_ue_decode_mib()` reports the decoded MIB plus PCI/SSB index |
| `openair2/RRC/NR_UE/rrc_UE.c` | SIB1 reported before processing; random access is not started in passive mode |
| `executables/nr-uesoftmodem.c` | Calls `nr_sniffer_init()` so `--sniffer.*` options are registered |
| `CMakeLists.txt` | `add_subdirectory(nr-sniffer)` if the directory exists (binary dir `nr-sniffer-build`, executable next to `nr-softmodem`) |
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

## NR-Scope feature map

| NR-Scope feature | OAI building block (2026.w41) | Status / work needed |
|---|---|---|
| Cell search, MIB | `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c`, `nr_pbch.c`; MAC `nr_ue_decode_mib()` | **Done (M1)**, reused as is |
| Scan of all GSCN/SSB points (`nrscan`) | `nr_initial_sync.c` searches one frequency | M4: loop over the band's GSCN raster and re-tune between attempts |
| SIB1 decoding | MAC `ue_dci_configuration()` Type0 CSS; `dci_nr.c`; `nr_dlsch_decoding.c`; RRC BCCH decoder | **Done (M1)** |
| Other SIBs | `nr_decode_SI()` in `rrc_UE.c` already schedules SI windows | M2: add a hook in `nr_decode_SI()` |
| RACH tracking (RAR / TC-RNTI) | MAC `nr_ra_procedures.c` handles RA-RNTI/TC-RNTI only for *our own* preamble | M2: new passive RA monitor that blind-decodes DCI 1_0 with every RA-RNTI valid for the PRACH occasions in SIB1, parses every RAR MAC subPDU and collects TC-RNTIs |
| Msg4 / RRCSetup decoding | `rrc_UE.c` RRCSetup handling, CellGroupConfig parsing in `config_ue.c` | M2: decode PDSCH for each TC-RNTI with DCI 1_0, extract the RRCSetup and keep a per-RNTI copy of `NR_CellGroupConfig_t` |
| DCI decoding for many UEs | `dci_nr.c: nr_dci_decoding_procedure()` checks the CRC against **one** RNTI per search-space PDU | M2/M3: **main refactor**, see below |
| DCI → PRB/MCS/TBS per UE | MAC `nr_extract_dci_info()`, `nr_ue_process_dci_indication_pdu()`; TBS via `nr_compute_tbs()` | M2: call the DCI field extraction with the per-RNTI config, without acting on the grant |
| 64QAM vs 256QAM MCS table | `pdsch-Config.mcs-Table` in the CellGroupConfig | Config option `--sniffer.mcs_table` (already parsed) |
| Known RRCReconfiguration (NR-Scope `fixed_rnti`) | RRCReconfiguration is ciphered | M3: `--sniffer.cellgroup_file` loads a CellGroupConfig (UPER/XER). On ExPECA it can be exported from our own gNB |
| Worker pool / circular slot buffer | nrUE already processes slots with an actor/thread pool (`executables/nr-ue.c`) | M3: give DCI search for tracked RNTIs its own actor queue so it can lag behind real time |
| Multiple USRPs / cells | one nrUE instance per process | Run one `nr-sniffer` per radio, each with its own `out_dir` |
| CSV logs | — | **Done (M1)** for MIB/SIB1; DCI schema below |
| BigQuery upload | — | Out of scope |
| PDCCH constellation GUI | OAI's own `nr_phy_scope` / imscope already show PDCCH | Reused via `-d` / `--imscope` |

## Main refactor: multi-RNTI DCI decoding

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

OAI's T-tracer (`common/utils/T`) already emits `GNB_MAC_DL_PDU_WITH_DATA`,
`GNB_MAC_UL_PDU_WITH_DATA`, `GNB_MAC_DL_RAR_PDU_WITH_DATA` and related events. A small T-tracer
client (`nr-sniffer/tools/gnb_truth`, M2) writes these into the same CSV schema. If a needed field
(for example CCE index or aggregation level) is not traced, add one T event in
`openair2/LAYER2/NR_MAC_gNB/nr_mac_gNB.c` / the DCI fill functions.

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

`dci.csv` (M2, shared with the gNB ground-truth logger):

```
timestamp_us,sfn,slot,rnti,rnti_type,dci_format,direction,bwp_id,coreset_id,ss_id,aggregation_level,
cce_index,prb_start,prb_len,mcs,mcs_table,tbs_bits,ndi,rv,harq_id,source
```

## Passive operation

- `--sniffer.passive 1` (default) stops the UE stack from starting random access, so no PRACH is ever
  sent. The RF-simulator test checks the gNB log for any RA attempt.
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
| `mcs_table` | `qam256` | MCS table assumed for DCI decoding (M2) |
| `dump_xml` | `1` | Write each new SIB1 as XER |

Tuning uses the standard nrUE options: `-C <DL centre Hz> -r <PRBs> --numerology <mu> --band <n> [--ssb <start subcarrier>]`.

## ExPECA notes

- ExPECA's SDR nodes are USRP **E320**s driven over the network from containers on the testbed's
  Kubernetes workers. Build with `EXTRA_TARGETS=oai_usrpdevif` and run with
  `--usrp-args "type=e3xx,addr=<sdr ip>"`.
- The testbed is in a shielded underground hall, so the only cells visible are the ones
  experimenters run. The sniffer should be pointed at a gNB you reserved, with matching `-C`/`-r`.
- Because the gNB is ours, the gNB config gives everything the encrypted RRCReconfiguration
  would hide (M3, `cellgroup_file`), and T-tracer gives exact ground truth. This is a big advantage
  over sniffing commercial cells.
