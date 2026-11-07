# Experimental frozen package and module sources

`experimentalFrozenSources` retains syntax and preprocessing state for selected
stable package and module inputs, including explicit-filelist full-design mode.
That syntax experiment does not retain semantic symbols across compilations.
The separate [semantic experiment](#fixed-dependency-semantic-experiment-under-development),
`experimentalScopeSemanticCache`, retains certified package/definition semantics
and eligible elaborated instance subtrees. Both experiments are disabled by default.

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

Package and definition names are separate selectors. `modules` also accepts stable
interface and program definitions, so providers can explicitly include an interface
dependency declared after the selected RTL. Each name must identify exactly one
declaration of its requested kind among the configured native compilation inputs.
Missing and ambiguous names are reported by status. No file
is added or reordered to obtain a hit. To select a UVM distribution as an input,
use the independent [UVM source configuration](uvm-prefix-cache.md).

The server resolves each selected declaration to its real source roots, including
include ancestry. The syntax experiment retains the complete ordered
prefix through the last selected declaration. Earlier roots remain dependencies,
even if their packages were not selected. A root containing both library and
user declarations remains one root: editing any part invalidates that group.
Selecting a declaration in the last root may therefore offer no edit-time benefit.
The semantic experiment first distinguishes physical provider ownership from its
include/input context. A closed declaration provider fixes its actual declaration,
used macro and type-dependency files, including included headers. A common
includer alone does not fix a different included consumer file. The current native
parse still validates declaration order, include/provider identity and parser
context before importing semantics. Changed used providers require
`slang.rebuildScopeLibrary`.

An ordered-prefix optimization is used only when it does not enlarge that fixed
physical footprint. Status reports the chosen route. When that route is used,
its earlier inputs are fixed dependencies too; mutable consumers must remain
outside that prefix. Moving stable library/RTL inputs before mutable consumers
can enable preprocessing resumption, but declaration semantics can reuse without
resuming a prefix.

The semantic experiment can retain a closed subset of selected definitions and
supported compilation-unit types, imports, classes and subroutines. Dependencies
close across separated fixed roots while preserving native declaration order and
compilation-unit boundaries. Unavailable, ambiguous, source-library or unsupported
dependencies exclude the affected declaration and its dependent parents;
independent safe descendants remain candidates. Entirely fixed physical files may
contain both reused declarations and ordinary-analysis remainders. This does not
permit editing a sibling inside a fixed file: the whole file is still fixed.

An error elsewhere in a native unit does not alone exclude an intact declaration.
Selected syntax must have no errors, missing tokens or recovery trivia. A semantic
error with a unique declaration owner excludes that declaration; ambiguous errors
exclude the affected physical root. Dependency rejection propagates before a
fresh closed provider is checked. Unattributed errors keep ordinary analysis
rather than guessing. The complete current native analysis still reports its
original errors; invalid bodies are not frozen.
Selected files remain fixed in full even when only some of their declarations
are reused. In particular, a macro used by the selected RTL can fix its wrapper
file without also fixing every other file that wrapper includes. Native remainders
inside that fixed wrapper keep current query routing; editing the wrapper itself
still requires rebuilding. Compilation-unit type/subroutine context follows the
original include/input unit without merging that context into the fixed file set.
Shared macro text can remain fixed across independent compilation units. This
never merges their declaration ownership or nominal type identities; unproved
shared declaration contexts keep ordinary analysis.

Projected providers use the current native parse to check expanded declaration
syntax, locations and per-declaration preprocessing state before importing their
semantics. A changed macro or directive context suspends the import; editing a
fixed provider or its retained include still requires manual rebuilding. No
filesystem freshness proof is added to the fixed-dependency contract.

An unchanged parser-state directive from a different physical input can move when
that editable input gains lines. Its text, source paths and effective declaration
metadata must still agree; only its byte offset may differ. This applies to
`resetall`, `default_nettype`, `timescale`, `unconnected_drive`,
`nounconnected_drive`, `celldefine` and `endcelldefine` trivia. Macro, conditional,
line-map and recovery witnesses retain strict location checks. Fixed declaration
tokens themselves must keep their original locations.

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

Prefix checkpoints retain `timescale`, `default_nettype`, `unconnected_drive`,
`celldefine` and their reset directives, including state inherited by the next
source file. Parse archives preserve the same terminal state. Keyword stacks and
unsupported pragma/protection state, syntax recovery at the group boundary,
live preprocessing callbacks, inherited compilation-unit state and unsupported
source-library contexts use native parsing. An explicit-filelist elaborated design can reuse the same
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

Fixed-size unpacked integral array parameters can form part of a retained body
signature, including nested arrays and overrides. Import checks the declared
ranges, nominal element types, override status and full four-state value. An
independent owner reconstructs overridden values without retaining their mutable
parent expressions. Dynamic arrays, queues and nonintegral elements still need
native analysis.

If fixed semantic preparation is refused because its dependency closure has errors,
`slang.scopeLibraryStatus` includes the first error's source file, line and message.
The server keeps native diagnostics; the status message does not suppress an error
or turn a refused library into a cache hit.

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
With a declaration-only closure, the server normally reparses mutable roots.
The optional [configured syntax cache](configured-syntax-prefix-cache.md) can
reuse the checked syntax of preceding roots separately. `syntaxResumes` may
stay zero even when semantic imports succeed.

Do not enable `experimentalFrozenSources` or method overlays with this route.
The server prepares fixed declarations through an ordered prefix or an isolated
declaration view. Module selectors and concrete RTL instance selectors prepare
the corresponding definition closure. Parameters, attributes,
bind state and instance counts belong to each consumer; virtual interface types
keep the shared definition identity. `instanceBodiesShared` reports whether the
independent subtree registry has accumulated any whole-body hits. It does not
prove that the latest consumer reused the selected root. `lastModelSharedRoots`
lists the actual imported root paths in the last completed full model; this
value-only snapshot survives model retirement and is not a document-version
certificate. Use it to distinguish prepared roots from roots actually reused.
Virtual-interface arguments from a fixed library retain their original type
identity. Before a retained DUT can use them, a private interface instance with
the same resolved parameters must pass the complete body-independence audit.
Its certificate is separate from the DUT certificate: proof instances never
become design hierarchy, imported roots, or replayed DUT diagnostics. Interfaces
with caller-dependent methods or unsupported proof dependencies still decline
whole-root reuse. Proof storage and owned argument values count toward the budget.
Existing assertion-control errors are preserved. The independence audit can retain
the native invalid-scope-target error for two-argument `$asserton`, `$assertoff`
and `$assertkill` calls with a decimal integer literal level and a string literal
target. Native rejects that target without looking up the hierarchy. Dynamic
arguments and other errors remain outside this exception.
Type dimensions may use `$bits` of local, fixed integral signals. This certifies
their type without treating the query as a runtime read; references to mutable
signals outside the retained subtree still decline. Diagnostic certificates own
their replay data, so additional private proof instances cannot invalidate it.
Declaration analysis defers virtual-interface body checking. Each complete
consumer creates private diagnostic instances with the same parameter values,
including interfaces reached through another retained library. Their checks use
the current design hierarchy without modifying the shared type owner. The
independent subtree owner still checks only its certified root; it does not serve
as a complete design diagnostic result.
Prefix reuse declines inherited multi-unit preprocessing; a positively proved
native declaration projection can still retain a closed subset. Unsupported unit
contexts, ambiguous selectors, errors within retained declarations and budget
overflow use ordinary analysis. A module/interface owner
also requires the consumer's compilation-unit visibility to match the retained
view. A dependency on a function, task, type, variable, class or import outside
that view keeps the affected declaration native when its dependency cannot be
proved. Declarations inside an editable module are unaffected. The current
compilation resolves `bind` directives before deciding which bodies can reuse.
Package-only UVM reuse has its separate package namespace boundary.
`namespaceDeclines` counts imports rejected by the namespace check.

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

Repeated queries validate a fixed source view against the same provider tree
used to construct it. Namespace compatibility results, including rejections, are
reused only while the provider and every ordered configured syntax tree retain
their identities. An edit, replaced compilation unit or rebuilt provider requires
a fresh proof. These keys are weak references and do not keep retired source
generations alive; fixed-source editor checks still run for each lookup.

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
reflects cumulative hits. Charges include reserved reference-profile hash storage and
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
continue through native analysis. The server resolves mutable `bind` directives
in the current compilation. Whole-body reuse requires a matching effective bind
context or a proof that the differences leave that body unaffected. Unresolved
or opaque contexts keep the affected body native. A rejected parent can still
expose independently certified children.

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
memory charges. Finite integral array overrides are compared by shape, nominal
element types and every element, and detached from old caller expressions.
String parameters, finite string arrays and builtin `string` type arguments also
participate in exact signatures. String bytes are owned and charged; generated
literals escape quotes, backslashes and nonprintable bytes. Embedded zero bytes
and strings too wide for a native packed literal keep native analysis rather than
changing the value during literal conversion. This restriction also applies to
string leaves of an array. No runtime string value is cached.
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
Runtime builtins with a final fixed-signature input binder and a final
non-constant evaluator can share their bound call graph. This covers the native
`$urandom` / `$urandom_range` binding family and fixed-signature runtime file
operations such as `$fseek`; method receivers, output signatures and `with`
contexts require separate proofs. Every argument and the result type still passes
the ownership / nominal-type audit. Runtime random values, file state and other
simulation state are never evaluated or cached. Native constant-context errors
remain errors, and a nested argument from mutable outside state rejects reuse.
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
Certification preserves Native's recorded read / write direction, including
constant operands; dependency checks do not manufacture extra reads. Certified
generic classes with a compilation-unit parent also use the current consumer's
reference profile, so removing an outside caller cannot inherit an old owner's
"used" flags.
Package declaration usage is captured once at the first export by moving the
existing profile. Later owner-side hover / completion lookups use a separate
overlay and cannot suppress unused-package warnings in subsequent consumers.

Internal semantic warnings retain raw per-occurrence records, including disabled
warnings, and are coalesced using the current consumer's instance counts. Current
diagnostic policy remains responsible for severity and suppression. Warnings with
locations, ranges or notes outside the certified declarations and closed class
dependencies conservatively prevent that body from being retained. Outside root
connections are diagnosed afresh.

Closed downward hierarchy writes and calls retain their binding origins. Only
visited instance bodies schedule those effects in the current Native finalization
pass. That pass can invalidate a canonical skip and revisit a retained descendant;
previously skipped raw diagnostics remain available, while already replayed
occurrences are not added again. Certified class diagnostics are emitted at Native's
existing class-specialization visits, preserving its representative ordering across
different parameters rather than depending on root import order.

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

Body certification has an explicit inventory gate for expression, statement,
timing, pattern, assertion, constraint and coverage-selector kinds. New native
kinds absent from that inventory decline with `unreviewed-ast-kind`. Listing a
kind is only the outer boundary: its type, ownership, hidden dependencies and
context must still pass the existing semantic checks. Invalid nodes are excluded.

Sequence delay and repetition bounds retain their original bound expressions for
dependency certification even after native analysis folds them to integer ranges.
This covers inline assertions as well as named sequences / properties, including
unbounded upper ranges. Local parameters and fixed-package bounds can reuse;
bounds from mutable providers require native analysis. Recording is enabled only
with body-reference tracking and does not memoize runtime assertion values.

The type operator `type(expression)` also retains its original source binding for
certification. A resolved builtin type alone does not prove independence: changing
a mutable operand's type can change assignment-pattern type-key matching without
changing the retained result type. Local and fixed-provider type sources can
reuse; mutable outside sources decline. Type-only dependencies do not require a
runtime-value usage proof for a fixed provider. Structured assignment patterns
explicitly certify their type-setter and field-setter metadata in addition to
native expression children. Controls and newly closed types are audited together
until no further dependencies appear, including controls discovered by lazy binding.

Local pattern bindings use the same type and ownership certification as other
local variables. Structure / tagged pattern field metadata must close over a
certified aggregate type; this does not admit otherwise unsupported union types.
Loop certification explicitly visits declared `for` variables and `foreach`
dimension variables, including unused variables and skipped dimensions. Iterator
symbols also certify their retained array type, not just their index / element
value type. These metadata checks supplement the native expression-child walk.

`slang.scopeLibraryStatus` includes `bodies.retainedRoots`, the deduplicated paths
currently held by independent owners. A root hit can cover its entire subtree;
the hit count is not the number of descendant instances skipped. Root retention
by itself does not guarantee every subsequent parameterization can import it.

Real-workload acceptance tests must set `semanticCacheUnlimited: true`
and omit numeric semantic cache budgets. Large designs are expected workloads;
a quota-triggered fallback does not establish the implementation's supported
scale. Test explicitly configured protective limits separately. This bypasses fixed-scope and dedicated
UVM semantic retention limits, including registries created when a driver is
promoted or rebuilt. It does not enable either semantic experiment. Scope and UVM
status expose `budgetUnlimited`; `bodies.budgetBytes` is zero when that flag is
true, meaning no finite quota rather than a disabled cache. Charged bytes remain
available as an accounting metric. Measure actual process RSS and physical
footprint separately; quota-truncated results do not describe full workload
capability. The semantic experiments and this setting remain disabled by default.

An RTL subset can include harmless module / interface / program siblings from
the already fixed physical roots. The root set comes from the eligible requested
definition graph before adding siblings. Sibling dependencies can close within
those roots, including other siblings; they cannot expand the set to unrelated
mutable providers. Additional macro-origin roots decline with
`outside-fixed-root-context`, and out-of-set module dependencies decline with
`outside-fixed-root-dependency`. Explicitly selected providers and dependencies
already belonging to the requested graph retain the usual fixed-input contract.
This does not enable routine editing of any part of a fixed provider file.

The RTL subset route can preserve safe declarations beside unsafe declarations
in the same fixed file. Unprepared declarations remain in the current native
compilation, including their diagnostics and current parameterizations. The
`nativeRemainderFiles` status lists fixed files containing such unprepared members.
Queries in these files use the complete current compilation instead of the
private projected provider view. Every part of these files remains fixed until
manual rebuild; this does not enable the unsupported fixed/editable mixed-file
use case. A semantic error uniquely attributable to one declaration excludes
that declaration and its dependents (`semantic-error-declaration`). Ambiguous
macro expansions or global error locations retain conservative physical-root
refusal (`semantic-error-root`), or decline preparation when no safe attribution
exists. A partial provider is replaced by the whole-prefix fast path only after the complete prefix passes parsing and semantic preparation; the successful prepared prefix is reused directly.
Changes in an outside preprocessing context can also fail
the strict declaration witness: matching tokens alone are insufficient when
leading directive trivia differs. Native analysis then reports current values;
manual rebuild establishes a new reusable generation.

`slang.scopeLibraryStatus.lastModelLibraryImport` reports the last committed full
model's namespace import independently of preparation and body reuse. Its
`status` is `no-model`, `not-configured`, `disabled`, `declined` or `imported`,
and `generation` identifies the library considered by that model. A failed
current-input namespace / preprocessing witness reports `namespace-witness` in
`failureCategory`; an import option without a library reports `library-unavailable`.
For `namespace-witness`, `witnessFailure` distinguishes `provider-path-conflict`,
`mutable-unit-binding`, `unclosed-global-name`, `duplicate-projected-definition`,
`missing-provider-declaration`, `provider-metadata`, `parser-context`,
`projected-syntax` and `missing-current-definition`. These are value-only
categories; no retained pointer into rejected or obsolete editor state is exposed.
This is a pre-body-hook outcome: `imported` does not imply any instance body was
reused. Check `lastModelSharedRoots` for actual body imports.

`lastModelCurrent` indicates whether that committed result covers the current
inputs. Editing, cancelling replacement analysis or retiring the model leaves
its value-only result visible with `lastModelCurrent: false`. An interrupted
candidate never publishes its import outcome; private source queries and
analyses of the same library generation cannot overwrite it. The aggregate
`namespaceDeclines` remains a lifetime counter and can increase during these
private or cancelled attempts.

`slang.scopeLibraryStatus.preparation` describes declaration preparation separately
from body certification. `status` is `not-requested`, `declined` or `prepared`;
`route` identifies `rtl-closed`, `rtl-subset`, `ordered-prefix`,
`package-projection`, or `native` after an overall decline. `failureCategory`
classifies overall selection, preprocessing, native-error, context or budget
failure; the existing top-level `reason` preserves the detailed explanation.

`attempts` records preparation routes considered, with `not-applicable`,
`candidate`, `superseded`, `discarded` or `used` outcomes. Only the final published
route is `used`. A failed subset attempt does not imply rejected declarations if
the later prefix route retains them. When no provider is published, tentative
exclusions are discarded and the overall failure / attempt outcomes explain the
native path.

For the chosen provider, `candidateDeclarations` counts the chosen declaration
graph. `preparedDeclarations` counts public packages and definitions plus
`preparedUnitTypes` and `preparedUnitSubroutines`; enum export names do not count
as additional declarations. `excludedDeclarations` counts rejected graph
declarations once each. It is
not a count of instances, source lines or reused bodies. `refusals` groups each
excluded declaration by its first cause, including unavailable / ambiguous
dependencies, dependency propagation, opaque unit names, source-library / unit
contexts, syntax errors, shared compilation-unit roots and semantic errors.
Unavailable means unavailable to this provider graph; the ordinary native loader
may still resolve the dependency. A semantic-error-root category describes the
whole excluded physical root, not a claim every sibling caused the error.

If an ordered prefix fails because of unrelated named `$unit` declarations,
preparation can try `closed-unit-subset`. Omitted unit declarations remain native,
including their errors. Retained declarations must close their named dependencies;
references to omitted bindings (including deferred generic bodies) refuse reuse.
Direct package members shadow imported / unit names, but explicit `$unit::` access
remains outside this projection. Package dependencies cannot absorb a file shared
with an editable consumer. Native syntax and preprocessing context are preserved;
disabled preprocessor text is permitted, while actual parser recovery refuses the
affected declaration. Macro definition roots remain dependencies; another file
using the same macro does not thereby become a fixed declaration owner. Named
port types and package-qualified references can close over known providers;
actual interface connections and external unit bindings retain their separate
context checks. Native binding distinguishes class / type-parameter qualifiers
from package dependencies. This route may retain safe children when a parent cannot
be prepared. Check actual imported roots before interpreting preparation success
as a performance result.

The report retains at most 32 categories, eight declaration-name samples, eight
binding-name samples (at most two per category), and eight route attempts;
`sampleBindings` identifies representative names causing a refusal. These samples
are separate from complete declaration counts. Names are shortened at a UTF-8
boundary to 160 bytes. Declaration counts
remain complete through `uncategorizedDeclarations`; truncation flags identify
omitted detail. These are report-size limits and do not limit semantic cache
capacity or freezing coverage. `preparationReportBytes` accounts retained report
storage, included in a published provider's semantic charge. Reports contain only
copied values and no AST owners. Ordinary edits share the immutable generation's
report; manual rebuild creates a fresh one.

Selective providers can retain CU classes, typedefs (including enum exports),
and simple free functions / tasks from a selected stable design's physical entry
files, including their headers. Pure CU type-only dependency files can also be
retained. Subroutines in unrelated input files, CU variables / parameters, legal
forward-declaration pairs, and extern class-method implementations remain outside
this selective route. They use the complete-prefix route when valid or stay
native. CU providers currently require one configured compilation unit; other
independent units can retain providers that do not need imported CU declarations.
Native binding determines exported names and ownership, and ambiguous ownership
refuses reuse. Exact declaration syntax, physical entry roots, and unit membership
are rechecked before import. Complete native inputs restore declarations at their
original positions instead of making types visible before declaration.

Module-instance syntax is not proof that a dependency is active for every
parameterization. The selective route preserves such references and defers actual
child binding to the existing body certificate. All active descendant definitions
must still be fixed and independently supported. An unresolved active child stays
native with its diagnostics, while an inactive branch can permit parent reuse.
`deferredModuleNames` counts syntax-only names eligible for this namespace rule;
these are not counts of cached bodies or missing implementations. A name used in
another lookup, including a deferred generic body or a repeated macro argument,
does not receive this exception.

The `coverage` object separates configured `requestedPackages`,
`requestedModules` and `requestedInstances` from `preparedDeclarations`,
`retainedRoots` and `lastModelImportedRoots`. Prepared declarations count top-level
packages and module/interface/program definitions, not source bytes or elaborated
instances. Retained roots are available cache entries; imported roots are entries
actually used by the last committed full model. A pending edit does not force a
model rebuild just to answer this status request. `lastModelSharedRoots` lists
those imported paths and `instanceBodiesShared` derives from that list, rather
than a lifetime hit counter.

`bodies.refusals` reports compiler certification and cache admission categories
without requiring debug tracing. Its `attempts` values are lifetime candidate
attempts within this library generation, including cached failures and repeated
edits; they are not distinct rejected instances or a coverage percentage. The
report keeps at most 32 categories and eight representative root paths overall,
with paths limited to 255 bytes without splitting UTF-8 characters.
`refusalSamplesTruncated` indicates omitted or shortened samples, and
`uncategorizedRefusals` counts attempts beyond the category limit. The fixed report
storage appears as `refusalReportBytes` and is charged in `baseBytes` and the
scope's total `chargedBytes`. Internal budget checks do not construct report
copies. Rebuilding the library starts a fresh report; any new analysis can then
record new refusals.

For example, `assertion-formal-ports` means that the selected root contains an
assertion declaration whose formal binding is outside the current certificate;
`user-nettype-resolution` keeps an internal custom net's resolution dependency in
ordinary analysis, including nettypes imported from a fixed package. Builtin net
types still use the normal closed-type proof. `budget-source-floor` and
`budget-owner` describe admission limits, not syntax restrictions. A retained
parent's safe descendants can still be attempted when the parent fails. Provider
preparation failures are separately explained by the top-level `reason`; a body
report does not claim to enumerate declaration-closure exclusions.

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


Hierarchy lookup can fold an instance / generate-array selector into its final
symbol path. The selector's original bound expression participates in the same
elaboration-control dependency proof as generate conditions, streaming slice
sizes, assertion bounds and `type(expr)` operands. This applies to both value
references and control targets such as `disable`. A local root parameter or fixed
provider constant can select a reusable local target; a mutable outside constant
cannot make that target safe merely because the resulting target belongs to the
retained subtree. Root parameter variants retain separate matching signatures.

### Finite semantic contract and conformance evidence

The reusable subset is defined by semantic ownership and dependency closure,
independently of project names. A recognized AST kind is only the entry gate;
it does not authorize reuse without its type, context, hidden metadata, usage,
diagnostic and current-boundary checks. The machine-readable inventory in
`validation/data/freeze-language-contract-2026-10-30.json` links 219 native kinds
to explicit rules and family-level Native oracle tests. It also inventories 99
native system binder classes, including inherited contracts. These counts are
inventory coordinates, not counts of always reusable language features or a
claim of exhaustive dynamic coverage.

| Family | Conditions for reuse | Ordinary-analysis boundary |
| --- | --- | --- |
| Stable declarations | Fixed module-like/package providers, preserved ordinary compilation-unit and preprocessing context, closed declaration graph | Unmodeled source-library/unit/config context, unavailable or ambiguous dependency, changed namespace witness |
| Hierarchy and ports | Fixed descendants with current consumer connections/overrides; independent matching owners and nominal identities | Ref/variable-inout or direct interface/modport aliases, unclosed upward/bind/config effects |
| Values and types | Closed scalar, packed/aggregate/container and class types; owned deep parameter keys; closed interface prototypes | A type accepted as a runtime value is not automatically a valid parameter key; unclosed nominal, alias or caller-dependent type graphs refuse |
| Expressions and control flow | Normal child traversal plus explicit pattern/iterator/loop metadata and folded-control checks | A local result or selected target does not hide mutable operands that were folded away |
| Calls | Explicit argument-only system binder contract or dedicated context proof; owned outputs and closed signatures/bodies/providers | Unproven binders, receivers, extra context or external destinations; runtime IO/random values are never cached results |
| Assertions and timing | Closed clocks/disables, zero-formal named assertions, complete expanded expressions and folded bounds | Assertion formals/recursive contexts, inherited external clocks/disables, clocking items/aliases |
| Constraints and coverage | Closed scope-randomize operands, distributions/weights/filters/options and local type graphs | Unproven class randomization, constraint restrictions or external operands |
| Opaque body contexts | The affected parent stays native while independently certified children may reuse | Specify/timing-check state, user primitives/resolution, procedural checker and randsequence graphs require further context proofs |

The finite-boundary conformance test uses an ordinary positive and six explicit
negative contexts: randsequence, procedural checker, specify, ref port, clocking
items and assertion formals. It verifies actual parent/child imports and Native
semantic/dataflow parity through baseline, structural edit, user error and repair.
Other linked tests cover type/parameter/nominal identity, call outputs, folding,
net resolution, incoming usage removal and independent owner lifetime. A family
oracle does not establish every kind/context combination; the inventory retains
that distinction. Final frozen-code query, formal latency/RSS and sanitizer
qualification remain separate gates.


Attributes are metadata on declarations, statements, expressions and port
connections, rather than normal expression children. Their original bound
expressions remain available after constant evaluation. Certification collects
attributes whose binding scopes belong to the retained region, including scopes
closed later through class/interface proofs, and applies the ordinary dependency
audit to those expressions. Local/fixed attribute values can reuse; mutable
outside dependencies refuse the affected parent while safe children can reuse.
Attributes on the caller's root instance and its connections are evaluated in the
current caller. Changing or removing those attributes does not invalidate an
otherwise independent body, and must not retain old caller reference flags.

Statement traversal also covers the declarations attached to each block scope;
a variable initializer must be audited even when the variable is never read.
The current source-field review covers all 31 noninvalid statement kinds,
nine timing-control kinds and five pattern kinds. Ordinary visitors are
supplemented by explicit checks for loop/pattern symbols and block-event/disable
targets. Randsequence and procedural checker graphs remain explicit exclusions.
The statement dependency fixture compares local, fixed-package and mutable-package
operands through value changes, user errors and repair. It checks actual parent
and child reuse and semantic/dataflow diagnostics against ordinary analysis.
Source-field review does not imply exhaustive dynamic coverage of every context.

Coverage selectors also need an ownership check for the symbol selected by
`binsof`: that target is metadata, rather than a normal expression child.
Local targets and targets owned by an imported fixed package may reuse. A target
from a mutable package keeps the affected parent native, even when the name is
package-qualified and produces no upward-reference side effect. Independently
certified children can still reuse. The regression compares target type widths
as well as diagnostic results after the external declaration changes.

Class certification closes every declared implemented or extended interface
class, in addition to the ordinary base class and generic arguments. An owned
local interface's pure virtual methods are signature contracts; their types and
defaults still need the normal dependency checks. Mutable interface providers
keep the affected parent native. Constructor arguments written in an `extends`
clause are also audited explicitly, because they are stored on the class rather
than in the constructor's statement body.

Lexical completion in a fixed module's source file preserves that file's
physical include-root view. Unrelated compilation-unit declarations and bound
instances declared in other roots do not expand its lexical suggestions.
Hierarchical member access and the hierarchy view still use the selected full
instance. Retained members mapped to the current instance count as local members;
their different arena addresses must not add a spurious module description.

Query comparisons need the same source index in both configurations. In
particular, an isolated test workspace with an external build file must also
index the original source directories so ordinary package lookup has its
providers. The current 16-project lexical query matrix uses identical explicit
index roots and checks hover, definition, completion, completion resolution,
references and diagnostic maps across initial analysis, structural edit, error
and repair. It separately verifies actual imported-body membership and preserved
reuse. This is correctness evidence, not a latency or memory result.

Namespace authentication can reuse a proof for the exact ordered immutable syntax
trees, source manager and live source-owner identity that produced it. These weak
records do not retain retired editing namespaces; changing a tree or rebuilding
the semantic library requires a new proof. Physical-buffer path and kind matching
is local to a syntax comparison. Macro expansion locations, token offsets,
spelling, trivia and effective parser context retain their existing checks.
