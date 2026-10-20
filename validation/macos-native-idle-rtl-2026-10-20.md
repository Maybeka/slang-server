# Current Mac RTL hierarchy performance

Candidate SHA-256: `c30d503050c7f499e30c27f36fd25dc0f058a11decb08ded4540baa53db624a9`. Accepted phase-1 reference: `8ee6e5d00c3b95c672779d24d5038866ec39a80ad6398d6e37acbe012dc68cac`. All new caches remain disabled by default.

All 720 fresh sequential processes pass: twenty randomized paired blocks per cell, three real projects, drained/burst input and six arms. Every state checks primary/repeated hierarchy and chain queries with complete cross-arm hierarchy equality. Drained waits for every matching target-version diagnostic; burst waits for initial/final diagnostics. Status, cache hit/selection and combined retention-budget checks run after timing. Old binaries never receive unsupported status commands. No other workload or memory sampler overlaps timing. All observations remain in the 20,000-draw paired bootstrap; no tail guarantee.

## Whole-session medians (ms)

| Project | Mode | Accepted native | Accepted UVM | Current native | Current UVM | Syntax only | Instances |
|---|---|---:|---:|---:|---:|---:|---:|
| official-ubus | burst | 901.62 | 527.71 | 881.56 | 505.48 | 674.49 | 677.32 |
| official-ubus | drained | 1418.22 | 1034.54 | 1759.63 | 1387.99 | 1579.30 | 1604.70 |
| spi-block | burst | 889.68 | 535.62 | 854.02 | 490.00 | 735.12 | 761.14 |
| spi-block | drained | 1398.65 | 1009.63 | 1710.49 | 1325.23 | 1602.19 | 1673.41 |
| spi-pss | burst | 1460.28 | 1102.28 | 1442.59 | 1075.84 | 1299.47 | 927.35 |
| spi-pss | drained | 2130.79 | 1736.95 | 2338.00 | 1950.70 | 2224.55 | 2313.85 |

## Paired instance-cache contrasts

Positive reduction/saving favors instance reuse. Intervals are paired mean saving in milliseconds.

| Project | Mode | Control | Median reduction | Mean saving 95% interval |
|---|---|---|---:|---:|
| official-ubus | burst | accepted-uvm | -28.35% | [-162.73, -135.64] |
| official-ubus | burst | current-native | 23.17% | [183.35, 209.43] |
| official-ubus | burst | current-uvm | -34.00% | [-184.13, -149.57] |
| official-ubus | burst | syntax-only | -0.42% | [-22.64, 10.99] |
| official-ubus | drained | accepted-uvm | -55.11% | [-576.28, -539.82] |
| official-ubus | drained | current-native | 8.80% | [131.37, 172.79] |
| official-ubus | drained | current-uvm | -15.61% | [-264.21, -212.59] |
| official-ubus | drained | syntax-only | -1.61% | [-57.28, -8.18] |
| spi-block | burst | accepted-uvm | -42.10% | [-245.83, -202.28] |
| spi-block | burst | current-native | 10.88% | [64.40, 114.28] |
| spi-block | burst | current-uvm | -55.34% | [-300.70, -257.25] |
| spi-block | burst | syntax-only | -3.54% | [-48.56, 6.16] |
| spi-block | drained | accepted-uvm | -65.74% | [-680.95, -622.56] |
| spi-block | drained | current-native | 2.17% | [3.23, 64.94] |
| spi-block | drained | current-uvm | -26.27% | [-374.95, -322.23] |
| spi-block | drained | syntax-only | -4.44% | [-89.46, -25.52] |
| spi-pss | burst | accepted-uvm | 15.87% | [169.60, 202.42] |
| spi-pss | burst | current-native | 35.72% | [505.96, 551.70] |
| spi-pss | burst | current-uvm | 13.80% | [137.50, 177.97] |
| spi-pss | burst | syntax-only | 28.64% | [365.13, 401.13] |
| spi-pss | drained | accepted-uvm | -33.21% | [-594.85, -540.50] |
| spi-pss | drained | current-native | 1.03% | [-3.77, 35.63] |
| spi-pss | drained | current-uvm | -18.62% | [-396.29, -351.37] |
| spi-pss | drained | syntax-only | -4.01% | [-123.92, -73.43] |

## First and continuous response

Median milliseconds, current dedicated UVM / instances. Startup includes preparation; continuous is the median of edited-state primary queries, not diagnostic completion.

| Project | Mode | Startup through first response | First edit | Continuous |
|---|---|---:|---:|---:|
| official-ubus | burst | 283.65 / 317.43 | 16.55 / 21.00 | 13.64 / 20.46 |
| official-ubus | drained | 291.00 / 306.82 | 16.64 / 21.17 | 33.97 / 39.33 |
| spi-block | burst | 277.39 / 311.09 | 15.55 / 25.24 | 12.53 / 25.12 |
| spi-block | drained | 299.36 / 314.77 | 15.45 / 25.64 | 30.90 / 42.67 |
| spi-pss | burst | 393.37 / 413.09 | 64.89 / 25.73 | 59.51 / 25.33 |
| spi-pss | drained | 402.92 / 437.94 | 64.35 / 26.83 | 78.53 / 43.72 |

## Strategy conclusions and tradeoffs

PSS burst sessions establish a semantic-cache benefit: whole-session median improves 15.87% against accepted UVM, 13.80% against current dedicated UVM and 28.64% against syntax-only, with positive paired intervals. Current PSS first/continuous edited hierarchy responses fall from about 65/60 ms to about 26/25 ms. This supports the narrow certified subtree/body-edit contract, not a claim of arbitrary elaboration reuse.

SPI and UBUS show no established value-cache benefit beyond syntax-only (burst intervals include zero); their dedicated UVM route is faster. Do not recommend enabling instance caching for these small subtrees to obtain a speedup. Generic instance configurations beat current cache-off native in burst mode by 10.88%, 35.72% and 23.17% on SPI/PSS/UBUS, but that comparison does not erase the faster dedicated UVM alternative. User-permitted separate UVM handling is retained.

When every edit waits for complete diagnostics, instance caching does not improve the overall session versus dedicated UVM. All three drained syntax-only ablations also regress with the value cache. Against current native, drained PSS has no established whole-session saving (interval includes zero), SPI saves only about 2%, UBUS about 9%. These are negative results. First startup responses also cost more than accepted references; preparation and exact source certification are visible costs. The experiment stays opt-in and should be chosen for measured interactive hierarchy workloads.

The cache-off current full-design drained route also performs more work than the accepted baseline: current full-design diagnostics use the complete current compilation, correcting the older shallow diagnostic context (including bind/cross-hierarchy behavior). The native-only scheduling window mitigates repeated-query blocking, but does not remove complete diagnostic computation. This remaining throughput/startup cost is an explicit correctness/performance tradeoff, not a universal performance win. Ordinary exploration retains zero settling delay and its dedicated UVM medians remain within about 1% of accepted values in the separate 300-process comparison.

All 1,320 formal current-candidate processes (normal300 + fallback300 + RTL720) pass semantic assertions. The five-arm full-design and worker short/1,000-edit memory matrices complete all
180 cells and raw audits, plus twelve matched native UBUS controls. See the
[memory evaluation](macos-native-idle-memory-2026-10-20.md) for exact-instance
lifetime, 48/16 MiB budgets, real retirement and negative/variable native results.
The paired ordinary-route compatibility audit is also complete. Publication
requires the final source/SDK identity and normal main remote verification.

Evidence: `native-idle-rtl-confirm.json`, `native-idle-rtl-confirm-analysis.json`, their hashed scripts/logs and `native-idle-performance-gates.json` under `untracked/frozen-registry-2026-10-20/`. Older d678/f926/7e67 results remain separately pinned.
