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
    "maxMiB": 64
  }
}
```

Set `experimentalFrozenSources` to `true` to evaluate reuse. A selection alone
never enables caching. `maxMiB` ranges from 0 to 512; zero disables retention,
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

An explicit full design can also select canonical elaborated instance paths:

```json
{
  "build": "design.f",
  "experimentalFrozenSources": false,
  "frozenSources": {
    "instances": ["hdl_top_pss.DUT"],
    "maxMiB": 64
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
For 64 MiB these limits are 16 and 48 MiB. Worker retention remains independently
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
