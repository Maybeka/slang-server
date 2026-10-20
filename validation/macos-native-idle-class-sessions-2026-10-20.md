# Current candidate normal and macro-fallback sessions

Candidate SHA-256: `c30d503050c7f499e30c27f36fd25dc0f058a11decb08ded4540baa53db624a9`. Accepted reference: `8ee6e5d00c3b95c672779d24d5038866ec39a80ad6398d6e37acbe012dc68cac`.

Both normal and macro-fallback matrices complete all 300 fresh sequential processes, twenty randomized paired blocks, three real cases, five arms and ten edits. All semantic assertions pass. Whole-session timing starts at spawn, includes preparation, first and continuous queries, cached repeats, selected completion resolve and matching-version diagnostics; shutdown is excluded. No build, editor, trace, test or memory sampler overlapped timing. All observations remain in the offline 20,000-draw paired analysis. No tail guarantee.

UVM is the dedicated prefix option with generic freezing disabled; frozen is generic package freezing. Their configurations are kept distinct. Fallback inserts a literal UVM macro override into the user source; the raw workload and driver are pinned separately.

| Scenario | Case | Accepted native | Accepted UVM | Current native | Current UVM | Current frozen |
|---|---|---:|---:|---:|---:|---:|
| selected | W01-compare | 2096.38 | 817.33 | 2124.47 | 826.51 | 1113.31 |
| selected | W07-pss-multidoc | 2572.82 | 1284.67 | 2589.21 | 1299.41 | 1607.06 |
| selected | W08-ubus-task | 2147.09 | 905.45 | 2163.56 | 911.88 | 1126.43 |
| fallback | W01-compare | 2148.55 | 1480.73 | 2159.40 | 1464.20 | 1799.71 |
| fallback | W07-pss-multidoc | 2614.10 | 1937.59 | 2629.14 | 1951.47 | 2314.16 |
| fallback | W08-ubus-task | 2212.88 | 1527.58 | 2212.83 | 1539.84 | 1817.40 |

Median whole-session milliseconds. Dedicated current UVM differs from accepted UVM by about 1% in these ordinary sessions, recovering the unconditional-window prototype regression. The separate paired compatibility audit is complete; details follow below. Generic freezing improves normal sessions versus current native by 37.9–47.9%, but regresses 24.4–36.2% versus accepted UVM. Under macro fallback it improves versus current native by 12.0–17.9%, while regressing 19.0–21.5% versus accepted UVM. Generic-versus-accepted-UVM paired mean saving intervals are negative in every case; those regressions are not discarded.

For UVM-heavy class editing, keep the dedicated UVM route available. Generic package/module/RTL reuse is evaluated independently; setting both flags does not prove dedicated continuation is being used. The user explicitly permits separate implementations where unification costs performance. This does not remove general scope features or their invalidation/diagnostic/memory requirements.

The current six-arm full-design RTL matrix completes all 720 processes; its first/continuous/whole-session results and syntax-only ablation are recorded in [the RTL report](macos-native-idle-rtl-2026-10-20.md), including negative results. Three-repeat short/1,000-edit memory controls and raw audits are complete; see [memory evaluation](macos-native-idle-memory-2026-10-20.md). All experiments stay off by default. Publication uses the tested source identity and normal main workflow.

Evidence under `untracked/frozen-registry-2026-10-20/`: `native-idle-selected-confirm.json`, `native-idle-selected-confirm-analysis.json`, `native-idle-fallback-confirm.json`, `native-idle-fallback-confirm-analysis.json` and `native-idle-performance-gates.json`. Binary/driver/harness/workload/corpus hashes are retained. Earlier d678/f926/7e67 reports remain separately pinned.

## Paired compatibility audit

Both 300-process inputs pass the separate twenty-pair native/dedicated audit.
All observations remain, with 20,000 paired bootstrap draws and no equivalence,
multiplicity-adjustment or tail claim. Positive slowdown means worse.

| Scenario | Case | Dedicated whole median change | Paired mean slowdown 95% interval (ms) |
|---|---|---:|---:|
| selected | W01-compare | +1.12% | [-3.38, 11.35] |
| selected | W07-pss-multidoc | +1.15% | [9.51, 21.68] |
| selected | W08-ubus-task | +0.71% | [1.88, 8.17] |
| fallback | W01-compare | -1.12% | [-17.53, 13.08] |
| fallback | W07-pss-multidoc | +0.72% | [5.79, 29.99] |
| fallback | W08-ubus-task | +0.80% | [-3.64, 23.93] |

Small ordinary PSS/UBUS dedicated-route slowdowns remain statistically visible;
SPI's interval includes zero. These are retained, not called equivalence.
Native whole-session medians change by -0.003% to +1.34% across both scenarios.
Native/dedicated startup, first-edit, continuous, repeated and selected-resolve
medians show no >10% regression in these ordinary exploration sessions. This
does not erase the separate full-design drained diagnostic tradeoff.

Audit files: `native-idle-selected-compatibility.json` and
`native-idle-fallback-compatibility.json`, with input/script identities and all
per-metric paired intervals under the untracked evidence directory.
