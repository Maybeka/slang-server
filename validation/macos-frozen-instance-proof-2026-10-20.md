# Incremental instance proof on Mac

The default-off candidate is pinned by executable SHA-256
`0c0558179239f058940ecea7d74a9c85ffb7604d49ae4299d592c37f2603ffa2`.
Private manifests preserve the exact changed sources and executable. Only
repository Markdown changed after freezing. This is an intermediate milestone;
performance, long-memory and integrated publication gates remain open.

## Correctness repair

The initial instance prototype omitted the entire eligible task body, including
its newlines. A real reproduction demonstrates stale RTL hierarchy values:
adding lines before a later `__LINE__` changes native parameter W from 4 to 7,
while the old cached answer remains 4. The old executable and reproduction remain
available, and its validation report is explicitly marked superseded.

The successor retains logical newline counts in the normalized task-body proof.
LF, CRLF and bare CR are covered; same-line statement changes can still reuse
values. Changed line counts invalidate and rebuild. This is necessary even when
the task's runtime statements have no direct elaboration dependency.

## Cost reduction and lifetime

Each cache belongs to one live SourceManager, with exact normalized proofs per
physical source file. Same-manager buffer IDs identify immutable source versions;
they are not portable semantic handles. Exact unchanged file contents avoid
normalization. A partial walk visits changed files and all include ancestors,
skipping unaffected class/module subtrees only if their two ends belong to the
same physical buffer occurrence. Compilation-unit wrappers are never skipped
using the first token alone. Failed partial comparison triggers a complete
forest walk before a new baseline is admitted. A test changes an earlier root's
macro so an unchanged nested task becomes a module task instead of a class task;
the old class-task mask is correctly discarded.

A completed filesystem witness is shareable only within one dispatched RPC,
with the same context owner/revision and unchanged native syntax/generation.
Evidence dies at dispatch end. A subsequent RPC still performs exact filesystem
checks, including negative include lookups and symlink resolution. No TTL or
file-monitor-only freshness assumption is introduced. Cache storage performs
its own complete witness. Unsaved overlays retain precedence.

## Verification

- CTest: 678/678; Python system/method tests: 16/16.
- Focused group: 2,661 assertions across 90 cases; instance cases: 245 assertions
  across seven cases; standalone transport: 43/43.
- Six real native/enabled SPI/PSS/UBUS processes compare five unsaved states:
  complete hierarchy values and chains, completion and every resolve,
  definition, hover, references, highlights, symbols and current-version
  diagnostics. User task errors appear and disappear after repair. Only outer
  diagnostic order is normalized; duplicates and related information remain.
- Additional regressions cover fixed-manager ownership, dispatch evidence
  lifetime/repeated client IDs, same-size content changes with restored mtime,
  newly created higher-priority includes, symlink retargeting, unsaved parameters
  and interface connection/data-width changes.

Each enabled real replay records two preparations, 13 hits, eight cross-edit hits,
one full proof, four incremental proofs and ten request witness reuses. The
last partial walks visit 302 nodes for SPI and PSS and 3,350 for UBUS, compared
with 1,482/1,482/45,823 in the preceding per-file-proof candidate. This measures
traversal work, not complete latency.

Charged semantic retention is 3,170,414 / 3,172,688 / 3,174,560 bytes for
SPI/PSS/UBUS. Each remains within its 16 MiB reserve; syntax and semantic
retention together remain within 64 MiB. These values are not process RSS and
do not establish behavior over 1,000 edits.

## Remaining acceptance

A 54-process randomized three-pair pilot on this exact executable passes all
semantic/counter checks. The following are medians in milliseconds, ordered as
native / matched syntax-only / instance reuse. Every session includes startup,
11 query states, ten unsaved edits, repeated scope and scope-chain queries, and
required diagnostics. The old UVM-prefix experiment is off in all three arms;
syntax budget is matched at 48 MiB, with an extra 16 MiB semantic reserve for
instance reuse. No builds, tests, editors or RSS sampling overlap timing.

| Project | Input mode | Complete session, ms | First edit query, ms | Median continuous query, ms |
|---|---|---|---|---|
| SPI | Diagnostic-drained | 1853.66 / 1624.54 / 1760.49 | 48.36 / 32.15 / 34.70 | 65.26 / 48.19 / 51.34 |
| SPI | Continuous burst | 1567.46 / 798.54 / 854.74 | 48.80 / 32.48 / 32.50 | 57.18 / 30.10 / 32.15 |
| PSS | Diagnostic-drained | 2441.88 / 2289.51 / 2349.80 | 98.32 / 77.94 / 34.78 | 110.70 / 95.84 / 48.81 |
| PSS | Continuous burst | 2140.84 / 1492.88 / 1011.70 | 95.12 / 84.20 / 31.87 | 102.48 / 79.98 / 32.00 |
| UBUS | Diagnostic-drained | 1945.21 / 1865.80 / 1916.82 | 48.66 / 52.15 / 50.21 | 64.08 / 70.47 / 67.68 |
| UBUS | Continuous burst | 1614.36 / 983.29 / 1067.31 | 47.93 / 51.77 / 49.47 | 59.68 / 48.24 / 48.96 |

PSS burst complete sessions save approximately 32.2% relative to syntax-only,
but SPI/UBUS burst regress 7.0%/8.5%; all three diagnostic-drained sessions also
regress. Reduced traversal alone does not establish stable overall improvement.
These are three samples per cell, not confidence-qualified final acceptance.
Filesystem caches are warm. Pending diagnostic notices are not total publication
counts. Earlier candidates' results remain separately pinned.

Full acceptance still requires
20 paired runs with the accepted UVM-on baseline, the real class bulk-resolve
workload, repeated 1,000-edit parent/family RSS and fallback/refresh checks.
The older PSS/UBUS bulk-resolve regressions remain unresolved. Optional bundled
UVM packaging is still pending. No commit or push is justified by this milestone
alone; all experiments remain disabled by default.
