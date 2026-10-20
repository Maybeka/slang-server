# Shared parent planning for live alias witnesses on Mac

Candidate SHA-256:
`d678052a67e742b0052da5e2ce8092bc4b0dd7b485d6ccea9e1e5b7ac59e17c3`.
Comparison candidate:
`f9267d8f9b85dd4d0b522dd0426382bf77805cb1933680912ec3aaafa5d2f64e`.
Experiments remain disabled by default. Performance acceptance is pending.

## Implementation

Large live alias sets use four contiguous tasks and share the existing process
pool of four threads with closed-source reads. The two phases run sequentially;
this does not create another pool. The caller holds both source-manager read
locks. All source alias/target records are read-only during tasks.

The caller resolves each eligible parent once into an immutable table. Workers
still check every leaf's actual type/name, preserve native symlink/error fallback,
and perform complete alias resolution when the ordinary-parent shortcut cannot
be used. Each task records which parent entries it actually used in separate
vector slots. After all tasks complete, only those parents participate in the
existing fresh parent recheck after every closed-source byte/error comparison.
A used parent that could not be certified declines reuse. Unused parents do not
introduce false invalidations for symlink aliases whose resolved target is stable.
Nothing persists across witnesses; there is no timestamp/TTL/content shortcut.

Fixed future storage is established before submission. Cancellation is checked
only on the caller; an atomic stop flag lets alias workers finish early, and a
guard drains every submitted future before captured state and locks are released.
Chunk results are consumed in input order. Small sets and non-threaded builds
use the serial path. Temporary proof structures are distinct from retained cache
budgets and still require process-family memory measurement.

## Evidence so far

The live-source group passes 1,142 assertions in six cases. New large-graph tests
cover a shared parent becoming a symlink to equal bytes, an ordinary leaf becoming
a symlink, case-only filename identity, a missing file appearing, cancellation at
multiple stages and successful reuse after cancellation. Existing exact-byte,
negative-read, directory case, after-read parent change and serial tests remain.

The whole build and acceptance pass 685/685 CTest (including SDK tests), 16/16
Python, 7,557 focused assertions in 97 cases, and 43/43 transport. Six full-design
forced-language processes and twelve exploration/native comparisons pass on real
SPI/PSS/UBUS; the latter include every resolve, stale handles, warning filtering
off/on, all latest diagnostics and inactive regions. Forced retirements are
observed, not inferred. A separately compiled serial SourceManager branch passes
the same 1,142 live-source assertions; this is not a whole-SDK non-threaded build.

The reconstruction patch applies to a clean SDK archive and all 708 file hashes
match; four historical SDK-added files remain preserved. Patch SHA-256:
`074b504df491b71b5206bd6923a804587e579f1618fc46cd657667056855e8fa`.

An earlier independent-parent-per-chunk prototype passed correctness but had
mixed small-pilot performance: PSS sequence excluding setup improved 2.7% while
UBUS regressed 4.7%. It is retained as an experiment, not claimed as an accepted
optimization. The shared-parent pilot has 18 successful processes, three paired
observations per arm/case: PSS prior/current sequence medians are 1298.15/1238.27 ms;
UBUS 1812.89/1740.68 ms. These roughly 4–5% changes are exploratory and insufficient
for a stable benefit claim. The 120-process, twenty-pair confirmation is complete; exact results are below.

The separate 300-process comparison of f926 to accepted old UVM caching shows
that the generic route still regresses ordinary selected-resolve sessions by
29–42% versus the old UVM route. This experiment must not hide that finding or
replace the old route prematurely. See the
[integrated class-session report](macos-frozen-integrated-class-sessions-2026-10-20.md).
Final baseline/fallback/RTL/memory gates and publication remain incomplete.

## Twenty-pair confirmation

All 120 processes succeed. The prior/on arm is f926; current/on and native/off
use d678. The same real PSS and UBUS inputs, initial state, two unsaved edits,
cached queries, every resolve and complete target-version diagnostics are used.
No build, tests, editor, profiling or RSS sampling overlaps timing.

| Case / metric | Prior on | Current on | Reduction |
|---|---:|---:|---:|
| W07-pss-multidoc / Startup-inclusive whole session | 1416.54 | 1316.93 | 7.0% |
| W07-pss-multidoc / Every resolve sum | 883.26 | 794.12 | 10.1% |
| W07-pss-multidoc / Startup through first response | 115.14 | 115.99 | -0.7% |
| W08-ubus-task / Startup-inclusive whole session | 1911.80 | 1728.47 | 9.6% |
| W08-ubus-task / Every resolve sum | 1508.56 | 1337.85 | 11.3% |
| W08-ubus-task / Startup through first response | 107.70 | 109.79 | -1.9% |

Paired mean whole-session saving intervals are 66.69–129.06 ms for PSS and
158.32–253.38 ms for UBUS (20,000 paired bootstrap resamples, all observations
retained). Median whole reductions are 7.0%/9.6%; every-resolve sums improve
10.1%/11.3%. First-response paired intervals include zero, so no first-response
improvement is established. These results support an incremental mitigation,
not a resolution of the native/accepted-UVM regressions. Twenty samples do not
establish a tail guarantee.

Three repeats of 1,000 natural full-design edits on each SPI/PSS/UBUS completed
separately for d678; all nine processes pass. This is enabled-only long-memory coverage; old/native
comparisons and exploration-worker family coverage remain separate gates.

## Enabled-only repeated 1,000-edit memory result

All nine sequential processes complete 1,000 natural edits plus user-error/repair,
declaration-change/repair and manual refresh. Every state checks full hierarchy
and the complete current-version target diagnostics. Compressed raw hashes are
verified. Each process observes actual owner retirement and a sampled source
location-entry reset. Declaration changes increase invalidations without a
cross-edit value hit; refresh increases preparations. Combined syntax/value
retention stays within the configured 64 MiB throughout sampled marks.

Startup-inclusive parent/family peaks in MiB are below. All raw samples observe
only the server: full-design diagnostic snapshot capture intentionally uses the
native compilation and does not start a diagnostic worker. This validates parent
lifetime, not exploration-worker family bounds. RSS and footprint are separate
measures; sampling can miss peaks. All outliers remain included.

| Project | RSS median | RSS range | Footprint median | Footprint range | Retirements in each repeat |
|---|---:|---:|---:|---:|---|
| official-ubus | 242.38 | 241.69–242.59 | 171.20 | 170.56–171.50 | [31, 31, 31] |
| spi-block | 232.00 | 230.00–232.53 | 163.39 | 163.25–217.75 | [8, 8, 8] |
| spi-pss | 247.33 | 246.08–251.33 | 144.99 | 143.77–196.83 | [8, 8, 8] |

SPI and PSS have higher single-repeat footprint peaks; these are retained rather
than replaced with their medians. No baseline-relative memory saving follows
from this enabled-only matrix. Compatible accepted/native controls and long
exploration-worker runs remain necessary. This closes the enabled full-design
three-repeat lifetime run, not the complete memory or performance gate.
