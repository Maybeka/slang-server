# Mac UVM worker and retirement follow-up — 2026-10-19

This implements the three follow-ups to `2903a08`: stop capturing unused prefixes, seed retiring contexts with the incoming unsaved edit, and reuse an isolated worker’s own immutable UVM prefix. `experimentalUvmPrefixCache` remains **false by default**. This is an opt-in Mac evaluation, not a portable saved UVM database or a claim about arbitrary projects.

The final ordinary executable is F123c, SHA-256 `06e3a8784c79fe4dbdb6fc52181bafc3302ca74bbb59c8ff3fee3ac06930188c`. The previous-version comparison uses the frozen `2903a08` executable SHA-256 `89145d3b115efec1f5b4528c09ae6755d8a0da82a1db665a87af3e24d34e8de7`. The earlier original Native executable is used only as the correctness oracle. Mac arm64, macOS 27.0.1, 24 GiB; no Linux result is claimed.

The strongest measured gain is the complete edit-to-diagnostics chain. First-query overhead versus disabling the experiment remains. Across three 1,000-edit sessions per case, foreground query sums are essentially unchanged versus the previous enabled version (SPI slightly lower, PSS/UBUS slightly higher), although observed retirement p95 improves. Short completion process-family peaks rise by approximately 6–13 MiB versus old-on; the long-session sampled family peaks also rise, by about 4–39 MiB. Parent-final memory stays close to the previous enabled version. These are explicit tradeoffs, not a universal latency or memory win.

## Implementation and correctness

- Declined foreground parses and one-shot diagnostic parses clear capture in a local option copy. Shared input options and wire DTOs are preserved. A diagnostic session captures only when it can consume a compatible next checkpoint.
- Context retirement happens after applying the incoming edit. A successful replacement is already seeded with that unsaved version, avoiding a rebuild of the previous text followed immediately by invalidation. The existing larger-of-16-MiB-or-twice-seed source/table threshold remains. Failed replacements retain current text and can retry; old completion handles retain their existing generation checks.
- Only an enabled experiment assigns a nonzero worker session key, tied to the parent completion epoch. The worker owns all compiler objects locally. Options, ordered root/sort key, command-line generated definitions, retained file identities/bytes, include topology and fixed macros must match before resume. The SourceManager snapshot update invalidates non-retained IDs and never recycles IDs. A failed resume restarts the entire job with a fresh native manager.
- Every job still performs complete semantic diagnostics and inactive-region computation. Cancel, owner change, timeout, failure, shutdown or five seconds idle releases the worker. The worker applies its own source/table budget. Mac live-image authentication still runs before a new child receives its first request; parent lifetime and per-frame parent identity checks remain.

Final acceptance: **616/616 CTest**, **2,562 SDK cases / 23,723 assertions**, **8 system + 7 measurement/Lua Python tests**, and **43/43 standalone transport tests**. Transport code was unchanged between its standalone run and F123c. The SDK snapshot-update regression covers 112 assertions, including identity/topology/settings rejection, unrelated generated-file acceptance, retained bytes and non-recycled IDs. The SDK restoration patch applies to its pinned base and all 708 manifest files match; all four preexisting added SDK files are preserved.

All 15 real public cases pass with the experiment off, on, and forced retirement, including every completion resolve, stale handles, definition rebinding, macro/config/library changes, unsaved library overlays, failed-replacement recovery and cache pressure. The ordinary on run records 16 captures / 37 actual worker prefix reuses; forced retirement records 38 / 15. Snapshot lookup-topology changes reset contexts as intended. The previously failing original-Native W12 UTF-16 case remains excluded from successful performance/oracle claims; the negotiated UTF-8 case is included. The initial report retains that known failure.

**Diagnostic ordering caveat:** off preserves the original exact ordered oracle. On and forced runs compare complete diagnostic entries as a multiset: every field, duplicate and ordered `relatedInformation` list is preserved. Only the outer diagnostic-entry array is canonicalized. W08 UBUS task and W09 UBUS config reorder diagnostics sharing an expanded location and code because physical source IDs differ. Raw order differences remain recorded. Completion, definition and all resolve order comparisons are unchanged. Comparator regressions reject missing entries, changed notes and duplicate-count changes.

## Measurement design

All final comparative measurements are sequential fresh ordinary processes on the same fixed SPI/PSS/official-UBUS corpus, with warm filesystem caches. Old overlay and fast-CWD experiments are off. The three arms are **old-on** (previous version, UVM experiment enabled), **new-on**, and **new-off** (same final executable, setting disabled). Each ordinary cell has n=20 in ten shuffled mirrored six-process blocks. Every run requires an isolated diagnostic snapshot commit and rejects sequential-diagnostic fallback. No status calls, trace hooks, memory sampler, test suite or compilation run concurrently with ordinary timing. Failures and outliers are retained without replacement.

Coverage: 600 primary/selected-resolve runs, 360 complete-all-resolve runs, 360 fallback/invalidation runs, 27 independent 1,000-edit latency sessions, 75 separate RSS sessions, and 48 same-binary component-control runs: **1,470 successful final comparative processes**. A separate 64-process F123b readiness pilot is exploratory and is not mixed into final estimates. Forced phase traces are mechanism observations only. Short memory has n=3; long memory n=1 per cell.

Tables below show medians in milliseconds unless stated otherwise. Machine-readable results retain means, all raw samples, descriptive p95/max, and exploratory 95% bootstrap intervals for new-on minus each comparator, resampling complete block mean differences. These intervals are not median or tail guarantees; multiple comparisons have no correction. No failed process was replaced.

## First query, editing and complete diagnostics

Cold-first includes process startup through the first primary response. Cold-to-final includes startup, all edits, selected resolves and final diagnostics. Diagnostic completion uses the exact expected document version.

| Case / query | Cold first old / new / off | First edit old / new / off | Cold to final diagnostic old / new / off |
|---|---:|---:|---:|
| SPI / completion | 109.26 / 107.35 / 99.94 | 20.06 / 18.03 / 52.30 | 585.26 / 345.12 / 625.97 |
| SPI / definition | 105.71 / 106.05 / 98.52 | 16.04 / 14.88 / 50.09 | 557.87 / 324.13 / 603.06 |
| PSS multi-document / completion | 107.98 / 107.34 / 99.79 | 16.20 / 15.64 / 49.01 | 703.48 / 463.35 / 741.21 |
| PSS multi-document / definition | 106.19 / 105.19 / 97.58 | 16.30 / 14.60 / 49.57 | 679.50 / 435.90 / 718.47 |
| official UBUS task / completion | 101.10 / 100.57 / 93.54 | 18.22 / 16.65 / 50.51 | 573.11 / 342.20 / 614.97 |
| official UBUS task / definition | 99.68 / 100.10 / 94.08 | 17.65 / 15.94 / 50.44 | 557.85 / 324.47 / 601.27 |
| SPI continuous / completion | 108.81 / 108.71 / 99.76 | 20.02 / 18.49 / 52.15 | 1891.13 / 906.65 / 2158.85 |
| SPI continuous / definition | 104.29 / 104.23 / 98.01 | 16.16 / 14.53 / 49.86 | 1798.79 / 807.16 / 2064.68 |

The first query retains the prefix-capture overhead relative to new-off. This change primarily improves the complete edit-to-diagnostics sequence; it does not eliminate first-query authentication/freshness cost or claim uniform foreground-query acceleration.

## Resolve every returned candidate

All **747,360** returned candidates are resolved. These sessions drain every version’s diagnostics and include complete candidate enumeration and response construction.

| Case | Edit resolve sum old / new / off | Cold to all queries and diagnostics old / new / off |
|---|---:|---:|
| SPI macro | 3.42 / 3.42 / 11.58 | 563.81 / 328.44 / 624.37 |
| SPI | 519.56 / 524.59 / 618.10 | 1442.93 / 1211.03 / 1601.52 |
| PSS multi-document | 37.74 / 38.04 / 37.50 | 740.86 / 498.54 / 785.21 |
| official UBUS task | 90.07 / 88.51 / 87.79 | 708.88 / 474.92 / 745.03 |
| official UBUS config | 572.63 / 567.95 / 661.48 | 1389.51 / 1234.32 / 1682.80 |
| SPI continuous | 2587.11 / 2596.88 / 3111.18 | 4996.22 / 4017.10 / 5809.73 |

## Separate the worker mechanisms

Both component-control arms use the same F123c executable, enabled configuration and persistent transport. `SLANG_UVM_WORKER_PREFIX_DISABLE=1` disables only worker prefix capture/reuse, so it separates parsing reuse from avoiding process startup and Mac child authentication. It cannot enable the experiment. n=8 per arm/case; this is a component estimate, not a tail claim.

| Case | Edit sequence incl. diagnostics, persistent native / prefix reuse |
|---|---:|
| SPI | 259.38 / 225.70 |
| PSS multi-document | 379.93 / 341.65 |
| official UBUS task | 261.46 / 233.73 |

The complete previous-to-new gain includes both mechanisms. The 64-process pilot compares F12 (first two changes only) against the previous version and integrated worker prototype; it supports attribution but remains separate, with n=4 per cell. The first worker prototype passed ordered public output checks but recorded zero actual real-workload reuse hits because its generated-file-path check was too strict. That negative result is retained; it is not counted as a successful cache mechanism.

## Unsupported inputs and invalidation

Watch events are batched with the next ordinary edit/query so reload work is included. These six cases keep full diagnostics and native fallback semantics. Gains here can come from persistent transport even when the prefix is declined; they do not prove that unsupported syntax becomes reusable.

| Variant | First edit old / new / off | Cold to final diagnostic old / new / off |
|---|---:|---:|
| config-watch | 150.86 / 151.09 / 179.55 | 705.13 / 497.80 / 740.77 |
| library-watch | 108.80 / 108.76 / 137.45 | 659.63 / 450.20 / 691.16 |
| macro-override | 60.35 / 52.58 / 49.26 | 645.74 / 438.39 / 602.48 |
| nonprefix-order | 49.47 / 49.08 / 49.44 | 617.29 / 430.93 / 617.60 |
| tail-resetall | 55.43 / 49.09 / 49.82 | 654.06 / 447.60 / 615.38 |
| tail-timescale | 55.53 / 47.64 / 49.88 | 640.61 / 428.46 / 600.51 |

## Natural retirement during 1,000 ordinary edits

Three independent sessions per arm/case perform ordinary completion, cached repeat and selected resolve. Only the existing opaque completion epoch identifies retirement boundaries; no trace or status request runs here. Initial/final diagnostics are drained in burst mode. The table gives the median of the three session-level values. Boundary p95 is descriptive, particularly where there are few retirements.

| Case / arm | Retirements per session | Edit response sum | Boundary p95 | Ordinary-edit p95 |
|---|---:|---:|---:|---:|
| PSS multi-document / old-on | 8, 8, 8 | 16056.32 | 88.37 | 16.47 |
| PSS multi-document / new-on | 8, 8, 8 | 16079.22 | 83.08 | 16.61 |
| PSS multi-document / new-off | 0, 0, 0 | 48398.53 | — | 52.03 |
| official UBUS task / old-on | 32, 32, 32 | 18820.79 | 89.39 | 17.54 |
| official UBUS task / new-on | 31, 31, 31 | 18982.55 | 80.84 | 18.26 |
| official UBUS task / new-off | 0, 0, 0 | 52311.32 | — | 60.42 |
| SPI continuous / old-on | 8, 8, 8 | 18551.89 | 93.06 | 18.92 |
| SPI continuous / new-on | 8, 8, 8 | 18445.70 | 84.67 | 18.81 |
| SPI continuous / new-off | 0, 0, 0 | 51178.86 | — | 54.59 |

The median thousand-edit foreground sums change by about −0.6% for SPI, +0.1% for PSS and +0.9% for UBUS versus old-on. This small n=3 does not establish a stable overall query-speed gain. The lower observed boundary p95 is a narrower result.

The replacement regression verifies that the cached tree already contains the latest unsaved text before another query. Separate forced-limit phase traces measure release, construction, open-document copy and load/publish. Load/publish includes nested construction/copy work, so those fields must not be added together. In the separate forced traces, median construction is about 66–68 ms inside 69–70 ms load/publish, while copying open documents takes under 0.4 ms. These instrumented forced-limit values locate remaining work; they are not ordinary timing samples. Neither this threshold nor the number of typed characters establishes a bound on arbitrary process RSS.

## Separate process-family memory

RSS uses Darwin libproc at 20 ms, includes the server and diagnostic descendants, and counts shared pages once per process. Peaks between samples can be missed. Parent-final marks are explicit. These sampled runs are excluded from every latency table. Long burst uses 1,000 completion edits; long drained uses 500 definition edits.

| Stage / case / query | Peak family MiB old / new / off | Final parent MiB old / new / off |
|---|---:|---:|
| long-burst / PSS multi-document / completion | 210.94 / 214.69 / 1870.14 | 112.67 / 116.14 / 1750.36 |
| long-burst / official UBUS task / completion | 197.59 / 223.80 / 2402.86 | 127.22 / 126.70 / 2306.67 |
| long-burst / SPI continuous / completion | 188.17 / 218.36 / 1639.45 | 121.41 / 120.23 / 1163.89 |
| long-drained / PSS multi-document / definition | 215.83 / 242.42 / 1086.20 | 116.41 / 119.45 / 634.38 |
| long-drained / official UBUS task / definition | 225.75 / 264.55 / 981.75 | 128.80 / 126.73 / 761.73 |
| long-drained / SPI continuous / definition | 214.08 / 250.33 / 1011.73 | 116.38 / 116.03 / 633.30 |
| short / PSS multi-document / completion | 162.05 / 168.31 / 202.03 | 63.86 / 64.00 / 103.75 |
| short / PSS multi-document / definition | 161.77 / 168.05 / 201.77 | 63.59 / 63.72 / 103.45 |
| short / official UBUS task / completion | 175.17 / 187.66 / 202.45 | 78.59 / 78.67 / 112.80 |
| short / official UBUS task / definition | 159.27 / 187.47 / 195.48 | 78.33 / 78.23 / 112.34 |
| short / SPI continuous / completion | 170.27 / 177.23 / 214.36 | 72.19 / 71.61 / 131.66 |
| short / SPI continuous / definition | 164.56 / 172.89 / 210.11 | 66.80 / 66.84 / 128.62 |

| New-on idle control | Family MiB at final / after 6 s idle |
|---|---:|
| PSS multi-document | 168.11 / 63.70 |
| official UBUS task | 186.92 / 78.12 |
| SPI continuous | 172.81 / 66.78 |

This round does not demonstrate an overall memory saving against old-on: sampled long-session family peaks increase about 4–39 MiB while parent-final RSS remains close. The much smaller memory footprint versus new-off primarily reflects the already-existing generation retirement, and must not be credited entirely to these new changes. A warm worker retains a library between edits. Source/table retirement bounds retained generations, not all syntax, semantic models, allocator capacity or the process family. Short and long sessions must therefore be judged separately; no infinite-session RSS or universal memory-saving guarantee follows from these samples.

## Remaining optimization candidates

The next measurements should separate initial prefix capture and macro/freshness proof from worker snapshot decode and byte-copy costs. The remaining first-query overhead is about 6–9 ms versus new-off, while the new worker adds retained memory. A useful next prototype is an immutable, content-identified library-byte representation for snapshot transport/validation, retaining complete freshness, ordered-macro, include-topology and native-fallback checks. Its value still needs measurement; this round does not establish where all remaining copy/validation time is spent. Further retirement work should target the construction phase identified above and compare whole-session foreground sums, not only the retirement spike.

## Reproduction and evidence

The implementation and source restoration live in the repository. The private `untracked/uvm-followup-2026-10-19` directory holds frozen binaries, original corpus/oracle bodies and full local logs. The [machine-readable report](macos-uvm-followup-2026-10-19.json) binds candidate, baseline, drivers, corpus catalogs, schedules and results with hashes. Its [raw evidence archive](artifacts/uvm-followup-2026-10-19-F123c.tar.gz) includes the sequential measurement/validation drivers and compact raw outcomes, without tracking workload source bodies or handoff notes.

Archive SHA-256: `58cbca8f02a71ea46f807af18e35d6bee9c442f65e20195a458ce6d1742e7aa9`. The archived manifest was verified against every extracted member. The [initial prefix report](macos-uvm-prefix-2026-10-19.md) remains historical and is not relabeled as this candidate’s result.
