# General function/task body queries — Mac evaluation, 2026-10-22

This expands the [first restricted slice](method-body-query-evaluation.md).
`experimentalMethodBodyQueries` remains **false by default**. Completion,
completion-item resolution and definition queries can rebind a changed function
or task body in a private compilation using its retained lexical semantic owner.
Diagnostics and hover still use complete current native analysis.

## Supported scope and limits

Body changes may alter local declarations and types, expressions, calls, member
access, nested blocks, loops, conditional statements and macro-expanded code.
Package, module and class subroutines, inherited/parameterized contexts and
out-of-block methods are supported. The signature and surrounding declarations
must remain fixed. A warmed, unambiguous semantic context is required.

This is broad body rebinding, not a promise that every textual edit is local.
Preprocessor state changes escaping the body, ambiguous occurrences, changed
signatures, externally consumed constant-function results or method-local types,
and unsafe line-sensitive macro dependencies trigger native analysis. Incomplete
syntax is accepted only while its parsed boundaries remain confined to the body.
Cancellation, stale completion tickets and manual library rebuild fence the owner.
A certificate accepts at most 64 notifications. Normal source-memory retirement
also releases the certificate; the next eligible edit may certify again.

The retained owner is not modified by detached body binding. New attributes,
local generic specializations and body symbols belong to the scratch compilation;
standard-library types, lexical lookup and inherited context retain native type
identity. Constant evaluation and references to subroutine-local symbols track
external dependents only when the experiment is enabled. Current source ranges
are projected without rewriting the native source cache. Requests for enclosing
class documentation use current native analysis.

## Validation

Release build on the task's Mac, entirely from existing local corpus/dependencies.
Qualified executable SHA256:
`d40878b14d4f67aac716f0af6af894416ef0b404f5e49ac46a7229a2a1336430`.
The SDK gitlink is unchanged; `restoration/slang.patch` and its manifest carry the
SDK changes and verify all 709 inventoried files.

- 762/762 CTest cases; targeted body suite: 17 cases / 770 assertions.
- 17 Python system/method-scope tests and 43 diagnostic transport cases.
- Separate counted executable confirms each accepted edit performs one
  foreground mini-shaped parse; diagnostic work is measured separately. It is
  never used for latency measurements.
- General and original closed-helper synthetic workloads each cover 144 edited
  states: three paired blocks, both arms, plain/UVM, twelve edits per session.
  Public completion ordering, selected resolves, definition ranges and published
  diagnostics match the disabled native path. General edits include local type
  changes, structs, helper/member calls, nested foreach, invalid names and repair.
- Real CVA6 covers 42 edited states across three paired blocks. Both arms freeze
  UVM and the whole DUT. All completion/resolve/definition results and accumulated
  diagnostic maps across 612 files match native analysis, including invalid-name
  diagnostics, repair, source-memory retirement and an explicit macro fallback.

## Synthetic latency

Milliseconds, medians; 5000 neighboring classes. The first column includes first
certification; continuous samples are the eleven later edits in each session.
`didChange` plus immediate completion is the measured foreground operation.

| Workload | Mode | First query | Continuous query | Current diagnostics |
|---|---|---:|---:|---:|
| General plain | Native | 14.19 | 10.11 | 81.33 |
| General plain | Local | 13.35 | 5.38 | 78.22 |
| General UVM | Native | 19.64 | 18.67 | 22.93 |
| General UVM | Local | 21.57 | 11.27 | 29.58 |
| Closed helper UVM | Native | 19.92 | 18.12 | 22.27 |
| Closed helper UVM | Local | 21.51 | 10.48 | 27.60 |

These are same-build paired comparisons. The earlier restricted DTO slice
reported 7.13 ms continuous UVM queries in a separate run; this broader retained
semantic path does not establish non-regression against that specialized slice.
It supports substantially more edits and reduces its previous first-certification
cost, while retaining a native speedup in these workloads. Diagnostic timing does
not improve consistently and is not an optimization claim.

## Real CVA6 RTL and verification environment

Existing original `cv64a6_imafdc_sv39` RTL and full UVM environment, top
`uvmt_cva6_tb`, DUT
`uvmt_cva6_tb.cva6_dut_wrap.cva6_tb_wrapper_i.i_cva6`. No VERILATOR source
substitutions or source repairs. The actual out-of-block
`uvme_cva6_env::build_phase` keeps its original calls/macros while edits introduce
changing local types, a `uvm_phase` handle, `get_name`, and UVM logging. One edit
introduces an undefined user symbol; the next repairs it. The last edit changes
macro state and must use native analysis.

| Measurement | Native | Local enabled |
|---|---:|---:|
| First edited query, ms | 624.93 | 445.59 |
| Warm accepted queries, ms | — | 53.93 |
| All seven query samples, median ms | 622.89 | 444.61 |
| Sum of seven query latencies, median ms | 5662.88 | 3770.53 |
| Current diagnostics, median ms | 941.96 | 927.93 |

The seven-query sum improves about 33.4%, including first certification, normal
memory retirement, recertification and explicit fallback. Enabled per-state
medians are 445.59, 54.33, 53.93, 53.51, 2071.15, 444.61 and 642.27 ms.
The fifth query retires its semantic generation under the normal source-memory
policy; the last intentionally changes preprocessing state. Native has its own
retirement pause at another state. The 54 ms figure applies to warm accepted
queries, not every keystroke or diagnostics. Current diagnostics remain near
0.93 seconds: local diagnostic updating is still outstanding.

## Memory and reproducibility

Memory is sampled separately at 20 ms using process-family libproc accounting,
with a 3 GiB / 180 second guard. Three paired blocks per workload; these sampled
runs supply no latency conclusions. Peak values below are medians across sessions.

| Workload | Native RSS / physical, MiB | Local RSS / physical, MiB |
|---|---:|---:|
| General UVM, 5000 neighbors | 173.19 / 126.64 | 220.50 / 177.77 |
| Real CVA6 | 1341.63 / 1021.81 | 1501.20 / 1151.11 |

CVA6 costs about 160 MiB additional peak RSS and 129 MiB physical footprint.
The implementation retains one lexical owner plus the current scratch body,
without a generation chain; these short sessions are not a long-session bound.
No memory-reduction claim is made. Experiments remain off by default.

Tracked reproduction entry points are `tests/method_scope/general_queries.py`,
`body_queries.py` and `body_counts.py`. Local original-corpus scripts, final JSON,
logs, installation and checksums are retained under
`untracked/general-body-2026-10-22/`; the CVA6 script restores project settings
and asserts source bytes are unchanged. The previous evaluation is historical,
not the coverage description of this implementation.

Next useful work is local diagnostic invalidation/recomputation and reducing the
extra lexical-owner retention; neither is silently included in the query gain.
