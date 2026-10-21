# Fixed scope semantic reuse

Reuse explicitly stable provider semantics across edits to separate consumer files.
Dependencies are trusted fixed until manual rebuild. All new experiments default
off. Existing UVM distribution remains one shipped default preparse artifact and
project-local one-time artifacts for other configurations.

## Supported file boundary

**Fixed providers and editable consumers must be separate physical files.** Scope
selectors identify reusable providers; they do not make a sibling declaration in
the same file independently editable. Selecting a scope freezes its provider file
in full, including other packages/modules, comments and whitespace. Any editor
change to that file requires `slang.rebuildScopeLibrary`. Rebuild preserves unsaved
editor text. External provider changes are picked up on explicit rebuild.

Keep stable library / RTL inputs before editable consumers in the filelist. The
conservative ordered-prefix route can also retain preceding inputs and dependency
definitions. Supported package-only closures can exclude unrelated separate roots.
Check actual packages, definitions and preparation reason with
`slang.scopeLibraryStatus`; do not assume only the selector's text was frozen.
Independent updates inside mixed fixed/editable files are outside this feature's
scope and are not an acceptance blocker. Consumers needing routine edits must be
outside the fixed file/input group.

## Implementation order and behavior

1. **Stable packages.** `experimentalScopeSemanticCache` uses
   `frozenSources.packages`. One retained compilation owns the selected packages
   and their fixed dependencies. Supported declaration closures discover actual
   native package lookups, including lookups with suppressed diagnostics; a local
   class name does not by itself select a same-named package. Shared classes,
   enums, structs and `std` identities remain stable. Consumer-dependent generic
   specializations stay in each consumer. Compatible ordered prefixes resume
   preprocessing; declaration closures that exclude earlier mutable roots parse
   current roots normally. `syntaxResumes == 0` does not imply semantic reuse failed.
2. **Modules and RTL subtrees.** `frozenSources.modules` selects definitions;
   `frozenSources.instances` selects a canonical instance and its descendants.
   Fixed definition identities can be shared independently of body reuse. An
   independent owner retains eligible elaborated subtree variants. Parameter
   signatures and fixed dependency identities must match; current connections,
   caller references and driver analysis remain in the current consumer. Closed
   nested, generated and array descendants are covered by certificates. Unsupported
   variants or boundaries keep ordinary native elaboration. A definition import or
   cached hierarchy JSON is not evidence of a body hit: inspect
   `instanceBodiesShared` and the `bodies` counters.
3. **Lifecycle and memory.** Ordinary consumer edits and context retirement retain
   the semantic generation. Explicit rebuild replaces it and preserves editor
   overlays. Owners retain source storage and library identities without retaining
   Driver loaders or chains of previous editing compilations. Namespace syntax
   views preserve library metadata, charge the original CST and release intermediate
   wrappers; a 128-view lifetime regression checks bounded retention. Shared ASTs
   must not absorb current consumer bindings or reference profiles.
4. **Real evaluation and publication.** Compare native analysis, dedicated UVM
   semantics and combined scope semantics on SPI/PSS/UBUS and larger genuine fixed
   provider workloads. Complete the correctness, latency, memory and release gates
   below before the authorized main push.

### Capability boundaries

Real-workload acceptance and language capability claims are separate deliverables.
A passing project proves the tested configurations and edit states. General support
requires a semantic rule, positive and rejected cases, and independent workloads.
Production admission must depend on language / ownership properties rather than
project names. The body certificate currently has this explicit boundary; package
owner reuse has its own broader class / generic behavior and separate audits.

| Boundary | Body reuse currently covered | Native path / remaining proof |
| --- | --- | --- |
| Inputs and identity | Fixed physical provider files; scalar integral parameters; fixed-origin integral / packed nominal types and local aliases | Editable-file siblings are outside the contract; foreign aliases, member-dependent defaults and overridable array values need more support |
| Local aggregates | Fixed-size arrays with admitted elements; typed localparam tables with exact ranges and deep values | Scalar runtime strings and their fixed-origin aliases are admitted; string parameters / arrays, dynamic arrays, queues and real / object signatures remain outside admission |
| Hierarchy | Closed ordinary, generated and array descendants; downward references entirely inside the retained root | Upward / outside dependencies and interface or ref boundaries require native analysis or further certification; mixed parents can only promote through an independently certified fresh compact owner |
| Calls | Fixed builtin queries; input-only local / immutable-package functions including void returns; checked severity / simulation control tasks; unseeded parameterless runtime `$urandom()` call graphs; local-destination `$sformat` and audited `$fopen` / `$fwrite` / `$fclose` bound calls | Seeded random calls, time queries, other file / formatting operations, general output/ref arguments, tasks, DPI and object methods have further effects to audit |
| Elaboration controls | Loop initialization / condition / step, flattened conditional controls / case labels and bound streaming slice expressions | New binding constructs must retain original dependencies before constant folding; resolved values alone are insufficient |
| Current caller effects | Current ports, incoming direct/hierarchical reads/writes, native driver/load analysis and raw diagnostic occurrences | Ref arguments, arbitrary-symbol debug arguments and external function caller removal have compact/full-context regression coverage; generic/import origins have separate acceptance below; unadmitted implicit method effects stay native |
| Lifecycle | Manual rebuild with editor overlays, budget fallback, retained owner variants and pinned-owner charges | Top-level mutable package / definition collisions have native fallback and repair coverage; generic ownership and extended variant/retirement memory have separate acceptance below, including the high footprint observation |

Each expansion must include root identity / hit evidence, equivalent fresh-native
queries and diagnostics, mutable external dependencies that reject reuse, invalid
constant contexts where applicable, and change/error/repair sequences. Rejected
cases must keep correct current native behavior. Trace admission reasons using
`SLANG_SCOPE_BODY_TRACE=1`; the ordinary `declines` counter counts retention/budget
declines rather than every failed body admission. Aggregate user-facing coverage
reporting remains useful follow-up work.

The real CVA6 Verilator workload now retains its entire selected root with a
256 MiB budget, including the mock tracer's audited local string and format/file
calls. At an explicitly configured 64 MiB budget it retains six eligible child owners instead.
Local nominal types, fixed arrays / constant tables, streams, unseeded random calls
and fixed-package void configuration checks have category-level regressions.
Other configurations and unadmitted effects still need independent acceptance;
removing a guard because a single trace reaches it is insufficient.
Types forwarded from outside a selected root still need ancestor elaboration-input
certificates. Equal bit widths or printed layouts do not establish nominal identity.
A parent containing previously borrowed child bodies needs an independent fresh
owner before promotion. Whole-root evidence, broader configurations, full-graph
asynchronous latency, memory and the dependency-closure audits remain release gates.

## Controls and limits

`slang.scopeLibraryStatus` reports preparation, generation, imported packages and
definitions, consumers, syntax resumes, retained/AST charges, actual body hits,
publications, evictions and decline reasons. `slang.rebuildScopeLibrary` explicitly
rebuilds the owner while keeping open editor text.

`frozenSources.maxMiB` defaults to 256 MiB (0..512; zero disables retention). The
library and independent body registry share this budget. Evicted owners that old
consumers still pin remain charged until released. Shared storage may be charged
conservatively more than once; these charges are not process RSS or a temporary
preparation limit. Oversized libraries or variants use native analysis. Dedicated
UVM semantics has its existing separate `uvm.semanticMaxMiB` setting; a combined
scope owner does not automatically add both budgets.

Do not combine this route with `experimentalFrozenSources` or method overlays.
Ambiguous selectors, unsupported compilation-unit/preprocessing contexts, semantic
errors and unsupported body boundaries must decline safely, with status explaining
why. An unsaved fixed-file edit requires explicit rebuild rather than publication
of stale matching-version diagnostics. Claims of dependency closure completeness
must account for lazily bound generic code and namespace changes in mutable roots.

## Acceptance gates

- **Correctness:** compare complete diagnostics and public hover, definition,
  references, highlights, symbols, completion and every completion resolve against
  fresh native analysis. Include real driver/load queries, parameter variants,
  error/repair, fixed-file edit rejection, manual rebuild, context retirement,
  budget fallback and owner release. Verify actual cached identities and hit
  counters; do not infer semantic reuse from latency alone.
- **First and continuous latency:** use fresh randomized paired processes, separate
  cold preparation/startup from first query, continuous edits, natural diagnostic
  publication and explicit rebuild. Report medians, request-tail P95 and paired
  uncertainty. A fast cached-open response is not cold analysis. Retain slow cases
  and report startup tradeoffs; do not hide regression behind continuous medians.
- **Memory:** measure sampled Mac process-family RSS and physical footprint in
  separate runs, including diagnostic children, many edits, variant changes,
  retirement and rebuild. Report retained charges separately. Check absence of
  editing-generation growth; sampled peaks are not guaranteed absolute peaks.
- **Release:** rebuild all targets; run CTest, Python system/method-scope tests,
  diagnostic transport tests and SDK reconstruction. Verify install/release layout,
  one shipped default UVM artifact, project cache variants and experiment defaults.
  Review the final diff and remote main state before pushing the accepted result.

Existing unit and real-workload checkpoints are evidence for their exact source
and executable versions. They do not replace fresh final acceptance after code
changes. Larger-workload/whole-design cold cost, memory and remaining unsupported
body boundaries must be assessed explicitly; this plan does not mark them complete.

### Whole-core runtime string expansion (2026-10-21, in acceptance)

CVA6's Verilator tracer uses runtime strings and formatted file calls. These
operations retain binding only; the compiler never performs simulation file IO.
`$sformat` destinations must belong to the retained hierarchy, and all operands
still undergo the dependency audit. String parameter signatures are not admitted.

The first full-root experiment exposed five false unused-package warnings:
parameter overrides, alias unwrapping and uninstantiated generate branches can
record references that disappear from the executable bound expression tree.
Certificates now replay local-origin references into fixed packages, and type
lookup unwrapping records its origin. They never copy the aggregate usage of a
package or an earlier editable caller. Whole-root acceptance requires full query
and diagnostic equivalence as well as a trace proving retain/reuse of the actual
selected root. Functional acceptance is not latency or memory evidence.

### Natural asynchronous full-graph acceptance (2026-10-21)

A separate CVA6 Verilator run sent only `didChange` and waited for every one of
231 diagnostic URIs before issuing any query or save. All 288 edit states from
48 fresh sequential processes (12 randomized four-arm blocks) matched the fresh
native complete-graph oracle, including errors and repairs. The cached whole
root's full-publication median / P95 was 46.60 / 54.70 ms; native was
123.28 / 129.00 ms, previous partial-body reuse 63.66 / 73.54 ms, and budget
fallback 122.46 / 127.17 ms. First changed-state whole-root median was 54.40 ms;
this follows startup / unchanged open and is not cold preparation. Median paired
whole/native block ratio was 0.3788 (bootstrap 95% interval 0.3722..0.3826), and
whole/partial was 0.7315 (0.7223..0.7367). This is serial idle-after-edit behavior;
it does not establish typing-burst cancellation behavior or another configuration.

The system regression now checks complete provider/consumer natural publications,
current editor versions, incoming reference removal, error/repair and actual hits
with cache enabled and disabled; no query or save can trigger the measured edit.

Memory attribution of the same root is dominated by retained semantic objects:
107,935,848 allocator bytes, 1,968,808 reuse bookkeeping bytes, 4,608 context syntax
bytes, 72 context source bytes, 720,896 location-table bytes and 3,916,618 source
manager bytes. Shared library storage is separately charged. This does not reduce
memory, prove liveness of each allocation or explain all process RSS. Larger
reductions require investigating per-occurrence elaboration versus safe canonical
instance sharing, as well as rebuild overlap; dropping reference tracking without
replacing its correctness guarantees is not an acceptable optimization.

### Mutable namespace conflict acceptance (2026-10-21)

A reproduced conflict in an earlier mutable file caused a cached consumer to use
7 instead of native 77, and reversed the duplicate-package warning / previous
location. Library imports now inspect current top-level declarations before
registering fixed namespaces. A declaration from another physical provider path
forces native binding for that analysis, including private source views. The
library generation stays available for automatic reuse after repair. Old explicit
context errors are re-resolved before taking a fixed-source owner shortcut.

Regression coverage includes earlier/later declarations, direct/macro definitions,
selected/dependency package names, duplicate modules with suspended/restored body
hits, and exploration ambiguity/repair. A fresh LSP oracle compares hover,
definition, highlights, references, completion and all resolves, plus committed
diagnostics, in exploration and full-design modes. `namespaceDeclines` exposes
actual declined imports / views; preparation status is not an assertion that a
particular current analysis used the cache.

With the same **256 MiB** configured cache budget as the previous whole-root
control, 12 randomized paired four-arm blocks (48 processes, 288 edits) passed
complete 231-URI diagnostic and query equality. Natural full-publication median /
P95 was 46.93 / 54.79 ms versus previous whole-root 46.99 / 54.97 ms; paired
median ratio 0.9995, bootstrap 95% interval 0.9906..1.0078. Native was
123.40 / 129.99 ms. This supports no measured regression in this configuration.
It does not establish the same latency at an explicitly configured 64 MiB budget.

A separate 64-MiB CVA6 pilot (the default at the time) preserves all diagnostics/hover, but exposes
repeated attempts to build an oversized owner (about 114.5 MB, plus a 34.9 MB
library), followed by oversized full-context retries. No body owner is retained.
This is an open admission-cost issue: investigate bounded negative admission
records and budget-aware child selection, then measure default budgets, cold
preparation, changed variants and repair as well as the 256-MiB whole-root case.
Do not hide this case behind a larger configured budget or call this phase a
memory improvement. The SDK is unchanged in this phase.

### Budget-aware admission acceptance (2026-10-21)

Default-64-MiB CVA6 previously constructed an oversized compact owner and a full
context on each publication, retained no body, and regressed below native
performance. Admission now considers a candidate before descending into its
instances. Retained imports and successful candidates prune traversal; a rejected
parent still permits descendants. An unavoidable source-manager charge stops
construction when pinned / staged owners leave insufficient space. Compact
rejections are bounded exact strings, own no compiler or syntax, and are charged
with the registry. Cost advice only selects native analysis, never validates a
reuse. A full-context retry occurs at most once per publication.

Tests cover repeated large variants, a smaller changed parameter, budget/new
registry recovery, weak compilation/syntax lifetimes, 40 rejected variants with
32-record/64-KiB caps, genuine evicted pinned owners and budget-fitting children.
CVA6 retains six smaller owners at 64 MiB and one complete root at 256 MiB;
steady edits do not construct additional owners in the tested configuration.

A newly exercised retained source view exposed compilation-unit imports from
other physical roots in completion. Imported-definition views now preserve the
native source include-root boundary while ordinary native views keep their
existing behavior. Tests include an earlier separate import file and a same-root
included import, with indexed package dependencies. The old/new native CVA6
public outputs are also compared to avoid normalizing away a baseline change.

Twelve randomized five-arm blocks, 60 fresh processes, 240 edit states and 60
manual rebuild flows compared complete diagnostics and hover. Millisecond medians:

| Metric | Native | Previous 64 MiB | Current 64 MiB | Current 256 MiB |
| --- | ---: | ---: | ---: | ---: |
| Startup / preparation | 303.82 | 785.30 | 820.82 | 640.35 |
| First changed query | 132.62 | 274.92 | 100.61 | 49.73 |
| Continuous query | 129.22 | 274.25 | 101.09 | 49.68 |
| Synchronous save commit | 95.81 | 272.65 | 98.61 | 46.58 |
| Rebuild flow | 583.89 | 1686.95 | 1450.55 | 1073.52 |

Synchronous saves follow a query and are not natural edit-to-diagnostic latency.
A separate 60-process / 360-edit natural didChange-only full-231-URI run passed
all diagnostic/query values: current 64 MiB median/P95 96.33/100.94 ms, current
256 MiB 46.03/53.43 ms, native 123.49/130.65 ms, previous 64 MiB
267.74/280.85 ms. Paired median current64/native ratio was .7839 (bootstrap
95% .7746..7926); current64/previous64 .3577 (.3532..3632). No save, query or
polling could trigger these measured publications. This remains serial idle
editing, not a typing-burst/cancellation claim.

Twelve separate sampled-memory sessions (100 edits each, rebuild at 50) passed
all 1,200 diagnostic states; every owned process exited. Median peak RSS /
physical footprint MiB: native 329.70/214.45, previous64 510.36/336.13,
current64 513.64/336.19, current256 652.64/432.86. End-edit100 footprint was
149.52/144.24/154.39/222.31 respectively. Budget-aware admission fixes repeated
work and retains useful children, but does not reduce preparation/rebuild peaks.
Reducing initial probe allocation, safely sharing semantic objects, parent
promotion after partial imports, parameter/configuration families and typing
bursts remain separate acceptance work. No release/push claim follows this stage.

### Parent promotion through independent ownership (2026-10-21)

Changing a consumer parameter can close a previously outside-dependent selected
parent while its children are already borrowed from retained owners. The mixed
editing graph remains ineligible for retention. A failed certificate that first
encounters a borrowed child can request a fresh compact-context native owner;
that owner must independently pass the full certificate, definition/path/flags
and current parameter matching. No certificate or references from the mixed
consumer authorize reuse. Failure stores bounded, charged admission advice only,
so unsupported combinations do not rebuild a speculative owner on every edit.
Contexts requiring the full-context route retain their existing native behavior.

The new regression reproduces the previously missed parent hit, verifies native
semantic and unused/shadow diagnostics across parameter changes, and checks release
of the editing compilation and its syntax. Time-query and outside-reference cases
must stay native, with no repeated owner builds or budget overflow. These tests do
not prove promotion under every pinned-owner budget or cold preparation benefits.

### Budget policy clarification (2026-10-21)

The user chose a simple configurable retained-cache budget. Its default is now
256 MiB (range 0..512; zero disables retention). A candidate that cannot be
retained within the available budget stays native. Do not add pending replacements,
wait for old pins, or coordinate child-to-parent retirement to make a candidate
fit. The prototype for that transition was removed. Existing budget accounting,
bounded rejection advice and ordinary owner eviction remain; the charged budget
is not a process-memory or temporary-construction limit. Complete parent reuse
with sufficient room remains independently certified. Budget-pressure promotion
is no longer an acceptance requirement.

### Compilation-unit dependency audit (2026-10-21)

A fixed module calling a compilation-unit function declared in a later mutable
file lost that function through the imported definition's lexical scope. Fresh
native analysis produced parameter values 12, 13, 12 across edits; the imported
view had no value and different diagnostics. Import admission now selects native
binding when fixed definitions coexist with external compilation-unit declarations
or imports. Empty members and current-consumer bind directives are not lexical
bindings and retain their existing behavior. Package-only UVM ownership is
unchanged. This is a conservative capability boundary, not a proof that unrelated
unit declarations actually affect every selected module.

Regression coverage exercises functions, tasks, typedefs, variables, classes and
imports; value changes, removal/error and repair; fixed-source private views and
continued generation identity. The public reproducer compares full committed
diagnostics and provider hover values, including a function expanded from a fixed
macro at a mutable invocation site. Expansion-site paths govern admission.
Existing virtual-interface, parameter override and bind identity regressions pass.
The ordinary non-Verilator CVA6 profile has unsupported compilation-unit classes:
its six-state complete public/diagnostic oracle passes with native fallback. It
has no claimed semantic-cache speedup. Verilator-profile whole-root acceptance is
separate and continues to prove actual imported body hits.

### Character bursts and cancellation (2026-10-21)

After the compilation-unit fix, 12 randomized three-arm paired blocks (36 fresh
processes) compare native, omitted-budget default 256 MiB semantics, and 1 MiB
native fallback on the real CVA6 Verilator workload. Each process sends six
read/error/repair bursts: 32 consecutive single-character range insertions, then
16 insertions paced at 5 ms. There are 5,184 character insertions plus 216 full
state transitions. Every final 231-URI diagnostic batch and hover, definition,
highlights, references, completion and all resolves match fresh native analysis;
target publication versions never go backwards. No query/save/poll precedes the
final natural publication in this timed experiment.

Time from sending the last character to receiving the complete diagnostic graph:

| Input pattern | Native median / P95 | Default cache median / P95 | Budget fallback median / P95 |
| --- | --- | --- | --- |
| Consecutive characters | 127.13 / 130.24 ms | 48.84 / 55.85 ms | 126.68 / 131.72 ms |
| 5 ms between characters | 225.65 / 238.06 ms | 74.02 / 77.96 ms | 226.37 / 231.45 ms |

The paired block-median cache/native ratio is 0.3624 (bootstrap 95% interval
0.3566..0.4917). These are post-typing full-publication values, not first startup,
individual keystroke query latency, or a claim that every intermediate version
must publish. No outliers were removed. A separate six-block functional run
(18 fresh processes) injected 324 hover cancellation requests, all returning
-32800, with identical final complete diagnostics and public queries. Its latency
is not qualified. The permanent system regression now includes genuine range
insertions, error/repair, provider unused warnings and monotonic target versions.

### Real configuration variants, rebuild and memory (2026-10-21)

The unchanged CVA6 Verilator providers were driven by an editable module-local
configuration function setting `NrPMPEntries` to 4, 8, 16 or 32. Each four-edit
configuration group is followed by a stable configuration tail. Three randomized
Native/default-256-MiB/1-MiB-fallback blocks ran 120 edits per fresh process and a
manual rebuild at edit 60: 1,080 complete diagnostic/query states in the memory
run, and another 1,080 in a separate latency run. Parameter-sensitive CSR port
hover ranges and scope values match Native on every state. Rebuild preserves the
unsaved configuration and changes the semantic generation. Fixed provider files
remain unchanged. A trace-only 24-state pilot confirms actual selected-root and
eligible-child retention; not every later variant remains a whole-root hit.

Descriptive latency medians across three process blocks:

| Metric | Native | Default scope cache | Budget fallback |
| --- | --- | --- | --- |
| Fresh-process setup | 302.80 ms | 647.07 ms | 517.30 ms |
| First query after changing configuration | 125.72 ms | 264.02 ms | 126.58 ms |
| Query with unchanged configuration | 126.74 ms | 49.81 ms | 127.15 ms |
| Final stable-tail query | 127.23 ms | 50.09 ms | 127.40 ms |
| Synchronous save/response barrier | 91.87 ms | 42.86 ms | 92.37 ms |
| Rebuild/reselect/save flow | 615.80 ms | 1249.87 ms | 771.67 ms |

These synchronous saves and rebuild flows are not natural asynchronous diagnostic
latency. Configuration-change preparation is slower than Native; stable edits
amortize that cost. Three process blocks provide descriptive values, not a precise
independent-session tail or significance claim. All request outliers are retained.

Memory was sampled separately at 20 ms with no concurrent latency workload.
Median sampled process-family RSS peaks were 398.28 MiB Native, 843.58 MiB cache,
and 395.16 MiB fallback. Median sampled footprint peaks were 210.99, 492.11 and
215.88 MiB. The three cache peak footprints were 830.50, 492.11 and 491.41 MiB;
the 830.50 MiB observation also remained at the final sample. End footprints
ranged 183.45..205.75 MiB Native and 193.39..830.50 MiB cache. This is no memory
improvement claim, and medians must not hide the high retained-footprint case.
Cache retention charges stayed within 256 MiB; those charges are not a process
RSS or temporary-construction bound. Native fallback remains the policy when an
entry cannot fit. Stable-tail owner counts/bytes/build counts stop changing,
evicted-owner pins become zero, and all owned processes exit. These observations
and weak-owner unit regressions establish the tested lifetime boundary, not a
proof that every allocator page is immediately returned to macOS.

### Generic/import lifetime audit (2026-10-21)

A four-process public oracle covers 128 states through explicit and wildcard
imports, header-defined generic classes, out-of-block method implementations,
user class inheritance, 8/64-bit field replacements, missing fields and repair.
All complete diagnostics, hover/definition/references/highlights/symbols and
completion/resolves match Native. Shared library AST bytes stay constant. The
permanent regression adds 454 assertions checking that specializations belong to
the current compilation, fixed enum identity stays shared, and the fixed generic
definition's specialization count and allocator bytes do not grow across edits.

The import-origin audit also checks the admitted-body boundary: `Scope::find`
rejects explicit imports for hierarchical direct-member lookup; generic class
bodies are outside body-certificate admission. Library generic specializations
with mutable user types use the per-consumer path. No provenance guard was removed
or changed without a reproducer. This covers these concrete routes rather than
claiming all future language combinations are proven.


### Final release acceptance (2026-10-21)

The qualified Release executable has SHA-256
`012bbc3f908cc00ab5d28731868d2aed3ee2e87fcfcb27ccb63d1cfce9914706`.
All targets build; CTest passes 733/733, Python system/method-scope tests pass
17/17, and diagnostic transport checks pass 43/43. SDK restoration from base
`151a801ceaf6faf23d79a42d210ff67e50eb4660` reproduces all 709 exported files
exactly using patch SHA-256
`2c06cb75cca2ca062953bd1ffd0b2b2d48d87b886662e391b9c25e8523009048`.

Relocated installation and release-archive layouts pass native-versus-cached
completion/resolve/definition/hover/symbol/diagnostic comparisons through edits,
errors, repair and manual rebuild. Installation contains exactly one shipped
`preparsed/default.cst` beside the bundled UVM sources. Default bundled startup
records zero UVM parsing. Nondefault `UVM_NO_DPI` and
`UVM_USE_RESOURCE_CONVERTER` profiles parse once into the project cache; later
fresh semantic-cache processes read the same artifact without parsing.

A final sequential, randomized 12-block CVA6 comparison uses Native, omitted-budget
(default 256 MiB) scope reuse and an explicit 1 MiB fallback control. Every complete
231-URI diagnostic state and queried hover matches the native oracle, including
errors/repair and rebuild. The 225-root upstream Verilator profile retains the
selected whole root. Median times across independent process blocks:

| Metric | Native | Default scope reuse | Budget fallback |
| --- | --- | --- | --- |
| Fresh-process setup | 303.80 ms | 637.90 ms | 452.29 ms |
| First edit query after setup | 132.03 ms | 48.83 ms | 127.83 ms |
| Continuous edit query | 128.62 ms | 48.77 ms | 126.68 ms |
| Error-state query | 125.91 ms | 49.31 ms | 126.52 ms |
| Synchronous save/response barrier | 95.73 ms | 46.41 ms | 95.47 ms |
| Rebuild/reselect/save flow | 581.02 ms | 1069.72 ms | 722.83 ms |

First-open here is an unchanged didOpen after setup, about 20 ms in every arm;
it is not a cold first-edit result. Save barriers are not asynchronous diagnostic
latency. The separately recorded natural character-input bursts above establish
that tested asynchronous behavior. Preparation and rebuilding cost more, while
stable edits amortize them. The non-Verilator CVA6 profile retains its documented
native fallback for unsupported compilation-unit declarations.

The budget policy is final: a user-configurable 256 MiB default; an entry that
cannot fit uses native analysis. No pending replacement / draining mechanism is
required. Retention charges do not cap process RSS or construction peaks. The
variant-memory observations above remain part of acceptance and are not reported
as a memory reduction. All experiments remain disabled by default.


Final UVM preservation uses 20 randomized paired blocks for each SPI/PSS/UBUS
workload, 180 sequential fresh processes in total. Complete public responses,
completion resolves and diagnostic states match Native across all arms. Median
continuous diagnostic times (Native / dedicated UVM / combined scope) are
46.76 / 21.17 / 18.50 ms for SPI, 48.65 / 23.21 / 18.76 ms for PSS, and
48.29 / 21.99 / 21.93 ms for UBUS. Continuous query medians are respectively
15.42 / 12.86 / 11.51 ms, 16.74 / 14.16 / 12.12 ms and
17.27 / 14.72 / 14.65 ms. These results preserve dedicated UVM gains; UBUS
shows essentially the same cost in the dedicated and combined routes. Startup
through the first response remains slower with semantic preparation:
65.50 / 93.44 / 102.82 ms, 66.80 / 94.61 / 106.22 ms, and
63.31 / 92.06 / 92.07 ms. No outliers were dropped. Filesystem caches are warm;
these results do not claim physical cold-disk startup performance.

Final review covered main-server lifecycle/query changes, independent-owner
admission/retirement, SDK dependency certificates, parameter matching, current
caller effects and reference/diagnostic replay. Enum initializer expressions and
builtin query operands remain available to the certificate traversal. No new
reproducible blocker was found; this is a review of the supported boundary, not
proof for all future SystemVerilog constructs. Local release evidence resides in
`untracked/scope-release-2026-10-21`, with final UVM paired data under
`untracked/scope-semantic-2026-10-21/release-final-uvm-paired20.json`.
