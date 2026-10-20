# Integrated class-session baseline comparison on Mac

This report measures candidate
`f9267d8f9b85dd4d0b522dd0426382bf77805cb1933680912ec3aaafa5d2f64e`
against the accepted explicit-UVM-path binary
`8ee6e5d00c3b95c672779d24d5038866ec39a80ad6398d6e37acbe012dc68cac`.
It is not a measurement of the subsequent alias-parallel prototypes.

All 300 fresh processes succeed: 20 randomized paired blocks across three real
projects and five arms. SPI/W01 queries a definition; PSS/W07 and UBUS/W08 query
completion and resolve the selected item. Each process includes preparation,
initial query, ten alternating unsaved edits, cached repeats and the full target
version's diagnostic publication after every state. Cross-file and complete public
response equality are covered by separate real-workload oracles. No build, tests,
editor, status, trace or memory sampling overlaps primary timing. Filesystem
caches are warm. Every observation is retained; twenty observations do not prove
a tail guarantee. The preflight fifteen-process run remains separately pinned.

Accepted native/UVMon use the accepted binary with the old UVM cache off/on.
Current native/UVMon use f926 with the same switches, with generic freezing off.
Current frozen enables generic freezing, disables the old cache, and selects
`uvm_pkg` plus `ahb_types_pkg` for PSS. The latter adds general scope/dependency
certification; it is not automatically a faster replacement for the old UVM path.

Milliseconds below are medians. Whole-session time starts at process launch and
ends at final complete target diagnostics, excluding shutdown. Continuous latency
excludes the first edit. A zero resolve value for SPI is a definition-only case,
not a measured completion-resolve latency.

| Case / arm | Whole session | Startup first response | First edit | Continuous | Cached repeat | Selected resolve |
|---|---:|---:|---:|---:|---:|---:|
| W01-compare / accepted-native | 2072.50 | 105.29 | 54.23 | 51.92 | 0.36 | 0.00 |
| W01-compare / accepted-uvm | 810.10 | 110.41 | 15.51 | 14.99 | 0.33 | 0.00 |
| W01-compare / current-native | 2092.34 | 106.02 | 52.41 | 51.38 | 0.37 | 0.00 |
| W01-compare / current-uvm | 815.94 | 110.06 | 14.99 | 14.70 | 0.33 | 0.00 |
| W01-compare / current-frozen | 1149.96 | 117.02 | 30.30 | 30.19 | 5.50 | 0.00 |
| W07-pss-multidoc / accepted-native | 2527.29 | 104.41 | 53.26 | 53.11 | 1.74 | 1.52 |
| W07-pss-multidoc / accepted-uvm | 1260.25 | 111.90 | 15.51 | 15.37 | 1.80 | 1.52 |
| W07-pss-multidoc / current-native | 2558.47 | 105.14 | 53.83 | 52.59 | 1.74 | 1.51 |
| W07-pss-multidoc / current-uvm | 1282.51 | 112.55 | 15.23 | 15.19 | 1.80 | 1.53 |
| W07-pss-multidoc / current-frozen | 1649.26 | 117.70 | 34.55 | 29.74 | 5.59 | 5.15 |
| W08-ubus-task / accepted-native | 2126.47 | 96.95 | 52.63 | 53.36 | 1.90 | 1.32 |
| W08-ubus-task / accepted-uvm | 886.39 | 106.09 | 16.72 | 16.38 | 2.03 | 1.31 |
| W08-ubus-task / current-native | 2135.63 | 97.88 | 53.20 | 52.54 | 1.91 | 1.32 |
| W08-ubus-task / current-uvm | 900.71 | 105.18 | 16.65 | 16.31 | 2.04 | 1.31 |
| W08-ubus-task / current-frozen | 1146.05 | 108.25 | 26.74 | 26.22 | 4.47 | 3.73 |

Current generic whole sessions improve approximately 45.0%, 35.5%, 46.3% versus
current native, with positive paired mean saving intervals. However, they regress
42.0%, 30.9%, 29.3% versus the accepted UVM-enabled binary. The corresponding
paired mean saving intervals are all negative: -373.39 to -331.70 ms (SPI),
-415.31 to -376.18 ms (PSS), -265.21 to -247.90 ms (UBUS), using 20,000 paired
bootstrap draws. Normal selected-resolve use therefore also shows a regression
against existing UVM optimization; this is not only an all-item stress problem.

Current old-UVM whole medians stay within approximately 0.7%, 1.8%, 1.6% of the
accepted UVM-enabled medians. Current native medians stay within approximately
1.0%, 1.2%, 0.4% of accepted native. These are observed median differences, not
equivalence proofs. The old UVM route must remain available while the generic
route's additional certification cost is reduced. Source freshness is not relaxed
to make these comparisons green. No main push or integrated acceptance follows
from this report. Fallback, RTL and long-memory accepted-baseline comparisons are
still required, alongside further optimization and validation.

## Exact d678 current confirmation

Candidate `d678052a67e742b0052da5e2ce8092bc4b0dd7b485d6ccea9e1e5b7ac59e17c3` completes a separate 300-process normal selected-query confirmation: twenty randomized paired blocks, all five arms, ten edits, complete matching target-version diagnostics. All processes pass; all observations remain in the paired 20,000-draw bootstrap. These results do not relabel the f926 evidence above.

Median whole-session milliseconds, including startup/preparation:

| Case | Accepted native | Accepted UVM | Current native | Current UVM | Generic frozen | Frozen vs accepted UVM | Mean saving 95% interval (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|
| W01-compare | 2113.53 | 829.97 | 2120.50 | 829.15 | 1128.71 | -35.99% | [-322.52, -288.99] |
| W07-pss-multidoc | 2587.85 | 1289.97 | 2586.98 | 1292.71 | 1658.24 | -28.55% | [-382.24, -352.30] |
| W08-ubus-task | 2174.71 | 909.52 | 2167.15 | 913.07 | 1128.48 | -24.07% | [-230.34, -207.54] |

Generic frozen beats current native by 35.90–47.93%, but regresses 24.07–35.99% against accepted UVM. The old legacy route remains approximately compatible. Stable integrated performance is still unaccepted; further mitigation must be compared against the accepted UVM route, not only native. Raw files: `alias-plan-selected-confirm.json` and `alias-plan-selected-confirm-analysis.json`, with binary/driver/harness/catalog hashes. Fallback and final memory/install/editor/reconstruction/publication gates remain open.
