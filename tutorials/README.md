# nr-sniffer tutorials

| Tutorial | What you do |
|---|---|
| [01 – nr-sniffer with the OAI RF simulator](01-rfsim-tutorial.md) | Build everything, run a gNB, UEs and the sniffer in the RF simulator, watch the sniffer follow the UEs, check it against the gNB, generate traffic, follow a UE that is already connected |
| [02 – Output reference](02-output-reference.md) | Every log line, CSV file and column the sniffer and the gNB logger write, and what it means |

## Five-minute version

After the build (tutorial 01, step 1), run this from the repository root:

```bash
scripts/rfsim_demo.sh --ues 2
```

This starts an OAI gNB, the gNB ground-truth logger, `nr-sniffer` and two UEs, all on one machine
and connected through OAI's RF simulator, then shows the sniffer's live view. Wait until both UEs
appear in the table, about 20 to 40 s, then press **Ctrl-C**. Everything stops, and the comparison
of what the sniffer decoded against what the gNB really sent is written to
`rfsim-demo/report.md`.

With traffic (phy-test mode, DCI 1_1/0_1, a few Mbit/s):

```bash
GNB_ARGS="-m 16 -M 80" scripts/rfsim_demo.sh --phytest
```
