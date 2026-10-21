# Experimental frozen package and module sources

The experiment retains syntax and preprocessing state for selected stable package
and module inputs, including explicit-filelist full-design mode. It is disabled
by default. It does not serialize a
compiler database or retain semantic symbols across compilations.

```json
{
  "compilationFilelist": "design.f",
  "experimentalFrozenSources": false,
  "frozenSources": {
    "packages": ["common_types_pkg"],
    "modules": ["spi_master"],
    "maxMiB": 256
  }
}
```

Set `experimentalFrozenSources` to `true` to evaluate reuse. A selection alone
never enables caching. `maxMiB` defaults to 256 MiB and ranges from 0 to 512; zero disables retention,
and an out-of-range budget declines retention. This budget is shared between
compilation-input groups and independent completion-detail parses of their source
dependencies. It charges retained syntax/source bytes and cache keys, not total
process RSS. Existing compilations can continue owning valid syntax after eviction.
The older UVM-prefix experiment remains a separate opt-in setting.

UVM does not need to use generic package freezing. For a UVM-only experiment,
configure the library and enable `experimentalUvmPrefixCache`, leaving
`experimentalFrozenSources` disabled. Evaluate generic freezing for other stable
packages, modules or instance subtrees separately. Enabling both switches does
not guarantee that the dedicated UVM continuation is used: a compatible generic
input group currently takes precedence. Compare the combined configuration before
assuming that it combines both performance benefits. All experiments stay off by
default; see [UVM prefix reuse](uvm-prefix-cache.md) for that route's boundaries.

An isolated diagnostic worker can retain the selected prefix too. It owns its
compiler state independently and applies the same configured budget to its
retained prefix, source manager and accumulated source locations, with additional
charges for its key and option storage. Parent and worker budgets are separate;
`maxMiB` is not a limit on their combined RSS or temporary semantic analysis.

## Selection and preprocessing boundaries

Package and module names are separate selectors. Each must identify exactly one
declaration of its requested kind among the configured native compilation inputs.
Missing and ambiguous names are reported by status. No file
is added or reordered to obtain a hit. To select a UVM distribution as an input,
use the independent [UVM source configuration](uvm-prefix-cache.md).

The server resolves each selected declaration to its real source roots, including
include ancestry. For each compilation unit it retains the complete ordered
prefix through the last selected declaration. Earlier roots remain dependencies,
even if their packages were not selected. A root containing both library and
user declarations remains one root: editing any part invalidates that group.
Selecting a declaration in the last root may therefore offer no edit-time benefit.
In full-design mode, a selection covering the entire input unit uses native
parsing when it needs rebuilding: there is no remaining suffix to resume.
Multi-root groups of this kind also avoid staging an unused initial UVM view.
An unchanged document keeps its ordinary native syntax tree.

Compatible edits parse the remaining roots and compose one logical compilation
unit, preserving the package nodes' immutable physical ancestry and metadata.
Dependent semantic analysis runs again. Rename, duplicate, remove or restore a
package declaration and the plan is recomputed; no manual unfreeze is needed.
An independent completion-detail lookup keeps its own original command-line macro
context, instead of borrowing the project's preceding-file macros. Its parse can
be retained only for dependencies of the selected input groups.

Unsafe directives, syntax recovery at the group boundary, live preprocessing
callbacks, inherited compilation-unit state and unsupported source-library contexts
use native parsing. An explicit-filelist elaborated design can reuse the same
syntax prefix. Each instance is elaborated again with current parameters,
generate conditions, interface connections and bindings. Active instance
selections are restored by their paths in the new compilation. Index-discovered
inputs without a resolved configured source group keep native parsing.

## Exact instance subtrees

The semantic experiment also retains fixed compilation-unit declarations in a
single configured unit: classes, forward types, typedefs, functions, tasks,
variables, parameters and DPI imports. Consumer units receive transparent names
while the declarations retain their original lexical owner. This covers tracer
classes declared outside a package. Generic specializations depending on consumer
types remain in the consumer. Unsupported compilation-unit constructs still use
native analysis.

Fixed global package imports are restored with consumer-local lookup records.
When fixed compilation-unit state exists, package selections keep the ordered
provider prefix instead of projecting away that namespace. Duplicate-import
diagnostics and explicit `$unit::` references retain native behavior.

New consumer compilation-unit declarations conservatively suspend this import.
Imports from fixed packages and unrelated DPI declarations are allowed only when
they cannot change the fixed namespace or an existing DPI C binding. Dependencies
remain fixed until manual rebuild; this check does not watch provider contents.

Errors outside an independent RTL candidate do not by themselves disable body
reuse. Its certificate still rejects local errors, unscoped diagnostics and
unproven dependencies; a retained body owner must have no semantic errors.
Independent contexts preserve inherited bind-target flags. Actual bound children
and types supplied by mutable ancestors can still require native analysis.
Inspect body hits separately from library preparation: this does not promise
reuse of the entire selected DUT in every testbench.

An explicit full design can also select canonical elaborated instance paths:

```json
{
  "build": "design.f",
  "experimentalFrozenSources": false,
  "frozenSources": {
    "instances": ["hdl_top_pss.DUT"],
    "maxMiB": 256
  }
}
```

Enable the experiment explicitly to evaluate it. The selection covers that
instance and its descendants, including indexed instances such as `top.array[0]`.
Missing paths, bare module definitions, array containers and noncanonical aliases
are reported separately. Resolved module definitions below the selected instance
also contribute to the complete-root syntax plan; interfaces keep their native
preprocessing context.

The current semantic prototype retains value-only `slang.getScope` and
`slang.getScopes` responses. It can reuse them across edits confined to eligible
automatic class-task process bodies. The complete input graph remains exact
outside those bodies. The initial whitelist excludes declarations, macros,
directives, loops, block statements and static tasks; unsupported edits rebuild
natively. This is a complete-input equivalence proof, not a dependency slice
guessed from names. Changes to parameters, generate structure, binds, defparams,
interfaces or other declarations invalidate the certificate. Returned source
locations additionally require their entire physical source files to remain
unchanged. A task edit sharing a file with returned RTL locations can therefore
miss conservatively.

Omitted task bodies must preserve their logical newline counts: a later
`__LINE__` expansion can affect elaboration even when the edited statements do
not. CRLF counts as one newline. Proofs belong to one live SourceManager and
compare exact physical-file contents. On compatible edits, traversal follows
changed files and all their include ancestors; a failed partial comparison
rebuilds the entire proof before admitting a new baseline. Earlier macro changes
can therefore change the interpretation of an otherwise unchanged include.

Successful filesystem checks can be shared only within the current dispatched
request, with the same context revision and unchanged native syntax generation.
Each new request still verifies current filesystem inputs. There is no time-based
freshness window or reliance on file-monitor events alone.

Completion, hover, references, driver analysis and diagnostics continue using
current native semantic state. Reused hierarchy values never suppress task-body
errors: changed code still receives matching-version diagnostics. No compiler
symbol pointers are transplanted into another compilation.

With instance selectors, one quarter of `maxMiB` is reserved for hierarchy values
and certificates; the remaining three quarters bound parent syntax retention.
For the default 256 MiB these limits are 64 and 192 MiB. Worker retention remains independently
bounded. Inspect both `sources.statistics.retainedBytes` and
`sources.instances.statistics.retainedBytes`; their sum remains within the parent
budget. These counters are not process RSS. Refresh and driver replacement release
the value cache too.

`sources.instances` reports resolved selectors, derived module names, its reserved
budget and preparation/hit/`crossEditHits`/invalidation/eviction/decline counters.
`fullCertifications`, `incrementalCertifications`, `lastVisitedSyntaxNodes`,
`reusedSourceFiles` and `requestWitnessReuses` expose proof costs separately.
A same-generation hit does not prove incremental reuse; `crossEditHits` identifies
reuse after the physical input generation changes. The prototype still has
substantial certification costs. The current twenty-pair formal comparison finds
a benefit for the larger PSS hierarchy during burst input: whole-session median
improves 13.80% against the current dedicated UVM route, with a positive paired
mean-saving interval. SPI and UBUS do not establish an additional benefit over
syntax-only reuse; the dedicated UVM route is faster in those cases. Waiting for
complete diagnostics after every edit removes the overall benefit against the
dedicated route in all three projects. Startup is also more expensive. See the
[current RTL evaluation](../../validation/macos-native-idle-rtl-2026-10-20.md)
for first/continuous responses, all controls and negative results. The current
[three-repeat memory evaluation](../../validation/macos-native-idle-memory-2026-10-20.md)
covers short and 1,000-edit exact-instance/package sessions, parent/worker family
peaks, real retirement and post-error/fallback/refresh transitions. These results
do not justify enabling the experiment by default.

## Freshness and lifetime

Before protocol analysis the enabled experiment checks exact closed-file bytes,
failed lookups, source/include directory patterns, alias resolution and command-file
contents. It does not rely only on watcher events, file size or timestamps.
Open unsaved editor text wins over disk, including an open root deleted on disk.
A change rebuilds the native source context with the current editor overlays;
a stale SourceManager cache is not used as a native fallback. A concurrent change
during query preparation can reject that request with an instruction to retry.
Contexts that cannot provide a witness decline reuse.

Within one certification, ordinary and missing file lookups can share their
parent-directory resolution. Every leaf and closed-file byte/error result is
still checked, and the shared parents are checked again before acceptance.
These observations are discarded after the call. They are never a timestamp or
watcher substitute for the next query's proof. Background certification can yield
between filesystem reads when a foreground message arrives; every incremental
text notification is still synchronized before cancellation can stop analysis.

When the initial native parse supplies a compatible UVM first-root checkpoint,
the registry can retain that immutable view and extend it through subsequent
selected package roots. Staging a view does not certify freshness: its first use
still requires the complete current-input witness. The budget charges the full
retained native owner, which can exceed the selected prefix size; an oversized
owner is skipped. This optimization does not enable the older UVM experiment.

This certification performs filesystem I/O and can increase first-query and
cached-query cost. A group without a usable initial checkpoint is prepared when
first needed, so its first edit may pay a preparation cost. Compare complete first/continuous/fallback and
completion-resolution workloads before deciding whether to enable the experiment.
No universal latency or memory improvement is promised.

The registry evicts least-recently-used groups within its budget. Trees pin their
raw source storage until their final consumer releases them. Source-location
storage also follows the existing generation retirement threshold; it never
recycles IDs inside a live manager. Completion handles carry a context epoch,
so a replacement manager cannot interpret an old numeric ID as a new source.
Diagnostic workers own their source snapshots independently and currently use
their own complete-root checkpoints for an eligible single compilation unit.
They independently validate package declarations, ordered roots, options, generated
command-line sources and the immutable snapshot's source/lookup identity. A changed
dependency discards the old context. Unsafe resumption starts the entire job again
in a fresh native manager; repairing the inputs can prepare a new context.
Unsupported multi-unit worker jobs retain native parsing. Idle workers, context
replacement and refresh also release worker state.

If the current parent analysis has already declined a group's reuse, the worker
receives a negative-only hint and parses that job natively instead of repeating
the failed preparation. It cannot enable caching. A repaired group can prepare
again after a successful parent composition; the decline is not permanent.

A literal `define` or `undef` of a retained macro in a suffix root, or an
`undefineall`, can also trigger an early native decline before prefix preparation.
This avoids attempting a composition that would be rejected afterward. The check
can conservatively match comments or inactive directives; it never authorizes
reuse. Indirect header changes still undergo complete macro-environment validation,
and every successful reuse still requires its exact current-input witness.

## Status and refresh

Use `workspace/executeCommand` with no arguments:

- `slang.frozenSourcesStatus`: configured selections, complete ordered/selected
  roots, preparations, hits, independent-lookup counters, suffix resumes,
  invalidations/evictions/declines, charged retained bytes and the latest reason.
  `nativeSeeds` counts staged native views; `nativeSeedUses` counts their witnessed
  consumption. A staged view alone is not a successful reuse.
  `preflightDeclines` counts early native decisions. Per-unit
  `workerNativeReason`, when present, explains the current negative worker hint.
  `selectedRoots` describes the eligible plan; counters and bytes show actual
  preparation and retention. `witnessReloads` counts certification-triggered native
  context replacements, separately from ordinary file-watch/configuration reloads.
- `slang.refreshFrozenSources`: release cached groups and dependent query models;
  the next query prepares current inputs again. It preserves editor text and config.

For example, in a client connected to this server:

```lua
vim.lsp.buf.execute_command({ command = "slang.frozenSourcesStatus", arguments = {} })
vim.lsp.buf.execute_command({ command = "slang.refreshFrozenSources", arguments = {} })
```

The raw result is returned to the client; this does not install a new editor UI.
Status calls are mechanism checks and should stay outside latency measurements.

## Full-design context lifetime

Enabling either experimental cache also enables retirement of growing parser
contexts for full builds and explicitly selected top files. Replacements keep the
selected build/top, current unsaved documents and their versions, and still-valid
active instance/generate paths. Only paths are copied; compiler symbols and old
SourceManager IDs are not transplanted. Completion data keeps its owner epoch;
an expired handle can resolve by a supported current logical name, while bare
physical handles cannot be interpreted in a replacement context.

Retirement is triggered by SourceManager source and location-table growth, using
at least 16 MiB or twice the fresh context's reported size. This is a trigger,
not a process RSS limit or a hard allocation ceiling. A failed replacement keeps
the edited owner and can retry after the configuration or source is repaired.
User syntax and semantic errors continue through current native diagnostics.
Defaults remain unchanged when both cache experiments are off.

`sources.sourceContext` exposes `sourceBytes`, `locationTableBytes` and
`locationEntries` independently of cache-retention counters. These values help
verify that actual contexts are replaced; they do not account for every semantic
model, allocator, shared page or diagnostic process. Manual cache refresh clears
retained entries and analyses and is distinct from replacing the SourceManager.

When a selected full-design group ends at its final input root, an explicitly
configured UVM entry in the first root can still supply a retained dependency
prefix. The selected root is parsed and rebound with current text. This preserves
source order and macro checks. `dependencyResumes` counts these partial resumes;
it does not mean that every requested `selectedRoot` was reused. Without an
eligible earlier library, the existing native no-suffix path remains available.

Source authentication uses current opened-directory identities on Mac and shares
ordinary directory-prefix proofs on other platforms only within one request. It
rechecks parents after closed-source reads, including case-only renames on Mac. Large read sets use at
most four filesystem tasks per process when SDK threading is enabled; this is
separate from parsing concurrency. Byte comparisons, negative lookups and native
symlink/error handling remain mandatory. Cancellation drains in-flight reads
before their source-manager storage is released. These changes do not introduce
a cross-request freshness cache.
On Mac, the ordinary-leaf shortcut also checks actual leaf spelling with a
no-follow type/name query; a case-only filename rename cannot retain an obsolete
canonical identity. This supplements exact content reads and negative lookups.

## Fixed-dependency semantic experiment (under development)

`experimentalScopeSemanticCache` is a separate, default-off experiment using the
same `frozenSources.packages` and `maxMiB` selectors. The default retained-cache
budget is 256 MiB; set `frozenSources.maxMiB` to change it. An entry that cannot fit
uses ordinary analysis, without waiting for older consumers to release caches. For supported package-only
inputs, it binds a declaration view and discovers dependency packages through
actual compiler package lookups, including lookups with suppressed diagnostics.
It imports the selected packages and their discovered dependencies from one
semantic owner. Separate, unrelated source roots can remain editable, even when
they precede a selected package. The original ordered-prefix route remains for
inputs that cannot yet be isolated, including interface-dependent libraries. Unlike certified syntax retention, its dependencies are explicitly assumed
fixed until `slang.rebuildScopeLibrary` or configuration reload; no per-edit file
witness is performed. Ordinary consumer edits and context retirement retain the
owner. Consumer-dependent generic specializations remain in the consumer.
A declaration-only closure reparses mutable roots rather than resuming a prefix
that includes them; `syntaxResumes` can therefore stay zero despite semantic reuse.

Do not enable `experimentalFrozenSources` or method overlays with this route.
The current prototype supports one configured fixed declaration prefix containing
packages and module/interface definitions. Module selectors and concrete RTL
instance selectors prepare that definition closure. Parameters, attributes,
bind state and instance counts belong to each consumer; virtual interface types
keep the shared definition identity. `instanceBodiesShared` reports actual
whole-body hits in the independent subtree registry; a false value means that
this consumer has not yet reused an eligible body.
Inherited multi-unit preprocessing, unsupported unit declarations, ambiguous
selectors, errors and budget overflow decline retention. A module/interface owner
also uses native binding for an analysis containing compilation-unit functions,
tasks, types, variables, classes or imports outside its fixed provider files: those
names can be visible to native code but absent from the retained definition's
lexical scope. Declarations inside an editable module are unaffected. Current
`bind` directives keep their existing native binding path. Package-only UVM reuse
has its separate package namespace boundary. `namespaceDeclines` counts these
rejected imports; a prepared library need not be used by every analysis.

**Put fixed providers
and editable consumers in separate physical files.** Selecting a scope also freezes
other declarations in its provider file; this experiment does not independently
update a sibling scope in that file. Any editor change to the fixed file, including
comments or whitespace, requires `slang.rebuildScopeLibrary`. A rebuild preserves
unsaved editor text. External provider changes are intentionally picked up on
explicit rebuild rather than authenticated on every edit.

Place stable libraries / RTL before editable consumers in the filelist. A
conservative prefix can also freeze preceding inputs and dependency definitions;
inspect the actual packages, definitions and preparation reason in
`slang.scopeLibraryStatus`. Keep code that needs routine editing outside that fixed
input group. Independent mixed-file sibling editing is outside this feature's
supported usage.

Dedicated UVM semantics can join a compatible ordered package closure; otherwise the existing UVM route
remains available. Its shipped/default versus project-specific artifact policy
is unchanged.

`slang.scopeLibraryStatus` exposes actual preparation, imported packages, semantic
generation, consumers, syntax resumes, retained charge, AST charge and decline
reason. `namespaceDeclines` counts semantic imports / views declined because a
mutable root introduces a same-named fixed package or module/interface/program.
Those analyses follow native declaration order; removing the conflict allows the
existing library generation and body owners to be reused again. `prepared` means
the library is available, not that every current analysis imported it. `slang.rebuildScopeLibrary` rebuilds the owner while preserving editor
text. An unsaved fixed-source edit rejects source/consumer analysis with an
explicit rebuild instruction until that rebuild completes. Charges do not measure
RSS. The CVA6 and UVM acceptance results, including preparation and memory costs,
are recorded in the [implementation and acceptance plan](../development/scope-semantic-plan.md).
Passing those workloads does not extend the supported language boundary.

The fixed-scope experiment also automatically reuses eligible closed instance
subtrees at the same concrete path, including value-parameter variants, nested
children, loop generates and instance arrays. Both explicit module names and
instance paths select candidates. The registry prepares independent semantic owners
from already parsed trees; current outside port connections remain separate, while
closed internal connections remain shared. The parent is retained as one boundary
when its complete subtree is eligible. Unsupported parents can still admit eligible
children independently. Admitted values include fixed-size integral / packed
nominal arrays and scalar runtime strings with fixed-origin aliases. Input-only
local / fixed-package functions, local-destination `$sformat`, and audited
`$fopen` / `$fwrite` / `$fclose` bindings can be retained. These operations are
bound, never executed by the server. String parameter signatures and general
output/ref calls remain outside this boundary; every admitted call's operands
must still satisfy the dependency checks.

With one explicit top and no command-line parameter overrides, newly needed
closed subtrees can each use a minimal independent ancestor context. This includes
ordinary instance ancestors, active conditional generate branches and concrete
loop-generate iterations, with their original names and indices. Resolved root
value arguments are copied; fixed package enum typedefs preserve nominal assignment
types. Integral type arguments are reconstructed from resolved builtin types or
addressable fixed package typedefs. Each owner retains fixed provider definitions
and sources rather than the mutable consumer syntax. Multiple disjoint subtrees
can use separate compact owners.
The hierarchy path, definition identity, flags, complete parameter signature and
independence certificate are checked again against the native body before publication.
Unrelated unreferenced definitions are not provisionally instantiated in this
cache-only context; the current native design still receives complete diagnostics.
`bodies.compactPublications` counts actual owner publications through this route.
Instance-array ancestors, CLI overrides, unrepresentable arguments and failed
validation or combined compact-budget checks retain the full-context owner route
when its own syntax and memory budget permits it.

The registry uses the same `frozenSources.maxMiB` limit as the library. An
oversized parent does not prevent eligible descendants from being considered.
Repeated oversized compact variants can use bounded admission advice (at most
32 exact context keys and 64 KiB of charged storage, with a 16 KiB per-key cap).
This advice can only leave an analysis on the native path; it never authorizes
semantic sharing. New parameter contexts are considered independently, and
rebuilding the library replaces the advice with the registry.

Working owners pinned by the current analysis stay charged. When their remaining
budget cannot cover another owner's unavoidable source namespace charge, further
construction stops. Measured costs of matching retained variants can also
conservatively decline a new candidate. Every actual publication still requires
the full identity, parameter and independence checks. `ownerBuilds`,
`admissionHits`, `capacitySkips`, `admissionRecords` and `admissionBytes` expose
these decisions. Admission storage yields to semantic owners when needed.

The registry uses the same `frozenSources.maxMiB` limit as the library.
`chargedBytes` includes library, retained body-owner and evicted-but-still-pinned
charges. The nested `bodies` status reports the budget, base / owner / pinned
charges, owners, hits, publications, evictions and declines; `instanceBodiesShared`
reflects actual hits. Charges include reserved reference-profile hash storage and
subtree certificates. They can count shared prefix storage in more than one owner
and are not process RSS or a temporary preparation limit. Replacing the library
explicitly replaces the body registry; context retirement can retain it.

A variant matches resolved integral parameter values, widths, signedness, type
identity and override state. Current root argument expressions remain attached to
the current instance; retained root overrides store fixed values. Internal child
argument expressions remain intact. Defaults and local parameters can use
parameter-only constant expressions such as `$clog2(W)` and `$bits(W)`. Independent
owners bind every occurrence and retain per-occurrence reference profiles, avoiding
usage inherited from another parameter variant. Current consumer maps reproduce
native canonical-instance analysis without changing the shared instance objects.
Native instantiation-site counts, including grouped array elements, are replayed
for each current consumer.

Fixed package wildcard / explicit imports, immutable package constants,
multidimensional packed vectors and package-owned enums / packed structs / packed
unions are supported, including integral value parameters with those nominal types.
A foreign enum with the same numeric value cannot silently replace the declared
type. Type uses, constant reads and name-checked parameter defaults are replayed in
the current reference profile; unrelated package uses from old consumers are not.
Named assignment-pattern keys remain field metadata and do not count as parameter
reads.

Integral type parameters can use builtin scalars / packed vectors and fixed
package enums / packed structs / packed unions. Chained type-parameter defaults and
value parameters typed by those parameters are supported. Signatures distinguish
ranges, signedness, two-state / four-state representation, override state and fixed
nominal identity; two different fixed structs with the same fields do not share a
variant. Scalar spelling and fixed typedef names also distinguish variants so
editor queries cannot retain an old `reg` / `logic` or alias presentation.
Native name-checking of overridden defaults still contributes declaration
uses. Mutable aliases and member-dependent defaults use native analysis.
Anonymous packed struct / union and enum defaults inside the selected subtree
can also be reused. Their immutable declaration syntax, owning definition and
instance path must match, together with resolved field types / ranges and enum
values. Equal layouts from different declarations or instances do not establish
nominal identity. Enum parameter previews are prepared without elaborating ports,
variables or child instances, and are reused by later native elaboration after a
cache miss. Closed ancestor type arguments can be captured into an independent owner: integral
aliases, packed arrays, fixed unpacked arrays, enums, and packed structs / unions.
The captured graph preserves nominal declaration identity, alias presentation and
physical declaration locations without retaining a previous editing generation.
Current type layouts and declaration origins must match before import. Class-scoped
type dependencies stay native until specialization provenance is supported. Two
independent owners cannot overwrite one current nominal type identity; a conflicting
body stays native. Types with
unsupported dependencies or source locations outside the immutable source snapshot
continue through native analysis. Mutable module-target or hierarchical `bind`
directives also prevent importing affected bodies.

Body-local aliases, enums and packed structs / unions are also certified. Their
field types, enum values and declaration names / dimensions are audited so a
folded width cannot hide a dependency on a mutable package. Fixed-size unpacked
arrays can contain admitted integral or nominal elements, including nested arrays
and local typedefs forwarded to child instances inside the selected subtree.
Fixed-size `localparam` tables are supported when every leaf has an admitted
integral type and the full initializer / dimensions have fixed dependencies.
This includes nested tables of packed structs or enums and nonzero array bounds.
The native parameter vector includes body localparams: signatures compare their
resolved type / ranges and deep values, as well as ordinary scalar arguments.
Array vectors and large integer payloads in certified tables contribute to retained
memory charges. Overridable array-valued parameters still use native analysis.
Transparent enum members preserve their original nominal owner.
Pure built-in integral conversion, bit-count and fixed type / dimension queries
can be shared; arguments and `inside` range endpoints pass the same dependency
audit. Local functions with input-only arguments and admitted result / local types
are audited with their bodies; input-only void functions are included. Functions
owned by the immutable package library
reuse that package's bound graph and replay the actual call use. Closed class
specializations from fixed providers can also be owned independently, including
constructors and audited input-only methods. Runtime queues, dynamic arrays and
associative arrays can retain local container operations with audited arguments.
Virtual / randomization methods, DPI, output / reference arguments and accesses
to mutable outside state remain outside this admission path.
`$fatal`, `$error`, `$warning`, `$info`, `$stop` and `$finish` can be retained with
audited arguments; literal format strings are immutable data. Runtime display,
file-output and formatting calls, argument-free time queries, and supported
sampling calls can retain their bound graph without executing or caching a result.
Inherited outside clocking / disable contexts and unsupported sampling arguments
keep native analysis. Other unproven calls also keep native analysis.
An unseeded parameterless `$urandom()` can share its bound runtime call graph.
No random result is evaluated or cached: native constant evaluation still reports
an error and prevents retaining invalid constant contexts. Seeded random calls
and other unproven runtime functions continue to use native analysis.
Fixed-size streaming concatenations retain their bound slice-size expression for
the same dependency audit, rather than trusting only its folded numeric width.
Operands and `with` selections are also audited. A slice width from an editable
package cannot silently reuse a previously bound stream.
Loop-generate initialization, condition and step expressions are audited even
when no iteration is instantiated. Conditional-generate controls and case labels
are retained separately so directly nested constructs that flatten away a scope
cannot hide a mutable dependency.

Scope completion handles carry their context epoch from the first response,
including native fallback. Fresh and logical stale handles remain resolvable after
context retirement and explicit rebuild; expired unframed physical IDs are fenced.

Reading a frozen module with a selected current instance indexes that instance and
shares its full-design analysis. This preserves current parameter widths and driver
code in hover without adding default instances to the immutable library. Unbound
module declarations get a private default elaboration. Pure package views retain
the direct immutable-owner path.
Invalid selected-instance parameter arguments also keep the native private
shallow view until repaired; the current full design still diagnoses those errors.
Chained type parameters display their resolved target in completion details, and
overridden type parameters retain declaration documentation.

Reference profiles distinguish binding origin from declaration ownership. Only
uses within the certified subtree enter retained internal profiles, including
references between its child instance bodies; native syntax usage still includes
every reference. Downward hierarchical references are admitted only when every
resolved path component and the target remain inside that subtree. New consumers
bind outside hierarchical reads / writes again, so removed old callers cannot
hide unused or unassigned diagnostics. Incoming uses no longer require a scan of
the whole old consumer. Upward references and dependencies outside the retained
subtree still require native analysis.
Certified body uses are folded into the current reference map once on import;
each later declaration-use query does not scan every cached subtree.
Package declaration usage is captured once at the first export by moving the
existing profile. Later owner-side hover / completion lookups use a separate
overlay and cannot suppress unused-package warnings in subsequent consumers.

Internal semantic warnings retain raw per-occurrence records, including disabled
warnings, and are coalesced using the current consumer's instance counts. Current
diagnostic policy remains responsible for severity and suppression. Warnings with
locations, ranges or notes outside the fixed subtree conservatively prevent that
body from being retained. Outside root connections are diagnosed afresh.

For investigation, `SLANG_SCOPE_BODY_TRACE=1` emits admission reasons and actual
retained / reused hierarchy paths to stderr. It is disabled unless requested and
is not a timing mode. The ordinary `declines` counter does not count every failed
admission check.

Parameter defaults that require member elaboration (such as `$bits(signal)`),
consumer aliases outside the closed type capture, unsupported nonintegral types,
unsupported type parameters, interface / ref ports, and tasks / functions outside
the admitted closed / fixed provider cases
still use native analysis. Constructor-time exposure or creation of native
children can also prevent replacing a body. Broader coverage and formal
large-project performance / memory acceptance remain under
development. All new experiments remain disabled by default.

`slang.scopeLibraryStatus` includes `bodies.retainedRoots`, the deduplicated paths
currently held by independent owners. A root hit can cover its entire subtree;
the hit count is not the number of descendant instances skipped. Root retention
by itself does not guarantee every subsequent parameterization can import it.

### Experimental diagnostic delta publishing

`experimentalDiagnosticDeltaPublishing` defaults to `false` independently of the
semantic cache flags. When enabled, the server compares each completed diagnostic
list with the last list published for that URI before serializing an LSP message.
Ordering alone does not trigger a replacement; duplicate counts and every public
diagnostic field still participate. A changed document version is always published.
Errors removed by an edit are cleared normally. Compiler-context retirement hands
this publication history to its replacement, avoiding an empty-list / full-list
burst for unchanged sources. This reduces notification traffic and serialization;
it does not skip current semantic analysis or suppress warnings.

See the [Mac CVA6 whole-DUT evaluation](../development/cva6-dut-reuse-evaluation.md)
for measured latency, memory costs and complete acceptance boundaries.
