# Function/task edits under continuous input — Mac, 2026-10-22

This extends the [general body-query evaluation](general-method-body-query-evaluation.md).
`experimentalMethodBodyQueries` remains **off by default**. No fixed input debounce
was introduced. The existing full-design background grace remains 2 ms; this is
request-burst coalescing, not a human typing delay.

## Changes

- New edits cancel outstanding cancellable position queries for the same document.
  Queued obsolete completion/hover/definition requests return `ContentModified`
  before their handlers run. Running requests observe cancellation at checkpoints.
  Other document requests and workspace commands retain their behavior. The
  generic scheduler also supports context-aware signature-help handlers.
- A canceled edit with a valid body certificate synchronizes text once, retains
  its lexical owner and skips binding the obsolete intermediate body. The next
  live query binds and validates the latest body. Unsafe macro/interface/dependency
  changes still use native analysis. The version fence also covers deferred text.
- Full-design retirement in this mode migrates current unsaved overlays without
  synchronous per-document and subsequent whole-design diagnostic passes. It
  preserves eventual current diagnostics and active hierarchy selections.
- Full-design dataflow checks cancellation after procedures, scopes and assertions.
  It publishes its analysis manager only after completion. Cancellation discards
  the partial candidate, unfreezes the compilation and releases request listeners.
- Exact input witnesses use a single allocation and bulk reads instead of
  character-at-a-time extraction. Source bytes, missing files and symlink checks
  are still validated; EOF/short-read checks reject files changing during a read.
- Initial macro replay tests the token offset before resolving file paths and
  caches the target path. It still reconstructs the exact macro environment and
  checks cancellation; it does not assume source macros are fixed.

A phase-instrumented pilot found about 15 ms per witness-file validation and
326 ms for first macro replay in CVA6, while body parsing/binding itself was
sub-millisecond. After bulk reads and offset-first matching those pilot medians
were about 12 ms and 212 ms. These pilots identify mechanisms only; timings below
use the release binary without phase instrumentation.

Qualified release SHA256:
`57f436beb0e617a38b05a373fc07750f1b1f952c9940a982e4cad414f929b833`.
The installed binary has the same hash. This change adds no SDK modifications;
restoration verifies the same 709 inventoried files and unchanged SDK gitlink.

## Correctness

- 766/766 CTest cases; targeted cases: 23 / 2069 assertions.
- 17 Python system/method-scope tests and 43 diagnostic transport cases.
- Tests cover cancel-before/after-start query ordering, opt-out behavior, another
  document's requests, text synchronization with whole/ranged edits, incomplete
  bodies, deferred binding, macro-state fallback and native query/diagnostic equality.
- Canceled dataflow retries match native diagnostics, with no partial publication
  and a queryable, unfrozen owner between attempts.
- Separate counted executable still observes one foreground mini-shaped parse per
  accepted non-superseded edit. Superseded-body tests verify no mini parse occurs
  until the latest live query. Counted/instrumented runs supply no latency claims.
- Synthetic general and closed-helper workloads each cover 144 edited states.
  Real CVA6 covers 42 states. Ordered public completions, selected resolves,
  definitions and current diagnostic maps match same-build disabled native analysis.
  CVA6 comparisons include 612 accumulated file diagnostic maps, user errors and
  repair, normal memory retirement and explicit preprocessing fallback.
- Installed-resource smoke confirms shipped UVM preparse use, shared UVM diagnostic
  owner and an actual whole-DUT cache hit with the expected retained instance root.

## Serialized edit/query measurements

Three paired blocks, same original CVA6 RTL and full UVM verification environment,
UVM and whole DUT frozen in both arms. Edits modify the real out-of-block
`uvme_cva6_env::build_phase`, keeping existing calls/macros. Measurements include
`didChange` through completion response. Medians, milliseconds:

| Measurement | Native | Local enabled |
|---|---:|---:|
| First edited query | 621.00 | 303.34 |
| Warm accepted queries | — | 41.77 |
| Sum of seven query responses | 5597.62 | 2198.98 |
| Current diagnostics | 934.23 | 956.14 |

The seven-query sum improves about 60.7%, including first certification,
retirement, recertification and explicit macro fallback. Enabled per-state medians
are 303.34, 41.65, 42.28, 42.53, **822.88**, 301.57 and 636.32 ms. The fifth state
retires the context; the seventh intentionally changes preprocessing state.
This remains a visible retirement pause. Native has its own retirement pause.
Compared with the preceding release's separate run, local first/continuous medians
were 445.59/53.93 ms and its seven-query sum was 3770.53 ms. Same-build paired
native comparisons above are the primary timing evidence.

Synthetic 5000-neighbor-class medians:

| Workload | Native first / continuous | Local first / continuous |
|---|---:|---:|
| General plain | 11.28 / 9.94 | 4.34 / 2.30 |
| General UVM | 18.62 / 17.58 | 11.14 / 8.85 |
| Original closed UVM | 18.75 / 17.66 | 11.11 / 8.59 |

Diagnostics remain native and do not consistently improve. In general UVM,
current-diagnostic P50 is 21.26 ms native versus 24.96 ms local; the synthetic
fixture does not establish real-project diagnostic speedup.

## Independent typing arrivals

Three warm bursts, each twelve edits scheduled every 100 ms independently of
responses. Every edit immediately requests completion. Inputs include incomplete
arithmetic, an invalid name and repair, with UVM logging and member queries.
The function certificate is primed before each burst. There is **no 100 ms server
wait**: 100 ms is the client's input interval.

All arms use the same explicit UVM source directory from the preceding qualified
installation, avoiding different bundled resource URI roots. No project source
file is changed; edits are LSP overlays and config is restored. The current
binary's native arm supplies the oracle for every successful completion and final
current diagnostics. Previous-release completions are compared at the same states.
Opaque owner data is removed; diagnostic values and ranges are retained and sorted.

| Three-burst observation | Previous local release | Current local release |
|---|---:|---:|
| Last version response, median ms | 9147.08 | 56.40 |
| Last version response, range ms | 9133.43–9374.02 | 54.57–88.58 |
| Successful completion responses | 36 | 10 |
| Obsolete completion cancellations | 0 | 26 |

The current successful responses range up to 103.15 ms in these bursts; each
successful response and final diagnostics match native analysis. This is **not**
36 fast successful completions: superseded work is explicitly canceled. Canceled
responses can still wait for an indivisible native phase before their checkpoint;
this result does not prove an instantaneous cancellation or hard latency bound.
The native arm's last responses were about ten seconds because old queries also
accumulated; comparing only sequential service times would hide that queue cost.

One additional unprimed burst deliberately starts without a body certificate.
Eleven obsolete requests are canceled; the latest query takes 1035.10 ms and uses
native analysis because certification was interrupted. Previous-release and native
last responses in that burst take 6166.69 and 8475.29 ms. This is one cold observation,
not a cold-query bound, and shows that warm typing gains do not remove cold fallback.

## Memory

Separate 20 ms process-family sampling, three paired blocks each. Median peaks,
MiB; no latency conclusion uses these sampled sessions:

| Workload | Native RSS / physical | Local RSS / physical |
|---|---:|---:|
| General UVM, 5000 neighbors | 173.17 / 126.69 | 220.91 / 182.27 |
| Real CVA6, seven serialized edits | 1382.14 / 1034.55 | 1513.48 / 1153.89 |

The lexical owner still costs additional memory (about 131 MiB RSS / 119 MiB
physical in CVA6 against same-build native). This is not a memory-reduction
result. The typing bursts do not supply memory peaks or a long-session bound.

## Remaining work and evidence

Cold certification can still be canceled before a lexical owner is prepared.
Native parsing/elaboration, large individual dataflow procedures and context
replacement remain indivisible in places. The experiment does not yet guarantee
that arbitrary subsequent queries avoid every analysis pause. In particular,
retirement still runs synchronously at the existing source-manager threshold;
only its duplicate diagnostic work was removed. A dedicated scheduling/ownership
change is still needed to eliminate that pause rather than hide it behind a delay.
Local diagnostic invalidation and recomputation also remain outstanding.

All corpus/dependencies are local; no source/dependency download is performed.
Local final JSON, scripts, logs, installation and checksums are retained under
`untracked/body-costs-2026-10-22/`. `typing.py` measures arrival-to-response latency,
including queue time; `cva6-body.py` provides serialized native oracles and memory
runs. Memory runs are separate from timing and use the existing 20 ms sampler
with a 3 GiB / 180-second process-family guard. Short-run peaks are not a
long-session memory bound.

Aggregated results and report hashes are tracked in the
[machine-readable summary](../../validation/data/body-edit-responsiveness-2026-10-22.json).
