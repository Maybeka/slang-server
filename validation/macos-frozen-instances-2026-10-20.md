# Exact instance hierarchy prototype on Mac

**Superseded correctness evidence:** subsequent testing found that omitting a
task body's line count could preserve stale hierarchy values when a later
`__LINE__` expansion changed an RTL parameter. The executable below contains
that bug. Its original measurements remain historical evidence, not acceptance
of the current implementation. The successor preserves logical newline counts,
binds proofs to one SourceManager, and checks incremental physical-file proofs.

This default-off prototype implements exact RTL instance-subtree selectors and
cross-edit reuse of hierarchy values. Performance acceptance is incomplete; it
does not complete the overall frozen-analysis goal or justify publication yet.

The executable SHA-256 is
`f6902c7e7901bf8cb069298ae7a47c4c067940b6c8d234fdad98392189f95871`.
The private candidate manifest and changed-source archive pin the implementation.
Only documentation changed after that freeze.

## Implementation and correctness

`frozenSources.instances` resolves canonical instance paths and descendants,
including indexed elements. Missing paths, definitions and whole arrays receive
status reasons. Selected module definitions feed the existing syntax registry.
The semantic cache stores serialized hierarchy values, exact input certificates
and physical-location witnesses, with no retained AST symbol pointers. It serves
`slang.getScope` and `slang.getScopes`; other language features and diagnostics
continue to rebuild current native semantic state.

The initial permitted edit is an automatic class-task process body with no local
declarations, directives/macros, loops or block statements. All other effective
inputs remain exact. The complete physical graph and unit order participate in
the certificate, and returned locations require their physical files unchanged.
This permits runtime-only task edits without guessing the design's dependency
closure. Unsupported edits invalidate and use native analysis. More permissive
incremental proofs require separate implementation and tests.

Parent syntax retention uses three quarters of the configured budget and instance
values/certificates one quarter. At the default 64 MiB these are 48/16 MiB; the
two reported retained-byte counters sum within the configured parent limit.
The diagnostic worker has its existing separate budget. None is an RSS limit.

Full CTest passes 673/673, Python 16/16, and the focused group passes 2,527
assertions in 85 cases. The two dedicated instance cases pass 111 assertions:
canonical/missing/array selectors, descendant and neighbor boundaries, actual
cross-edit hits without rebuilding the dirty model, exact value/step equivalence,
task error insertion/repair and versions, declarations/macros declining, package
parameters and generate disappearance, binds, defparams, structural cross-reference
edits, instance removal/repair, refresh, budget limits, cancellation, unavailable
witnesses and source-location movement within the edited physical file.

Six fresh real SPI/PSS/UBUS native/enabled processes compare complete hierarchy
responses and all current diagnostics through five unsaved states. Every enabled
project records two preparations, 13 hits and eight cross-edit hits. A genuinely
invalid task call is diagnosed at the matching editor version and disappears
after repair; no warnings or related information are filtered. Only outer
diagnostic order is normalized. Semantic retained bytes are 2,796,246 for SPI,
2,929,320 for PSS and 3,135,418 for UBUS in those replays.

Six isolated native/enabled LazyVim sessions pass on the same executable, with
real Mac file monitoring, actual unsaved task edits, unchanged hierarchy values,
cross-edit hits and matching-version error/repair diagnostics. User editor state
and configuration remain untouched. SDK source and restoration files are unchanged
from the preceding module milestone.

## Exploratory timing

54 sequential randomized paired fresh processes cover three projects, two input
patterns and three arms, with three samples per cell. Every state issues a primary
scope request, a repeated scope request and a scope-chain request. Ten alternating
unsaved edits follow the first state. All required diagnostic/error checks pass.
No builds, tests, editors, tracing, status requests or RSS sampling overlap primary
timing. Filesystem caches are not flushed.

- Native: generic experiment off, with the same source inputs.
- Syntax: generic syntax reuse on with explicit matching module names and 48 MiB.
- Instances: same syntax boundary and 48 MiB syntax allowance, plus exact instance
  selectors and a 16 MiB value/certificate allowance (64 MiB total).

This ablation includes selector preparation and proof overhead. The older UVM
experiment is off in all arms. The accepted older UVM-enabled binary is still a
required final comparison. These hierarchy lists are not the earlier UVM-class
bulk-completion-resolve workloads; those regressions remain open.

Complete process-start-through-final-matching-diagnostic medians, milliseconds:

| Project | Input pattern | Native | Syntax | Instances |
|---|---|---:|---:|---:|
| SPI | Wait for every diagnostic | 1814.53 | 1596.67 | 1910.33 |
| PSS | Wait for every diagnostic | 2434.13 | 2263.12 | 2527.61 |
| UBUS | Wait for every diagnostic | 1941.51 | 1843.23 | 2083.20 |
| SPI | Continuous input, initial/final diagnostic wait | 1592.70 | 834.22 | 1023.71 |
| PSS | Continuous input, initial/final diagnostic wait | 2161.89 | 1407.48 | 1227.26 |
| UBUS | Continuous input, initial/final diagnostic wait | 1636.28 | 1046.72 | 1225.11 |

PSS continuous input gains about 12.8% over the syntax-only arm, demonstrating
useful semantic reuse in the heavier hierarchy. Its first-edit median falls from
80.35 to 35.93 ms. SPI and UBUS regress against that ablation, while all three
diagnostic-drained sessions regress. Source-only reuse accounts for much of the
improvement over native. The additional proof and repeated-query costs must not
be attributed to useful semantic work.

These small samples do not establish stable tail latency or the required paired
confidence interval. The result is mixed and fails a broad performance acceptance.
Deferring work to diagnostics does not itself establish a complete-session saving.

## Next refinements and remaining gates

The current certificate revisits the complete syntax forest when input buffer IDs
change, even if nearly all physical files remain unchanged. A next refinement can
reuse immutable, unchanged physical-source proof components and rebuild only the
changed root's task ranges, with full revalidation after structural/preprocessing
changes. Exact closed-byte, alias and negative-lookup checks must remain per request.
Avoid storing another whole raw corpus merely to speed the proof: account for any
extra retained data and measure family RSS. Bind proof metadata explicitly to one
live source manager; never carry source IDs across a replacement manager.

Also investigate sharing a completed exact witness within one request, with a
request identity and current context revision. This must not become cross-request
memoization, a TTL, or watcher/timestamp-only freshness. The current prototype
still performs its independent witness.

Before acceptance, broaden the instance-enabled ordinary language-query oracle
and interface/dependency invalidation matrix, confirm performance with 20 pairs
including the accepted baseline, and run repeated 1,000-edit parent/family memory,
retirement and refresh checks. Optional explicit bundled UVM and final integrated
transport/SDK/format/docs/publication gates remain part of the overall objective.
