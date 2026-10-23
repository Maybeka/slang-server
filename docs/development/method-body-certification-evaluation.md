# Method-body certification: Mac CVA6 evaluation

The stage reduces repeated input verification and preserves an original diagnostic baseline
across superseded edits. `experimentalMethodBodyCertification` defaults to false; no fixed
debounce is added. See the [feature contract](../features/method-body-certification.md),
[five-category status](performance-roadmap-status.md) and
[evaluation data](../../validation/data/method-body-certification-2026-10-23.json).

Baseline main: `72a99f102ec347ba64a91ffba478244dd7c11069`, binary SHA256
`07dfe95cf883358c2d6b0655d565daa09a88d8ca2249d2f9b6cafb3966b4d228`.
Candidate binary SHA256:
`8b867738794e5cf7484a1332d37ee9122aa42c822e9c68f476aea2ea5150d77f`.
The cached CMake version is still `49569ff`; the receipt binds source and binary identity.
Both arms enable certificate renewal, cooperative analysis, method-body queries/diagnostics,
UVM semantic reuse and whole-DUT reuse. Only the candidate enables the new flag. Native
uses the candidate with method-body queries/diagnostics disabled. All arms use the same
physical UVM sources and original CVA6 + full core-v-verif project: 258 roots, 404 DUT
instances and 612 diagnostic URIs. Target: `uvme_cva6_env_c::build_phase`.

## Implementation and correctness boundary

Preflight checks owner, index/epoch, configuration, current directory and open-document
witnesses. Full directory listings and file bytes remain mandatory before installing a bound
edit, returning a query result or committing local diagnostics. Query-local lazy binding
uses the cheap check; its outer response performs the complete proof once after construction.
Completion, definition, hover, empty hover and resolve follow the same rule. There is no
mtime-only certificate or persistent memo of a previous successful publication check.

Before edited text invalidates the full-pass completion marker, capture one completed native
compilation, its raw diagnostics and one target-file text value. Superseded notifications can
synchronize text while retaining this original baseline. Detached-original comparison and
retained diagnostic formatting run on diagnostic demand. Cancellation preserves the capture
for retry without changing the published map. Successful certification releases the temporary
capture; the ordinary diagnostic cache retains one original owner. Failed preprocessing or
ineligible dependencies discard it before native fallback; retirement clears the whole context.
No chain of edited semantic owners is retained.

Current reference eligibility and later-macro line compatibility are checked before expensive
retained formatting. These checks preserve existing limits: line-count changes affecting
later macro bodies and new external reference usage still require native diagnostics. This
stage does not implement arbitrary method-body diagnostic updates or atomic filesystem snapshots.

## Ordinary latency: three paired sessions per short scenario

Independent inputs arrive every 100 ms; no phase trace or RSS sampler, diagnostic delta off.
Rows show the median of three sessions' final request and final diagnostic latency. The second
session reverses arm order. Each scenario has an independent Native oracle, including the
complete terminal 612-URI map and every successful hover. Cancelled obsolete queries are
counted separately; every final request succeeds.

| Scenario | Final hover, baseline → candidate | Final diagnostics, baseline → candidate | Successful / cancelled queries, baseline → candidate |
| --- | ---: | ---: | ---: |
| Historical cold, 12 inputs | 46.86 → 39.29 ms | 859.75 → 843.18 ms | 36 / 0 → 36 / 0 |
| Eligible first single input | 230.33 → 82.84 ms | 263.58 → 218.71 ms | 3 / 0 → 3 / 0 |
| Eligible first burst, 12 inputs | 51.09 → 36.65 ms | 856.05 → 175.56 ms | 33 / 3 → 35 / 1 |
| Warm, two priming edits then 12 inputs | 48.13 → 34.96 ms | 82.96 → 53.51 ms | 36 / 0 → 36 / 0 |

Historical cold keeps the previous multiline prefix, including a new `get_name()` call and
later UVM macro line shifts. Its diagnostic path remains native; the three candidate terminal
diagnostic samples are approximately 843, 930 and 836 ms. This stage does not solve that cold
tail. The eligible first variants are explicitly different: a single-line local declaration,
existing `phase` reference and constant-message UVM macro, preserving the external-reference
set and line count. Their single-input case includes first certificate construction; their
burst case demonstrates that superseded text sync no longer loses the original baseline.
They must not be pooled with historical cold to claim all first edits are faster.

## Long session and fallback

One ordinary 1,008-input session per arm, reverse order, includes the same two priming edits.
Successful hover p50/p95 are 45.80/77.84 → 34.85/55.86 ms. Baseline succeeds 1,007 times with
one obsolete cancellation; candidate succeeds all 1,008 times. Both accept 1,010 edits and
cross 15 certificate renewals with zero retained legacy handles. Candidate records 1,004 local
diagnostic updates versus baseline 900. Final complete diagnostics and all successful hovers
match Native; post-input idle preserves the map.

The final single sample regresses: hover 46.16 → 62.02 ms, diagnostics 72.58 → 79.90 ms.
This remains in the data rather than being replaced by a favorable run. Lower session p50/p95
and three short warm pairs support typical-cost improvement, not a universal tail guarantee.
The long session is one observation, not an indefinite-session distribution.

A separate 12-input global-macro fallback session has final hover 964.41 → 745.93 ms and
diagnostics 1294.18 → 1064.39 ms; successful/cancelled queries are 5/7 → 6/6. Both finish via
native fallback with identical complete maps. This single fallback observation is a safety
check, not evidence that the new certification path accelerates full semantic compilation.

## Mechanism and memory probes

Separate probes enable delta publishing, phase traces and 20 ms server-family RSS/physical
footprint sampling. Python is excluded. The 3 GiB/180-second per-arm guard is a test guard,
not a process-memory budget promise. Their timing samples are not pooled with ordinary runs.

| Probe | Peak family RSS, baseline → candidate | Peak footprint, baseline → candidate |
| --- | ---: | ---: |
| 1,008 continuous inputs | 1539.56 → 1537.14 MiB | 1194.56 → 1144.91 MiB |
| Forced retirement every 3 edits; 12 inputs | 1521.31 → 1521.16 MiB | 1176.49 → 1176.08 MiB |

In the long sampled probe, complete file-validation stages decrease 4,658 → 2,971,
with medians 15.189 → 15.635 ms and accumulated stage time
71.51 → 46.64 s. These stages can repeat within a request and are
not disjoint end-to-end latency samples. Local parse/bind medians remain
0.267/0.096 ms.

Natural sampled retirement records one successful replacement per arm; cancelled replacements
are 172 → 303. Query cancellations are separate, 42 → 8. These counts do not establish a retirement-efficiency win.
Both probes pass their guards, all final queries and full diagnostic maps; delta-mode post-input
idle adds zero publications. Source storage growth and old/candidate overlap remain; one finite
memory probe does not establish an indefinite bound or a repeatable memory reduction.


The final-binary eligible-first mechanism probe records initial source-witness construction
at 63.29 ms, raw baseline capture at 8.07 ms and first diagnostic certification at 123.94 ms.
This explains why preserving the baseline does not make first diagnostics tens of milliseconds.
Next candidates are reusing a provably projectable native diagnostic representation and
avoiding duplicate initial witness reads; neither is implemented here.

## Validation and delivery

Seven new certification cases plus four extended query cases pass 989 assertions. Coverage
includes primary/include files, consecutive superseded edits, cancellation and retry, exact
publication proof counts, same-size/same-timestamp disk mutation during construction, diagnostic
mutation before publication, fallback/recapture, hard retirement, macro line changes and
preprocessing failure/repair with an outside-file diagnostic. The latter prevents keeping stale
outside diagnostics when no foreground query runs between an unsafe edit and repair.

Complete qualification passes 792 CTest entries, 17 Python tests and 43 transport cases.
Clean SDK archive plus restoration patch reproduces 709 files byte for byte; gitlink remains
`151a801ceaf6faf23d79a42d210ff67e50eb4660`. No new SDK change is needed in this stage.

The original-project public oracle passes 42 states across both query orders, covering hover,
completion/resolve, definition, all three macro previews, error/repair, repeated results, native
macro fallback and complete diagnostic maps. Installed default UVM uses the shipped preparse
with zero syntax parse time; the external profile hits its project cache with zero syntax
parse time. Both UVM and entire DUT reuse remain active. Only one default preparse ships;
other configurations remain project-local once-caches. Full qualification is accepted.
Experiments remain default off; final source/commit/binary receipts accompany the raw evidence.

Raw scripts and evidence: `untracked/body-certification-2026-10-23/`. Exploratory older-binary
measurements are kept separately from final qualification; no network source/dependency fetch
is required. Temporary test products and obsolete intermediate logs are removed, while active
sources, reusable project caches, comparison installations and raw final evidence are retained.
