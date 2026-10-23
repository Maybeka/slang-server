# Cooperative analysis and retirement: Mac CVA6 evaluation

The default-off `experimentalCooperativeAnalysis` experiment reduces queue blocking by
cancelling disposable syntax parses and constructing retirement replacements in background.
It preserves the current query owner until the replacement commits. It does **not** make
semantic elaboration fully preemptible. See the [feature contract](../features/cooperative-context-retirement.md).

The comparison uses the same original CVA6 + full core-v-verif workload, explicit UVM 1.2
sources, active whole-DUT hierarchy (404 instances), UVM/scope semantic caches, body queries,
and body diagnostics. Baseline is main `49569ff`, qualified installation SHA256
`8f4d096f62438e63b7453adda48fd397acf50d02672b20660d0f1ed1714078cf`.
Candidate SHA256 is `5b74a4ed82499a2bf01c53e0ef95efb41b94319e1b71c2e0505b30c95678f2cf`,
built from that base plus this change. Only the new experiment differs between these arms.
A third arm runs the candidate with body-query/body-diagnostic experiments disabled as the
native public-result oracle; its UVM and whole-DUT caches remain enabled.

## Changes

- Ordinary source-storage retirement triggers enqueue work after current diagnostics.
  A newly queued message cancels candidate parsing. All parallel parsers join before
  cancellation propagates, so a discarded candidate cannot retain stack captures in workers.
- A candidate does not release current query caches or clear editor diagnostics. Publication
  ownership, closed-file rendered diagnostics, versions and previous build-source URIs transfer
  at successful commit. Failed configuration waits for another edit before retrying.
- Native configured-unit background reanalysis now passes its request into disposable parsing.
  Cancelled reanalysis retains newest text and pending diagnostics, without installing a partial
  configured-unit list or treating cancellation as a permanent prefix-cache decline.
- Parser callbacks are transient and release their request after construction. The shipped
  default UVM artifact remains build-generated; no extra UVM configuration is shipped.
- At twice the ordinary source-storage trigger, synchronous retirement remains the hard
  fallback. Cancelled edit notifications also check this limit. There is no added typing debounce.

## Independent 100 ms input: three sessions per arm and scenario

Each session sends 12 edits independently of query responses, including incomplete arithmetic
and an unresolved user name. Each edit immediately requests hover. Both execution orders are
represented. Timing ends at receipt of the response body, before JSON decoding. These runs have
no phase tracing, generation tracing or memory sampler. Diagnostic delta publishing remains off
in all arms, matching the starting workload. All final requests return successfully.

| Scenario | Last hover median, baseline → candidate | Last diagnostic median, baseline → candidate | Successful / cancelled candidate queries |
| --- | ---: | ---: | ---: |
| Cold body certificate | 1481.87 → 53.68 ms | 1820.05 → 884.98 ms | 35 / 1 |
| Warm body certificate | 51.37 → 54.10 ms | 86.65 → 92.54 ms | 36 / 0 |
| Source macro change forcing native fallback | 1188.51 → 875.56 ms | 1525.94 → 1199.44 ms | 16 / 20 |
| Warm, forced source retirement every 3 edits | 1019.52 → 55.28 ms | 1351.04 → 92.23 ms | 32 / 4 |

Baseline success/cancellation counts are respectively 9/27, 36/0, 9/27 and 6/30. Cancellation
is an obsolete-content response, not a successful low-latency result. The pooled successful warm
hover median is 51.23 → 50.15 ms; none of the three paired warm medians regresses by 10%.
Cold final hover improves by 96.4%, exceeding the predeclared 20% target. Cold final diagnostics
still take about 0.9 seconds. Macro fallback also remains expensive. Warm diagnostic completion
is slightly slower in these three samples; no claim of improvement is made for it.

There are 36 complete terminal diagnostic-map comparisons, each containing 612 URIs. Every
successful hover matches the native result. Macro/forced-retirement sessions additionally
verify that the complete diagnostic map remains unchanged after two seconds of client silence.
This client-side pause observes retirement completion; the server does not add such a delay.

## Correctness and memory probes

These separate runs enable diagnostic delta publishing in every arm and use 20 ms family RSS /
physical-footprint sampling with a 3 GiB / 180 second per-session guard. They are not ordinary
latency qualification. The family includes the server and any diagnostic children, excluding
Python. The guard is not a configurable product RSS promise.

Both hover-first and completion-first orders pass 42 complete state comparisons: hover,
completion, completion resolve, definition, source/expanded/both macro preview, repeated queries,
and all 612 diagnostic URI buckets. User error introduction and repair match native analysis.

| Probe | Peak family RSS, baseline → candidate | Peak footprint, baseline → candidate |
| --- | ---: | ---: |
| Seven edits, hover first | 1290.17 → 1296.08 MiB | 1036.63 → 1039.99 MiB |
| Seven edits, completion first | 1291.91 → 1294.75 MiB | 1037.28 → 1039.22 MiB |
| 96 warm edits, natural retirement | 1449.09 → 1436.09 MiB | 1068.08 → 1093.47 MiB |
| Forced retirement, 12 warm edits | 1306.31 → 1520.62 MiB | 1036.91 → 1175.36 MiB |

Forced retirement temporarily retains both the usable owner and a candidate: the observed
RSS peak increases by about 214 MiB. The trace records five cancelled replacements and one
successful retirement, followed by no duplicate diagnostic publications with delta publishing
on. Natural retirement also completes and preserves the full diagnostic map. All probes remain
within their guard and leave no diagnostic child behind; these finite probes do not establish
an indefinite process-memory bound.

The 96-edit probe exposes a remaining cliff. The existing 64-notification body certificate
expires; subsequent edits require a current native query context. Both versions cancel 33
obsolete queries, and the candidate's final query still takes about 1.06 seconds under these
probes (baseline about 1.17 seconds). This is not presented as a latency win: tracing and sampling
are enabled, and this is one session per arm. The experiment does not remove that certificate
bound. A future change should prepare/revalidate the next certificate without discarding a
still-provable query owner. Owner-resolution paths that do not carry a request context, model
construction, and complete semantic elaboration remain further cancellation targets.

## Validation and evidence

The new server tests cover cancellation during parsing, after input construction, after model
construction and before commit; retry after cancellation/configuration failure; preserved closed
file diagnostics with delta publishing on and off; newest unsaved text; and the hard source-storage
fallback. Three server cases pass 208 assertions. The SDK's parallel/single-unit disposable-parse
test passes 18 assertions and verifies retry after worker join.

The compact [evaluation data](../../validation/data/cooperative-analysis-2026-10-23.json) binds
binary identities, measurements and raw artifact hashes. Local reproduction scripts and retained
raw evidence are in the task's `untracked/responsiveness-2026-10-23/` directory. SDK restoration
retains gitlink `151a801ceaf6faf23d79a42d210ff67e50eb4660`; clean archive + patch replay verifies all
709 inventory files byte for byte. Full-suite and installed-UVM checks are recorded in the data.

Full validation passes 782 CTest entries (including 2,590 SDK cases), 17 Python tests and 43
transport cases. The installed default UVM reports shipped-preparse use with zero syntax-parse
time. The external-profile smoke parses once (about 364 ms in this instance), then a separate
server session reports a project-cache hit with zero syntax-parse time. These counters verify
artifact behavior; they are not a repeated UVM performance benchmark.
