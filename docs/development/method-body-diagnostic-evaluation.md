# Local class-method diagnostics — Mac, 2026-10-22/23

`experimentalMethodBodyDiagnostics` is disabled by default and requires
`experimentalMethodBodyQueries`. This extends the
[continuous-input evaluation](body-edit-responsiveness.md); it does not change
UVM packaging or the fixed-provider/manual-rebuild contract.

## Workload and comparison

The original CVA6 `cv64a6_imafdc_sv39`, full core-v-verif environment and UVM
are unchanged. Both primary arms use the same installed release binary, shipped
default UVM CST, UVM semantic owner and entire frozen DUT
`uvmt_cva6_tb.cva6_dut_wrap.cva6_tb_wrapper_i.i_cva6`. There are 258 explicit
roots, 612 diagnostic URIs and 404 selected DUT instances. Source pins and census
are those of the [full-DUT evaluation](cva6-dut-reuse-evaluation.md). Dependencies
were reused locally; network was used only for Git remote verification/publishing.

Both arms enable body queries. Only the local diagnostic flag changes. Each of
three randomized paired blocks edits the existing out-of-block
`uvme_cva6_env_c::build_phase` seven times: new int/longint locals, a phase handle,
`get_name`, ordinary UVM logging, an undefined initializer and its repair, then
an explicit preprocessing change requiring native fallback. The first edit
adds lines before later logging macros and also takes full diagnostics.

Full accumulated diagnostic maps preserve duplicate entries, empty URI lists,
notes and ranges, with only outer diagnostic ordering normalized. Baseline
strict-language errors remain 164, increase to 165 and return to 164. Ordered
public completions, definitions and selected `get_name` resolves match the
same-build full-analysis oracle across all 42 states.

Qualified installed binary SHA256:
`cdd21786757b34a35057093ef821a17c0ab4917d1def571d6104fadb1ce1ff2d`.
The embedded build base is `b6a752e`; the local receipt binds that frozen binary
to the final source commit. No rebuild was done merely to change a version label.

## Serialized response times

Milliseconds; first take each session's selected-state median, then the median
across three sessions. Query time starts before `didChange` and ends on completion
receipt. Diagnostic time uses the corresponding current-version notification and
includes the query window. The two columns must not be added together.

| Measurement | Full diagnostics | Local diagnostics enabled |
|---|---:|---:|
| First edited query; first edit needs full diagnostics | 303.31 | 301.49 |
| First edited diagnostic window | 1082.51 | 1077.54 |
| First eligible diagnostic-certificate query | 42.37 | 169.29 |
| First eligible diagnostic window, including certification | 828.73 | 208.86 |
| Same warm states: query | 41.70 | 41.69 |
| Same warm states: diagnostics | 814.27 | 81.24 |
| Seven query-response times summed | 2192.34 | 1290.73 |
| Seven edit-to-diagnostic windows summed | 6695.00 | 2584.03 |
| Explicit macro fallback: query | 629.76 | 642.88 |
| Explicit macro fallback: diagnostics | 947.73 | 962.76 |

The identical warm states are versions 4 and 5, including the introduced error.
Over versions 4–7, local diagnostic windows have a session-median median of
81.23 ms; observed local windows range from 78.78 to 108.24 ms. These are sample
observations, not a latency bound. The full arm retires its context at version 6
and recertifies at version 7; local diagnostics avoid those repeated full parses
in this short sequence. Including those events, the seven-query sum drops about
41.1% and diagnostic-window sum about 61.4%.

First diagnostic certification adds about 127 ms to that query. The first line
change and final macro fallback still take roughly a second through diagnostics.
The small fallback median increase is retained; warm hits are not substituted for
fallback results. This sample does not establish cold-disk or tail-percentile bounds.

## Independent input arrivals

Three separate bursts schedule twelve edits every 100 ms independently of
responses. This is a client arrival interval, not server debounce. Inputs include
incomplete arithmetic, parentheses, an unresolved name and repair. Two edits are
primed beforehand to establish the query and diagnostic certificates.

| Mode | Successful / canceled, out of 36 | Latest query median ms | Maximum successful query ms |
|---|---:|---:|---:|
| Both local experiments off | 36 / 0 | 9821.92 | 9842.22 |
| Body queries only | 9 / 27 | 817.61 | 818.02 |
| Body queries + local diagnostics | 36 / 0 | 40.80 | 49.17 |

All successful completions and final target diagnostics match the full-analysis
oracle. The new mode's successful-query median is 41.14 ms; cancellations in the
query-only arm are reported separately and are not counted as fast completions.
One additional live-burst functional run compares all 612 accumulated final
file diagnostic maps: all three modes have the same digest. A separate serialized
sequence compares all 612 maps through each incomplete-input state and repair
(26 states including fallback).

One unprimed burst remains slower: latest response is 1032.39 ms with body queries
only and 1040.49 ms with local diagnostics; each succeeds once and cancels eleven
obsolete queries. There is no cold-burst speedup attributable to this change.
Native parsing/elaboration, individual large procedures and retirement can still
block model access between cancellation checkpoints. Hover is unchanged.

## Memory and parser mechanism

Memory is sampled separately at 20 ms over three paired seven-edit blocks.
Values are medians of sampled server/diagnostic-process-family peaks in MiB;
the Python client is excluded. No latency claims use these sampled runs.

| Mode | Peak RSS MiB | Peak physical footprint MiB |
|---|---:|---:|
| Body queries, full diagnostics | 1508.73 | 1147.86 |
| Body queries, local diagnostics | 1284.45 | 1022.70 |

One additional 84-edit paired soak compares complete diagnostic digests at every
state, retains default retirement and exercises the 64-edit certificate fallback
at version 66 followed by recertification. Peak RSS is 1526.80 / 1276.55 MiB
(full / local); peak footprint is 1447.30 / 1302.19 MiB. All runs complete within
the 3 GiB / 180-second family sampling guard. These windows show a memory benefit
but do not establish an unlimited-session bound. The fixed-library 256 MiB
accounting budget is separate from these process-memory measurements.

A separate counted executable sees one compilation-unit parser entry per accepted
warm edit including its completed local diagnostics, versus two with full
diagnostics. Its cold certification includes an additional mini parse; no timing
claim uses the counted executable. C++ checks also verify that a successful local
publication leaves the full design dirty rather than rebuilding it. A separate
forced-retirement CVA6 pair matches every complete diagnostic map.

## Correctness gates and scope

- 777/777 CTest entries pass, including 2,588 SDK cases. On Mac the test environment
  uses canonical `TMPDIR=/private/tmp`; the first run with `/var`-aliased temporary
  paths exposed existing fixture path assumptions, rather than a local-diagnostic
  failure.
- 17 Python system/method-scope tests and 43 diagnostic transport cases pass.
- The new local-diagnostic subset has 11 cases / 329 assertions. It covers parse
  recovery, initializer errors, error repair, missing returns, attributes, local
  conflicts, warning pragmas, line projection, import/read-write changes,
  persistent drivers, ordinary tasks, timing invalidation, cancellation/retry,
  unsaved retirement and generic value widths.
- An earlier probe found a missing delayed initializer error; forcing all local
  declarations before collecting semantic diagnostics fixes it. A second negative
  reproducer demonstrates why equal numeric generic values can still change
  diagnostics when their widths differ. Method-local generic dependencies now
  deliberately use full analysis. Neither failed probe is acceptance evidence.
- Default and explicit-source installed UVM smoke tests retain the whole selected
  DUT and shared UVM diagnostic owner. The SDK gitlink remains at
  `151a801ceaf6faf23d79a42d210ff67e50eb4660`; a clean reconstruction from that pin
  plus the exported patch verifies all 709 inventory files, including five added
  restoration files.

This first diagnostic boundary is narrower than general body queries: full-build
nonstatic class methods with one context, stable outside references/dependencies,
and no unsupported cross-procedure effects. Package/module subroutines, static
methods, tasks with timing, local-dependent generic specializations and other
unproven effects retain full current analysis. See the
[feature contract](../features/method-scope-overlay.md#local-class-method-diagnostics-default-off).

Remaining priorities are lower first-certification cost, cold/temporarily unsafe
input handling, hover reuse and wider dependency invalidation. A complete native
result published after an unsafe edit can change diagnostics in another file;
retaining an old local baseline across that result requires reconciliation, not
just replaying the old method diagnostics.

Raw reports, scripts, installation and cleanup receipt are under
`untracked/body-diagnostics-2026-10-22/`. They are not tracked. The compact tracked
receipt is `validation/data/method-body-diagnostics-2026-10-23.json`.
