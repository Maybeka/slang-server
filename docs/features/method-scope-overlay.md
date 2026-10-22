# Experimental method scope overlay

`experimentalMethodScopeOverlay` defaults to `false`. It is an experiment for
package exploration using a literal complete `compilationFilelist`,
`--single-unit --threads 1`, and a uniquely included target class header. Other
sibling and nested includes are supported when their complete occurrence graph
reaches the configured roots and contains no unsupported preprocessing. It preserves the
native full analysis path for unsupported edits and queries.

The first implementation accepts one change per notification, at most eight
consecutive accepted edits, equal-length ASCII text, unchanged CR/LF positions,
and a fixed number of initialized scalar `int` locals. The selected ordinary
instance function must have explicit scalar `input int` arguments, a scalar
`int` result, flat initialized locals, and one final return. Its expressions may
use those locals and arguments, integer literals, parentheses, unary `+`/`-`,
and binary `+`/`-`/`*`. A mini compilation verifies the new bindings and types.

Only the target header may be open. Macro environments, repeated target includes,
imports in the containing package, inheritance, local names shadowing enclosing-scope
members, nonlocal expressions, interface changes, variable-length edits,
line-layout changes, and local additions/removals are outside this first slice.
Configuration, command files, source bytes, successful and failed lookups,
source generations, and index revisions must still match the preparation
certificate. Unknown cases take the native path.

Every include edge, root and source buffer is accounted for. Repeated unrelated
headers are allowed, but recursive physical paths, system includes and unexplained
buffers are rejected. Non-editor source bytes must match the captured parse at
certification, repeated observations of the same disk path must agree, and every
frozen source/lookup is checked again before reuse. A later lookup cannot replace
a source witness that was already compared with the parsed bytes.
An external dependency edit invalidates reuse even without a watcher notification.
Equal-length local declaration renames together with all references are supported;
function signatures, variable-length renames and added/removed locals remain outside
the certificate. The first actual changed byte selects the method; there is no
hard-coded method index.

Completion and definition responses cover current argument/local reference
sites. Completion preserves the full native item order and field values, using
new method-owned items and value snapshots for the unchanged environment.
Definition links come from current mini-compilation bindings. Old automatic
local/formal completion handles cannot resolve to a new declaration merely
because its name or offset matches. Queries requiring changed enclosing-method
documentation take the native path. Diagnostics still run over a complete
current source snapshot in the isolated worker.

The source manager and AST for a mini compilation have a separate lifetime.
Preparation retains completion values and source witnesses, rather than base
AST or symbol pointers. A constructor-local symbol identity index avoids scanning
all visible scopes again for each completion item. Completion ordering still
comes from the native queries. The outer DTO vector remains unchanged after
certification. Sorted method slot indices select a separate replacement vector,
so an accepted edit builds and publishes only method-owned completion values;
it no longer copies or scans the outer DTO array. Resolution retains only the
fields that native resolution supplies, rather than a second complete item and
identity JSON. The constructor reserves exact native item capacity and releases
its temporary symbol index before capturing the diagnostic snapshot. Mini
compilations do not share symbols or ASTs. Full source witness validation and
completion response construction still scale with the certified environment.
Cancellation discards an unpublished candidate; editor
text synchronization still occurs exactly once. Once synchronization starts,
an exception cannot replay the same incremental range.

## Validation tools

`tests/method_scope/check.py` compares full ordered JSON against native queries,
including binding changes and source-generation checks. `tests/cpp/MethodScopeOverlayTests.cpp`
checks cancellation, publication failure, fallback, and lifecycle invalidation.
On Linux, `slang_server_counted` wraps the parser entry point for count evidence.
On macOS with a static SDK, it compiles an instrumented generated copy of the
parser translation unit ahead of the SDK archive, without changing SDK sources
or ordinary objects. Neither counted executable may supply performance samples.
The ordinary `slang-server` executable has no parser counter definition.

`server_unittests "[.method-profile]"` explicitly runs a phase observer at 500,
5,000 and 10,000 classes (three samples each). This attributes mechanism costs
and adds observer work during syntax traversal; its times are not the LSP metric.

`tests/method_scope/benchmark.py` records a plan before measurement and applies
one framing/timestamp protocol to both arms of the same ordinary binary. It
reports completion and definition separately, keeps cold preparation, whole
sequence and diagnostic waits separate, and reports the ninth edit separately
from the eight accepted edits. Use `--classes` when recording a plan to bind a
new scale; replay uses that saved scale. The oracle and count drivers accept the
same option. `interactiveSumMs` sums all nine response latencies, including
certification and fallback. `sequenceMs` also includes diagnostic waits. An
empirical p95 is shown only with at least 100 successful samples in a cell and
is not a confidence interval or a production tail guarantee. Measurements from another build are not current
validation. This experiment does not establish general responsiveness gains or
UVM support.

The 2026-10-17 Linux rebuild passed its recorded correctness checks, but its
synthetic timing medians were slower at every measured step for both query
kinds. See [the validation record](https://github.com/Maybeka/slang-server/blob/main/validation/linux-2026-10-17.json). The
experiment remains default off.

The 2026-10-18 extension certifies multi-include contexts and adds declaration
rename / actual method-selection oracle cases. Its new class/package fixture
showed lower medians for accepted edits 2–8 and higher medians for first-edit
certification and ninth-edit fallback (n=6 per arm per step). This is a separate
synthetic experiment, not a historical F1/F2 reproduction or UVM result. See
[the current validation record](https://github.com/Maybeka/slang-server/blob/main/validation/linux-2026-10-18.json).


The 2026-10-19 macOS follow-up removes the per-item repeated scope scan and
shares immutable completion DTO values across accepted edits. Its 5,000- and
10,000-class fixtures use 100 samples per arm/query/step, balanced by block.
Nine-response median sums (including first certification and ninth fallback)
are 13–20% lower; first responses and fallback remain slower than native.
The 5,000-class diagnostic-including sequence is essentially unchanged; the
10,000-class sequence improves about 2%. Separate one-observation-per-case RSS
measurements are higher in the method arm. The small 500-class fixture still
has no net response-sum gain. See [the Mac report](../../validation/macos-2026-10-19.json)
for empirical p95, paired mean intervals, exact hashes and platform limits.
This remains a restricted synthetic result and the experiment stays default off.

### External real-project measurements

`tests/method_scope/project_benchmark.py` consumes an existing workspace and a
scenario JSON containing `files` (relative source/filelist SHA-256 map), `target`,
`before`, `after`, `query`, and `queryDelta`. It never generates replacement
classes or edits the source files on disk. The workspace must already contain a
complete compilation filelist and `.slang/server.json`. The driver alternates
one exact text replacement for nine edits in fresh native/experimental processes,
with balanced ABBA/BAAB blocks and a fixed seed. The initial request includes
`didOpen`; its startup and response metrics differ from the synthetic driver's
prewarmed first-edit metric. All configured inputs are checked before each run.

Use `--oracle --blocks 1` separately to compare the ordered response, every
completion resolve, definition, and diagnostics across all ten versions. Physical
source handles are normalized only with source-byte witnesses. Macro expansion
handles remain exact opaque values: equality checks native determinism and does
not establish macro freshness. These oracle runs are excluded from latency
measurements. An ordinary binary is required. `--blocks 10` gives 20 observations
per query/arm; empirical p95 is withheld below 100 observations.

Real UVM classes currently use the native fallback, including when the configured
flags or macro/inheritance environment falls outside the certificate contract.
Their ninth edit is another fallback observation, not evidence that the eight-edit
reuse limit was reached. Keep this evidence separate from accepted local-method
measurements and do not interpret an unchanged fallback as a UVM speedup.

The subsequent [real-UVM and sparse-value Mac report](../../validation/macos-uvm-2026-10-19.json)
interleaves previous and new binaries in complete randomized blocks, separates
first query from first edit, and records repeated process peak-RSS observations.
It measures a modest further first-certificate reduction and tests the sparse
replacement implementation against the same large synthetic fixture. The actual
SPI UVM laboratory still takes full native fallback; it establishes no UVM
local-edit speedup. Both experiments remain default off.

## Body-only queries with semantic libraries

`experimentalMethodBodyQueries` is a separate, default-off first step toward
local semantic updates. Do not enable the legacy `experimentalMethodScopeOverlay`
flag with UVM semantic caching. The new flag can accompany
`experimentalUvmSemanticCache`, `experimentalScopeSemanticCache` and diagnostic
delta publishing; it does not change their defaults.

This slice supports a uniquely occurring, inline, ordinary instance function in
a non-parameterized package class. The containing class may inherit from a UVM
class and use imports and registration macros outside the selected function.
The function itself must be macro-free, return plain scalar `int`, and have
explicit `input int` arguments, flat initialized `int` locals and a final return.
Expressions use arguments, locals, integer literals, parentheses and unary
`+`/`-` or binary `+`/`-`/`*`. Method signatures and all local declaration tokens
remain unchanged; initializer expressions may change. The current slice requires
ASCII source. Calls, member-dependent expressions, control flow, out-of-block
methods, parameterized classes and declaration edits use native analysis.

Unlike the legacy experiment, body edits may grow, shrink and insert or remove
lines. Each accepted edit builds an independent method-sized compilation.
Completion values for the enclosing environment remain immutable, while local
bindings and source positions are regenerated. Unchanged declarations after the
method receive projected current offsets. Old local completion handles cannot
resolve to a later generation. No base semantic graph or symbol pointer is kept.
The source snapshot used for certification is released after creating witnesses
and query values, leaving no additional retained copy of the complete project.

Preparation requires an existing current query analysis; an edit never forces
analysis of the previous generation solely to prepare this optimization. A
certificate accepts at most 64 edits, after which the next edit takes the native
path. Context retirement, configuration changes, library generation replacement,
other document edits, dependency changes and unsupported queries also cause
native fallback. The implementation validates mutable source bytes, successful
and failed lookups, and watched directory membership. Explicit immutable library
files keep their existing fixed-dependency / manual-rebuild contract; they are
not read again for every local query.

Only lexical completion at verified local/argument reference sites, definition
at those sites and matching deferred completion resolution use this path. Hover
and other queries retain their native implementation. Diagnostics always use the
complete current source and the current semantic library owners; the new mode
never replays an old diagnostic result or substitutes local diagnostics for a
full pass. A hit therefore does not imply a local-only diagnostic update.

Run `tests/method_scope/body_queries.py --binary <server> --output <report.json>`
for a paired oracle and timing check with large synthetic packages, both with and
without the bundled UVM semantic library. It covers variable lengths, line
changes, twelve edits, completion ordering, selected resolves, definition ranges
and diagnostic values. These are closed-helper fixtures, not proof of coverage or
performance on arbitrary UVM project methods. C++ `[method-body]` tests additionally
cover rejected declaration / dependency / macro edits and cancellation.

For actual parser scope, run `body_counts.py` with the separate
`slang_server_counted` executable. It observes counters in the same queued
foreground batch and records later diagnostic parsing separately. Never use the
counted binary for latency measurements. See the
[Mac evaluation](../development/method-body-query-evaluation.md) for measured
first-edit cost, continuous-query gains, diagnostic and memory tradeoffs, and
real CVA6 fallback coverage.
