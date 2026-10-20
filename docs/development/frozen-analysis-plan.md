# Frozen analysis and macro hover implementation plan

Status: started 2026-10-20. This document is an implementation and acceptance
plan. Historical progress entries retain their original states; the final
qualification below and linked acceptance ledger describe the current candidate.
The implementation baseline is `53abb6b27a687ce8b26e612a9a0b22e11f4ffced`.

## Contract

Users select stable UVM sources, package/module definitions or an RTL instance
subtree. "Frozen" means eligible for reuse while its inputs remain valid, not
permission to ignore edits or return obsolete semantics. Open unsaved text takes
precedence over disk. No manual unfreeze is required to make an edit visible.
All new analysis caches remain experimental and disabled by default.

Parsing and semantic reuse are distinct milestones. An unchanged module
definition is not an unchanged instance: parameters, generate branches, interface
connections, packages, binds, defparams and cross-hierarchy references can change
its meaning. Unsupported or uncertain dependency relationships decline reuse.
Declaration names and hierarchical instance paths are separate selector kinds;
ambiguous, missing and recursive selections receive actionable status messages.

Scope selection first resolves to actual source/compilation boundaries. A scope
is not extracted as text and parsed independently of its surrounding macro,
directive or compilation-unit environment. Cache ownership cannot retain AST
pointers from a retired compilation. Cross-process use retains the existing
isolated worker protocol and never shares live compiler pointers.

## Ordered milestones

| Order | Delivery | Exit criterion |
|---|---|---|
| 0 | Pin native baseline, workload inventory and invalidation matrix | Native semantic expectations recorded; existing baseline limitations named; frozen fixtures do not modify the immutable UVM corpus |
| 0a | Establish unsaved cross-file RTL query freshness | External package and parent-parameter edits are reflected in queries before save; the initial native probes expose stale results, which must be resolved before adding caches |
| 1 | Explicit UVM source directory/entry, configuration validation and preparation | Real SPI/PSS/UBUS use the selected library without duplicate injection; missing/conflicting inputs reported; preparation and actual reuse independently observable |
| 2 | General frozen-source registry and package reuse | More than one library/package can be retained within dependency and preprocessing boundaries; changed and unsaved dependencies invalidate correctly |
| 3 | Module definition reuse | Syntax shared across instances; different parameterizations, generate branches and interface bindings remain correct; module edits and external package changes refresh consumers |
| 4 | RTL instance-subtree selection and measured semantic prototype | Selected subtree and dependencies tracked; affected elaboration re-runs; bind/defparam/cross-hierarchy changes either invalidate safely or explicitly decline reuse |
| 5 | Original/expanded/both macro hover and temporary preview command | Macro calls and macro-generated symbols show the selected view using unsaved text; temporary override leaves config unchanged; LazyVim integration passes |
| 6 | Optional bundled UVM and packaging decision | Fixed source/version/hash/license inventory; explicit opt-in, no silent injection; project/library conflict checks and offline install tests pass |
| 7 | Integrated Mac acceptance, documentation and publication | Full tests, real workload oracle, timing and memory gates pass; evidence reviewed; ordinary push to main and remote identity verified |

Hover can be delivered independently once the source-location tests are ready;
it does not wait on instance semantic caching. Its initial default preserves the
existing combined display for compatibility; users can choose source-only.
RTL semantic reuse must demonstrate a benefit before being expanded beyond its
supported dependency contract. A declined semantic prototype is not completion
of the instance-freezing performance milestone: record the result and refine it.

## Configuration and lifecycle design

Configuration names will be finalized with schema and round-trip tests before
release. Required concepts are:

- A UVM source directory, optional explicit entry file, and an independent cache
  enable switch. The directory resolves relative to the workspace; the entry
  resolves relative to that directory. Absolute paths are also supported.
- Separate selectors for package name, module definition name and exact RTL
  instance path, plus a bounded cache budget. No overloaded string that guesses
  which selector the user intended.
- Hover mode `source`, `expanded` or `both`, plus a temporary preview command.
- Status and refresh operations reporting selected inputs, preparation, hits,
  invalidation reasons and current retained bytes. Timing instrumentation stays
  outside primary benchmark runs.

An explicit UVM path configures source loading even when caching is off. Existing
matching compilation input is reused; conflicting UVM definitions are reported,
not silently replaced or duplicated. The initial cache milestone may decline
unsafe preprocessing order, but the configured project must still analyze via
the native path. No source reorder is allowed merely to obtain a cache hit.
Command-line/configuration macros affecting UVM are fixed within one generation;
changes create a new generation. Preparation happens once per compatible input
generation and compiler context, not once forever or by filesystem path alone.

Keys cover exact source/overlay content, include resolution including negative
lookups, ordered roots, compilation units, macro state, directives and compiler
options. Semantic keys additionally cover resolved dependencies and instance
context. File-watch notifications trigger invalidation but are not sole proof of
freshness; uncertainty and watcher overflow require validation or native fallback.
Do not weaken existing source witnesses to claim constant-time certification.

Parent and worker own separate bounded compiler state; repeated characters must
not retain an unbounded generation chain. Refresh, configuration reload, idle
worker release and context retirement release obsolete entries. Existing handles
keep current generation checks. Phase 1 is in-memory reuse only; portable disk
serialization requires a separate schema, compiler/build identity and integrity
contract, and is not represented as saving compiler pointers.

UVM warning filtering remains an independent opt-in setting. Selecting a library
does not suppress its diagnostics. Cross-boundary errors/warnings remain current.
Bundled UVM is an optional fixed-version fallback: project inputs first, explicit
user library next, built-in only by explicit choice. No automatic network download
or silent activation is required for opening a file.

## Correctness gates

For each supported selection compare native versus enabled public responses:
ordered completion items, every resolve, definitions, references, hover, current
version diagnostics and inactive regions. Preserve duplicate diagnostics and
ordered relatedInformation. Any known outer diagnostic order normalization is
reported separately and must not hide missing/changed entries.

Required transitions include first query, repeated query, unsaved single/multiple
document edits, 1,000 character edits, library edit, dependent package/type edit,
macro/config/include/filelist changes, missing-to-present includes, root order,
instance parameter changes, generate/interface/bind/defparam/cross-reference
changes, error insertion/repair, cancellation, stale handles, forced/natural
retirement and manual refresh. Unsupported transitions must prove native fallback,
not merely produce a zero-hit counter. Hover temporary mode must leave config
unchanged and avoid another full analysis when the current analysis is reusable.

The existing W12 UTF-16 oracle failure remains a separately recorded native
baseline issue; it is not counted as a new success or silently omitted. Include
negotiated UTF-8 and CRLF coverage. Small constructed RTL tests establish semantics,
not large-design performance. Use existing real SPI/PSS RTL and UBUS/UVM fixtures
for measured workloads; unavailable bind/defparam contexts get explicit fixtures.

## Performance and memory gates

Freeze baseline binaries, source hashes and workload manifests before modifying
product code. The initial phase used `53abb6b` native/old-UVM arms. After the
explicit UVM-path milestone was accepted and published as `13f2d30`, integrated
comparisons use that accepted phase-1 executable (`8ee6e5d…`) as the frozen
native/old-UVM reference, alongside the same candidate with new caches off/on.
The original baseline evidence stays historical; the two references are not
relabelled as one another. Feature-off and backward-compatibility tests are mandatory.
Do not reuse October 19 timing results as measurements of the new code.

Use sequential randomized paired blocks, at least 20 observations per short
timing cell. Measure startup through first query including preparation, first
edit, continuous edit queries, every resolve, complete versioned diagnostics,
fallback and retirement. Report absolute values, whole-session totals, spread and
paired uncertainty; do not advertise a tail guarantee from 20 runs. Run timing
without build/tests/RSS sampling concurrently. Keep failures and outliers.

Initial performance acceptance target: at least 10% median whole-edit-session
saving on two representative intended workloads, with a positive paired mean
saving interval. A first-query or fallback regression above 10% needs a measured
mitigation or an explicitly documented tradeoff; it is not hidden behind warm
query timings. These are development gates, not promised results. Refine or
decline a strategy that does not meet its intended use case.

Measure parent and simultaneous process-family RSS separately: at least three
short repeats and three 1,000-edit sessions per representative workload. Compare
baseline, off and on, retained bytes, peak, post-retirement and post-refresh memory.
Enforce the configured retention budget and prove generation retirement/release.
A process-family peak increase above 10% requires investigation and a separately
reported budget/performance tradeoff before acceptance; bounded cache bytes alone
do not establish bounded process RSS. No universal speed or memory benefit claim.

Final gates include full CTest, SDK tests, Python tests, standalone transport when
protocol changes, headless LazyVim on SPI/PSS/UBUS and RTL, SDK patch reconstruction
and manifest verification, formatting and whitespace checks. No failing required
check is relabeled as supported. Commit tested source, docs and compact evidence;
private corpus, handoff and binaries remain under the workspace's untracked area.
After acceptance use a normal push to main under the standing user instruction.

## Progress

- Goal created and active on 2026-10-20.
- Phase 0: baseline executable pinned and the three inherited real-workload
  inventories verified (209 SPI, 209 PSS, 172 UBUS files). RTL dependency/instance
  oracles added before cache work.
- Initial native RTL probes: instance specialization passes; unsaved external
  package and parent-parameter changes expose stale cross-file hover results.
  Three cases contain 72 assertions, initially 68 passing and four failing.
  These are pending correctness requirements, not successful acceptance. Existing
  full-design source code explicitly retains save-triggered dependency behavior.
  Saved transitions and unsaved transitions are evaluated separately.
- Phase 0a implemented: edits invalidate full-design semantic state; semantic
  consumers refresh lazily, preserving active selections. Full diagnostic
  publication has separate freshness bookkeeping. The expanded focused group
  passes 102 assertions in six cases, CTest 624/624 and Python 15/15. All 56
  real-project off/on processes and three headless LazyVim checks pass. See the
  [milestone report](../../validation/macos-frozen-foundation-2026-10-20.md).
  No new cache speedup is claimed.
- Phase 1 implemented: `uvm.sourceDirectory` and optional `uvm.entryFile` select
  literal library inputs independently of the default-off experiment. Matching
  project roots/wrappers retain their original order; an absent library is added
  once, and conflicts are reported. The selected entry participates in parsing,
  snapshot/worker identity and prefix eligibility. Missing-entry repair and disk
  updates preserve editor overlays. Standalone library preparation reuses its
  native tree. See the [milestone report](../../validation/macos-uvm-path-2026-10-20.md)
  for acceptance and remaining restrictions. This is in-memory syntax reuse;
  generic scope selection and semantic reuse remain pending.
- Phase 2 candidate implemented, not accepted or published: default-off package
  selections, a bounded complete-root registry, independent completion-detail
  parses, status/refresh operations, exact filesystem/overlay/command witnesses
  and fresh-context fallback are integrated. The first candidate passes all 60
  real-workload off/on processes, including complete resolve lists and the extra
  hover/reference/highlight/symbol/diagnostic/inactive-region comparisons. A later
  certification refinement shares parent resolution within one witness and
  permits cancellation between reads, while preserving notification synchronization.
  It passes CTest 655/655, Python 16/16 (including an enabled frozen-source burst),
  SDK restoration and private LazyVim queries through two unsaved edits in each
  SPI/PSS/UBUS project. Its full real replay and performance gates remain pending.
  Three-pair exploratory performance cells expose a whole-session regression from
  repeated certification; less parsing alone does not satisfy acceptance. Keep
  the experiment off and refine certification/worker costs before claiming benefit.
- The certification refinement subsequently passes all 60 real-workload processes.
  The next candidate extends isolated diagnostics to independently owned generic
  multi-package checkpoints, with root/options/snapshot proof, a separate worker
  budget and fresh native retry on unsafe resumption. It passes CTest 658/658,
  Python 16/16, standalone transport 43/43 and the three unsaved-edit LazyVim
  checks. Trace-only real runs prove actual worker captures/resumes with the older
  UVM cache off. A 20-pair performance evaluation includes an otherwise identical
  persistent worker with prefix reuse disabled, separating worker parsing savings
  from process startup savings. Full real replay and process-family memory for
  this worker candidate remain required before acceptance.
- The worker candidate's 180-session timing run (20 randomized paired sessions
  per case/arm) reduces median process-start-through-final-diagnostic time by
  38.5% on SPI, 28.7% on PSS and 38.5% on UBUS. Paired mean saving intervals are
  positive. These compare the same candidate with the new experiment off/on,
  keeping the older UVM cache off; they do not establish superiority to the older
  UVM-enabled arm. Separate three-repeat, 20-edit RSS runs show lower sampled
  parent/family peaks. First edit still rises from about 52 ms to 72–78 ms, and
  repeated-query certification remains an overhead. The worker's full 60-process
  real oracle and 16-process forced-retirement oracle pass, with actual retirement
  observed. A real SPI macro-override probe also proves fresh native fallback;
  its timing remains unmeasured. Full-resolve timing and long memory remain
  pending. Initial native-arena sharing is the next preparation-cost refinement.
- The next refinement forwards a current parent decline as a negative-only worker
  hint, avoiding repeated failed prefix preparation. A real macro-override
  fallback evaluation completes 120 fresh processes, with positive paired
  whole-session saving intervals on all three projects (median savings 7.5%,
  3.3% and 9.7%). Its first-edit preparation cost remains higher than native.
  Repair tests prove the decline is reversible.
- Initial native-arena sharing is implemented in a further candidate: the generic
  registry can stage an existing UVM first-root view, then require full freshness
  proof before consumption or extension through other selected package roots.
  The full native owner is charged; an unavailable or oversized view uses ordinary
  preparation. Independent CLI-context parses remain distinct from compilation
  groups. This candidate passes CTest 662/662, Python 16/16, the three unsaved-edit
  LazyVim checks and fresh SDK patch reconstruction. Trace-only real runs show
  witnessed parent seed consumption plus worker capture/resume on all projects.
  Its 180-process confirmation reduces whole-session medians by 40.0%, 29.5% and
  41.2%, with positive paired intervals. First-edit medians improve from
  51.0/52.5/52.6 to 41.4/42.3/31.3 ms, while startup and repeated certification
  still cost more. Its full real 60-process and forced-retirement 16-process
  oracles pass. Separate three-repeat, 20-edit RSS runs show lower parent/family
  peaks. See the [candidate report](../../validation/macos-frozen-native-seed-2026-10-20.md).
  The subsequent 80-process all-resolve evaluation exposes a regression: for two
  unsaved edits with every completion item resolved at all three states, PSS and
  UBUS full-session medians rise from 789/764 to 1835/2554 ms. Exact certification
  runs for each request, so selected-resolve benefits do not cover bulk resolve.
  This remains a required optimization/usage-tradeoff decision. Older accepted
  baseline and long integrated memory remain required too; no universal request
  latency gain is claimed.
- Phase 5 has an independently tested implementation: `hovers.macroView` selects
  `source`, `expanded` or the backward-compatible default `both`. The
  `slang.previewHover` command returns one temporary view without modifying config
  or replacing valid current analysis. Source excerpts use retained editor text;
  generated symbols and driver excerpts honor the view. CTest 664/664, Python
  16/16 and the new focused 57-assertion group pass. Isolated LazyVim SPI/PSS/UBUS
  checks exercise real UVM calls, all modes, temporary floating previews and two
  unsaved edits. The sandbox's native file-monitor EMFILE was reproduced without
  an LSP server; the isolated editor checks pass outside that sandbox. See
  [usage](../features/macro-hover.md). No user editor/config was changed.
- Phase 3 module syntax candidate now passes CTest 669/669, Python 16/16 and the
  focused 2,218-assertion group. Real full-design SPI/PSS/UBUS off/on responses
  match, including every completion resolve, current diagnostics and PSS bind
  removal/repair. SPI/PSS each resume three times; UBUS's selected module shares
  the edited final root and does not resume. Full-design unsaved diagnostics and
  generic-specialization diagnostic order were fixed during this validation.
  Six isolated full-design LazyVim sessions also pass with matching-version
  diagnostics, width edits, completion resolves and bind removal/repair.
  These are correctness results; current module timing and long-memory
  gates remain open. See the [module report](../../validation/macos-frozen-modules-2026-10-20.md).
- Next: validate module performance and extend the registry to exact instance
  selections while keeping the experimental switch off. Preserve the unresolved
  bulk-resolve cost as an explicit integrated-performance requirement, together
  with older accepted baseline and long memory checks. Macro-hover integration,
  instance semantic reuse, bundled UVM and final publication remain open; passing
  one milestone does not complete the overall goal.
- Module performance investigation completed 54 initial and 81 reference-paired
  exploratory processes (three pairs per project/scenario/arm). The latter
  includes selected resolve, every RTL completion resolve and a literal macro
  override fallback. All semantic checks pass. An early native decline avoids
  needless prefix certification/composition for literal macro overrides, with
  150 new assertions; SPI/PSS fallback first-edit medians fall from 114.2/166.9
  to 103.3/155.9 ms in this small sample. These are provisional measurements,
  not the final 20-pair acceptance or a resolution of the older UVM-class bulk
  resolve regression.
- A further candidate avoids preparing full-design groups with no editable
  suffix and avoids staging unused multi-root native seeds. Its real UBUS replay
  has zero preparations and zero retained cache bytes while preserving all public
  results; SPI/PSS retain three actual resumes each. CTest 671/671, Python 16/16
  and the focused 2,416 assertions in 83 cases pass. Timing and final integrated
  acceptance remain required before publication.
- Phase 4 now has an implemented, default-off semantic prototype: canonical
  instance paths select descendants and derive module syntax selections. A
  bounded value cache serves hierarchy responses across certified automatic
  class-task process edits while diagnostics remain native and current. CTest
  673/673, Python 16/16, two dedicated cases (111 assertions), six real processes
  and six isolated editor sessions pass. All three real projects record eight
  cross-edit hits. A 54-process, three-pair timing pilot finds a 12.8% complete
  continuous-session saving on PSS versus matched syntax-only reuse, but SPI/UBUS
  and diagnostic-drained sessions regress. This is useful prototype evidence,
  not performance acceptance. Refine certificate traversal and request-local
  proof costs, broaden instance-enabled language-query checks, then confirm the
  integrated performance/memory gates. See the
  [instance report](../../validation/macos-frozen-instances-2026-10-20.md).

- The instance proof now binds to one live SourceManager and reuses exact per-file
  certificates. A real `__LINE__` regression in the initial prototype is fixed by
  preserving task-body logical newline counts; the old report is marked
  superseded. Changed-file traversal includes all physical include ancestors,
  and a partial mismatch rebuilds the complete proof to account for macro-driven
  context changes in unchanged includes. Request evidence is shared only inside
  one dispatched RPC with unchanged native syntax and context revision; every
  new RPC still checks the filesystem. CTest 678/678, Python 16/16, 2,661 focused
  assertions in 90 cases and 43 transport checks pass. Six real native/enabled
  language-query replays match, including every completion resolve and current
  error insertion/repair diagnostics. Current performance and long-memory gates
  remain open; these correctness results do not imply overall acceptance.
- Phase 6 now ships unchanged, explicitly selected UVM 1.2 with a fixed upstream
  commit, complete SHA-256/source/license inventory, default-off configuration,
  conflict validation and executable-relative build/install/release layouts.
  Actual relocated offline installations, release layouts, missing-resource
  behavior and negative inventory checks pass. CTest 682/682, Python 16/16,
  6,646 focused assertions and 43 transport checks pass. Six original-library
  SPI/PSS/UBUS language replays and six isolated LazyVim sessions also pass.
  See the [bundled UVM report](../../validation/macos-bundled-uvm-2026-10-20.md).
  This completes optional source packaging, not the integrated performance and
  memory gate: long full-design lifetime, older class bulk-resolve regressions,
  20-pair baseline comparisons and final publication remain required.
- Full-design context retirement is now implemented for experimental builds/top
  selections. Replacements preserve unsaved text and versions, selected build/top
  and valid active paths; failed construction retains the edited owner and retries
  after repair. Extending this path exposed and fixed missing top-wrapper versions
  and legacy-UVM active-selection preservation. Two dedicated cases cover nine
  entry/switch combinations plus failed filelist/top replacement, 534 assertions.
  Full CTest 684/684, Python 16/16, 7,180 focused assertions and 43 transport checks
  pass. Six real native/enabled language replays match with two forced retirements
  per enabled project. Natural 1,000-edit memory checks are running separately;
  forced correctness does not establish natural lifetime or performance acceptance.

- The first natural 1,000-edit full-design run passes on each SPI/PSS/UBUS project,
  with error/repair, declaration fallback and refresh. Actual retirements are
  8/8/111; startup-inclusive sampled peak RSS is 230.0/249.5/257.9 MiB and physical
  footprint 162.8/143.2/161.0 MiB. This is one repeat, not final three-repeat or
  paired baseline acceptance. UBUS still reparses its stable earlier UVM root
  because its selected module is in the edited final root: the next source-reuse
  change must support that dependency-prefix fallback and reduce frequent context
  replacement. See the [retirement report](../../validation/macos-full-design-retirement-2026-10-20.md).
- The edited-final-root case now retains an earlier explicitly configured UVM
  dependency prefix while reparsing the selected root. Initial native checkpoint
  staging avoids a second UVM preparation. CTest 685/685, Python 16/16, 7,557
  focused assertions and 43 transport checks pass; real language-query replays
  match. UBUS has 11 actual dependency resumes in the short workload, and its
  separate 1,000-edit run needs 31 natural replacements instead of the previous
  run's 111. Small paired timings improve UBUS burst sessions, but class bulk
  resolves still regress. Temporary profiling attributes that cost to per-request
  exact filesystem witnesses; probes were restored. See the
  [dependency-prefix report](../../validation/macos-uvm-dependency-prefix-2026-10-20.md).

- Invocation-local directory-prefix sharing and four bounded closed-read chunks
  now preserve per-request source authentication. New component-order,
  after-read parent replacement, large-graph exact-byte/negative and cancellation
  tests pass; full CTest/Python/transport and forced full-design real language
  checks pass. All-resolve pilots improve versus the preceding enabled candidate
  but still regress against native. The 20-pair confirmation and separate family
  memory results are tracked in the
  [witness-batching report](../../validation/macos-frozen-witness-batching-2026-10-20.md).
  Integrated performance/memory and publication remain incomplete.

- Mac review found that ordinary directory type checks cannot prove canonical
  spelling on a case-insensitive filesystem. Case-only rename regressions fail
  against the preceding cdb4 SourceManager and pass after replacing Darwin parent
  resolution with reopened-directory `F_GETPATH` identities. Current full tests
  and 708-file patch reconstruction pass; real/editor and performance replays
  are being repeated for the exact new binary. Historical cdb4 timings are not
  relabeled. See the [correction report](../../validation/macos-frozen-mac-directory-identity-2026-10-20.md).

- The canonical-identity correction also covers ordinary leaf case-only renames:
  Darwin uses one no-follow type/name query before the parent shortcut. The
  directory-only candidate fails the new leaf regression; current code passes
  all 477 live-witness assertions, including the separately compiled serial
  witness branch, and full CTest/Python/transport. Current binary is f9267d8;
  4d88/cdb4 results remain separate. Exact real/editor replays and fresh timing
  are still required. No publication or final performance acceptance yet.

- Exact f9267d8 confirmation and separate family memory complete: 120 timing
  processes, 20 paired observations/cell, show whole-session reductions of
  20.6%/25.0% versus the previous enabled strategy, with positive paired mean
  saving intervals. Whole sessions still regress 83.8%/147.7% versus native in
  PSS/UBUS every-resolve stress; first-response regressions are 12.0%/6.8%.
  Twenty-edit family-memory runs pass all 27 processes (three repeats/cell),
  including actual diagnostic descendants and RSS/physical footprint. This
  establishes mitigation and short memory observations, not final acceptance.
  Normal selected-resolve whole sessions, accepted older-UVM comparisons,
  repeated 1,000-edit memory and remaining integrated gates stay open. Current
  implementation, regression proofs and exact tables are in the
  [Mac identity report](../../validation/macos-frozen-mac-directory-identity-2026-10-20.md).

- The accepted old-UVM baseline comparison now covers 300 successful normal-use
  processes (twenty paired blocks, three projects, five arms, ten edits). Generic
  f926 improves versus native but regresses 29–42% versus accepted UVM-on; current
  old-UVM remains close to accepted old-UVM. This is an explicit additional
  regression, including selected-resolve use, and keeps optimization/publication
  gates open. See the [class-session report](../../validation/macos-frozen-integrated-class-sessions-2026-10-20.md).
- Bounded alias tasks with a single immutable parent plan pass full tests, real
  forced-retirement oracles and the separately compiled serial branch. Shared
  read/alias concurrency stays four; final used-parent checks remain mandatory.
  The pilot shows only about 4–5% improvement; twenty-pair confirmation is pending.
  See the [alias-plan evaluation](../../validation/macos-frozen-alias-plan-2026-10-20.md).

- Shared-parent alias confirmation completes all 120 processes with positive
  paired mean saving intervals. Whole medians improve 7.0%/9.6% versus f926;
  every-resolve sums improve 10.1%/11.3%, while first-response improvement is
  unproven. This is an incremental mitigation. The current d678 code starts
  enabled-only three-repeat 1,000-edit full-design memory runs on SPI/PSS/UBUS;
  native/old and exploration-family comparisons remain required. No push yet.

- The d678 enabled-only full-design memory matrix completes all nine processes:
  three repeats of 1,000 edits per SPI/PSS/UBUS. Hierarchy/current-version
  diagnostics, error/repair, actual declaration invalidation, refresh preparation,
  owner retirement, source-context resets and combined cache budgets pass. RSS
  peak medians are about 232/247/242 MiB; footprint ranges retain high individual
  SPI/PSS runs. These are parent-only full-design observations; compatible
  baseline controls and exploration-worker families are still open. Old/current
  six-arm RTL preflight now passes 36 processes with cross-arm hierarchy hashes;
  formal twenty-pair timing is still pending.

- Integrated d678 RTL confirmation completes all 720 sequential processes with
  zero failures: twenty paired blocks, three projects, drained/burst, six arms,
  complete hierarchy equality and post-timing budget/hit checks. Paired analysis
  retains all observations. Stable performance remains unaccepted: drained
  sessions regress 32–60% versus accepted UVM; PSS burst improves 18.83%, other
  burst cases regress. Startup first response regresses in all six cells. The
  cache-off repeated hierarchy query regression also requires investigation; do
  not attribute it solely to frozen witnesses. See the
  [integrated RTL report](../../validation/macos-frozen-integrated-rtl-2026-10-20.md).
  Current d678 five-arm selected-query 300-process confirmation is now running;
  fallback, old/native and exploration long-memory, final install/editor/SDK
  rebuild and normal main publication remain open.

## UVM specialization policy (user clarification)

UVM may keep a dedicated preparation/reuse implementation. Unification with
generic freezing is optional and must not add material cost or lose performance.
The existing explicit UVM path plus `experimentalUvmPrefixCache` remains a
separate available route, disabled by default. Do not force UVM through generic
package freezing simply to share implementation.

Next strategy order is: retain and validate the dedicated UVM route against the
accepted UVM baseline; investigate native background scheduling independently;
measure dedicated UVM together with general package/module/RTL selection; only
share checkpoints, ownership or input certification when the combined route
preserves correctness and demonstrates an acceptable cost. If composition is
slower, keep separate internal strategies and expose the actual chosen route.
Do not silently disable user selections or remove invalidation, macro/config
guards, diagnostics, native fallback or memory budgets to obtain a fast result.

The measured d678 generic-UVM regressions remain evidence. They disprove a
mandatory universal UVM implementation, rather than requiring the dedicated
route to be replaced. General scope and RTL capabilities still require their own
real-workload acceptance; the user clarification does not remove those features.

- Idle coalescing candidate 7e67b013: complete685CTest/16Python/43transport and
  7,626 assertions/103focused cases pass. Six current full-design forced-retirement
  processes and twelve exploration public-response processes pass. The separate
  independent708-file reconstructedSDK passes24,882assertions/2,570cases with the
  same Mac static visibility override as production. Initial standalone-hidden
  failures remain recorded. A180-process three-pair RTL pilot shows native/legacy
  repeated-query scheduling mitigation; a separate9-process route trace proves
  both-switch configurations currently bypass the dedicated continuation. This
  does not establish combined UVM/general performance. Current formal normal300
  is running. See the [idle coalescing report](../../validation/macos-idle-coalesce-2026-10-20.md).

- Unconditional2ms7e67formal300passessemanticsbutregressesordinarydedicatedUVM
  4.31–9.19%versusacceptedandnative2.47–4.46%;allpairedintervalspositive. Scope
  coalescingtonativefull-designbackgrounddiagnosticsonly,keepingexplorationzero.
  Currentc30d5030whole/realoracles/LazyVim6instance+3macro/install-relocation/
  archive/defaultschemagatespass. Compatiblefull-designandworker-familypreflights
  eachpass12processeswithpost-error/fallback/refreshprobes. Formalnormal300,
  fallback300andRTL720nowrunsequentially;longmemorycontrolsremain. See the
  [native-only idle report](../../validation/macos-native-idle-2026-10-20.md).

- Currentc30normal300andmacro-fallback300completewithzerosemanticfailuresand
  pairedanalyses. DedicatedUVMnormalwholemedians826.51/1299.41/911.88ms versus
  accepted817.33/1284.67/905.45ms; ordinary-windowregressionrecoveredtoabout1%.
  Genericnormal37.9–47.9%fasterthancurrentnativebut24.4–36.2%slowerthanaccepted
  UVM;fallback12.0–17.9%fasterthannativebut19.0–21.5%slowerthanacceptedUVM.
  Retainallnegativesanddedicatedroute; noforcedUVMunification. CurrentRTL720
  remainsLIVE,thenlongmemory. Worker-memoryv3parentretirementtraceprepared
  (v2executedpreflightunchanged);v3shortpreflightstillrequiredbeforelongrun.

- Currentc30all1,320formalprocessespass(normal300/fallback300/RTL720),analyses
  complete. PSSburstvaluecachewhole15.87%betterthanacceptedUVM,13.80%better
  thancurrentUVM,28.64%bettersyntax-only,positivepairedintervals. SPI/UBUSsmall
  subtrees:valuereuseaddsnoestablishedbenefit;dedicatedUVMrecommended.Drained
  wholecachebenefitversusdedicatedUVMnotestablishedandinitialpreparationcost
  remainsvisible. Cache-offfulldesignnowperformscompletecurrentdiagnostics,
  retainingthecorrectness/throughputtradeoffversusoldershallowdiagnostics.
  Five-armshort3repeat+long3x1000full/worker-memorycollectionstartedsequentially;
  guardedcasesstayfailures/censored,notfastsuccesses. No codechangespending.

- Currentfive-armshortmemorymatricescompleteall90processes(45full-design+
  45worker-family),three20-editrepeatspercase/armwithposterror/fallback/refresh
  probes;zerofailures. Source/trace-awareworkerdriveractuallyexecuted. Full-design
  45-cell1,000-editmatrixisnowrunning,thenworker-familylongmatrix. Strictraw
  auditorpreparedbutnotyetexecuted;shortsuccessdoesnotcloselong-memorygate.

## Current qualification after integrated evaluation

The c30 candidate completes 1,320 formal timing sessions, 180 main memory cells,
twelve matched native UBUS memory controls, all raw audits and paired ordinary
compatibility analyses. Whole CTest/Python/transport, real public-response and
LazyVim checks, offline installed/archive routes and independent SDK reconstruction
pass for the same runtime source. The [acceptance ledger](frozen-analysis-acceptance.md)
and [memory report](../../validation/macos-native-idle-memory-2026-10-20.md)
retain the detailed evidence and final publication identity checks.

General instance reuse meets the initial burst-versus-native target on all three
workloads, but superiority to the dedicated UVM alternative is established only
for the larger PSS hierarchy. Decline broad small-subtree/drained recommendations;
keep the features experimental and default off. UVM specialization is retained,
with ordinary dedicated sessions showing approximately 1% whole-session differences
from the accepted route. Small statistically visible slowdowns are reported.
Completion-detail certification and full-design complete-diagnostic costs remain
explicit measured tradeoffs, not a universal optimization claim. The original
UBUS instrumented memory increase is retained; the matched three-repeat sequence
does not reproduce it and proves no equivalence. No correctness guard is weakened.
