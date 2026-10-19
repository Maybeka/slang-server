# Mac UVM prefix acceptance, 2026-10-19

The opt-in parsed-prefix implementation improves first and repeated edits in
the measured SPI/PSS/UBUS workloads. It also shares compatible library parses
during resolution and retires source generations. Cold first response,
unsupported fallback and retirement spikes remain real costs. The setting
`experimentalUvmPrefixCache` remains **default false**.

This is an in-process library artifact, with no disk serialization. The
[design and eligibility rules](../docs/features/uvm-prefix-cache.md) describe
ownership, exact-context checks, fixed external macros and native fallback.

## Inputs and acceptance

All performance comparisons below use Config off/on in the same frozen
ordinary binary, SHA256 `89145d3b115efec1f5b4528c09ae6755d8a0da82a1db665a87af3e24d34e8de7`. The source starts from
`73f82dc`; the [machine-readable report](macos-uvm-prefix-2026-10-19.json)
records the actual changed-source hashes and commands. The earlier immutable
Native binary is used independently for correctness, not as a paired timing arm.

The corpus contains the local educational SPI/PSS laboratory (209 files and
77,665 inventory lines per configuration) and official UVM 1.2 UBUS (172 files,
78,866 inventory lines). UVM is pinned at
`71f30a7473118b3cea582d7673c3af8e161fce71`. Original HDL text, ordered
roots, include paths, fixed flags and tops are preserved. These inventories
include unreached files. This is static language-server acceptance, without
VCS runtime/debug/DPI linking or simulation, and does not represent production
design scale. All 590 inventory files still match after the measurements.

- CTest **613/613**; SDK **2,561 cases / 23,611 assertions**.
- Python system **8**, measurement/Lua **7**, transport **35**, all pass.
- Specialized guards: **169 assertions / 3 cases**, including concurrent
  extraction/binding, immutable ancestry/preview links and source-token lifetime.
- Each of off/on/forced-retirement matches the original Native public
  completion order, definition, every resolve and selected stale responses
  in 15 cases (14 real editing scenarios plus a UVM-owned case).
- Macro freshness under config/library watches and unsaved library overlays,
  replacement failure/recovery and bounded-cache pressure pass.
- Clean SDK patch application matches all **708** retained file hashes.

The native W12 UTF16 query issue predates this candidate and remains excluded;
negotiated UTF8 and CRLF alternatives are included. No new Linux or GUI-editor
acceptance is claimed.

## First and repeated queries

Values are medians in **ms**, **off / on**, n=20 per arm/cell. Cold first
response includes initialization and the initial query. Edit response sums
exclude that initial query and include recovery. The whole interval begins
at cold startup and ends with the final versioned diagnostic notification.
The table drains diagnostics after each state; SPI continuous uses ten edits,
the other cases use edit plus restore. Burst cases remain in the JSON report.

| Case / query | Cold first response | First edit | Edit response sum | Cold to final diagnostic |
|---|---:|---:|---:|---:|
| SPI / completion | 100.22 / 108.46 | 52.36 / 19.83 | 105.63 / 39.23 | 623.29 / 582.85 |
| SPI / definition | 99.49 / 106.35 | 48.61 / 16.07 | 99.26 / 31.83 | 603.82 / 557.81 |
| PSS / completion | 99.48 / 105.81 | 49.30 / 16.37 | 100.04 / 32.97 | 724.79 / 685.33 |
| PSS / definition | 97.48 / 104.49 | 49.05 / 16.20 | 98.85 / 32.50 | 716.25 / 673.16 |
| UBUS task / completion | 93.43 / 101.24 | 49.81 / 18.10 | 100.92 / 36.05 | 609.34 / 570.80 |
| UBUS task / definition | 94.35 / 100.07 | 50.18 / 17.54 | 101.25 / 34.85 | 596.70 / 557.30 |
| SPI (10 edits) / completion | 100.27 / 108.57 | 52.03 / 19.90 | 518.85 / 198.37 | 2141.31 / 1885.15 |
| SPI (10 edits) / definition | 99.45 / 107.97 | 49.94 / 16.80 | 496.46 / 164.46 | 2050.07 / 1795.43 |

Cold first response costs about **6–9 ms**. First edits drop from roughly
**49–53 ms to 16–20 ms**. This is suffix parsing and native semantic analysis,
not arbitrary constant-time editing. Diagnostic workers still perform native
full parsing, so their cost remains in the whole-session results.

## Every completion resolve

These separate n=20 cells resolve every returned candidate at every state;
all **498,240** resolve requests succeed. Medians in ms, off / on.

| Case | Initial all-resolve sum | Edit all-resolve sum | Cold to all queries and diagnostics |
|---|---:|---:|---:|
| Macro control | 5.81 / 2.51 | 11.65 / 3.47 | 620.95 / 561.70 |
| SPI | 309.30 / 277.03 | 613.28 / 506.38 | 1592.80 / 1416.91 |
| PSS | 17.27 / 17.79 | 36.92 / 37.91 | 780.05 / 738.20 |
| UBUS task | 45.91 / 45.30 | 89.87 / 90.90 | 763.13 / 711.17 |
| UBUS config | 336.00 / 304.02 | 666.07 / 574.33 | 1682.42 / 1386.56 |
| SPI (10 edits) | 306.41 / 281.40 | 3103.15 / 2577.57 | 5743.65 / 4963.00 |

SPI and UBUS config reach the shared library path, including the verified
transparent UBUS wrapper. PSS and UBUS task do not gain materially from resolve
cache hits themselves; their whole-session saving comes mainly from edits.

## Invalidation and fallback

All 240 processes succeed. Each variant has n=20 per arm, with watched-file
notifications included in the measured sequence. Medians in ms, off / on.

| Variant | First edit | Restore | Cold to final diagnostic |
|---|---:|---:|---:|
| config-watch | 171.82 / 148.30 | 49.44 / 16.52 | 721.65 / 690.93 |
| library-watch | 137.28 / 108.92 | 50.98 / 18.14 | 690.12 / 656.16 |
| macro-override | 50.17 / 60.49 | 51.29 / 57.90 | 602.57 / 642.50 |
| nonprefix-order | 50.23 / 50.49 | 51.35 / 51.53 | 599.59 / 599.90 |
| tail-resetall | 50.45 / 56.30 | 52.51 / 57.81 | 602.61 / 636.85 |
| tail-timescale | 50.02 / 55.69 | 51.74 / 57.08 | 605.45 / 640.67 |

Config/library watch sequences improve and nonprefix root ordering is near
parity. Source macro override, tail resetall and tail timescale preserve native
results but add about **35–40 ms** over these short sequences. That penalty
is included in acceptance and is one reason to keep the experiment opt-in.

## Thousand-edit ordinary latency

Three fresh processes per arm/case, 1,000 completion edits each. Every step
also repeats completion and resolves a selected item; initial/final diagnostics
are drained. No status probes, trace output or RSS sampler runs in these cells.
Retirement boundaries come from normal opaque generation data returned by
completion. Total seconds are per-process medians, off / on.

| Case | Whole sequence (s) | On retirements per run | On boundary p95 / max (ms) |
|---|---:|---:|---:|
| PSS | 52.62 / 18.07 | [8, 8, 8] | 89.53 / 92.97 |
| UBUS task | 53.36 / 23.46 | [32, 32, 32] | 86.41 / 94.08 |
| SPI | 73.42 / 27.09 | [8, 8, 8] | 91.98 / 93.86 |

The measured whole-session gain includes the retirement work. A roughly
**94 ms** observed boundary maximum is still substantially above ordinary
edited-query latency. These small samples do not establish a tail guarantee.

## Separate memory replay

RSS is sampled every 20 ms through libproc. Parent RSS at the final observed
query and peak simultaneous process-family RSS are separate metrics. Family
sums include diagnostic workers and count shared pages repeatedly; short
worker peaks may be missed. Memory timings are excluded from latency inference.

Completion memory sessions resolve a selected item. The every-candidate resolve
matrix above is separate and does not establish its fully populated cache RSS.

Short drained sessions use n=3 per arm/query. Medians in MiB, off / on.

| Case / query | Final parent RSS | Peak family RSS |
|---|---:|---:|
| PSS / completion | 103.44 / 63.91 | 201.53 / 162.11 |
| PSS / definition | 103.25 / 63.66 | 201.41 / 161.81 |
| UBUS task / completion | 112.69 / 78.66 | 189.50 / 175.05 |
| UBUS task / definition | 112.33 / 78.28 | 191.88 / 154.95 |
| SPI (10 edits) / completion | 131.59 / 71.62 | 213.91 / 169.33 |
| SPI (10 edits) / definition | 128.44 / 66.77 | 218.36 / 164.50 |

Long burst sessions use 1,000 completion edits and n=1 per arm/case.
`prefix-only` disables generation retirement while retaining prefix reuse.
Long drained sessions use 500 definition edits, comparing prefix-only and on;
there is no native drained arm in this memory matrix. All final-step marks
are explicit. MiB, individual observations:

| Session / case / arm | Final parent RSS | Peak family RSS | Retirements |
|---|---:|---:|---:|---:|
| long-burst / PSS / off | 1138.36 | 1505.02 | 0 |
| long-burst / PSS / on | 113.20 | 211.58 | 8 |
| long-burst / PSS / prefix-only | 153.72 | 252.92 | 0 |
| long-burst / UBUS task / off | 1414.00 | 2147.31 | 0 |
| long-burst / UBUS task / on | 127.14 | 206.09 | 32 |
| long-burst / UBUS task / prefix-only | 544.84 | 725.02 | 0 |
| long-burst / SPI / off | 1163.19 | 1694.77 | 0 |
| long-burst / SPI / on | 125.12 | 212.98 | 8 |
| long-burst / SPI / prefix-only | 227.42 | 314.83 | 0 |
| long-drained / PSS / on | 91.98 | 211.78 | 4 |
| long-drained / PSS / prefix-only | 108.17 | 207.55 | 0 |
| long-drained / UBUS task / on | 124.22 | 223.88 | 16 |
| long-drained / UBUS task / prefix-only | 244.55 | 381.89 | 0 |
| long-drained / SPI / on | 113.31 | 213.95 | 4 |
| long-drained / SPI / prefix-only | 107.73 | 220.45 | 0 |

The JSON retains RSS at every observed retirement as well as per-process
query distributions and all outcomes. Repeated natural retirements show the
behavior over these sessions; the source-generation threshold is not a bound
on arbitrary process RSS. Earlier prototype/retirement observations remain
separate. Their missing explicit final-step marker was corrected in this matrix.

## Decision and next optimization priorities

Retain the implementation behind the opt-in flag: unchanged UVM prefix sharing,
one native initial parent parse, compatible resolve reuse and context retirement
all pass correctness acceptance and improve the supported whole sessions.
Do not enable it globally: cold-start overhead, unsupported-input penalties
and retirement spikes remain workload-dependent costs.

1. Stop creating unused prefix checkpoints after a permanent fallback and in
   diagnostic-only workers. Target the measured fallback penalty and cold cost;
   validate with a new frozen binary and the same complete off/on matrix.
2. Reduce replacement reconstruction and overlapping owners at retirement.
   Preserve unsaved text, rollback behavior and stale-handle generation checks;
   require lower boundary latency without losing the measured RSS reduction.
3. Investigate immutable library data for diagnostic workers separately.
   The current parent cache does not remove their native parse. Cross-process
   source-location ownership and complete diagnostic equivalence need proof.

These follow-up strategies are proposed, not additional implemented gains.

## Evidence and statistical limits

All **949** performance/memory processes succeed. The [raw archive](artifacts/uvm-prefix-2026-10-19-v4.tar.gz)
is 6,626,734 bytes, SHA256
`d1e431e95e52234b0b095dc6b41631dac1b27673ae32d75f8f4baea03520f7fa`; every archived member was read back and hash-verified.
It contains plans, exact drivers, per-request timings, RSS samples, mechanism
summaries and test logs. Project sources and full oracle response bodies remain
local. Drivers preserve the local fixture layout; they are not a standalone
public reproduction of the private educational project.

Short timing cells use ten balanced four-process ABBA/BAAB blocks. Exploratory
95% intervals in JSON resample block mean differences (on minus off), not
medians. n=20 p95 and long-session maxima are descriptive; there is no p99,
tail or multiplicity-adjusted claim. No failures/outliers were removed.
Fresh processes used a warm filesystem without CPU pinning/frequency control;
other apps were not stopped. Builds, tests and sampling were separated from
ordinary latency runs.
