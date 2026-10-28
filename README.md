# oai-nr-scope: nr-sniffer for OpenAirInterface5G

A passive 5G SA telemetry tool in the spirit of [NR-Scope](https://github.com/PrincetonUniversity/NR-Scope),
built on the OpenAirInterface5G UE receive chain (**OAI 2026.w41**), for the
[ExPECA testbed](https://expeca.proj.kth.se/docs). See [PLAN.md](PLAN.md) for the milestones and
[doc/DESIGN.md](doc/DESIGN.md) for the architecture.

## Status

Milestone 1 is **done**: `nr-sniffer` builds against OAI 2026.w41, and `scripts/rfsim_test.sh` prints
`RESULT: PASS`. All 27 checked MIB/SIB1 fields match the gNB config, and the gNB logs no
random-access attempt.

- [x] OAI hook patch (`oai-patches/0001-nr-sniffer-hooks.patch`, applies cleanly to 2026.w41)
- [x] `nr-sniffer` executable target, MIB/SIB1 CSV logging, passive mode, `--sniffer.*` options
- [x] `doc/DESIGN.md`: NR-Scope feature → OAI code map, multi-RNTI DCI refactor plan
- [x] `scripts/rfsim_test.sh` + `scripts/check_cell.py`: gNB-in-rfsim end-to-end check
- [x] Compile `nr-sniffer` and fix any build errors
- [x] Run `scripts/rfsim_test.sh` until it prints `RESULT: PASS`
- [x] Stretch: T-tracer ground-truth logger (`nr-gnb-dci-logger`, `scripts/gnb_dci_test.sh` passes)

Milestone 2 (RACH/Msg4 tracking + DCI decoding for one UE) is **done** in the RF simulator.
`scripts/dci_test.sh` prints `RESULT: PASS`: the sniffer follows the UE through RAR and Msg4. It
misses 0 of its DCIs (e.g. 0/575 in 20 s), with 0 false positives and identical fields. The gNB
sees no transmission from the sniffer.

- [x] RA-RNTIs from SIB1, RAR → TC-RNTI tracking (`nr_sniffer_ra.c`, `rar.csv`)
- [x] Msg4/RRCSetup → follow the UE with OAI's own UE MAC/RRC (`ue.csv`)
- [x] DCI 0_0/0_1/1_0/1_1 logging with PRBs, MCS, TBS, HARQ (`dci.csv`, same schema as the gNB logger)
- [x] All UL dropped when passive; `scripts/compare_dci.py` checks miss rate, false positives, fields, passivity
- [ ] DCI 0_1/1_1 exercised end to end. Without a core the gNB never leaves the common SS; see
      [doc/DESIGN.md](doc/DESIGN.md#milestone-2-results-and-limits)

## Quick start

```bash
scripts/setup_oai.sh                    # clones OAI 2026.w41 into ./oai, applies patches, links nr-sniffer
(cd oai/cmake_targets && ./build_oai -I) # OAI build dependencies (once)
scripts/build.sh                        # nr-sniffer, nr-softmodem, nr-uesoftmodem, rfsimulator
scripts/rfsim_test.sh                   # gNB (rfsim) + sniffer, checks MIB/SIB1 against the gNB config
scripts/gnb_dci_test.sh                 # gNB (rfsim, T-tracer) + nr-gnb-dci-logger + one UE
scripts/dci_test.sh                     # + nr-sniffer following the UE; compares its DCIs with the gNB's
```

Running the sniffer against a cell (start it before the UEs you want to follow attach):

```bash
cd oai/cmake_targets/ran_build/build
./nr-sniffer -C 3319680000 -r 106 --numerology 1 --band 78 \
    --rfsim --rfsimulator.[0].serveraddr 127.0.0.1 --sniffer.out_dir ./sniffer-logs
```

## Running the gNB without a core network

No core network or code change is needed for SIB1. The OAI gNB starts its cell after the internal
F1 Setup between its CU and DU, which does not depend on NG Setup with an AMF. MIB and SIB1 are
broadcast while NGAP keeps retrying in the background. `scripts/rfsim_test.sh` only rewrites the
AMF and NG/NGU addresses in a copy of the config to `127.0.0.1`, so the gNB starts cleanly
without the `192.168.71.x` interfaces from OAI's docker setup. Without a core, an `nr-uesoftmodem` still completes RA, gets RRCSetup in Msg4 and reaches
RRC_CONNECTED (`RRCSetupComplete` is received). NAS registration then goes nowhere, so there is
no RRCReconfiguration or security. The gNB keeps scheduling the UE with DCI 0_0/1_0 on its C-RNTI,
mostly periodic UL grants and some DL. That is enough for component B and milestone 2.

### Passivity check

The test fails if the gNB log contains `Initiating RA procedure`. The NR gNB PHY logs it on every
detected PRACH, and NR MAC logs a lowercase variant when it starts an RA process. A verdict counts
only if the gNB is still running and logged `Client connects` for the sniffer. As a positive
control, running the sniffer with `--sniffer.passive 0` makes the gNB log both lines within a
few seconds.

### No IPv6 in the container

The cloud container's kernel has no IPv6 (`socket(AF_INET6)` fails with `EAFNOSUPPORT`). OAI's
rfsim server binds IPv6 only, so the gNB aborted with `could not open a socket`. The OAI patch
adds an IPv4 fallback to `startServer()` in `radio/rfsimulator/simulator.cpp`. The rfsim client
already uses `AF_UNSPEC`.

## gNB ground truth (component B)

```bash
./nr-softmodem -O gnb.conf --rfsim ... --T_stdout 2   # T-tracer on; waits for a tracer to connect
./nr-gnb-dci-logger -d ../../../common/utils/T/T_messages.txt -o gnb_dci.csv
```

The OAI patch adds a `GNB_MAC_DCI` T event (see [doc/DESIGN.md](doc/DESIGN.md)). It is free when
the T-tracer is off, which is the default. On ExPECA it works the same way with the real gNB, so
the logger can run on the gNB host or anywhere that reaches its T port (`-ip`, `-p`).

## Building in a restricted cloud VM

Claude cloud VMs with **Trusted** network access can reach GitHub and language package registries
but not `archive.ubuntu.com` or Docker Hub. In that case `build_oai -I` reports success but installs
almost nothing. These were needed by hand (all from GitHub):

| Missing | Fix |
|---|---|
| `flex` (for OAI's asn1c) | build `westes/flex` v2.6.4 release tarball, then `source tools/build_helper && install_asn1c_from_source` |
| `libconfig` | build `hyperrealm/libconfig` v1.7.3 with CMake; add `/usr/local/lib/pkgconfig/libconfig.pc` by hand |
| `libsctp` | build `sctp/lksctp-tools` v1.0.21 (`./bootstrap && ./configure && make install`) |
| `xxd` | tiny Python stand-in for `xxd -i` (only mode the T-tracer build uses) |

In a later session `archive.ubuntu.com` was reachable, and `build_oai -I` installed everything
(about 1 min) with no manual fixes. The first full `scripts/build.sh` (12k ninja steps, 4 vCPU) took about 5 min.

Alternatively, set the environment's network access to **Full**, or add `*.ubuntu.com` to a
**Custom** allowlist, and `build_oai -I` works normally.
