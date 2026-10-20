# Integrated RTL hierarchy comparison on Mac

Candidate SHA-256: `d678052a67e742b0052da5e2ce8092bc4b0dd7b485d6ccea9e1e5b7ac59e17c3`. Accepted UVM-path reference: `8ee6e5d00c3b95c672779d24d5038866ec39a80ad6398d6e37acbe012dc68cac`. New experiments remain disabled by default.

All 720 fresh sequential processes passed. Twenty randomized paired blocks cover three real projects, drained and burst input, and six arms. Each process includes startup/preparation and ten alternating unsaved edits; every state checks primary/repeated `getScope` plus `getScopes`. Complete hierarchy hashes match across all arms and states. Drained mode waits for each target-version diagnostic; burst waits for initial/final diagnostics. Current status/budget/hit checks run after timing; the old binary is never queried for unsupported status commands. No concurrent builds, tests, editor, profiling or memory sampler ran. Warm filesystem caches; no tail guarantee.

## Whole-session results

Median milliseconds. Accepted/current native have both cache mechanisms off; UVM uses the respective legacy route. Syntax-only uses 48 MiB; instances use a combined 64 MiB (48 syntax, 16 value).

| Project | Mode | Accepted native | Accepted UVM | Current native | Current UVM | Syntax only | Instances |
|---|---|---:|---:|---:|---:|---:|---:|
| official-ubus | burst | 894.18 | 528.63 | 1689.16 | 1317.95 | 682.52 | 678.64 |
| official-ubus | drained | 1427.14 | 1046.01 | 2009.91 | 1638.54 | 1532.80 | 1576.27 |
| spi-block | burst | 916.10 | 539.22 | 1600.44 | 1226.70 | 764.75 | 745.74 |
| spi-block | drained | 1384.79 | 1019.39 | 1887.23 | 1515.41 | 1589.57 | 1627.40 |
| spi-pss | burst | 1478.86 | 1098.70 | 2207.33 | 1845.13 | 1344.18 | 891.79 |
| spi-pss | drained | 2103.28 | 1731.49 | 2511.05 | 2126.05 | 2210.17 | 2285.60 |

## Paired contrasts

Positive median reduction / positive mean saving favors instances. Bootstrap intervals use all twenty paired observations, 20,000 draws, seed 2026102037. No outliers are discarded.

| Project | Mode | Reference | Median reduction | Mean saving 95% interval (ms) |
|---|---|---|---:|---:|
| official-ubus | burst | accepted-uvm | -28.38% | [-171.73, -140.47] |
| official-ubus | burst | syntax-only | 0.57% | [-22.44, 13.34] |
| official-ubus | drained | accepted-uvm | -50.69% | [-525.16, -471.07] |
| official-ubus | drained | syntax-only | -2.84% | [-68.20, -15.96] |
| spi-block | burst | accepted-uvm | -38.30% | [-239.64, -199.13] |
| spi-block | burst | syntax-only | 2.49% | [-16.47, 36.00] |
| spi-block | drained | accepted-uvm | -59.64% | [-625.09, -568.95] |
| spi-block | drained | syntax-only | -2.38% | [-66.94, -25.62] |
| spi-pss | burst | accepted-uvm | 18.83% | [160.49, 207.97] |
| spi-pss | burst | syntax-only | 33.66% | [398.20, 447.98] |
| spi-pss | drained | accepted-uvm | -32.00% | [-569.67, -523.51] |
| spi-pss | drained | syntax-only | -3.41% | [-102.97, -62.71] |

## First and continuous response

Median milliseconds, accepted UVM / instances. Continuous response is the median of the ten primary edited-state hierarchy queries; whole-session results also include repeated queries, chain queries and the specified diagnostic waits.

| Project | Mode | Startup through first response | First edit | Continuous |
|---|---|---:|---:|---:|
| official-ubus | burst | 251.12 / 314.52 | 20.17 / 21.70 | 19.39 / 21.82 |
| official-ubus | drained | 259.62 / 365.27 | 20.28 / 21.42 | 16.13 / 40.02 |
| spi-block | burst | 251.88 / 310.68 | 19.19 / 25.30 | 13.55 / 25.29 |
| spi-block | drained | 254.01 / 307.20 | 19.44 / 25.74 | 15.29 / 42.47 |
| spi-pss | burst | 323.98 / 415.50 | 65.23 / 25.09 | 60.23 / 24.51 |
| spi-pss | drained | 324.29 / 433.16 | 66.09 / 27.95 | 60.49 / 42.28 |

## Acceptance and next investigation

Semantic acceptance passes; stable performance acceptance remains open. PSS burst sessions improve 18.83% versus accepted UVM, but every drained project regresses (32.00–59.64%). Startup through first response is slower in all six cells. Against syntax-only, instances improve PSS burst substantially; SPI/UBUS burst intervals include zero, and all three drained cells regress. These results do not support a universal instance-cache win.

The cache-off current arms also regress versus accepted cache-off. Repeated `getScope` medians rise from roughly 0.7–1.1 ms to roughly 49.7–53.6 ms. The source of this disabled-path regression needs isolated profiling after the current normal-session timing finishes; it must not be attributed solely to frozen input witnesses. Investigate request/background scheduling and native diagnostics, then use matched experiments before changing freshness checks. Preserve the full old-UVM comparison and the syntax/value ablation.

Raw evidence: `untracked/frozen-registry-2026-10-20/integrated-rtl-confirm.json` and `integrated-rtl-confirm-analysis.json`; both driver and analyzer hashes are embedded. This report does not replace normal class-session, fallback or process-family memory gates.
