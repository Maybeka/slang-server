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

The body-query experiment supports uniquely occurring functions and tasks in class,
package and module contexts, including inherited and parameterized contexts and
out-of-block definitions. Local declarations and types can change, as can arbitrary
statements, control flow, calls and member accesses. Macro invocations inherit the
macro environment at the original method entry. Variable-length, multiline and
batched LSP changes are supported. The header and the enclosing source must remain
unchanged. Parser recovery inside the original body boundaries is allowed.

Each edit parses one method and binds new semantic objects against the retained
original lexical scopes. The original member tables are never replaced or edited.
Native completion classification, ordered candidate generation, deferred resolve
and definition lookup operate on the detached body index. Definitions after the
edited body receive projected current locations. Old local completion handles
cannot resolve to a later generation. This implementation retains one original
semantic context in addition to the latest method; its memory cost must be included
in measurements. It is no longer a DTO-only certificate.

Preparation requires an existing current query analysis. It captures the exact
macro environment from a transient change journal recorded during the normal parse.
The journal stores macro definitions/removals/reset events and function/task keyword
entry positions, rather than a complete macro-map copy per method. It is enabled
only with this experiment. Older/restored trees without entry metadata retain the
ordered-root preprocessing replay; bundled UVM keeps its precompiled macro prefix.
Both preparation and fallback costs belong to the first edit. Subsequent edits parse only the selected method. A certificate
accepts at most 64 notifications. Context retirement, configuration / library
replacement, other document edits and mutable dependency changes invalidate it.
Explicit immutable library files retain their fixed-dependency / manual-rebuild
contract and are not reread for every query.

Edits that change signatures (including non-ANSI body argument declarations),
escape method parse boundaries, change the surrounding preprocessing environment,
or affect outside semantic dependencies use native analysis. Constant
function evaluation, including failed evaluations, and external references to method-local declarations prevent
reuse. A scoped evaluation guard also rejects newly encountered lazy dependencies
on the obsolete body. Macro expansions in later declarations or signatures also
force native analysis when a newline edit could change their `__LINE__` values;
line-sensitive sibling bodies are guarded against constant / local-type reuse.
These are nonlocal changes even when their spelling lies
inside a function. An unbound / ambiguous source occurrence also uses native
analysis. Runtime side effects outside the local diagnostic boundary use full current diagnostics.

Dependency tracking is enabled only with this experiment; the disabled path does
not scan subroutine dependency scopes.

Completion, deferred member resolution, definition and eligible hover requests
inside the selected body use this path. Incomplete macro completion lists and other
query families keep native behavior. Hover requiring current persistent-value
or design-driver information also uses native analysis. Diagnostics use full current analysis by default.
The separate `experimentalMethodBodyDiagnostics` experiment described below can
update diagnostics for eligible class methods. A body-query hit alone does not
imply that the diagnostic path was local.

With `experimentalMethodBodyQueries` enabled, new edits invalidate outstanding
cancellable completion, hover and definition requests for the same document.
Queued obsolete requests return LSP `ContentModified` before entering their
handlers; running requests observe cancellation at existing checkpoints. Other
documents and workspace commands are not canceled this way. No fixed edit-delay
or debounce window is added.

A canceled edit with an already certified body synchronizes its text exactly once
and retains the fixed lexical owner when its body boundary and input witnesses
still hold. Binding of that intermediate body is skipped. The next live query
binds the latest body and rechecks macro state, interfaces and external semantic
dependencies; an unsafe latest body still uses native analysis. The status counter
includes these synchronized notifications and does not imply each intermediate
version was bound. Versions still fence all completion handles.

Full-design retirement now honors deferred diagnostics in this mode, migrating
current unsaved overlays without immediately diagnosing every open document and
then diagnosing the design again. Retirement still uses the existing memory
threshold and can rebuild synchronously; it has not become an independent worker.
Full-design dataflow checks cancellation after procedures/scopes/assertions and
publishes only a complete analysis candidate. A canceled candidate is discarded
and its compilation is unfrozen before foreground queries resume. Native parsing,
semantic elaboration and some large individual procedures remain indivisible.
These changes reduce queue amplification; they do not establish a hard response
latency bound. See [continuous input measurements](../development/body-edit-responsiveness.md).

Run `tests/method_scope/general_queries.py --binary <server> --output <report.json>`
for broad body edits with large neighboring packages and an inherited UVM arm. It
compares changed local declarations/types, struct/member/helper calls, nested
foreach blocks, deliberately invalid references and recovery against the disabled
native path, including completion ordering, resolves, definition ranges and
published diagnostics. C++ `[general-body]` tests additionally cover module and
package tasks, out-of-block methods and constant-function invalidation.

Run `tests/method_scope/body_queries.py --binary <server> --output <report.json>`
for the original closed-helper regression workload. It now exercises the same
retained-context implementation. These synthetic fixtures do not establish a
real-project speedup.

For actual parser scope, run `body_counts.py` with the separate
`slang_server_counted` executable. It observes counters in the same queued
foreground batch and records later diagnostic parsing separately. Never use the
counted binary for latency measurements. See the
[Mac evaluation](../development/general-method-body-query-evaluation.md) for measured
first-edit cost, continuous-query gains, diagnostic and memory tradeoffs, and
real CVA6 fallback coverage.


## Local class-method diagnostics (default off)

```json
{
  "experimentalMethodBodyQueries": true,
  "experimentalMethodBodyDiagnostics": true
}
```

This first implementation requires a selected full build (`slang.setBuildFile`
or `slang.setTopLevel`) and an already completed current full-design diagnostic
baseline. Setting only `compilationFilelist` selects exploration and does not
satisfy that requirement. UVM / RTL semantic freezing remains independent and
can be combined with this experiment.

The local unit is an entire nonstatic class function, or a class task without
timing controls or calls to other tasks. Only one full-design method context
may correspond to the edited source occurrence. The signature, enclosing code,
other inputs and immutable-library generations must remain fixed. Ordinary
local declarations, scalar/vector types, expressions, control flow, calls,
logging macros and parser recovery within the method boundary can be updated.

A private compilation borrows the completed full-design lexical owner, not the
shallower completion context. It realizes local declarations, initializers,
attributes and deferred name conflicts, then runs the native procedure and
local-scope dataflow checks. Its reference bookkeeping, including borrowed
import lookups, stays private. Native-rendered local baseline diagnostics must
match the full design before the certificate is admitted.

The original complete diagnostics are partitioned by analysis phase and actual
publication URI. An update replaces the method's results, projects retained
source locations and related notes, and republishes affected files. Unaffected
files retain their diagnostics without rebinding, reformatting or serialization.
Cancellation discards the private candidate; text/version/input witnesses are
checked again before the diagnostic transaction is published.

Full current analysis is used when external read/write references or evaluated
generic dependencies change. Generic arguments that refer to method-local
names are deliberately outside this first boundary: equal numeric values with
different widths can change a specialization's diagnostics. Constructors,
static methods, package/module subroutines, exploration-only projects, multiple
method contexts, persistent nonlocal drivers, non-instance initializer calls,
hierarchy/virtual-interface finalization, local nominal types/imports/checkers,
and concurrent assertion declarations also use full diagnostics. So do bounded
or fatal diagnostic streams and newline edits affecting later macro bodies.
Existing body-query invalidation and the 64-notification certificate bound still
apply. Both the original and replacement preprocessing environments are checked;
removing an existing state-changing directive or macro cannot retain its old suffix.

There is no fixed debounce. First baseline preparation has a visible cost;
unprimed typing and native fallbacks can still take around a second in CVA6.
Hover can use the local path described below. See the
[Mac diagnostic evaluation](../development/method-body-diagnostic-evaluation.md)
for measured costs, complete diagnostic oracles, memory and limitations.


## Local method-body hover

`experimentalMethodBodyQueries` also enables local `textDocument/hover` and
`slang.previewHover` within the retained body. There is no additional flag and the
experiment remains disabled by default. `experimentalMethodBodyDiagnostics` is
independent; enabling it avoids competing full diagnostics on eligible edits.

Hover uses the same detached semantic index, original lexical context, evaluation
guard and source projection as definition/completion. Changed local declarations,
types and fields, inherited methods, system subroutines and macro source/expanded/
both views keep native rendering. A valid empty hover does not rebuild the design.
Canceled requests release the projection and retain a reusable context. Requests
outside the selected body or beyond the existing dependency boundary use native
analysis.

The private body view has no complete driver graph. Automatic variables are excluded
from native procedure driver registration; parameters cannot be driven and fields
are selected through aggregate roots. These values can render locally. A persistent
value, design signal, connected ref-port value or other unsupported driver target
requests native analysis, preserving driver source snippets and links instead of
silently omitting them. This includes ordinary module-task locals with static
lifetime; automatic module-task locals can use the local path.

Mutable inputs are still validated for each request. This has a visible cost even
for a repeated hover; an idle native context can answer faster than a local view.
The [CVA6 hover evaluation](../development/method-body-hover-evaluation.md) separates
first certification, warm edits, repeated idle requests, retirement and fallback,
and compares complete public results against native analysis.
