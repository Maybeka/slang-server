# Semantic reuse: correctness, coverage and cost

Started 2026-11-07; baseline main `1a44100d86d846ab99910e03dfb4636d026e1a9b`.

## Scope and order

Finish A before implementing B, and B before C. Later discoveries reopen the
owning stage explicitly. Existing ownership/index improvements remain in use.
Experiments stay disabled by default; real workload semantic caches are unlimited.
Fixed providers and editable consumers stay in separate physical files. Fixed
dependencies are trusted until manual rebuild, which preserves unsaved text.

| Stage | Work | Exit criterion |
| --- | --- | --- |
| A: diagnostic consistency | Follow generic-specialization occurrence collection, diagnostic grouping, representative selection, alias provenance and rendering across ordinary, tracked, retained and imported analyses. Fix shared mechanisms; do not normalize away visible differences in the test oracle. | Reduced tests cover order, parameter/type specializations, aliases, notes, counts and locations. DMA/OTBN match complete Native diagnostic payloads through edits and rebuild. Existing representative-order tests remain correct; no added whole-owner scan per query. |
| B: general freeze contract | Reconcile the existing AST/symbol contract ledger with slang's elaboration, lookup, type, evaluation, reference, driver and diagnostic mechanisms. Define reusable objects, all contextual contributions, dependencies and ownership for each semantic family. Implement missing common mechanisms rather than project-specific exceptions. | Each inventoried family has an explicit supported, context-dependent or inherently mutable boundary and supporting tests. Closed supported selections reuse original semantics; a failed parent preserves maximal independently proven child closures. Unknown or external mutable dependencies safely decline with actionable status. SPI and chip cases validate the mechanism without defining its scope. |
| C: scalability | Separate canonical instance semantics from per-occurrence contributions; make affected reference/call/driver/timing/diagnostic updates follow their dependency closure. Measure work units and cancellation, including unavoidable full analysis. | Feature ladders distinguish repeated instances from unique variants, local changes from wide dependency fan-out, and raw diagnostic occurrences from visible output count. Show reduced redundant work and preserved Native results; no claim that every edit is constant-time. |
| Release | Complete tests, existing and added opt-in workloads, package/SDK restoration and serial performance/memory qualification. Retain conclusions/data/code and input identities; retire terminal logs and disposable replay outputs. | First/continuous edit, startup, structural fallback, cancellation and memory assessed with matching actual reuse coverage. Push main only after acceptance; directly verify remote identity. |

## Whole-language scope

The existing `freeze-language-contract-2026-10-30.json` inventories 111
noninvalid outer AST kinds and 43 admitted symbol kinds. This is a starting
inventory, not proof of full slang-language support. Reuse it and audit its
semantic rules, including declarations/types, generic specialization, lookup and
imports, parameters/constants/system functions, generate/arrays/interfaces,
virtual interfaces, hierarchical references, bind/defparam, procedures/tasks,
assertions/checkers, drivers and reference/diagnostic contributions.

A syntax kind is not by itself a reusable semantic boundary. Record which
context makes its meaning fixed, which facts are per occurrence, how imported
objects keep identity and source ownership, and which contributions must be
replayed in a consumer. No source-text extraction, dependency suppression,
placeholder inputs or skipped warnings to obtain a cache hit. A genuine Native
error is preserved and distinguished from an artificial error introduced by
provider-only analysis. Full slang coverage is an assessment target, not an
unconditional promise to freeze context-dependent code unchanged.

## Validation policy

Use small language/mechanism cases, feature-isolated scale ladders, existing
real projects and a held-out real workload. Large projects remain opt-in; reduce
new concrete failures into point tests. Compare complete public diagnostics,
queries, document versions and recovery. Distinguish a requested freeze,
prepared closure, actual original-object reuse and local-update reuse.
Investigate latency increases over 50 ms and memory increases over both 5% and
20 MiB. Preserve all original samples and bounded follow-ups. Small latency
tradeoffs may be justified by larger end-to-end gains, never by weakening
correctness. No p99 assertion from three process samples.

## Progress

These entries record each stage at the time of its measurement. The final
candidate's qualification is recorded at the end.

- Goal active. Stage A passed. B implemented the closed ownership contract
  below, and C implemented targeted publication, method and syntax reuse.
  Cross-project and release acceptance are tracked at the end.
- DMA submits both address widths in both paths, with opposite raw order.
  Its logically equivalent generic warnings previously selected the first
  occurrence. Select a stable presentation without reordering raw references
  or assigning hierarchical instance counts to generic warnings.
- OTBN has one raw occurrence per group: the same class specialization can
  first be reached through a forwarded type parameter or its concrete typedef.
  Resolve the forwarding alias for diagnostic class arguments; preserve the
  explicit typedef and leave ordinary type printing unchanged.
- Both reduced fixtures fail on the published baseline and pass with the fix.
  The focused suite passed 9 cases / 1961 assertions; full CTest passed 1095.
  DMA and OTBN each passed all 45 Native/freeze/combined lifecycle states with
  their real designs loaded and edits in a separate mutable consumer. In the
  original-file control, DMA's initial diagnostics matched, and editing a file
  in the fixed closure was correctly refused. This does not expand the frozen
  provider file boundary.
- B0 reconciled the previous ledger against the current eight native kind
  inventories: 101 symbol kinds and 118 non-symbol kinds, including invalid
  sentinels. No enum kind lacks a recorded policy. Those policies still need
  to be checked against the current ownership and dependency gates.
- The first cross-family coverage gap is before body admission. Projected
  declaration preparation can decline on a name collision or a provider-only
  semantic error, preventing already-bound subtree certification from running.
  Separate eligibility to reuse a declaration namespace from eligibility to
  retain a closed body in its actual analyzed context. Deferred generic
  bindings, negative lookups, preprocessing and current external connections
  still require explicit proofs. Existing child admission remains the basis
  for safe partial coverage.
- B separates design-unit lookup from declaration and formal labels. Package
  and ordinary compilation-unit bindings retain the complete name inventory.
  For an omitted design unit that collides with a package routine local, every
  candidate occurrence needs a unique bound value in the same ordinary routine.
  The ordered current compilation units must preserve the supplying package
  syntax. Ambiguous positions, deferred generic bodies and unresolved names
  still require the ordinary namespace proof.
- Independent declaration projection now also follows prefix preprocessing and
  syntax refusal. The recovered-prefix test compares the original errors and
  proves actual borrowing of the unaffected body after an edit.
- Compilation-unit wildcard dependencies use a package's visible members and
  potential re-exports. Private imports add no external names. The wildcard
  regression also exercises an explicit re-export.
- B2 passed all 90 SPI Host and Device edit states with exact Native public
  results. Device reached 154 actual borrowed bodies in freeze-only and 14 in
  combined. Host exercised package import and local transactions. The readiness
  data records repeated owner builds in Device freeze-only for the later cost
  investigation. B4 passed 1099 CTest entries.
- Chip's constant-evaluation `$fatal` is present in Native. Removing false
  wildcard dependencies admits a declaration subset with 374 prepared entries.
  The current whole-library namespace gate still declines this subset before
  any body import. B continues with selective admission under the actual native
  namespace and the remaining semantic-family boundaries.
- The definition-name proof can combine deferred instance types with uniquely
  bound package locals of the same spelling. The definition still resolves in
  the consumer. An omitted native package can also supply a negative wildcard
  witness when its direct export surface is structural, disjoint from provider
  lookups, and its unique current declaration matches the original syntax.
  Re-exports and ranged enum names remain conservative. Full CTest passed
  1099 entries. Chip advanced from `i2c` to another omitted definition at the
  same gate and still imports zero bodies. The probe measured eligibility.

## Selective admission and family coverage

Use the declaration graph as the unit of rejection at preparation. Each
namespace proof should identify which provider declarations require it and
whether the dependency affects provider binding, current-context binding, or
both. Reject an unproven declaration and its reverse dependents, then apply
the current namespace witness to the resulting closed projection before
publishing it. Preserve atomicity for declarations that share nominal identity
or ambient state. Admission of a body still requires the original owner and
the current parameter, connection, hierarchy and contribution proofs. A
namespace conflict during an edit can decline the prepared generation and let
Native handle the edit. Projection construction belongs to preparation and
manual rebuild.

Validate by semantic family using the native AST ledger and observable behavior.
Use a reusable case and a context-changing or refusal case where the family
has both outcomes. Include qualified and negative lookups, wildcard ambiguity
and source order; nominal types, specialization and deferred member binding;
constant functions, system functions, instance parameters and generate
selection; hierarchical paths, interface connections, bind and defparam; and
driver, reference and diagnostic contributions. A mixed fixture should prove
that an invalid or incompatible parent preserves independently closed child
reuse while a cross-boundary dependent remains Native. Check complete Native
results and actual imported owners. Use Chip and SPI to exercise combinations
after the mechanism-level checks.

### Dependency and ownership contracts

The following contracts apply across syntax kinds. Their composition is still
under B acceptance. The enum inventory and individual tests do not establish
all combinations of these mechanisms.

| Family | Reuse contract and Native boundary | Representative regression in UvmLibraryTests.cpp |
| --- | --- | --- |
| Lexical and global names | Preserve positive bindings and the absence of a competing binding. Declaration labels, actual deferred instance types, direct package shadows and proven routine locals have distinct roles. Attribute a failed proof to its declaring node and propagate rejection through dependencies before constructing the accepted projection. Newly introduced consumer bindings still invalidate the current namespace witness. | An unproven definition lookup leaves an independent RTL child reusable |
| Imports and source order | Preserve compilation units, ordered imports and original parser metadata. Private imports are not transitive exports. A native package supplies a negative wildcard witness only for a disjoint structural name surface and an unchanged unique declaration. Re-exports and binding-generated names require the ordinary semantic proof. | Private package imports do not enter a compilation-unit wildcard namespace |
| Types and specialization | Retain nominal type identity and the exact owning specialization. Parameter signatures include value, width, signedness and type provenance. A type that requires an uncaptured class or caller context stays Native. No structural type equality substitutes for nominal identity. | Native closed type provenance retains objects and rejects contextual types atomically |
| Evaluation and generated structure | Preserve folded values and their dependencies, including type-query arguments, assertions, patterns, coverage and attributes. Current parameters and elaboration context must match before a body can be imported. Runtime values and dynamic type-query shapes retain their existing Native boundary. | Type-query certification still refuses runtime values dynamic shapes and public defaults |
| Hierarchy and external connections | A declaration projection does not certify an instance. Validate actual child definitions, parameters, hierarchy and current outside port bindings at body admission. A refused parent can leave independently proven descendants eligible under the original selection. | Fixed hierarchy queries follow current external port connections across edits |
| Bind, defparam and parser context | Apply consumer contributions in the current model and keep original imported-definition identities and directive metadata. Context that cannot be attributed to an independent node rejects the corresponding shared scope or generation. | Imported definition identities retain binds and defparams absent from consumer CST |
| Calls, drivers and analysis effects | Audit actual arguments, outputs and required runtime dependencies. Replay current-boundary driver and reference contributions even when procedures or bodies retain their original semantics. | Imported body procedure replay preserves fresh consumer driver checks |
| Diagnostics | Preserve native public payloads, locations, macro notes, parameter context and occurrence contributions. A provider-only error does not suppress its Native counterpart. Detached replay requires an exact source witness. | Detached body diagnostics require an exact retained native source prefix |
| Lifetime and publication | Retain original syntax, source and semantic owners through every imported pointer. Publish only an accepted immutable generation. Cancellation and source retirement release obsolete consumers while live queries retain their own owners. | Cancelled RTL consumers release models and evicted source generations |

The direct package-shadow witness records the supplying package as well as the
name. Its current syntax must match before the proof can be reused. A bare
spelling match cannot establish a lexical binding. Declaration rejection is
monotonic within a preparation attempt, including declarations depending on
an unproven shared import. An unattributed failure rejects the attempt.

The current positive local-value proof visits ordinary bound package routines.
Deferred generic bodies and template-local values without a bound actual
instance keep their name dependency. The projection may omit those declarations
and still retain closed descendants. This boundary must appear in the final
support contract and tests. Expanding it requires a binding witness for the
original actual instance.

### Bind witness prerequisite revealed by Chip

B7 admits the Chip declaration namespace and publishes retained instance bodies.
The first edited query then spends minutes in the existing per-body bind-context
proof. A one-second process sample locates repeated syntax traversal beneath
`Compilation::isInstanceBodyBindContextCompatible`. Complete the B import
contract with a compilation-owned index of mutable bind target names and
hierarchical targets, excluding fixed imported syntax. Invalidate the index
when syntax or the imported namespace changes. Keep the same conservative
opaque-type, external-call and subtree rules. Verify index invalidation and
unchanged Native results before moving to C's canonical-variant and diagnostic
cost work. The full edited Chip gate remains open.

B8 implements that bind syntax index and invalidates it on syntax, package,
definition and compilation-unit imports. Normal CTest passed 1100 entries and
ASan passed 133 semantic/diagnostic cases. The Chip initial plus two-edit probe
matches Native for both experimental arms, but its actual RTL body hits remain
zero. Package import does not satisfy RTL admission. B therefore remains open.
Capture the rejecting import guard and audit the native bind-resolution result
as the next context witness, including the exact current target's hierarchy
overrides. Unresolved or opaque contexts keep their refusal.

A subsequent sample also identifies a linear search through original instance
paths for every retained root during full-owner publication. That indexing work
belongs to C after B proves real child reuse in the Chip context. The release
gate still requires full startup/edit/error/structural/rebuild/cancellation and
memory qualification on the accepted implementation.

## B acceptance and C entry

B9 adds an exact current-target witness after native bind/defparam resolution
has converged and every target has resolved. The target and its descendants
must have no hierarchy override, and opaque dependency contexts keep the
global refusal. The native algorithm still applies its existing diagnostics
and partial results when this additional witness is unavailable. Syntax and
namespace changes invalidate the witness together with the bind index.

B is accepted for the dependency and ownership contracts above. B9 passed
1100 CTest entries, the expanded bind regression (668 assertions), 2650 native
SDK cases and 133 ASan semantic/diagnostic cases. The eight AST kind inventories
are unchanged. SPI Host and Device passed 90 complete lifecycle states with
Native public results. Chip passed initial and two edits per arm (9 states);
freeze-only actually imported 11,936 and 14,391 roots after its edits. The
remaining Native parent is consistent with the supported child contract.
The final release matrix still covers the longer Chip lifecycle.

The Chip combined arm currently takes the existing local-certificate refusal
for symlink inputs and imports packages only. C must investigate preservation
of RTL reuse on that fallback. C begins with the measured publication search,
then duplicate owners/canonical contexts and local diagnostic/fallback costs.
The first change builds one exact original-path index per full publication
and retains the existing first-match behavior and subtree comparisons.

C1 passed its path-index scale case at 8, 32 and 128 retained roots, with
exactly one indexed path per original instance and one lookup per retained
root. The Chip probe preserved both imported-root inventories and Native
public results. Its first full publication indexed 39,246 paths and looked
up 12,970 retained roots. First-edit diagnostic readiness was about 10.4 s.

C2 uses the same bind context to select publication roots and to import
them. A rejected parent exposes its closed children in the original owner
at initial publication. The same-context edit regression retains one owner;
removing the bind still permits a newly valid parent. Chip imported 14,491
roots on both edits with one owner and no additional owner builds. Its
owner charge remained 2.25 GB, and observed peak physical footprint after
two edits was 5.72 GB. The corresponding C1 readiness observations were
6.96 GB owner charge and 9.96 GB peak footprint. The current Native peak in
this limited probe is 2.85 GB. These are readiness data.

C3 follows the exact positive path recorded by slang for an include lookup.
The optimized certificate reads the physical file once and rechecks raw lookup
resolution before admitting a certificate or publishing a result. Equal bytes
at a new target still require a reload. Negative lookups and inputs with no
recorded physical target retain their existing checks. This covers both parent
path segments and symlinks that slang already resolved.

In Chip, the old proof refused `tb/../autogen/tb__xbar_connect.sv`, reloading
the driver on every edit. With the new proof, both tested body edits used
local diagnostics on the original frozen model with one scope generation.
All nine initial and body states matched Native. Combined hover and diagnostic
readiness were 117 and 164 ms on the first edit and 24 and 45 ms on the second.
Combined peak physical footprint was 3.20 GB. Full CTest passed 1,102 cases,
including a regression for path spelling, equal-byte retargeting, content
changes and cancellation. Measure the remaining whole-design diagnostic
preparation and replay on real edits and structural fallback next.

The traced Chip edit in `chip_base_test.sv` matched Native through all 15
lifecycle states. Its ordinary body edits still took the frozen fallback
because the textual macro proof rejects any inherited token paste. Median
hover and edited-document diagnostic readiness were 4.00 s and 6.37 s,
versus 9.72 s and 11.78 s in Native. The certificate's median input capture
was 37 ms. The diagnostic baseline did not reach local replay, and combined
peak physical footprint was 8.10 GB across the lifecycle including manual
rebuild. These trace data are for locating work, without release qualification.

C4 admits token pasting through actual slang preprocessing evidence for both
the original and edited fragment. Original method entry and exit record
cumulative compiler directives consumed from the macro expansion stream.
A disposable edited parse rejects such a directive before it can act. The
existing direct-directive, exit environment, source location and line-shift
proofs still apply. A composed syntax tree uses its suffix's own journal.
Incomplete journals retain the existing Native fallback.

In a Chip real-edit readiness run, all 15 public states matched Native.
The first body edit reached hover and current-document diagnostics in 104
and 155 ms. Five later body edits took 24–30 and 46–52 ms. Line insertion,
intentional error and repair also reused the original model. Structural
edit and repair imported frozen RTL but took 6.60 and 6.82 s to diagnose.
A separate trace put the configured-unit owner phase at 3.56 s for each
structural edit, and subsequent cooperative diagnostic preparation at
2.14–2.19 s. Preparation included 0.80–0.84 s of private semantics and
0.52–0.55 s of private dataflow. The selective model took no syntax prefix
path in either state. Evaluate a proven ordered syntax prefix independently
of the selective semantic provider, with peak memory and Native parity gates.

C5 captures an ordered syntax checkpoint during the first required fallback
parse, including units that start with an existing UVM archive. Capture
refuses an open conditional, active macro stream or unsupported directive
state. Before reuse, the server checks the loaded source and command-file
reads, source-loader selections and configuration layers. A valid checkpoint
can include mutable roots whose syntax is still current.

Chip's default 256 MiB syntax cache refused the checkpoint on budget grounds.
With a 4096 MiB test limit, the next structural repair reused 871 roots and
charged about 320 MiB. The trace measured 162 ms for input checks, 21 ms for
checkpoint extraction and 130 ms for suffix parsing. Hover and diagnostics
were ready in 1.11 and 4.95 s, with exact Native results. The first fallback
still performs a complete parse. This readiness run overlapped other checks.
Serial measurement must establish the capture cost, repair benefit and memory
profile. The syntax cache retains its independent configurable limit.

C5c records the pending trivia at the raw input boundary. On resume, the
first raw suffix token receives it before directive processing. Already
processed prefix directives are composed separately. The SDK test covers a
blank intermediate root and a suffix beginning with an include. The SerDes
verification profile again imports all six eligible RTL child bodies across
three edited states, with two syntax hits and exact public parity. The Chip
readiness rerun also matches Native in all 12 states per arm.

The bind-context probe for OpenTitan AES and UART found complete
definition-target bind lists with no instance override under the rejected
DUT. B10 compares the ordered, resolved definition-target effects in the
two models, including the bound definition, expanded tokens, physical and
logical source provenance, and native duplicate handling. This proof is
available only after the native prepass converges. Config-sensitive binds,
hierarchical overrides and unresolved effects retain their conservative
paths. A changed definition-target effect still requires a subtree to prove
independence before reuse. The final cross-project query gate passed 16
projects and 128 public states, including whole-DUT reuse in AES and UART in
all three edited states. The reduced test changes a bind argument, removes
the bind, moves its source location and restores the original binding.
DMA, OTBN, USB device and both SPI profiles passed 225 background-edit
lifecycle states with all three arms. Each freeze-only arm imported RTL
in 13 states. Each combined arm used original-model local updates in 10
states and imported RTL in three others. The remaining states prepare or
rebuild the provider. These edits use a separate consumer file with the
complete design loaded. Chip's real DV edit is a separate gate.

The [validation record](../../validation/data/semantic-reuse-2026-11-07.json)
ties the public-result digests, actual reuse counts, test suites and SDK
restoration results to the candidate binary and inputs.

## Chip serial qualification

Three real-edit rounds compare the same candidate with unlimited semantic
caches and a 4 GiB independent syntax cache limit. The published binary's
fixed preparation declined on Chip, so these three arms measure the change
in latency and memory as actual reuse becomes available. All 135 lifecycle
states have exact Native public results. Each metric is the median across
three processes per arm.

| Measurement | Native | Freeze | Combined |
| --- | ---: | ---: | ---: |
| New process through initial hover | 27.25 s | 21.79 s | 22.08 s |
| First edit hover / current document diagnostic | 9.83 / 11.99 s | 3.84 / 6.14 s | 101 / 151 ms |
| Later five edits hover / current document diagnostic | 9.78 / 11.85 s | 553 / 2,799 ms | 24 / 47 ms |
| Structural edit hover / current document diagnostic | 9.82 / 11.88 s | 563 / 2,824 ms | 3.86 / 6.45 s |
| Structural repair hover / current document diagnostic | 9.82 / 11.88 s | 547 / 2,751 ms | 666 / 3,246 ms |
| Peak physical footprint before manual rebuild | 2.82 GB | 5.51 GB | 5.63 GB |
| Peak physical footprint through the whole lifecycle | 5.10 GB | 8.17 GB | 6.34 GB |

Both experimental arms imported 14,491 RTL roots during structural repair
and hit the syntax cache. Freeze ran nonlocal analysis on ordinary edits,
so it had a checkpoint available earlier in the sequence. Combined
used local updates for ordinary edits and captured its first checkpoint
during the required structural parse. Edited-document diagnosis measures
receipt of that document's matching version, while the oracle compares
the complete observed diagnostic map.

The long session passed 256 alternating body edits and the full lifecycle.
During the 250 extended edit samples, Combined kept one scope generation,
two query text buffers and a constant 2.810 GB physical footprint. Its
median diagnostic latency was 46.6 ms in those samples. The structural
sequence raised the pre-rebuild peak to 4.82 GB. The whole session peaked
at 6.53 GB during manual rebuild, which validates the new driver while
keeping the previous driver available. This separates ordinary-edit
retention from the cost of switching models.

The default 256 MiB syntax budget passes all 45 states in a separate
one-process-per-arm run. Both experimental arms still import 14,491 RTL
roots on structural repair. The syntax checkpoint is refused at this
budget, with zero prefix hits. Combined later-body diagnostics remain
at 47 ms. Structural repair takes 6.79 s at the default budget, versus
3.25 s in the three-round high-budget measurement. Both runs leave the
semantic budget unlimited. The default-cap run observes a 6.65 GB
Combined lifecycle peak and a 9.31 GB Freeze peak.

Chip and CVA6 pass the real-project cancellation replay on this candidate.
The server returns RequestCancelled for the structural hover. Replaying
the same editor version and then repairing the document produces exact
Native results through all 48 states.

## Resolved-bind proof cost

Five isolated configurations vary the number of retained leaves, the
number of active definition binds in an unrelated retained scope and the
number of parameter variants. The B10 candidate borrows every selected
root and preserves all 50 public states. With 128 leaves and 128 active
definition binds, its ordinary edited hover takes about 104 ms versus
8 ms in Native. Eight leaves with the same binds take about 9 ms in
Freeze. The original comparison walked the full effective bind lists
once per retained root.

The follow-up keeps a single resolved bind delta in the current compilation,
keyed by a weak retained-owner identity and its bind revision. It
charges the cached target set to the same reuse accounting. Only lists
owned by the two compilations can enter the memo. Missing overlays still
read the supplying definition's bind list on demand. Changed targets still
go through the per-subtree dependency proof. The regression checks that
two roots sharing one owner compare its resolved list once, including on
edits that change or move the bind. The same 128-leaf, 128-bind workload
takes 9.6 ms in Freeze after the change and 8.5 ms in Native. The distinct
parameter variant also takes under 10 ms in Freeze. All selected roots
still import and all 50 public states remain exact.

The follow-up candidate passes 1,105 CTest tests, 1,090 ASan server
cases and 2,653 ASan SDK cases. The installed default-off path and
three UVM startup profiles pass. Its public results match Native in
128 states across the 16 existing projects and 225 states across five
additional opt-in DV profiles. The final candidate's high syntax budget
Chip replay passes 45 exact states, with 14,491 borrowed RTL roots on
structural repair. Combined diagnoses later body edits in 45.8 ms and
structural repair in 3.17 s. The three earlier repeats above are from
the candidate before bind delta memoization. The final 256-edit session
passes 280 exact states. Across the 250 extended samples it keeps one
scope generation, with a 64 KiB physical footprint range and a 46.3 ms
median diagnostic latency. The session's lifecycle peak is 6.17 GB
during manual rebuild. At the default 256 MiB syntax budget the final
candidate passes 45 exact states, with 46.7 ms later-body diagnostics,
one syntax checkpoint refusal and 14,491 borrowed RTL roots at
structural repair. That repair takes 6.79 s. The final Chip and CVA6
structural cancellation replays return RequestCancelled, then match
Native through 48 states after replay and repair.

The 18 matched published comparisons pass 1,170 exact states. The five
added DV background profiles pass a further 225 serial states with
unlimited semantic budgets. Combined later-edit diagnostics on their
separate editable consumer range from 3.0 ms for DMA to 16.3 ms for SPI
Host. The paired review follows up the latency and memory threshold flags
in SoC real edit, CVA6 real edit and background, and UBus real edit.

The paired SoC real-edit runs reproduce the freeze-only footprint increase
at the sixth edit. The candidate uses 671–674 MiB with five prefix hits,
versus 631–632 MiB on published main. Diagnosis of that edit is 87 ms faster
in both pairs. With a zero syntax budget, the candidate uses 630 MiB and
preserves the semantic reuse route. All seven historical SoC latency flags
stay below threshold in both adjacent pairs.

UBus combined peaks at 261–262 MiB during manual rebuild, versus 232 MiB
on published main. The candidate retains about 24 MiB of accounted syntax
at that boundary, and structural repair diagnosis takes 63–66 ms versus
96 ms. With a zero syntax budget the candidate peaks at 235 MiB and the
repair takes 96 ms. Both zero-budget controls preserve Native results and
keep semantic caches unlimited.

CVA6 real-edit paired runs preserve all 180 public states. None of
the eight original latency flags recur in both pairs. Freeze startup
is 27–52 ms slower and Combined startup is 31 ms slower in one pair.
The other Combined sample is 314 ms slower, with 141 ms more in
initialization and 163 ms more in the build-file request. The first
hover after build selection takes 147 ms in the other Combined samples
and 157 ms in that run. Later Combined diagnostics stay at 11.8–11.9 ms.

The CVA6 background pairs preserve another 180 exact states. Freeze
startup increases by 10–32 ms, and Combined by 29–39 ms. The build-file
request accounts for most of the increase. Neither historical background
flag recurs. Across the four paired comparisons, 720 public states match
Native. Only the documented SoC and UBus memory differences recur above
the investigation thresholds in both pairs. The budget-zero controls add
46 exact states and isolate the opt-in retention tradeoff.

The final binary also passes 17 Python and 43 transport checks, including
the held-diagnostics version test. Installation verifies 157 package files,
the default-off path and the three UVM profiles. A fresh pinned SDK archive
replays the restored patch and verifies 709 SDK and 1,296 offline dependency
files. ASan runs with leak detection disabled. The versioned result data
records the executable, SDK patch, input and sample identities.
