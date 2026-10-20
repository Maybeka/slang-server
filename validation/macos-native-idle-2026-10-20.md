# Mac native-only idle coalescing acceptance

Candidate SHA-256: `c30d503050c7f499e30c27f36fd25dc0f058a11decb08ded4540baa53db624a9`.
The SDK patch remains `074b504df491b71b5206bd6923a804587e579f1618fc46cd657667056855e8fa`.
All cache experiments remain off by default. Publication uses the tested source identity and ordinary main workflow.

## Why the scheduling policy is limited

The earlier unconditional two-millisecond window (7e67) passes all 300 normal
sessions but regresses the dedicated UVM route by 9.19% / 4.31% / 6.09% on
SPI/PSS/UBUS. Native routes regress 4.46% / 2.90% / 2.47%. All twenty paired mean
slowdown intervals are positive. Raw evidence and compatibility analysis remain
in `idle-coalesce-selected-confirm.json`, its analysis and
`idle-coalesce-selected-compatibility.json`; these results are not relabeled.

The current policy requests a two-millisecond window only when a full-design
compilation requires native background diagnostics. Exploration has zero delay.
The protocol calls the policy under the state lock with the queue lock released,
then rechecks queued input and end-of-input after reacquiring the queue lock.
The optional-window scheduler harness exercises the existing ordering,
cancellation, wake and shutdown tests. Input certification, diagnostic completeness
and freshness rules are unchanged. This is not a freshness interval.

## Current completed checks

| Gate | Evidence |
|---|---|
| Whole checks | Build, 685/685 CTest including SDK, 16/16 Python, 43/43 transport, focused scheduler/frozen/macro/UVM/hover suite pass; `native-idle-acceptance.json` |
| Full-design real semantics | Six SPI/PSS/UBUS processes, five states, all public responses/every resolve, hierarchy and latest diagnostics/error repair match; each enabled process observes two retirements, disabled processes zero; `native-idle-forced-real-oracles.json` |
| Exploration real semantics | Twelve processes, every resolve/stale items/all latest diagnostics/inactive regions, warning suppression off/on; `native-idle-exploration-real-oracles.json` |
| LazyVim instance sessions | All six pass, unsaved edits/error repair, two actual retirements per enabled project and none disabled; isolated config/cache, unchanged user editor config |
| LazyVim macro sessions | All three projects pass completion/resolve/definition/hover and source/expanded/both previews, a real floating window, unchanged normal-hover output after temporary preview and unsaved edits |
| Full-design memory driver preflight | Twelve 20-edit processes pass across accepted UVM, current native/dedicated/generic; all raw hashes verified; each trace observes one family process |
| Worker memory driver preflight | Twelve 20-edit processes pass across the same arms; all raw hashes and postsequence checks verified; every trace observes two family processes |
| Installed product | Actual CMake install moved to a directory containing spaces; binary SHA matches the frozen candidate; six explicit/bundled × native/dedicated/generic runs, four states each, all public outputs equal |
| Release product | Actual compressed archive extracted and run; same six-route/four-state equality and manifest checks pass |
| Missing/tampered resources | Default/project-specific library stays usable without a shipped kit, explicit missing shipped kit reports the expected error; valid/modified/missing-license/extra-file inventory cases behave as expected |
| Schema and defaults | Regenerated JSON schema and TypeScript types match byte-for-byte. Generator emits no JSON default keys; defaults are proven by Config.h and passing C++ opt-in/default-input tests, not invented schema metadata |
| SDK reconstruction | Unchanged 708-file SDK source previously passes independent offline compilation and all 24,882 assertions / 2,570 cases with production Mac static visibility settings; original hidden-visibility failure remains documented |
| Remote identity | Read-only remote main check matches local baseline 13f2d300f19e2ecb1c2bf8ac1342b9fa082211f6; no remote mutation |

Memory preflights are protocol/guard checks, not long-memory acceptance or latency
evidence. They retain missing-include error/repair, observed generic macro
fallback/repair and save refresh. Worker manual-refresh probes query without a
new edit and require new generic preparation; the old binary never receives
unsupported status/refresh commands. Current-only frozen refresh is additional
stress, not baseline-equivalent work. RSS and physical footprint remain separate.

All packaging phases run offline without compilation or downloading a library.
Receipts retain hashes of the installed binary, archive, scripts and phase logs.
Completed release staging/extraction/no-resource binary copies were removed only
after all consumers passed (65,089,436 bytes); the frozen executable, validated
relocated install, compressed archive, reusable caches, source inputs and raw
results remain. See `native-idle-packaging-cleanup.json`.

## Integrated performance and memory

Formal current-candidate normal class, macro fallback and six-arm RTL comparisons
complete all 1,320 processes and their offline analyses. No build/editor/trace/
test/RSS work overlapped timing. See the current class-session and RTL reports;
the previous d678 and 7e67 measurements stay separate. Five-arm three-repeat
20-edit and 1,000-edit full-design and worker schedules each complete all 45
processes successfully (180 total), with no censored rows. All four independent
raw audits pass: binary/driver/raw hashes, recomputed parent/family RSS and
physical-footprint peaks, budget, post-error/fallback/refresh checks and actual
retirement. Every generic long run records 8/8/31 retirements for SPI/PSS/UBUS,
with sampled source-location entry resets. RSS and footprint remain separate.

The separate matched native UBUS investigation completes all twelve short/long
cells and raw audits. Long family-RSS medians are 1803.8 MiB accepted and
1776.6 MiB current (-1.51%); footprint medians change -1.60%. The original
instrumented +29.14% result remains reported. Three repeats and command differences
do not establish equivalence, a memory win or a causal explanation. This closes
the required investigation without a speculative runtime change. See the
[memory report](macos-native-idle-memory-2026-10-20.md) for all ranges and limits.
Both ordinary-route paired compatibility analyses are complete. Final source,
SDK/default/format and normal main remote verification close publication.

UVM unification is optional per the user's clarification. The dedicated UVM
route remains available. Both switches currently prefer a generic input group;
configuration alone does not prove dedicated continuation. Generic UVM and RTL
negative results remain reported. General package/module/instance capabilities,
macro views, invalidation/fallback and opt-in shipped UVM remain in scope.

## Route decision and completion-detail cost

Keep the dedicated UVM route for ordinary UVM class editing. Generic package and
module reuse remains useful as a separate opt-in capability; generic UVM is not
a mandatory replacement. For hierarchy queries, the larger PSS burst workload
establishes additional instance-value benefit. Small SPI/UBUS subtrees and
fully drained sessions do not justify enabling that cache for speed. Enabling
both switches currently permits generic precedence, not proven composition.

Every-item completion detail remains a certification-heavy workload. The
[shared-parent witness evaluation](macos-frozen-alias-plan-2026-10-20.md) measures
an incremental mitigation on its pinned d678 executable: whole sessions improve
7.0%/9.6% and every-resolve sums 10.1%/11.3% on PSS/UBUS relative to f926, but this
does not erase the native/dedicated regressions. Those timings are historical,
not relabeled c30 measurements. Current public-response oracles still exercise
every item and stale handles; selected-resolve timing is not substituted for an
all-item timing claim. The release decision retains exact per-request source
proof and default-off generic reuse, with the dedicated route recommended where
it wins. This explicitly accepts a certification/preparation cost for the general
capability, rather than claiming a universal first-query or resolution gain.
