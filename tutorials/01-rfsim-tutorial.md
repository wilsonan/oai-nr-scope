# Tutorial 01: nr-sniffer with the OAI RF simulator

In this tutorial you run a complete 5G NR cell on one Linux machine and watch `nr-sniffer` decode
it from the outside:

- an OAI gNB (`nr-softmodem`);
- one or more OAI UEs (`nr-uesoftmodem`);
- the passive sniffer (`nr-sniffer`);
- the gNB's own record of what it scheduled (`nr-gnb-dci-logger`), so you can check the sniffer
  against the truth.

No radio hardware and no core network are needed. Everything is connected through OAI's RF
simulator, which carries the I/Q samples over TCP sockets on localhost.

You will learn:

1. what the sniffer does and how the pieces connect;
2. how to build it;
3. how to run it in one command, then terminal by terminal;
4. what each output means, and how to tell whether the sniffer decoded correctly;
5. how to generate real traffic (phy-test mode);
6. how to follow a UE that connected before the sniffer started.

[02-output-reference.md](02-output-reference.md) describes every file and column in detail.

---

## 1. Concepts in five minutes

### What a 5G cell tells the world

A gNB tells the UEs what to do through two kinds of message on the downlink.

**Broadcast information (to everyone).**
- The **MIB**, sent in the SSB, gives the cell's basic numerology and where to find SIB1.
- **SIB1** gives the PLMN, the band and bandwidth, the TDD pattern and the random-access
  configuration (PRACH).

Anyone can decode these.

**DCIs (Downlink Control Information) on the PDCCH.** Each DCI is a short scheduling command, such as:

> "UE 0x270a: in this slot, PRBs 0 to 4 carry your downlink data, MCS 9, HARQ process 3".

- A DCI is not encrypted, but its CRC is scrambled with the target UE's **RNTI**, a 16-bit identifier.
- It can only be found by searching the right **search spaces**, which sit inside **CORESETs**.
- UE-specific search spaces are configured by RRC messages that the UE receives once it connects.

| Format | Meaning |
|---|---|
| 1_0 / 1_1 | downlink assignment |
| 0_0 / 0_1 | uplink grant |

The `_0` formats are the fallback formats used in common search spaces. The `_1` formats are the
full formats used once the UE is configured.

### What the sniffer does

`nr-sniffer` is a modified OAI UE that **never transmits**. It does the following:

1. Finds the cell (SSB/MIB) and decodes SIB1, like any UE.
2. Watches **random access**:
   - It computes the RA-RNTIs from the PRACH configuration in SIB1.
   - It decodes every **RAR** (Msg2), which hands a new UE its temporary C-RNTI (TC-RNTI).
   - It decodes **Msg4**, which carries the contention resolution and the UE's RRCSetup with its
     cell-group configuration (the search spaces, CORESETs and BWPs).
3. Creates a **shadow MAC** for each UE it saw connect. A shadow MAC is a UE MAC instance that holds
   that UE's configuration. From then on the sniffer searches the PDCCH for that UE's DCIs in that
   UE's search spaces, and decodes and logs every one.
4. Writes everything to CSV files in its output directory.

If the sniffer misses a UE's random access, for example because it started late, it can still follow
the UE when you give it the UE's RNTI and cell-group configuration (section 7).

### How you know it is right

The gNB has been patched to emit a **T-tracer** event for every DCI it sends.
`nr-gnb-dci-logger` writes those events to `gnb_dci.csv`, in exactly the same format as the
sniffer's `dci.csv`.

`scripts/compare_dci.py` then matches the two files slot by slot and reports, per UE:

- **missed DCIs**: sent by the gNB, not found by the sniffer;
- **false positives**: found by the sniffer, never sent;
- **field mismatches**: matched, but a field such as PRB, MCS or TBS differs;
- whether the sniffer stayed **passive**: it must send nothing, and the gNB must see only the real
  UEs' PRACH.

### How the processes connect

```
               RF simulator (TCP, localhost:4043)
  nr-softmodem ────────────── server ──────────────┐
    (gNB)        ▲                ▲                ▲
    │            │ DL+UL samples  │ DL, no UL*     │ DL+UL samples
    │       nr-uesoftmodem    nr-sniffer      nr-uesoftmodem
    │        (UE 1)            (passive)        (UE 2)
    │ T-tracer (TCP 2021)          │
    ▼                              ▼
  nr-gnb-dci-logger          sniffer/dci.csv, mib.csv, sib1.csv, rar.csv, ue.csv
    gnb_dci.csv, cellgroups/
                 └──────── compare_dci.py ────────┘ → report.md
```

\* The sniffer connects to the RF simulator like any UE, but drops every uplink transmission
(PRACH, PUCCH, PUSCH) before the PHY. `compare_dci.py` checks this from the gNB's PRACH count.

---

## 2. Build (once)

You need Ubuntu 22.04 or 24.04 (or similar), about 10 GB of disk and a few CPU cores. The commands
below assume the repository is in `~/oai-nr-scope`. Run them as a user with sudo; OAI's dependency
installer needs it.

```bash
cd ~/oai-nr-scope
```

```bash
scripts/setup_oai.sh
```

This clones OAI (tag 2026.w41) into `./oai` and applies `oai-patches/0001-nr-sniffer-hooks.patch`.

```bash
(cd oai/cmake_targets && ./build_oai -I)
```

This installs OAI's build dependencies. If a package fails, see the manual fixes in the top-level
README.

```bash
scripts/build.sh
```

This builds `nr-softmodem`, `nr-uesoftmodem`, `nr-sniffer`, `nr-gnb-dci-logger` and the unit tests,
with the RF simulator, into `oai/cmake_targets/ran_build/build/`.

```bash
scripts/unit_tests.sh
```

This runs the C and Python unit tests, which take a few seconds and need no simulator. The last line
should say all tests passed.

---

## 3. The one-command demo

```bash
scripts/rfsim_demo.sh --ues 2
```

What happens:

1. **Run directory.** `./rfsim-demo/` is created. `gnb.conf` is OAI's rfsim band-78 106-PRB gNB
   config, with the core-network addresses set to 127.0.0.1 so the gNB starts without a core.
2. **Ground-truth logger.** `nr-gnb-dci-logger` starts. It keeps retrying until the gNB is up.
3. **gNB.** The gNB starts with `--T_stdout 2` and **waits until the T-tracer logger connects**,
   then starts the cell.
4. **Sniffer.** `nr-sniffer` starts. The script waits for `Watching random access`, which means MIB
   and SIB1 are decoded.
5. **UEs.** Two UEs start, 3 s apart. Each does random access and reaches RRC_CONNECTED.
6. **Live view.** The live view, `scripts/sniffer_live.py`, refreshes every second.

When both UEs appear in the live view, about 20 to 40 s after the start, press **Ctrl-C**. The script
then:

- stops the UEs, the sniffer, the gNB and the logger, in that order;
- writes `rfsim-demo/report.md` and `rfsim-demo/per_ue.csv`;
- prints the checks, ending with `RESULT: PASS`.

Options:

| Option | Effect |
|---|---|
| `--ues N` | number of UEs (default 2; the sniffer follows up to 14) |
| `--phytest` | phy-test mode with continuous traffic (section 6) |
| `--out DIR` | run directory instead of `./rfsim-demo` |
| `GNB_ARGS="..."` | extra gNB options, such as `-m 16 -M 80` for phy-test MCS and PRBs |
| `SNIFFER_ARGS="..."` | extra sniffer options, such as `--sniffer.ue_timeout 3` |

---

## 4. Terminal by terminal

Running each program in its own terminal lets you watch each log. This is also the way to point the
sniffer at your own gNB configuration.

### Common setup

Do this in **every terminal**:

```bash
cd ~/oai-nr-scope
eval "$(scripts/rfsim_conf.sh ~/nr-sniffer-run)"
cd "$RUN"
```

`rfsim_conf.sh` creates the run directory with a core-less `gnb.conf`, and sets these shell
variables:

| Variable | Value |
|---|---|
| `$RUN` | the run directory |
| `$B` | the directory with the binaries |
| `$T_DB` | the T-tracer event database |
| `$GNB_CONF_RFSIM` | the gNB config |
| `$TUNE` | the tuning options for the UE and the sniffer |

`$TUNE` is derived from the gNB config. For the default config it is:

```
-r 106 --numerology 1 --band 78 -C 3319680000 --rfsim --rfsimulator.[0].serveraddr 127.0.0.1
```

To use another rfsim gNB config, pass it as a second argument:
`scripts/rfsim_conf.sh ~/nr-sniffer-run my-gnb.conf`.

Start the programs in this order.

### Terminal 1: ground-truth logger

```bash
mkdir -p cellgroups
"$B/nr-gnb-dci-logger" -d "$T_DB" -o gnb_dci.csv -c cellgroups
```

It prints `connect: Connection refused / trying again in 1s` until the gNB is up, then
`connected to 127.0.0.1:2021`.

- Every DCI the gNB sends goes to `gnb_dci.csv`.
- Every CellGroupConfig the gNB gives a UE goes to `cellgroups/` as a file you can feed to the
  sniffer (section 7).

### Terminal 2: gNB

```bash
"$B/nr-softmodem" -O "$GNB_CONF_RFSIM" --rfsim --rfsimulator.[0].serveraddr server --T_stdout 2 2>&1 | tee gnb.log
```

`--T_stdout 2` turns on the T-tracer and keeps the normal log on the console. The gNB blocks until
the logger connects. You then see `Running as server waiting opposite rfsimulators to connect`, and
the cell is live.

Without a core you also see NGAP/SCTP connection errors. They are expected and harmless: the gNB
still broadcasts SIB1 and accepts UEs up to RRC_CONNECTED.

### Terminal 3: sniffer

```bash
"$B/nr-sniffer" $TUNE --sniffer.out_dir sniffer 2>&1 | tee sniffer.log
```

`$TUNE` is not quoted on purpose: it holds several options.

Within a few seconds you should see:

```
[SNIFFER] Found cell: PCI 0, MIB SFN 490, SSB idx 0, SCS common 30 kHz, k_SSB 0, CORESET0 11, SearchSpace0 0, not barred
[SNIFFER] SIB1 decoded: PCI 0, PLMN 208/99, TAC 1, NCI 0x000bc614e, band n78, 106 PRB @ 30 kHz, initial BWP 0+106 PRB, PRACH idx 98, TDD 5ms 7D/2U
[SNIFFER] Watching random access: PRACH config 98, 3 RA-RNTIs: 010b 010f 0113 (following up to 8 UEs)
[SNIFFER] stats at 674.17: 2000 slots, 1 DCIs logged, 0 UEs followed, 1.00 PDCCH configurations per slot (0.00 more merged into them)
```

The sniffer is now ready, but nothing is happening in the cell yet.

### Terminal 4: live view

```bash
python3 ~/oai-nr-scope/scripts/sniffer_live.py sniffer
```

### Terminal 5 (and 6, 7, …): UEs

```bash
"$B/nr-uesoftmodem" -O ~/oai-nr-scope/oai/ci-scripts/conf_files/nrue.uicc.conf $TUNE --uicc0.imsi 208990100001101 2>&1 | tee ue1.log
```

For a second UE, use another terminal, `--uicc0.imsi 208990100001102` and `ue2.log`. Each UE needs
its own IMSI, otherwise the gNB treats the second one as the first one reconnecting.

As each UE attaches, the sniffer prints:

```
[SNIFFER] RAR at 720.10: RAPID 41, TA 0, TC-RNTI 270a
[SNIFFER] Msg4 at 722.1: following UE with C-RNTI 270a (shadow MAC 1, 1 UEs followed)
```

The UE prints `4-Step RA procedure succeeded` and `State = NR_RRC_CONNECTED`. The gNB prints
`Received RRCSetupComplete (RRC_CONNECTED reached)`.

### Stopping and evaluating

Press Ctrl-C in this order: **UEs → sniffer → gNB → logger**. Then, in `$RUN`:

```bash
python3 ~/oai-nr-scope/scripts/compare_dci.py gnb.log gnb_dci.csv sniffer --report report.md --report-csv per_ue.csv
```

Stopping the UEs first matters. A UE that is still running when the sniffer stops keeps receiving
DCIs that the sniffer can no longer log. `compare_dci.py` only counts DCIs up to the sniffer's last
logged slot, but stopping the UEs first gives the cleanest numbers.

---

## 5. What you are looking at

### The live view

```
10:23:19  ~/nr-sniffer-run/sniffer/dci.csv  (last 1 s)
Cell: PCI 0, SCS 30 kHz, CORESET#0 11, SearchSpace#0 0, not barred
      PLMN 208/99, TAC 1, band n78, 106 PRB, TDD 5ms 7D/2U, PRACH config 98
Random access: 2 RAR subPDUs decoded; following 2 UEs: 0x270a (RA), 0x5bad (RA)

RNTI     type  formats    DL DCI  UL DCI  DL PRB  UL PRB DL MCS UL MCS  reTx  DL Mbit/s  UL Mbit/s
0x270a   c     0_0,1_0         1      11     5.0     5.0    0.0    0.0     0      0.000      0.001
0x5bad   c     0_0,1_0         1      10     5.0     5.0    0.0    0.0     0      0.000      0.001
```

**Header lines**

| Line | Meaning |
|---|---|
| `Cell:` | from the MIB: the cell's PCI, subcarrier spacing, where CORESET#0 and SearchSpace#0 are (indexes into the 38.213 tables) and whether the cell is barred |
| second line | from SIB1: PLMN (MCC/MNC 208/99 is OAI's test network), tracking area, band, bandwidth, TDD pattern (5 ms with 7 downlink and 2 uplink slots) and PRACH configuration |
| `Random access:` | how many RAR subPDUs the sniffer decoded, and the UEs it follows now. `(RA)` means learned from the UE's random access, `(known)` means given with `--sniffer.cellgroup_file` |

**Table columns** (one row per RNTI and RNTI type, over the last second)

| Column | Meaning |
|---|---|
| `type` | `c`: C-RNTI (a connected UE). `ra`: RA-RNTI (a RAR). `tc`: TC-RNTI (Msg4, during random access). `si`: SI-RNTI (SIB1) |
| `formats` | the DCI formats seen. `0_0,1_0` before RRCReconfiguration, `0_1,1_1` after it (or in phy-test) |
| `DL DCI` / `UL DCI` | downlink assignments and uplink grants decoded |
| `DL PRB` / `UL PRB` | mean number of PRBs allocated per DCI |
| `DL MCS` / `UL MCS` | mean MCS index (the modulation and coding scheme; higher means more bits per PRB) |
| `reTx` | DCIs with redundancy version ≠ 0, which are HARQ retransmissions |
| `DL Mbit/s` / `UL Mbit/s` | the transport-block bits scheduled per second, retransmissions included |

You only see `ra`, `tc` and `si` rows at the moments they occur: SIB1 at start-up, RAR and Msg4
during each attach. Use `--once` to summarise a whole run instead of the last second:
`python3 scripts/sniffer_live.py ~/nr-sniffer-run/sniffer --once`.

### Why the numbers look like this

**SA mode without a core (sections 3 and 4).**
- Each UE gets about 10 UL grants per second of 5 PRB at MCS 0, and about one DL assignment per
  second.
- The UL grants are the gNB scheduling the UE so that it can send RRC and NAS messages and buffer
  status reports. There is no user traffic: without a core the UE never gets a PDU session.
- The formats stay 0_0/1_0, because the gNB only sends the RRCReconfiguration that moves the UE to
  1_1/0_1 after the core accepts it.
- So "0.001 Mbit/s" is correct: this is signalling only.

**Phy-test (section 6).**
- The gNB schedules the preconfigured UE in every downlink and uplink slot, with DCI 1_1 / 0_1, at
  the MCS and PRBs you choose. Here you see Mbit/s.

**Mbit/s are per wall-clock second.**
- The RF simulator is not tied to real time. On a loaded machine it runs slower than the air
  interface would. The ratio between DL and UL and between UEs is meaningful; the absolute value is
  only approximate.
- `compare_dci.py` compares the gNB and the sniffer over the same slots, so its throughput error is
  exact.

### The report

`report.md` from a two-UE demo:

| C-RNTI | followed via | gNB DCIs | missed | miss rate | false positives | field mismatches | seconds | DL Mbit/s gNB / sniffer (error) | UL Mbit/s gNB / sniffer (error) |
|---|---|---|---|---|---|---|---|---|---|
| 0x270a | random access | 3095 | 0 | 0.00% | 0 | 0 | 258.65 | 0.000 / 0.000 (+0.00%) | 0.002 / 0.002 (+0.00%) |
| 0x5bad | random access | 2619 | 0 | 0.00% | 0 | 0 | 229.59 | 0.000 / 0.000 (+0.00%) | 0.001 / 0.001 (+0.00%) |

```
PASS RAR: sniffer decoded a RAR with TC-RNTI 0x270a, 0x5bad
PASS Msg4: sniffer follows C-RNTI 0x270a, 0x5bad, gNB TC-RNTI 0x270a, 0x5bad
PASS passive: gNB detected 2 PRACH, the UEs sent 2, the sniffer sent 0; gNB UE contexts 2
PASS DCI miss rate: 0/5714 gNB DCIs missed (0.00%; 526 DL, 5188 UL, 2 UEs)
PASS false positives: 0 sniffer DCIs with no gNB DCI
PASS DCI fields: 5714 matched DCIs: all 15 fields equal
INFO scheduled bits for 2 UEs: gNB 780016, sniffer 780016 (+0.00%)
RESULT: PASS
```

What each check proves:

| Check | What it proves |
|---|---|
| `RAR` | the sniffer found the RA-RNTIs from SIB1 and decoded the gNB's random-access responses |
| `Msg4` | the sniffer decoded each UE's Msg4, took its configuration from the RRCSetup and followed it: every UE that completed random access at the gNB is followed |
| `passive` | the gNB saw exactly as many PRACH preambles as the real UEs sent, and created one UE context per real UE. The sniffer sent nothing |
| `DCI miss rate` | of all DCIs the gNB sent to these UEs, from each UE's RAR to the end of the sniffer's log, how many the sniffer did not find. This is the main metric. On the RF simulator, with a clean channel, it should be 0 |
| `false positives` | sniffer DCIs the gNB never sent. A non-zero value means a CRC false alarm or a wrongly assumed configuration |
| `DCI fields` | for matched DCIs, all 15 compared fields are equal: format, direction, BWP, CORESET, search space, aggregation level, CCE, PRB start and length, MCS, MCS table, TBS, NDI, RV, HARQ ID |

`per_ue.csv` holds the same per-UE numbers for scripts and spreadsheets.

---

## 6. Traffic: phy-test mode

To see the sniffer decode real data scheduling, with DCI 1_1/0_1 at high rate, use OAI's phy-test
mode:

- The gNB and UE skip random access and RRC.
- Both load the same preconfigured RRCReconfiguration (`reconfig.raw`, written by the gNB at start-up).
- The gNB schedules RNTI 0x1234 in every slot.

The sniffer is given the same file. This is the known-configuration mode of section 7, with the
configuration handed over directly.

One command:

```bash
GNB_ARGS="-m 16 -M 80" scripts/rfsim_demo.sh --phytest
```

Or terminal by terminal, after the common setup of section 4:

```bash
mkdir -p cellgroups; "$B/nr-gnb-dci-logger" -d "$T_DB" -o gnb_dci.csv -c cellgroups
```

```bash
"$B/nr-softmodem" -O "$GNB_CONF_RFSIM" --rfsim --rfsimulator.[0].serveraddr server --T_stdout 2 --phy-test -m 16 -M 80 2>&1 | tee gnb.log
```

```bash
"$B/nr-sniffer" $TUNE --phy-test --reconfig-file reconfig.raw --rbconfig-file rbconfig.raw --sniffer.out_dir sniffer 2>&1 | tee sniffer.log
```

```bash
python3 ~/oai-nr-scope/scripts/sniffer_live.py sniffer
```

```bash
"$B/nr-uesoftmodem" $TUNE --phy-test --reconfig-file reconfig.raw --rbconfig-file rbconfig.raw 2>&1 | tee ue1.log
```

Notes:

- Start the gNB before the sniffer and the UE: the gNB writes `reconfig.raw` and `rbconfig.raw` into
  its working directory, `$RUN`.
- The phy-test UE takes **no** `-O nrue.uicc.conf`. With it, the UE stalls, the gNB sees no HARQ
  acknowledgements and everything is retransmitted.
- There is no SIB1 in phy-test, so the sniffer prints `Found cell` but not `SIB1 decoded`.

The gNB options:

| Option | Meaning | Default |
|---|---|---|
| `-m` | DL MCS | 9 |
| `-M` | DL PRBs | 50 |
| `-t` | UL MCS | 9 |
| `-T` | UL PRBs | 50 |
| `-D` | DL slot bitmap | |
| `-U` | UL slot bitmap | |

The live view now shows:

```
RNTI     type  formats    DL DCI  UL DCI  DL PRB  UL PRB DL MCS UL MCS  reTx  DL Mbit/s  UL Mbit/s
0x1234   c     0_1,1_1      9687    9687    80.0    50.0   16.0    9.0  5836      8.188      2.542
```

That is 80 PRB at MCS 16 down and 50 PRB at MCS 9 up, about 8 Mbit/s DL on this machine (per
wall-clock second).

`reTx` is high because the phy-test gNB schedules from its first slot, before the UE has connected,
and retransmits everything that went unacknowledged until then. The gNB log line
`UE 1234: dlsch_rounds a/b/c/d` shows how many transmissions were first attempts (a) and how many
were retransmissions (b, c, d) once the UE is running.

Evaluate with the known RNTI:

```bash
python3 ~/oai-nr-scope/scripts/compare_dci.py gnb.log gnb_dci.csv sniffer --rnti 0x1234 --report report.md
```

Expected: `0/21742 gNB DCIs missed`, `all 15 fields equal`, `RESULT: PASS`.

Try other values of `-m` (0 to 27) and `-M` (up to 106 here), and check that
the sniffer's PRB, MCS and TBS follow.

---

## 7. Following a UE that is already connected (late join)

The sniffer learns a UE's configuration from its Msg4. If the sniffer starts after a UE connected, it
missed that Msg4. You can still follow the UE by giving the sniffer:

- the UE's **C-RNTI**: `--sniffer.rnti`;
- the UE's **CellGroupConfig**: `--sniffer.cellgroup_file`, a UPER-encoded CellGroupConfig, or an
  RRCReconfiguration such as phy-test's `reconfig.raw`.

In the RF simulator you take both from the gNB logger. It saves each CellGroupConfig it sees as
`cellgroups/cellgroup_<rnti>_<n>.uper` and lists it in `cellgroups/cellgroups.csv`:

```
timestamp_us,rnti,length,file
1793269856742134,0xa65c,134,cellgroup_a65c_0.uper
```

To try it:

1. Start terminals 1 (logger) and 2 (gNB) and a UE (terminal 5), but **not** the sniffer.
2. When the UE is connected, look up its RNTI:

   ```bash
   cat cellgroups/cellgroups.csv
   ```

3. Start the sniffer with that UE's file and RNTI, here `a65c`:

   ```bash
   "$B/nr-sniffer" $TUNE --sniffer.out_dir sniffer --sniffer.cellgroup_file cellgroups/cellgroup_a65c_0.uper --sniffer.rnti 0xa65c 2>&1 | tee sniffer.log
   ```

The sniffer first decodes SIB1 to get the common configuration, then follows the UE straight away.
`ue.csv` records the UE with the event `known`, and the live view shows `0xa65c (known)`.

The sniffer still watches random access, so UEs that attach later are followed as well.

Evaluate with `--rnti`:

```bash
python3 ~/oai-nr-scope/scripts/compare_dci.py gnb.log gnb_dci.csv sniffer --rnti 0xa65c
```

A test run gave `0/447 gNB DCIs missed` and `RESULT: PASS`.

On a real network you would not have the gNB's logger. The configuration would have to come from
somewhere else, such as an operator, a test network you control, or a captured RRC message. This
mode exists for that case.

---

## 8. Several UEs, UEs leaving, and tuning

- **More UEs.** Start more UE terminals, each with its own IMSI, or run
  `scripts/rfsim_demo.sh --ues 6`.
  - The sniffer follows up to `--sniffer.max_ues` UEs (default 8, at most 14), each in its own
    shadow MAC.
  - The stats line shows the PDCCH work per slot, for example
    `6 UEs followed, 6.56 PDCCH configurations per slot (6.44 more merged into them)`.
  - UEs in the same RRC state share identical common search spaces. With `--sniffer.merge_pdcch 1`
    (the default) the sniffer decodes each shared search space once and checks all those UEs'
    RNTIs against it, instead of once per UE.
- **UEs leaving.** When a UE has had no DCI for `--sniffer.ue_timeout` seconds (default 10), the
  sniffer stops following it and frees its shadow MAC for the next UE:

  ```
  [SNIFFER] UE 1bc7: no DCI for 3.0 s, no longer followed (shadow MAC 1 is free)
  ```

  `ue.csv` records this as the event `left`. Set it to 0 to never time out.
- **CPU.** DCI decoding runs on OAI's DL worker threads: `--num-dl-actors N` (default 4) and
  `--actor-affinity`. The stats line every 2000 slots gives the decoding load.
- **Only one cell.** With several gNBs, `--sniffer.pci N` keeps only cell N.
- **All options.** `nr-sniffer/conf/nr-sniffer.conf` lists every `--sniffer.*` option with its
  default.

---

## 9. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| gNB hangs at start, no `Running as server` | It is waiting for the T-tracer. Start `nr-gnb-dci-logger` (terminal 1), or drop `--T_stdout 2` if you don't need ground truth |
| Sniffer or UE keeps retrying to connect | The gNB (the rfsim server) is not up yet. It is fine to wait |
| Sniffer prints `Found cell` but never `SIB1 decoded` | Normal in phy-test. In SA, check that `$TUNE` matches the gNB config (re-run `rfsim_conf.sh` with the same config) |
| `Watching random access` but no `RAR` when a UE attaches | The UE failed RA itself (check `ue1.log`), or the sniffer started after the UE's attach (use section 7) |
| Second UE never connects, gNB logs the same IMSI | Give each UE its own `--uicc0.imsi` |
| phy-test: huge `reTx`, gNB `pucch0_DTX` growing | The UE was started with `-O nrue.uicc.conf`. Start it without |
| `bind`/`address already in use`, or odd results after a crash | A process from an earlier run is still alive: `pkill -f nr-softmodem; pkill -f nr-uesoftmodem; pkill -f nr-sniffer` |
| `compare_dci.py`: `FAIL passive` | The gNB saw more PRACH than the UEs sent. Make sure no other UE or sniffer from another run is connected to the same gNB |
| Live view shows `(no DCI in this window)` | No UE is connected, or between events. With no core, a connected UE still gets about 10 UL grants per second |
| Permission errors (TUN, real-time priority) | OAI normally runs with `sudo`. The sniffer itself creates no TUN interface |

Logs worth reading:

| Log | What to look for |
|---|---|
| `gnb.log` | `RRC_CONNECTED reached`; the per-UE `dlsch_rounds` / `BLER` lines every few seconds |
| `ueN.log` | `RA procedure succeeded`, `NR_RRC_CONNECTED` |
| `sniffer.log` | every `[SNIFFER]` line (see [02-output-reference.md](02-output-reference.md)) |
| `gnb_dci_logger.log` | `connected to` |
