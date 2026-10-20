# Mac source-witness batching evaluation

This is a development milestone, not integrated acceptance or a publication.
The cdb4 prototype below is superseded: subsequent case-only directory rename
regressions expose an incorrect canonical-identity shortcut on Mac. Its original
measurements remain historical evidence, not acceptance of the current binary.
See [the correction](macos-frozen-mac-directory-identity-2026-10-20.md).
Candidate SHA-256:
`cdb4b6d7d78e2ad7ae326afb3af8b8b678e9f254495498546884ff209c43c151`.
The prior dependency-prefix candidate is
`2efade522bc96523fd50b4bdf83cd5f0f7f12cf5f9a0b30f934742a9c23de67c`.
All new caching experiments remain disabled by default.

## Changes and preserved contract

Each source witness now shares proofs of ordinary directory prefixes within that
invocation. It visits original components in order, checking a directory before
handling a following `..`. Symlinks, missing components, unusual roots and errors
use native weak canonical resolution. Every source alias and leaf is still
checked. Directory proof state is discarded before the final parent-identity
recheck, which runs after all closed reads; nothing is retained across requests.

Graphs with at least 64 closed read witnesses use a process-local pool of four
threads and at most four contiguous read chunks. Every positive source is still
read and compared byte-for-byte; negative read outcomes are still compared with
their recorded errors. Smaller graphs and builds without `SLANG_USE_THREADS` use
serial reads. The caller holds both source-manager read locks throughout. Workers
only inspect immutable data and filesystem inputs. Results are consumed in their
original lookup order, preserving which failure is reported first.

Cancellation and exceptions drain submitted work before releasing locks and
captured storage. Future storage is reserved before submission, so allocating a
future container cannot strand a running task. This is bounded additional I/O
concurrency, independent of graph size and separate from parser thread settings.
It is not a claim that cache bytes bound thread stacks or process-family memory.

## Correctness and restoration

The live-witness group passes 464 assertions in five cases, including the existing
symlink, missing-source, equal-metadata changed-byte and cancellation checks.
New sections cover ordinary nested `.`/`..`, a symlink preceding `..`, a missing
intervening directory that later becomes a symlink, parent replacement after
closed reads, a 129-entry graph, changed bytes in different chunks, a negative
lookup becoming readable and late cancellation followed by successful reuse.

The first new missing-component test incorrectly expected the initial source
read to fail. Native weak canonical resolution normalizes the absent suffix;
the corrected test requires initial success, success after an ordinary directory
appears, and failure when that component becomes a symlink to a different parent.
The initial failure log is retained and no product guard was weakened.

After rebuilding all targets, CTest passes 685/685 (including SDK unit tests),
Python passes 16/16, the focused server group passes 7,557 assertions in 97 cases,
and standalone transport passes 43/43. Six full-design native/enabled language
replays pass across five unsaved states on SPI/PSS/UBUS, including every resolve,
all current diagnostics, error/repair and two actual forced owner retirements per
enabled process. These are correctness runs, not latency samples.

The restoration patch was applied to a clean SDK archive and all 708 manifest
hashes match. Four historical SDK additions remain included. Current patch SHA:
`7dd004ddba6737664f86c6aeba7d3e5d7e4ccfa564e6dc2461e3640013c03397`.
The dirty SDK gitlink must not be committed; publication uses the patch and its
manifest. Temporary profiling probes from the previous milestone remain removed.

## Measurement scope

The exploratory directory-only run uses 18 processes, three paired repetitions
per case, with native/off, prior/on and candidate/on arms. Relative to prior/on,
median sequences excluding setup improve from 1720.74 to 1591.15 ms for PSS and
2234.38 to 2171.28 ms for UBUS. This is a small pilot, not a confidence claim.

A separate combined directory/four-read pilot uses the same 18-process design.
PSS prior/candidate medians are 1855.63/1235.67 ms; UBUS 2524.72/1707.85 ms.
Native medians are 695.41/681.97 ms, so the remaining all-resolve regression is
substantial. These numbers are from distinct runs and cannot be combined into a
controlled estimate of the individual contribution of each optimization.

The confirmation run uses 20 randomized paired observations per case and arm,
120 sequential fresh processes in total. Each case includes initial query,
two alternating unsaved edits, cached queries, every completion resolve and
complete matching-version diagnostics. PSS resolves 171 items total, UBUS 393.
Warm filesystem caches are retained. Builds, tests, editors, profiling and RSS
sampling do not run concurrently with these measurements. All observations and
outliers are retained. Twenty observations cannot establish a tail guarantee.

Confirmation and separate process-family memory results are recorded below when
complete. Full integrated timing against the accepted older UVM-enabled baseline,
long repeated memory, current editor checks and final publication remain open.

## Twenty-pair confirmation

All 120 processes succeed. Milliseconds below are medians; whole sessions start
at process launch and include setup, all requests and final versioned diagnostics.

| Case / metric | Native off | Prior on | Candidate on | Reduction vs prior |
|---|---:|---:|---:|---:|
| W07-pss-multidoc / Whole session | 814.88 | 1896.66 | 1445.14 | 23.8% |
| W07-pss-multidoc / Startup through first response | 109.83 | 119.73 | 122.05 | -1.9% |
| W07-pss-multidoc / First edit | 54.22 | 47.99 | 35.42 | 26.2% |
| W07-pss-multidoc / Every resolve sum | 65.86 | 1306.27 | 903.28 | 30.9% |
| W08-ubus-task / Whole session | 780.14 | 2754.60 | 1918.22 | 30.4% |
| W08-ubus-task / Startup through first response | 100.24 | 114.11 | 111.36 | 2.4% |
| W08-ubus-task / First edit | 53.10 | 33.62 | 26.99 | 19.7% |
| W08-ubus-task / Every resolve sum | 155.88 | 2313.64 | 1515.53 | 34.5% |

Paired mean whole-session savings versus prior/on have percentile-bootstrap
95% intervals of 370.68–527.59 ms (PSS) and 773.07–898.04 ms (UBUS), using
20,000 paired resamples and retaining every observation. Against native the
intervals are negative: -699.20 to -611.90 ms and -1182.82 to -1084.73 ms.
Candidate whole-session medians regress 77.3% and 145.9% versus native. Startup
through first response regresses 11.1% on both cases. Thus this is a verified
mitigation of the preceding enabled strategy, not a pass of integrated performance
acceptance. No first-response speedup versus prior is established by the paired
intervals. Further work must address the remaining per-request alias/input proofs
and source reads, and measure selected-resolve use as well as this stress case.

Six additional exploration comparisons (12 native/enabled processes) pass
W01/SPI, W07/PSS and W08/UBUS, each with macro warning suppression off and on.
They compare complete public responses, every resolve, stale logical-item resolves,
hover/references/highlights/symbols, inactive regions and all latest diagnostic
states. Each enabled process undergoes one forced owner retirement. These runs
are separate from timing and supplement the full-design forced-language checks.

## Separate short family-memory observations (historical cdb4 prototype)

Twenty edits, three repetitions per case/arm, 27 fresh processes all pass. The
sampler observes the server and its diagnostic child simultaneously from before
spawn until immediately before orderly shutdown; Python is excluded. Every row
observes a two-process family. Values are median startup-inclusive peaks in MiB,
reported separately for RSS and physical footprint; samples may miss peaks and
family RSS counts shared pages. There is no latency claim from these runs.

| Case / arm | Parent RSS | Family RSS | Parent footprint | Family footprint |
|---|---:|---:|---:|---:|
| W01-compare / native-off | 161.59 | 238.34 | 127.11 | 194.64 |
| W01-compare / previous-on | 67.81 | 173.30 | 55.53 | 128.95 |
| W01-compare / candidate-on | 68.30 | 173.78 | 56.00 | 113.31 |
| W07-pss-multidoc / native-off | 161.81 | 260.34 | 127.25 | 198.24 |
| W07-pss-multidoc / previous-on | 68.34 | 174.36 | 56.00 | 130.92 |
| W07-pss-multidoc / candidate-on | 68.64 | 174.61 | 56.28 | 139.59 |
| W08-ubus-task / native-off | 176.11 | 271.78 | 141.53 | 227.75 |
| W08-ubus-task / previous-on | 93.44 | 214.00 | 81.05 | 149.27 |
| W08-ubus-task / candidate-on | 93.61 | 214.36 | 81.23 | 149.69 |

The first memory helper incorrectly sampled during server shutdown, so one native
row completed every semantic check but failed the sampler's live-parent assertion
after exit. Its result and exact helper version remain preserved. The corrected
helper stops and joins its sampler before invoking the client's original close
method; the complete 27-process run uses a new tag. It does not suppress a live
process sampling error. These short runs neither establish long-session bounds
nor validate the later canonical-identity correction.
