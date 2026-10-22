# Local method-body hover and first certification — Mac, 2026-10-23

`experimentalMethodBodyQueries` remains disabled by default. It now covers eligible
hover and macro-preview requests through the same detached method index as completion
and definition. Local diagnostics remain independently disabled by default through
`experimentalMethodBodyDiagnostics`. No fixed debounce is added.

## Changes and correctness boundary

Hover borrows the retained lexical context and uses current method syntax, guarded
constant evaluation and projected source locations. The native hover renderer keeps
code, type widths, links, inherited methods, system subroutines and macro
source/expanded/both views. An empty result is distinct from a request to fall back.
Cancellation unwinds projection and keeps the reusable context.

A negative module-task test found that the private view omitted a native driver
snippet for a static local. The implementation now refuses private driver queries
for persistent values, design signals, connected ref-port values and other unproven
driver targets. These requests use current native analysis. Automatic variables,
parameters and aggregate fields can render locally. Tests cover package functions,
automatic module tasks, static module-task fallback, local packed-type width changes,
class locals, inherited methods, macro views, empty results and cancellation.

The first query certificate previously replayed every preprocessing token before the
method entry. Normal parsing can now record a transient journal of macro definitions,
removals and resets, plus function/task keyword positions and keyword versions.
Certification replays only those deltas, without copying the full macro environment
at every method. Command-line macros, inherited prefix definitions, redefinitions,
conditional branches and undefine-all retain native ordering. Recording is enabled
only with body queries; trees restored without the optional journal keep the original
preprocessing replay. The archive format and shipped default UVM preparse are unchanged.
Mutable file witnesses, outside dependencies and library generations are still checked.

## Workload and controls

The original CVA6 `cv64a6_imafdc_sv39`, full core-v-verif environment, 258 explicit
roots, 612 diagnostic URIs and complete selected DUT remain unchanged. UVM and
`uvmt_cva6_tb.cva6_dut_wrap.cva6_tb_wrapper_i.i_cva6` are frozen in every arm. Source
pins and census follow the [whole-DUT evaluation](cva6-dut-reuse-evaluation.md).
No workload source fixes or network downloads are used.

Three modes use the same command file and one explicit, shared UVM source directory
from the preceding qualified installation. This keeps actual definition and diagnostic
URIs identical across binaries; no paths or diagnostic contents are normalized.
Default bundled-UVM installation behavior is verified separately.

- **Native:** current binary, both body experiments off.
- **Previous:** qualified `8dc0d4f` binary, both body experiments on, native hover.
- **Local:** current binary, both body experiments on, local hover and macro journal.

Three randomized paired blocks test two request orders: hover immediately after
`didChange`, and completion followed by hover. Each session edits the existing
out-of-block `uvme_cva6_env_c::build_phase` seven times. Edits change int/longint
locals, access a phase handle and inherited `get_name`, use ordinary UVM logging,
introduce and repair an undefined initializer, then change preprocessing state to
force native fallback. The first edit adds lines before later macro bodies and
requires full diagnostics. The next eligible edit pays first diagnostic certification.

Every state compares complete accumulated diagnostics (including empty URI lists,
duplicates, notes and ranges), ordered public completions, selected resolves,
definitions, variable/method hovers and all three macro previews. Only outer diagnostic
ordering is normalized. Instrumented mechanism runs and memory samples are separated
from latency qualification. Query and diagnostic windows overlap and must not be added.

## Serialized response times

Milliseconds; median of three session-level measurements. Warm values use each
session's median over the identical versions 4 and 5, including the introduced
initializer error. An original query and full-design diagnostic baseline complete
before edits begin; these are first body-certificate costs, not cold server startup.

| Measurement | Previous local-query/diagnostic binary | Current local hover + journal |
|---|---:|---:|
| First edit directly to hover | 1155.32 | 119.12 |
| Warm edit directly to hover | 774.67 | 55.38 |
| First completion response, completion-first order | 366.71 | 116.13 |
| Warm edit through completion and hover | 851.53 | 64.64 |
| Hover request after warm local completion | 787.47 | 14.72 |
| First eligible diagnostic-certificate completion | 187.76 | 178.50 |
| Seven edit-to-hover windows summed, direct order | 6563.25 | 1236.61 |
| Explicit macro fallback, direct order | 740.69 | 725.69 |
| Repeated idle hover after diagnostics, direct order | 0.24 | 16.84 |

All **126** states match the public oracle. Local diagnostic counters show five
accepted publications per local session; a body-query hit is not substituted for
a completed diagnostic hit. Native controls are retained in the compact data.
Initial setup through original-query diagnostics is approximately 3.3–3.4 seconds;
the change does not remove startup analysis.

Mechanism traces on the qualified installation restore the macro environment in
87 / 85 microseconds (bundled / explicit UVM). The preceding qualified binary's
instrumented replays take 217–285 ms in the same CVA6 recipe. These traces are
separate from timing qualification. The optional metadata is a preprocessing
journal, not a persisted or general semantic checkpoint.

First diagnostic certification still costs about 178 ms through the completion
response, compared with the preceding 188 ms sample. This round mainly removes
prefix replay and native hover rebuilding; it does not eliminate baseline binding,
whole-baseline diagnostic partitioning or mutable-source witness reads.

The idle repeated-hover regression is retained: exact mutable-input validation
costs roughly 14–17 ms locally, versus about 0.2–0.3 ms in a ready native context.
The gain concerns changed method bodies. Native driver fallbacks and unprepared
contexts still have visible analysis costs. No percentile or latency bound follows
from these short samples.

This query-rich sequence deliberately issues completion, definition and macro
previews before waiting for diagnostics. Its local diagnostic windows are about
198 / 171 ms (direct / completion-first order), including those additional requests.
The original completion-only scenario is measured separately below; these two
workloads' diagnostic windows are not interchangeable.

## Independent input arrivals

Three primed and three unprimed trials schedule twelve edits every 100 ms,
independently of responses. This interval belongs to the client, not server debounce.
Expressions include incomplete arithmetic, parentheses, an unresolved name and
repair. Primed trials first establish query and diagnostic certificates using two
edits. Unprimed trials still have the original query/full-diagnostic context but
have no body certificate.

| Primed mode | Successful / canceled, out of 36 | Latest-response median ms | Successful-response median ms | Maximum successful ms |
|---|---:|---:|---:|---:|
| native | 36 / 0 | 11732.51 | 6734.56 | 11734.43 |
| previous | 3 / 33 | 1108.65 | 1108.65 | 1427.41 |
| local | 36 / 0 | 46.57 | 49.27 | 96.22 |

Every successful hover matches its native oracle, including valid empty results;
cancellations are not counted as successful responses. Each trial also compares
the final complete 612-file diagnostic map.

Unprimed trials remain problematic. Current mode succeeds 9 times and cancels 27;
the previous mode succeeds 3 times and cancels 33. Latest-response median is
**1171.68 ms current versus 989.68 ms previous**. More intermediate hovers return,
but the final cold response regresses in this sample. First-line native diagnostics,
incomplete-input fallback and noninterruptible native phases remain priorities;
the warm result must not be presented as a cold-input guarantee.

## Completion/diagnostic continuity and memory

A separate three-block run preserves the preceding completion-only CVA6 script:
42 states compare complete diagnostic maps, ordered completions, definitions and
selected resolves. On the current binary, local diagnostics enabled/disabled give
warm query medians 56.33 / 519.20 ms and diagnostic windows 112.83 / 1123.13 ms.
The disabled arm includes a context-retirement event in the selected warm states.
A same-session previous/current completion-only pair is recorded in compact data.

Three additional randomized previous/current pairs keep both local experiments
on and use the identical explicit UVM path. All 42 complete diagnostic/query states
match. First completion is **400.58 → 115.66 ms**, warm completion **54.25 → 53.67 ms**,
and warm diagnostics **107.99 → 106.22 ms**. This contemporaneous comparison preserves
the previous continuous-edit gain. The earlier 42/81 ms measurements were taken in
a different run; comparing only those historical numbers would conflate host/run
conditions with a product regression.

Memory is sampled separately every 20 ms over one paired block (two request orders,
two sessions per mode). Values below are medians of observed server/diagnostic-process
family peaks; the Python client is excluded. No sampled-run latency is used above.

| Mode | Peak RSS MiB | Peak physical footprint MiB |
|---|---:|---:|
| native | 1385.12 | 1153.57 |
| previous | 1627.34 | 1345.74 |
| local | 1263.77 | 1023.96 |

All sessions stay within the sampler's 3 GiB / 180-second guard and exit. These
finite windows do not establish an unlimited-session memory bound. The journal is
proportional to macro changes plus function/task keyword occurrences; continuous
mini parses do not append it to a chain of older method generations.



## Validation and evidence

779/779 CTest entries pass, including 2,589 SDK cases; 17 Python tests and 43
transport cases pass. Hover-specific tests have 2 cases / 147 assertions, and the
macro-journal native-environment comparison has 1 case / 16 assertions. Tests use
canonical `TMPDIR=/private/tmp` on Mac.

Qualified installed SHA256:
`8f4d096f62438e63b7453adda48fd397acf50d02672b20660d0f1ed1714078cf`. Its embedded source base is `8dc0d4f`.
The qualified binary and final source commit are bound by the local receipt rather
than rebuilding solely for its embedded version string. Raw reports, scripts,
installation and cleanup receipt are in `untracked/hover-local-2026-10-23/`.
Compact tracked data are in `validation/data/method-body-hover-2026-10-23.json`.

SDK restoration preserves its original gitlink. A clean export/reconstruction checks
all 709 inventory files, including the five restoration additions. The macro journal
is transient metadata rather than a persisted checkpoint; the shipped default UVM
archive still loads without parsing UVM sources. Other configurations retain their
project-local one-time cache behavior.
