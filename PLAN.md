# OAI NR Sniffer: an NR-Scope-style 5G SA telemetry tool for OpenAirInterface5G on ExPECA

## Goal

Base: **OAI 2026.w41** (tag `2026.w41` = `v3.0.0`), pinned in `OAI_VERSION`.


Build a passive 5G SA telemetry tool on top of OpenAirInterface5G (OAI), modelled on
[NR-Scope](https://github.com/PrincetonUniversity/NR-Scope) (srsRAN-based, CoNEXT 2024).
Pair it with a gNB-side ground-truth logger, so that on the ExPECA testbed (KTH R1, OAI gNB
on USRP E320) we can measure how many DCIs the sniffer misses.

Three components:

| # | Component | What it does |
|---|-----------|--------------|
| A | `nr-sniffer` (passive) | Cell search, MIB, SIB1, RACH/Msg4 tracking, multi-RNTI DCI decoding, per-UE PRB/MCS/TBS, CSV output |
| B | gNB ground-truth logger | Logs every DCI the OAI gNB schedules (RNTI, format, PRBs, MCS, TBS, HARQ) in the same CSV schema |
| C | Evaluation tool | Joins A and B on (frame, slot, RNTI) and reports DCI miss rate, false positives and throughput error |

## Important constraints

- **Naming:** OAI already has a GUI called the "nr scope" (`openair1/PHY/TOOLS/nr_phy_scope*`).
  Call the new tool `nr-sniffer` to avoid confusion.
- **Licensing:** NR-Scope is AGPL-3.0 and OAI uses the OAI Public License. **Do not copy NR-Scope code.**
  Use NR-Scope and its paper as a functional reference only, and reimplement everything using OAI's own PHY/RRC code.
- **No radio hardware in the cloud:** All development testing uses OAI's RF simulator
  (gNB plus `nr-uesoftmodem` plus `nr-sniffer` as an extra rfsim client). Over-the-air validation
  on ExPECA's E320s is done manually by the user.
- **Encrypted RRCReconfiguration:** As with NR-Scope's `fixed_rnti` branch, the sniffer can't read
  dedicated configuration sent after security activation. On ExPECA we control the gNB, so the
  sniffer must accept a known CellGroupConfig/RRCReconfiguration (exported from the gNB config)
  through its config file.
- **MCS table:** Must be configurable (64QAM vs 256QAM), as NR-Scope's `srsran` vs `main` branches show.

## Architecture (component A)

Reuse OAI nrUE building blocks rather than writing DSP from scratch:

1. **Radio input**: OAI radio abstraction (`radio/`): USRP (E320 via UHD), rfsimulator, and an
   IQ-file replay for offline tests.
2. **Cell search + MIB**: nrUE initial sync (PSS/SSS/PBCH) and the MIB decoder.
3. **SIB1**: Type0-PDCCH CSS in CORESET#0 with SI-RNTI, then PDSCH decode and ASN.1 decode of SIB1
   (gives TDD pattern, initial BWP, RACH config). Other SIBs come later.
4. **RACH tracking**: monitor RA-RNTIs → decode RAR → learn TC-RNTI → decode Msg4 (RRCSetup) on
   TC-RNTI → extract CellGroupConfig (dedicated BWP, CORESETs, search spaces) → register the C-RNTI.
5. **Multi-RNTI DCI decoding**: OAI's UE PHY assumes one RNTI. Refactor PDCCH blind decoding so that
   a single PDCCH demodulation per slot is checked against a set of RNTIs and their search spaces
   (CRC-unmasking per candidate). Formats: 0_0, 1_0, 0_1, 1_1.
6. **Worker pool**: slot buffers are handed to worker threads (like NR-Scope's circular buffer plus workers),
   so decoding doesn't have to finish within one slot.
7. **Output**: CSV per cell, using the schema below. Remote upload is out of scope.

## Component B: ground-truth logger

Prefer OAI's existing **T-tracer** (`common/utils/T`) over patching the scheduler. The gNB already
emits MAC/PHY trace events. Write a small T-tracer client that subscribes to the DL/UL DCI and
scheduling events and writes the same CSV schema. Patch the gNB MAC
(`openair2/LAYER2/NR_MAC_gNB/`) only if a needed field isn't traced.

## CSV schema (shared by A and B)

```
timestamp_us,sfn,slot,rnti,rnti_type,dci_format,direction,bwp_id,coreset_id,ss_id,aggregation_level,
cce_index,prb_start,prb_len,mcs,mcs_table,tbs_bits,ndi,rv,harq_id,source
```

`source` is `sniffer` or `gnb`.

## Milestones

### Milestone 1: design + build + MIB/SIB1 (first cloud session)

1. Write `doc/nr-sniffer/DESIGN.md`. It maps every NR-Scope feature to the OAI functions/files that
   will be reused or refactored, and lists the refactors needed for multi-RNTI decoding.
2. Add an `nr-sniffer` executable target to the CMake build (`--build-lib`-style option in `build_oai`
   is not required; a CMake target is enough).
3. Implement cell search + MIB + SIB1 decode with RF-simulator input, plus a config file
   (`nr-sniffer.conf`: frequency/ARFCN, SCS, bandwidth, PCI filter, MCS table, gain, output dir).
4. Add `ci/nr-sniffer/rfsim_test.sh`: start an OAI gNB (rfsim, band 78, 106 PRB, 30 kHz),
   start `nr-sniffer` as an rfsim client, and assert that MIB and SIB1 fields match the gNB config.
5. Stretch goal: the T-tracer ground-truth logger (component B).

**Done when:** the test script passes in the cloud VM, and DESIGN.md is committed.

### Milestone 2: RACH/Msg4 tracking + DCI decoding for one UE
- Single `nr-uesoftmodem` attaching through rfsim; sniffer logs its DCIs. Compare against component B.

### Milestone 3: multi-UE, worker pool, evaluation tool (component C)
- Several rfsim UEs; miss-rate report; known-RRCReconfiguration support.

### Milestone 4: ExPECA deployment
- Dockerfile based on ExPECA's OAI image; E320 config (`type=e3xx`); run notes for reserving an
  SDR + worker node via the KTH-EXPECA/examples notebooks; over-the-air validation by the user.

## Cloud VM notes

- Up to 4 vCPU / 16 GB RAM / 30 GB disk, Ubuntu 24.04, no GPU (one session observed only 2 vCPU / 7 GB, so a full build can take over an hour).
- Build only what's needed: `./build_oai -I` (dependencies), then `--gNB --nrUE -w SIMU` plus the new target.
  Dependency install takes longer than 5 minutes, so it won't be cached by an environment setup script.
  Install it in-session and keep the build directory.
- Use `-j4`. Avoid building with USRP support in the cloud (UHD not needed for rfsim).
