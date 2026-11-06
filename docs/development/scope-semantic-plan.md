# Fixed scope semantic reuse

Reuse explicitly stable provider semantics across edits to separate consumer files.
Dependencies are trusted fixed until manual rebuild. All new experiments default
off. Existing UVM distribution remains one shipped default preparse artifact and
project-local one-time artifacts for other configurations.

## Current evaluation policy (2026-11-01)

The finite conditional contract maps all 111 noninvalid outer AST kinds and 43
admitted symbol kinds to explicit dependency rules. This is a source inventory
and family-level oracle matrix, not exhaustive dynamic coverage of every slang
kind in every context. Unknown or unproved contexts keep ordinary analysis.
See [the contract ledger](../../validation/data/freeze-language-contract-2026-10-30.json).

All real-project scope / UVM semantic evaluations use
`semanticCacheUnlimited: true` and omit numeric semantic budgets. A large project's
retained graph must not be truncated to meet an evaluation cap. Record actual RSS
and physical footprint independently of internal byte accounting. Explicit
protective-budget tests are separate from workload acceptance.

The complete checkpoint38 evaluation covered all 16 projects in both background
and original-file editing scenarios: three Native / freeze / combined arms, three
independent processes per arm, and fifteen editor lifecycle states. All 4,320
state oracles matched ordinary analysis, with unlimited semantic caches. Twelve
selected project roots import in full; four use safe partial hierarchies, including
161 maximal OpenTitan SoC roots and 41 OpenOFDM roots. Actual original-file
selections differ from background selections (the large SoC real-edit recipe is
package-only); report those selections with the timing data.

Checkpoint38 also passed four 1,024-edit memory workloads and an 8,192-edit PCS
verification workload. Automatic source-history retirement occurred at edit
4,458 while retaining the semantic generation and Native query / diagnostic
parity. These are bounded-workload observations, not a hard process-memory cap.

Checkpoint39 passed 1,065 CTest entries, all 16 seven-stage public-query oracles,
all 16 fifteen-state original-file editing oracles in three arms, full server ASan
(1,051 cases / 205,126 assertions), real-project ASan, and installed default/custom
UVM profiles. It removed an unnecessary whole-compilation scan from completion
resolve: the large SoC resolve observation fell from about 420 ms to about 0.5 ms.
The subsequent representative editor measurement lane was interrupted; its
unfinished cases are not passes.

Current qualification adds exact-input weak namespace witnesses and local buffer
path / kind witnesses. These avoid repeated proofs without retaining old editing
generations or weakening expanded syntax, macro, offset or parser-context checks.
The first fixed-source query after a consumer edit can still construct a current
instance view; time it separately from repeated unchanged queries. In the targeted
OpenOFDM lifecycle observation, first-query hover fell from about 488 ms to about
124 ms, while unchanged repeated hover remained below 1 ms. This is one process
per arm, not a tail estimate or an independent repeated performance result.
Current-source qualification is complete: 1,066 CTest entries, 113 scope cases /
33,693 assertions, full server ASan (1,052 cases / 205,189 assertions), all sixteen
public and original-file editing cohorts, installed profiles and offline integrity.
All 32 scenarios completed three independent Native/freeze/combined processes and
4,320 matching editor-state oracles. The current 8,192-edit PCS recheck again retired
source/index history at edit 4,458 without changing semantic generation or results.
See [the final Mac validation](../../validation/macos-generic-freezing-2026-11-01.md)
for all first/warm/fallback/memory observations and important negative results.
Experiments remain off by default. Large SoC background startup costs about 47–48 s
and memory can increase; WiFi original-file edits have a small overhead. These
costs are not hidden by the large continuous-edit wins in other scenarios.

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
outside the fixed physical files. Separate files can share an include context;
that context alone does not make an editable sibling fixed.

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
| Calls | Fixed builtin queries; closed local / immutable-package functions; checked severity / simulation tasks; argument-only builtin contracts, including final fixed-signature input-only runtime functions/tasks with final non-constant evaluation; local-destination formatting and audited memory/file operations | Runtime values are never cached. Methods, output/ref signatures, iterator/randomize `with` state and other hidden context require their own proof; unknown binders stay native |
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
Local nominal types, fixed arrays / constant tables, streams, runtime random
binding, constant-context errors, outside-argument rejection and fixed-package
void configuration checks have category-level regressions.
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


## 2026-11-02: large-SoC startup, stage 1

Validated implementation and measurements are recorded in
[the versioned dataset](../../validation/macos-soc-startup-stage1-2026-11-02.json).
The experiment remains disabled by default. Real-project tests use unlimited
semantic caches, with no numeric `maxMiB` / `semanticMaxMiB` overrides; source-history
retirement thresholds remain unchanged. The top_earlgrey background scenario
contains no UVM and retains exactly the same 161 safe RTL roots throughout edits,
error/repair, structural fallback, reopening and manual rebuilding.

Scope-locality answers are memoized, invalidating answers when the closed-class
set grows. Native local/cross-reference records are grouped by scope when their
monotonic maps grow; current usage bits are read rather than copied into a stale
index. Elaboration-control records have an incremental scope index extended on
every dependency-closure pass. Binding occurs after releasing index iterators,
and local records are audited in native order. Index allocations are included in
owner accounting. None of the dependency, boundary or diagnostic checks is removed.

The owner planner recognizes structurally fragmented selections before creating
many wrappers. At eight certified frontier roots it may try an independent full
forest first; this is a route heuristic, not a root limit or a cache quota.
Context-dependent refusals, particularly type provenance, keep the compact
closure route. A CVA6 coverage regression discovered during development was fixed
and covered by a dedicated test; its entire DUT remains reusable. Full-owner
structural refusals retain bounded exact-context negative advice, avoiding fresh
owner attempts after borrowed descendants mask the original failure. Advice stores
strings, never an editing graph. Independent SourceManager/Compilation ownership
and Native subtree matching remain mandatory.

The same-host three-process results below use medians. Startup runs from process
launch through the first hover response. First edit means body-1; continuous edits
aggregate body-1 through body-6 across the three processes. Diagnostic time ends at
the matching edited-document-version notification, not global diagnostic completion.
Peak memory is the median of each main server process's lifetime maximum physical
footprint, not peak RSS or a memory ceiling. OS and project parse caches can be warm.

| Metric | Frozen before → current | Frozen + local updates before → current |
|---|---:|---:|
| Startup (s) | 48.00 → 15.62 | 47.12 → 15.77 |
| First-edit query (ms) | 408.52 → 300.46 | 15.12 → 11.11 |
| First-edit diagnostic (ms) | 2726.46 → 693.89 | 2400.17 → 423.78 |
| Continuous query (ms) | 238.37 → 235.76 | 1.06 → 1.02 |
| Continuous diagnostic (ms) | 623.55 → 616.99 | 1.50 → 1.42 |
| Lifetime peak physical footprint (MiB) | 5899 → 4539 | 4163 → 3073 |

Native startup is 2.45 → 2.42s. Owner builds fall from 28 at startup / 37 after the first edit to one, with no first-edit increase.

The other scene data retains tradeoffs: CVA6 original-file combined startup is
2.18 → 3.07s while its continuous diagnostic is 13.59 → 13.45ms. Its UVM fixture
path changed after cleanup, so that cold-start difference is not attributed solely
to this optimization. SerDes continuous diagnostic is 1.48 → 1.53ms; its local
update benefit remains. No outliers are removed.

A separate single-process release trace observes owner binding at
1.49s, certificates at 6.31s and total publication at
9.54s. These timers nest and are not additive to the
three-process series. Instrumented work counters retain 171 full-owner candidate
certificates / 161 accepted roots. Their locality calls decrease from 258,002,501 to
90,226,784, and parent-scope steps from 2,176,948,773 to 110,471,844.
Counter-build elapsed times were collected alongside other jobs and do not qualify
latency. Independent certification still has material cost; this stage does not
make freezing free or implement direct retention of the original analysis.

Validation passed: 1068/1068 CTest; 115 scope cases / 33764 assertions; full server
ASan 1054 cases / 205260 assertions (leak detection disabled); 16-project public
API and original-file lifecycle parity against Native; real UBUS/CVA6/SoC ASan;
installed default/custom cold/custom warm UVM profiles, whole-CVA6-DUT reuse,
default-off behavior, 157-file installation integrity and offline SDK/dependency
restoration. Five serial scenarios × three processes × three arms × 15 states
provide 675 current Native-oracle matches. The dataset includes startup segments,
first/continuous edits, error/repair, structural fallback, reopening, full manual
rebuild through response readiness and per-state memory. Original-file SoC
package reuse is explicitly distinct from freezing the edited RTL.

Legacy test configurations referenced a removed installation. They now use the
retained repository UVM source; 155 source files were checked byte-for-byte against
the previously qualified installation. SoC has no UVM, so its before/after comparison
is unaffected. Cold UVM-path differences in other scenarios are not attributed
solely to the optimization. Temporary instrumented builds, obsolete candidates and
ended logs are removed; compact data, code-version receipts and still-used caches
are retained. The supported language/dependency boundary is unchanged.


## 2026-11-02: bounded direct initial-model retention

[Versioned measurements](../../validation/macos-soc-direct-retention-2026-11-02.json)
compare this candidate with main b7004f9, using the same five scenarios and three
processes per Native/frozen/combined arm. Experiments remain disabled by default.
Real-project semantic caches remain unlimited; source-history limits are unchanged.

The initial model can retain its standalone Compilation without retaining the
editing analysis wrapper. For a single selected top-level instance with no prior
body groups, syntax views bind in a separately retained SourceManager namespace,
with complete per-occurrence reference tracking enabled before elaboration.
Structurally fragmented full-forest publication can reuse this model after required
dataflow finishes, without constructing another owner. Compact type closure and
Native fallback remain available, including whole-CVA6-DUT reuse. Ordinary publication
keeps its existing cancellation timing. Direct eligibility is a restricted prototype,
not a promise to eliminate library preparation, all analysis or certification.

Reference metadata now indexes instance/class ancestor subtrees. Each certificate
selects its own reference buckets rather than filtering every bucket in the design.
The index follows native parent-instance links, rebuilds when reference maps grow,
reads current usage bits, includes closed classes and retains final locality checks.
Its allocated storage is charged to owner memory accounting.

Complete occurrence tracking exposed warning representatives that depended on
canonical body paths. Warning variants are ordered by semantic message before path,
and the first eligible representative is retained consistently. Errors preserve
legacy representative behavior; package diagnostics are unchanged. Dedicated tests
cover warning parity and graph lifetime: retained roots survive while the editing
wrapper and original source lease expire. Temporary project-specific tracing is removed.

Same top_earlgrey background, 161 retained roots, combined mode (medians):

| Metric | b7004f9 → current |
|---|---:|
| Startup | 15.77 → 12.65 s |
| First-edit query | 11.11 → 10.85 ms |
| First-edit diagnostic | 423.78 → 408.08 ms |
| Continuous query | 1.02 → 1.08 ms |
| Continuous diagnostic | 1.42 → 1.48 ms |
| Lifetime peak physical footprint | 3073.19 → 2836.32 MiB |

All three processes build zero additional owners at startup or first edit. A separate
single-process trace records certification at 3.69s and total publication at
5.34s, versus 6.31s / 9.54s in the previous stage. These timers nest and
are not additive. Instrumented locality calls decrease from 90,226,784 to
49,104,017; parent steps decrease from 110,471,844 to 60,483,329.
There are still 171 candidate certificates and 161 accepted roots. Counters attribute
work; concurrent instrumented elapsed times are not latency evidence.

Validation: 1070/1070 CTest, 117 scope cases / 33783 assertions, full server ASan
1056 cases / 205279 assertions (leak detection disabled), 16-project public API
and original-file lifecycle parity, UBUS/CVA6/SoC ASan, installed default/custom
cold/custom warm UVM, default-off behavior, 157 installed source files and offline
709-file SDK/dependency integrity. Five serial scenarios × three processes × three
arms × 15 states match the current Native oracle (675 states). Error/repair, structural
fallback, reopening and full manual-rebuild readiness are included.

Startup ends at the first hover response. Diagnostic timing ends at the matching
edited-document notification, not whole-workspace completion. Memory is the median
of process lifetime peak physical footprint, not peak RSS. OS/project parse caches
may be warm. Small sub-50ms timing changes are retained in the data rather than hidden;
continuous SoC diagnosis remains around 1.5ms. First-edit diagnosis remains hundreds
of milliseconds. The original-file SoC scenario uses package reuse and does not freeze
its edited RTL. Baseline/current UVM fixtures use identical canonical source paths.
Compact evidence and code versions are preserved; ended logs and obsolete probe
binaries are removed while active reusable caches remain.

### Single-analysis retention and incremental certification (2026-11-02)

The next stage retains the exact isolated initial Compilation for eligible module
and instance selections. Direct publication neither constructs a second semantic
owner nor compares a subtree with itself. A complete fixed physical prefix is
proved before attempting declaration projections; its syntax and declaration
Compilation are kept. Unsuccessful proofs keep the existing subset/Native routes.

Certification now indexes elaboration controls and definition sites by structural
ancestors. Reference buckets are constructed once and extended as lazy binding
adds keys, instead of repartitioning the entire design for every small map growth.
Usage bits remain live; locality and dependency checks remain required. A regression
covers new local/cross-scope references and updates to existing read/write usage.

Complete occurrence binding keeps Native canonical analysis identity in a
Compilation-local sidecar without mutating canonical fields on shared semantic
objects. Raw diagnostic occurrences remain available to certificates; presentation
uses Native canonical occurrences. Interface-only templates and formal type
parameter contexts retain the established path. Compact reconstruction releases
its unused isolated source-namespace lease, with a destruction/reuse regression.
Thus this stage broadens direct retention without claiming that every selected
hierarchy can avoid compact type-context reconstruction. CVA6 whole-DUT reuse
remains available through that route.

Experiments remain off by default. Real-project semantic cache budgets are unlimited
in these tests; source-history limits are unchanged. The versioned evidence below
records process startup through first hover, edited-document diagnostic notification,
and process lifetime peak physical footprint separately. These are distinct from
OS cold boot, whole-workspace diagnostic completion and RSS.

[Versioned measurements](../../validation/macos-single-analysis-freeze-2026-11-02.json) compare main `0d1e108` with this stage. Three processes per arm, 15 lifecycle states per process, seven scenarios (945 current public states) match Native. Medians, combined mode:

| Scenario | Startup s, before → after | Continuous query ms | Continuous edited diagnostic ms | Lifetime peak physical MiB |
|---|---:|---:|---:|---:|
| opentitan-soc-large / background | 12.65 → 5.39 | 1.08 → 1.09 | 1.48 → 1.50 | 2836.32 → 2671.83 |
| opentitan-soc-large / real-edit | 2.14 → 2.17 | 16.81 → 19.42 | 32.09 → 36.65 | 1576.41 → 1576.72 |
| cva6-uvm / background | 2.97 → 3.14 | 6.30 → 7.05 | 11.44 → 12.79 | 995.77 → 1056.69 |
| cva6-uvm / real-edit | 2.86 → 3.50 | 6.53 → 7.43 | 11.83 → 13.47 | 858.97 → 970.20 |
| serdes-pcs-verification / background | 0.08 → 0.08 | 0.71 → 0.85 | 1.28 → 1.53 | 49.36 → 49.44 |
| opentitan-aes-uvm / background | 2.38 → 2.49 | 2.26 → 3.78 | 3.46 → 5.71 | 559.83 → 697.63 |
| ethernet-udp-medium / background | 0.09 → 0.06 | 0.46 → 0.43 | 0.67 → 0.61 | 26.77 → 26.66 |

The principal gain is SoC frozen-background startup, reduced by about 57%, with
161 retained roots and zero additional owner builds through the tested lifecycle.
A separate mechanism trace measures certificates at 1027.209ms and enclosing
publication at 1028.173ms; these nested timers are not additive. First-edit
latency is retained in the evidence and is not represented by continuous medians.
SoC original-file editing leaves its edited RTL Native and freezes packages.

This is not a universal memory improvement: CVA6 peak physical footprint increases
about 6% / 13% in the two scenarios; AES+UVM increases about 25%. CVA6 original-file
startup also increases from 2.86s to 3.50s in this cohort. These regressions remain
visible, while continuous diagnostic timings stay below 50ms. They are limitations
of this experimental stage, not reasons to enable it by default. Formal type
contexts can require a compact owner. Manual provider rebuilds may yield partial
CVA6 roots as in the prior version, rather than preserving a stale whole-DUT model.

Validation: 1072/1072 CTest; 119 scope cases / 33844 assertions; full server ASan
1058 cases / 205340 assertions with leak detection disabled; UBUS/CVA6 real-edit
and SoC public ASan scenarios; 16-project public and real-edit lifecycle parity;
installed default/custom cold/custom warm UVM profiles; installed source/archive
integrity, default-off smoke and fresh offline restoration of all 709 SDK files.
Reproduction uses the version-pinned serial-performance harness with three repeats
per Native/frozen/combined arm, seven named scenarios and 15 states per process.
Evidence records hashes, configuration, per-state query/diagnostic times, fallback
and rebuild counters, and physical memory separately. Ended raw logs and superseded
probe programs/installations are discarded; useful reusable caches are retained.

### Four-stage retained architecture acceptance — 2026-11-02

The four planned stages are implemented and qualified with experiments still off
by default. Detailed configurations, code and binary hashes, metric definitions,
all 945 qualified states and the previous main comparison are recorded in
`validation/macos-frozen-architecture-2026-11-02.json`.

1. **Ownership and memory.** Native retained namespaces share a copy-on-write
   location table with owned macro names. Pinned replacement preserves original
   IDs; detached queries require the exact native prefix and an explicit source
   lifetime lease. Rebuildable admission indexes are released after publication;
   bound objects, reference facts and certificates remain. New certificates can
   rebuild the indexes. The SoC mechanism probe removed about 82MiB of accounted
   owner storage; accounted arena bytes and kernel footprint are distinct metrics.
2. **First-edit certification.** Tooling symbol mapping suppresses its own
   artificial scope-dependency event. Ordinary HDL lookup, missing-name dependency
   tracking, lazy binding and constant evaluation continue to collect facts in
   normal analysis. This avoids a false first-edit full fallback after hover.
3. **Progressive fallback.** Existing certified body updates retain their whole
   original context. Larger edits import eligible frozen instance scopes and can
   replay completed closed procedure summaries. The explicit replay mode preserves
   the old same-compilation reuse contract; ports, driver registration, constraints
   and consumer checks run freshly. Package callees outside the imported-body
   closure are recomputed. Only exact original owners publish flow snapshots;
   edited generations cannot form a retention chain. Snapshot release preserves
   active readers through separately accounted weak leases. Analysis remains
   immediate and cancellable; no debounce was added.
4. **Closed type contexts.** Explicit instance selectors may retain the original
   semantic objects for caller-provided simple integral types, enums, packed
   structs, untagged non-soft packed unions and fixed arrays of supported elements.
   Provenance is recorded before certification, without copying those objects.
   Fixed imported-provider members retain their existing identities without being
   mutated. Every consumer still checks provenance and complete concrete layout.
   Class/covergroup contexts, open types and arbitrary module-selector type
   contexts retain conservative fallback. An unsafe ancestor does not prevent
   eligible descendants from remaining frozen. CVA6 now directly retains the
   complete selected DUT with zero additional owner builds.

The original compilation's allocation arena remains indivisible. Direct ownership
can retain surrounding native context after a larger edit; it cannot free individual
unselected objects from that arena. This is a material cost of the current design,
especially in CVA6 real edits. Source sharing removes duplication where ownership
allows it, without claiming that all retained arena bytes are avoidable.

#### Qualified comparison with previous main

Values are previous main `e837638` → this implementation, using the same target
source bytes. Each entry is the median of three fresh processes. Startup includes
initialization, build selection and the initial hover response. Diagnostic latency
runs from the edit notification to the edited-file publication and overlaps query
time. Continuous latency is the within-process median of six body edits, followed
by the across-process median. Peak is the median lifetime physical-footprint peak
over all fifteen states, including manual rebuild. OS and project parse caches may
already be warm. Background fixtures and real project edits are separate scenarios.

| 场景 | 启动 ms | 首次诊断 ms | 连续诊断 ms | 结构回退诊断 ms | 全流程峰值 MiB |
|---|---:|---:|---:|---:|---:|
| opentitan-soc-large / background | 5390.27 → 5046.97 | 407.88 → 23.04 | 1.48 → 1.33 | 676.73 → 562.15 | 2671.83 → 2535.99 |
| opentitan-soc-large / real-edit | 2170.69 → 2127.38 | 76.26 → 69.17 | 36.62 → 32.48 | 1377.67 → 1384.35 | 1576.72 → 1575.35 |
| cva6-uvm / background | 3135.25 → 2893.23 | 801.63 → 48.22 | 12.59 → 11.30 | 946.28 → 1140.60 | 1056.69 → 939.44 |
| cva6-uvm / real-edit | 3504.70 → 2793.90 | 862.72 → 48.99 | 13.34 → 11.86 | 773.00 → 894.91 | 970.20 → 1260.14 |
| serdes-pcs-verification / background | 76.76 → 74.39 | 29.77 → 5.94 | 1.52 → 1.10 | 30.27 → 30.50 | 49.44 → 37.33 |
| opentitan-aes-uvm / background | 2489.21 → 2252.37 | 273.19 → 20.17 | 3.65 → 3.34 | 377.15 → 390.28 | 697.63 → 720.72 |
| ethernet-udp-medium / background | 57.44 → 58.81 | 7.96 → 4.61 | 0.59 → 0.63 | 10.05 → 9.27 | 26.66 → 26.22 |

CVA6 structural fallback regresses despite its large first-edit and startup gains:
background diagnostics increase by about 194ms, and real-edit diagnostics by about
122ms. CVA6 real-edit peak footprint also increases by about 290MiB. AES+UVM has a
smaller peak and structural-latency increase. These are recorded tradeoffs; this
release does not establish an improvement on every path. The next performance
priority is typed-context structural validation / flow replay cost and the lifetime
of the indivisible original arena. UDP sub-millisecond variation is not treated as
an optimization claim.

#### Correctness, packaging and reproduction

Acceptance passed 1079 CTest entries, including 2650 SDK unit cases; 1064 server
cases / 205263 assertions under ASan; and 43 source-snapshot cases / 8758 assertions
in both normal and ASan builds. Leak detection was disabled. Sixteen indexed public
query projects × two arms × seven states and sixteen real-edit projects × three
arms × fifteen states matched Native results. ASan additionally covered UBUS and
CVA6 real-edit lifecycles plus the SoC public-query lifecycle. Full project indexes
are required for the public-query oracle; otherwise Native lacks package symbols.

Default-off smoke, installed default/custom cold/custom warm UVM profiles, archive
and source integrity, and fresh offline SDK restoration of all 709 files passed.
Only the default UVM configuration is distributed as a preparsed result; custom
project configurations retain their existing one-time project cache policy.

The serial harness records cached-body imports separately from certified local
updates of the original full model. Both preserve already analyzed fixed code;
only imports increment import counters. Earlier four qualified scenarios used a
stricter post-measurement check; the remaining three use the explicit evidence
labels. Timing boundaries and timestamp code are unchanged.

Reproduction uses `untracked/frozen-architecture-2026-11-02` qualification scripts
and `untracked/rtl-freeze-audit-2026-10-30` workload harnesses. Functional checks use
`TMPDIR=/private/tmp` to avoid Mac temporary-directory alias differences. Timing
runs are serial after functional jobs end, with three Native/frozen/combined
processes per scenario and unlimited semantic test caches. Versioned evidence
contains per-state data and configuration; ended raw logs and expired probes are
removed. Useful source corpora, current builds and active caches remain available.

### Certification and diagnostic ownership indexes — 2026-11-06

This stage removes repeated metadata searches during fixed-body certification
and diagnostic replay. It does not add a syntax exception or change which
dependencies are allowed to be frozen. All experimental options remain off by
default; qualification explicitly uses unlimited semantic caches.

The compilation lazily indexes expression-bearing attributes, hierarchical
effects and raw diagnostic occurrences by their enclosing instance bodies and
classes. Initial metadata is indexed once, and subsequent attachments and
diagnostics are appended incrementally. Attribute attachment revisions discard
superseded entries; attribute and diagnostic records created before their scope
is parented wait for a complete ancestry. Fixed-point closure consumes only newly
relevant attribute entries. Admission-local indexes are released after publication
and included in the existing memory accounting while alive.

Raw diagnostic handles contain code, location and occurrence ordinal, rather
than pointers into growable vectors. Coalescing advances an order revision so
admission can rebuild an invalidated index. Each certificate owns diagnostic
snapshots and ordered body/class replay buckets. Selection preserves the original
occurrence order, Native canonical skips, current-consumer remapping and class
diagnostic phases. Diagnostic source membership uses merged declaration intervals
per buffer, built only if a diagnostic actually needs a location check.

Detached source projection uses one lazy reverse index shared by the immutable
source prefix. Keys narrow the search; full file bytes, line directives, include
provenance and macro identity are still compared. Equal keys retain source entry
order, and collisions cannot authorize reuse. Query source storage is never
retained by this index. Shared-prefix memory is charged once in aggregate storage
accounting.

Independent code-feature tests increase repeated instances through 1, 16, 64 and
256. Each adds one expression attribute, one hierarchical write and one raw
warning. The final indexed and visited counts are N for each feature; certifying
the first instance visits one relevant record even with N-1 unrelated siblings.
Projection tests increase macro occurrences through 16, 256 and 2048: one index
build and N+2 exact candidate checks cover the N expansions and two files. Further
tests exercise replacement attachments, 128 appended diagnostic occurrences,
vector/hash-table growth, coalescing revisions and changed macro identity.

These are bounded algorithmic claims. Ownership indexing still walks ancestry;
nested overlapping certificates can legitimately inspect the same subtree.
Unscoped diagnostics retain conservative checks, and still-unparented metadata
may need another ancestry check. Hash collisions or genuinely
duplicate provenance can produce multiple candidates. Full elaboration, global
diagnostic rendering/coalescing and nonlocal edits still perform their required
work; this stage does not claim to make those paths local.

Acceptance includes 1083 normal CTest entries (including 2650 SDK cases), the
entire ASan server suite (1068 cases / 206718 assertions) and entire ASan SDK
suite (2650 cases / 37128 assertions), with leak detection disabled. Sixteen
indexed public-query projects each compare two arms over seven states with
Native. ASan additionally checks all fifteen real-edit states in UBUS and CVA6
across three arms, and seven SoC public-query states across two arms. The four
new feature tests contain 1455 assertions. All passed.

Installed default-off, default shipped UVM, custom cold UVM and custom warm UVM
checks passed. The 157-file installed UVM manifest, license/notice files and
default archive digest were verified; only `default.cst` is distributed. Original
project configuration and source bytes remained unchanged. Fresh offline
restoration verified all 709 SDK files after applying the patch to raw baseline
Git blobs. Updated dependency content was also checked against the manifest.

Versioned measurements are in
`validation/macos-certification-indexes-2026-11-06.json`. Query latency measures
edit/request dispatch through the hover response; diagnostic latency measures
the same dispatch through the matching edited-document version publication.
These overlap and must not be added. Startup includes launch, initialization,
build/active-instance selection and initial hover. These are new processes with
potentially warm OS caches and existing one-time UVM artifacts, not cold-disk
measurements. Physical footprint is distinct from RSS; lifecycle peaks include
structural edits and manual rebuild. Serial awaited edits do not establish burst
tail latency or long-session memory behavior.

The main matrix covers sixteen real-edit projects plus whole-design background
scenarios for SoC and CVA6, three processes per arm/version: 4860 states. Five
SPI-PSS crossover pairs and two SoC crossover pairs add 630 qualified states.
All diagnostic comparisons passed, and 540 arm/state comparisons preserved the
exact retained-root count and root-set hash. Native uses the same scheduling and
index settings with semantic/body reuse disabled. The baseline is local main
`cb832f6514cbdacaab8485a8951e888cb3a20532`.

Selected main-matrix combined-mode medians (before → after):

| Scenario | Startup ms | First-edit diagnostics ms | Continuous diagnostics ms | Lifecycle peak MiB |
| --- | ---: | ---: | ---: | ---: |
| SoC real edit | 2138.120 → 2136.567 | 67.760 → 72.268 | 32.145 → 32.056 | 1575.580 → 1575.783 |
| CVA6+UVM real edit | 2799.779 → 2797.513 | 47.879 → 47.734 | 11.946 → 11.929 | 1258.173 → 1262.470 |
| Whole SoC background | 5097.150 → 5488.862 | 23.397 → 22.694 | 1.425 → 1.421 | 2639.065 → 2650.893 |
| CVA6+UVM background | 2869.733 → 2902.508 | 46.538 → 46.726 | 11.173 → 11.315 | 938.142 → 936.705 |

The results support improved certification scaling, with typical existing-project
edit latency largely unchanged. They do not demonstrate a stable overall startup
or peak-memory reduction. Important observations were investigated and retained:

- SPI-PSS combined startup initially rose 361.767 → 431.729 ms. Five serial
  crossover pairs did not reproduce that increase: 360.257 → 359.897 ms.
- Whole-SoC combined startup was also higher in two full-lifecycle crossover
  pairs (5147.357 → 5490.542 ms). Separate startup-only readiness runs were
  4961.903 → 4940.135 ms. Existing phase timers in separate traced readiness
  runs showed owner certification 1017.751 → 875.569 ms. These mechanism
  observations are excluded from qualified timing; nested phases overlap.
- SoC freeze-only lifecycle peak rose 3258.441 → 3868.722 MiB in the main matrix,
  concentrated at manual rebuild. Crossover runs showed large physical-peak
  variation and compression in both versions. Final active allocator bytes
  increased by 3548016 bytes (3.384 MiB), while body-6 footprint increased by
  about 4.5 MiB. These describe different memory quantities. The main peak
  observation is retained, and no peak-memory improvement is claimed.

Acceptance completed on 2026-11-07. Reusable qualification scripts and compact
data are under `untracked/certification-indexes-2026-11-06`; workload recipes and
corpora are reused offline. Ended raw logs, duplicate reports, temporary source
trees and expired binary/install copies are removed after recording code identities
and conclusions. Current build caches, dependencies and source corpora remain.
