# Frozen analysis and macro hover implementation plan

Status: started 2026-10-20. This document is an implementation and acceptance
plan, not a claim that the proposed configuration or caches already exist.
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
  enable switch. Directory and entry resolve relative to the workspace.
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
product code. Use prior `53abb6b` native/old-UVM arms and the same candidate with
new caches off/on. Feature-off and backward-compatibility tests are mandatory.
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
- Next implementation: explicit UVM path/entry configuration and source-loading
  integration, followed by reuse and invalidation coverage for that configuration.
- No new cache/configuration is claimed implemented by this plan.
