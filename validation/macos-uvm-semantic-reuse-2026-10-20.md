# Mac trusted UVM semantic reuse — 2026-10-20

Qualification: complete for the explicitly immutable UVM contract below. The new
experiment remains disabled by default. This qualifies UVM package semantic reuse;
it does not qualify general frozen packages/modules/RTL or a persistent database.

## What is shared

The selected UVM package is elaborated once. Subsequent compilations import the
same package, fixed classes/methods and built-in class identities. They retain its
syntax/source namespace across normal consumer context retirement. Generic
specializations depending on user types belong to the current consumer. Current
flow/usage diagnostics still run against combined library/consumer references.
This is more than sharing parsed syntax, and does not mean every diagnostic or
consumer-dependent operation has been precomputed.

Dependencies and library macros are trusted unchanged until the explicit
`slang.rebuildUvmLibrary` command or configuration reload. Known library watcher
notifications do not invalidate that generation. User overlays remain current.
Unsupported preparation or excessive initial retention charge uses ordinary
analysis. Configuration, status, limits and the rebuild command are described in
[the feature guide](../docs/features/uvm-semantic-cache.md).

Candidate executable SHA-256:
`261c36a22edbd8447c4188518c4d97616be5029146fad82bfdc30a8b96c6acc3`.
The same executable was used for final correctness, timing, memory and editor
checks. SDK base: `151a801ceaf6faf23d79a42d210ff67e50eb4660`;
SDK patch: `904c931b8938bd37741ece861a897f714ae361e7ed7bff6c473450eec27001f5`.
All 708 SDK files match a clean base export plus the patch. The SDK gitlink is
unchanged; its local modifications are represented by the restoration patch. The optional
Python binding annotation follows the new defaulted C++ argument; the server
configuration does not build that extension, so no standalone extension-build
qualification is claimed. This annotation leaves the measured server executable
byte-identical.

The released reference is main `5a5389877fab70df1a705c62794e309451572377`,
executable SHA-256 `c30d503050c7f499e30c27f36fd25dc0f058a11decb08ded4540baa53db624a9`.
It is this fork's previously released UVM cache, not an upstream comparison.
Earlier syntax-cache results are not relabelled as semantic-cache measurements.

## Correctness and lifetime

- Full CTest: 687/687, including the complete SDK unit executable; server Python tests: 16/16;
  standalone diagnostic transport: 43/43. Formatting uses clang-format 20.1.8.
- 60 real-project off/on processes compare ordered completions, every resolve,
  definitions, hover, references, highlights, document symbols, current-version
  diagnostics, inactive regions and stale handles. SPI, PSS and official UBUS
  retain their original source order. Outer diagnostic-list sorting preserves
  duplicates, values and ordered related information.
- Another 16 processes force actual consumer retirement. The same UVM generation
  and semantic allocation size survive retirement. Full-design off/on checks
  pass for all three projects, including unsaved widths and PSS bind removal and
  repair; all library warnings are included in those comparisons.
- Three real-library navigation/rebuild runs pass. Three isolated LazyVim runs
  pass completion/resolve/definition/hover, macro preview modes and two unsaved
  edits. Four relocated-install runs pass explicit/bundled UVM, off/on, errors,
  repair and reconstruction with equal public responses.
- Focused trusted-package SDK tests: 41 assertions in three cases. They cover
  type identity, consumer-specific generic errors/repair, extern method bodies,
  duplicate-package rejection and changing consumer references. Server trusted
  tests: 76 assertions in two cases. Held compilations keep old source storage
  readable after replacement, then weak ownership checks prove retired consumer
  and old library storage are released when the last lease is dropped. The budget
  decline test also performs an actual native UVM consumer analysis.

Two prototype defects were fixed before qualification: foreign generic extern
method bodies were allocated in the retained library, and imported packages were
omitted from full flow/usage analysis. Fixed-library AST bytes now stay constant;
full-design warning sets match ordinary analysis. Provisional invalid-generic
checks are superseded and diagnostics coalesced with actual consumer instances.

One preliminary forced comparison sampled secondary-document notices before
publication and differed. Under concurrent CTest load, before/after capture
reproduces a missing secondary PSS diagnostic/inactive notice, followed by equal
settled values. The additional 16-process comparison passively waits 400 ms for
secondary notifications, without discarding entries or changing values. Both
that comparison and the final original 16-process immediate comparison pass.
This establishes eventual secondary-notification equality, not equal arrival
order. Timing runs use neither this wait nor status probes. The historical W12
UTF-16 oracle issue remains excluded and is not counted as a new success.

## Latency

Apple M4, 24 GiB RAM, macOS 27.0.1 (26A434), Homebrew clang 23.1.3, Release build.
Warm filesystem, fresh sequential processes, randomized paired blocks, 20
observations per arm/case, ten alternating unsaved edits. Whole-session time
starts at spawn and includes initialization, preparation, first query, repeated
queries, selected resolves and each target-version diagnostic; shutdown is
excluded. Builds, tests, editor sessions, tracing and RSS sampling do not overlap
timing. All 560 scheduled observations succeed; failures from preliminary
functional investigations remain separate evidence.

Medians in milliseconds, old UVM syntax cache versus the same candidate's new
semantic mode:

| Scenario | SPI | PSS | UBUS |
|---|---:|---:|---:|
| Whole session, old → semantic | 843.74 → 395.77 | 1314.29 → 422.96 | 918.41 → 387.86 |
| Paired median whole-session saving | 53.36% | 67.88% | 57.97% |
| Startup through first response | 117.40 → 146.76 | 120.30 → 143.20 | 108.99 → 139.72 |
| First edit query | 16.18 → 15.02 | 16.31 → 16.29 | 17.40 → 16.11 |
| Continuous query | 15.92 → 15.15 | 16.17 → 16.09 | 16.93 → 15.77 |
| Continuous edit through diagnostic | 62.78 → 23.84 | 105.36 → 25.32 | 70.35 → 22.94 |

Paired mean whole-session saving bootstrap 95% intervals are
52.75–54.35%, 66.92–68.37%, and 57.35–58.18%, respectively. These describe the
sampled workload, not a latency-tail guarantee. Against the released old-UVM
executable, paired median savings are 53.58%, 67.89%, and 58.28%.

Preparation adds roughly 23–31 ms to the first response. This is an explicit
one-time tradeoff per trusted generation; normal context retirement does not
prepare another library generation.

Burst runs wait only for initial/final diagnostics, not intermediate ones.
Paired median total savings are SPI 7.48%, PSS 22.17%, UBUS 19.25%. SPI continuous
query median rises from 15.25 to 19.51 ms, while PSS/UBUS remain approximately
16 ms. Parent-process diagnostics can delay incoming foreground work. Idle
coalescing or reusable flow analysis is a follow-up strategy; it is not claimed
implemented here.

Resolving every item in each completion list retains PSS/UBUS paired median
whole-session savings of 55.59%/41.01%. SPI's large global list has no established
benefit: 3614.05 → 3653.31 ms, paired median saving −1.23%, paired mean saving
−0.14%, 95% interval −2.15% to +2.15%. That list is dominated by roughly 3.2 s of
bulk resolves. It is reported as neutral, not a speedup. The all-mode harness also
repeated SPI definition queries; those 40 controls are not called bulk completion
measurements. A separate 40-process SPI completion run supplies the actual bulk
results.

A contemporary default-off UBUS control, 20 pairs against the released native
executable, shows no established regression: whole medians 2190.65 → 2186.05 ms;
paired mean saving −0.56%, 95% interval −1.45% to +0.27%. This control covers one
heavy workload, not universal performance equivalence.

## Memory and generations

Memory is sampled separately at 20 ms using Mac process RSS and physical
footprint, for the parent and simultaneous descendants. Python is excluded;
process ledgers are summed and shared pages are not deduplicated. Peaks between
samples can be missed. Three repeats are observations, not confidence bounds.

For the common 20-edit sequence, median peaks in MiB are:

| Project | Parent RSS old → semantic | Family RSS old → semantic | Family footprint old → semantic |
|---|---:|---:|---:|
| SPI | 71.34 → 104.25 | 181.28 → 104.25 | 129.39 → 93.11 |
| PSS | 68.08 → 104.56 | 180.67 → 104.56 | 135.00 → 93.34 |
| UBUS | 93.00 → 123.48 | 219.20 → 123.48 | 156.11 → 112.30 |

The parent retains more semantic state; the isolated diagnostic child is avoided.
The family footprint decreases by roughly 28–31% in that matched sequence.
Error insertion/repair, suffix-macro changes and save are also checked. All raw
samples are retained with three explicit cuts: final edit, end of shared stress
phases, and the complete run including the current-only manual rebuild.

Manual reconstruction is additional stress only on the current executable;
it must not be folded into an unmatched ordinary-edit comparison. Including that
stress, semantic-mode short-run median RSS peaks are 167.58/180.47/181.48 MiB
and footprint peaks 139.86/162.14/161.95 MiB. The complete old-reference stress
runs have family footprint peaks 129.39/177.64/156.45 MiB, so reconstruction does
not show a universal footprint reduction. Old/new generations briefly coexist.
Both matched and complete-run peaks are preserved in the compact evidence.

Nine semantic-mode sessions perform 1,000 edits each, three repeats per project.
All matching-version diagnostics and error/repair/rebuild checks pass. SPI/PSS
perform 12 natural consumer retirements per session; UBUS performs 38. UVM
remains generation 1 with constant AST bytes throughout edits. The explicit
rebuild creates generation 2. Fixed AST allocation is 26,533,888 bytes for
SPI/PSS and 27,463,680 for UBUS; initial retention charge is about 55.7/59.4 MiB.
The charge is not an RSS limit and excludes later mutable-context history.

Long-run core RSS peaks are approximately 176/176/194 MiB. Samples at edits
500 and 1,000 are approximately flat; complete peaks including manual rebuild
reach 208/233/234 MiB. This is evidence for retirement and a plateau in the
sampled workload, not a proof of a universal memory ceiling. No fresh 1,000-edit
reference/off matrix was collected for this narrow implementation step, so no
comparative long-run memory saving is claimed.

## Evidence and next work

[Compact measured evidence](data/uvm-semantic-reuse-2026-10-20.json) records hashes,
counts, timing summaries and both memory scopes. Raw protocols, samples, failed
prototypes, scripts, manifests and pinned executables remain in the workspace's
untracked evidence directory. Reproducible fixture copies and unused exports are
cleaned after validation; the original corpus and active build caches remain.

Next candidates are to coalesce synchronous diagnostics during bursts, reduce
initial preparation cost, and avoid reconstructing flow results for fixed
library internals while recomputing current usage warnings. The large SPI global
resolve list needs its own attribution before promising further gains. Generic
stable-scope semantic reuse remains a separate implementation step.
