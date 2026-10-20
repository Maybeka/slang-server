# UVM dependency-prefix reuse for edited selected roots on Mac

The tested executable SHA-256 is
`2efade522bc96523fd50b4bdf83cd5f0f7f12cf5f9a0b30f934742a9c23de67c`.
The candidate source manifest/archive pins this implementation. All experiments
remain off by default; this milestone is not final integrated acceptance.

## Changed behavior

When a full selected group ends at the last input root, the selected declaration
itself needs current parsing. Previously this declined all prefix reuse, even
when an explicitly configured UVM entry was the unchanged first root. The server
now derives a shorter dependency plan for that first root and parses the remaining
roots with the native parser. It retains complete ordered roots and never
extracts or reorders a module's text. Existing behavior remains native without an
eligible earlier configured UVM root, with zero budget or with unsupported state.

The initial native UVM checkpoint can be staged for this case, avoiding a second
UVM preparation parse. Normal exact input witnesses still run before accepting
reuse and after composition. Macro-override detection and native fallback remain.
`dependencyResumes` and the status reason distinguish reuse of the library
prefix from reuse of the selected root; the latter is reparsed and rebound.

The dedicated case passes 377 assertions across enabled, disabled and zero-budget
modes. It compares fresh native hierarchy, scope chains, hover, definition,
completion and every resolve, plus unfiltered diagnostics. Checks include unsaved
class edits/errors/repair, a changed library header without a watcher event while
the experiment is enabled, literal macro override/native decline and refresh.
The disabled control is explicitly rebuilt for the external library-header edit,
matching the fresh native reference rather than assuming native cached analysis
observes an unannounced disk change.

CTest passes 685/685, Python 16/16, the focused group passes 7,557 assertions in
97 cases, and transport passes 43/43. Six real native/enabled SPI/PSS/UBUS language
replays match full public responses, all completion resolves and current-version
diagnostics across five unsaved states. UBUS's timing replay records one native
seed, zero independent preparation parses and 11 actual dependency resumes;
retained syntax is 30,191,259 bytes within the 48 MiB allowance.

Forced-retirement replays also pass all six native/enabled language processes and six isolated LazyVim sessions. Each enabled process observes two owner retirements; disabled controls observe zero. The editor checks use five unsaved states, preserve error/repair and hierarchy comparisons, and do not touch the live user editor. Results are pinned in `dependency-prefix-forced-editor-checks.json`.

## Exploratory timing

54 sequential randomized paired fresh processes cover three projects, drained and
burst input, and native/off, matched syntax-only and instance-value reuse arms.
There are three samples per cell and ten edits after the initial state. The old
UVM-prefix experiment is off in every arm. No builds, tests, editors, profiling or
RSS sampling overlap these timings. Filesystem caches are warm. These small
samples are not the final 20-pair confidence-qualified acceptance.

Median complete-session milliseconds, ordered native / syntax-only / instances:

| Project | Diagnostic-drained | Continuous burst |
|---|---|---|
| SPI | 1779.26 / 1607.41 / 1680.05 | 1558.20 / 780.21 / 844.70 |
| PSS | 2338.42 / 2185.95 / 2292.06 | 2094.08 / 1409.94 / 989.29 |
| UBUS | 1847.49 / 1560.33 / 1628.78 | 1635.68 / 772.12 / 768.91 |

UBUS burst first-edit queries are 46.88 / 29.51 / 26.49 ms and continuous-query
medians 58.79 / 26.82 / 26.10 ms. Instance sessions save about 53% against native
in this sample, while their drained complete sessions remain about 4.4% slower
than syntax-only. PSS burst still benefits from hierarchy-value reuse; SPI burst
and all drained instance sessions remain slower than their syntax-only ablation.
These tradeoffs remain visible and do not justify a universal improvement claim.

A separate enabled UBUS run completes 1,000 edits plus current-version user error,
repair, declaration fallback and refresh. It has 31 natural context replacements,
startup-inclusive sampled peak RSS 241.0 MiB and physical footprint 171.2 MiB.
The preceding implementation needed 111 replacements in its separately pinned
run. These are single runs, not paired three-repeat memory acceptance; fewer
replacements establish reduced repeated work rather than a universal RSS saving.
Sampling uses the same startup-inclusive RSS/footprint harness and no primary
latency measurements overlap it.

## Remaining class resolve cost

A separate 12-process three-pair class-workload pilot still finds regressions.
Across three query states, PSS resolves 171 items and UBUS 393 items. Median full
sequences including diagnostics are native/on 676.73/1468.03 ms for PSS and
644.42/2182.31 ms for UBUS. All-resolve sums are 60.25/1033.07 ms and
139.65/1880.66 ms respectively. The dependency-prefix improvement addresses the
full-design UBUS scene, not this exploration-mode per-request certification cost.

Temporary local SDK timing probes split one additional profiling run per case/
arm. Their timings are not acceptance results. Successful witness medians are:

| Phase | PSS | UBUS |
|---|---:|---:|
| Include-directory patterns | 246.6 us | 119.3 us |
| Source alias resolution | 1571.9 us | 1380.8 us |
| Closed-source reads / failed lookups | 2608.2 us | 2242.4 us |
| Parent-directory recheck | 792.7 us | 679.9 us |
| Input-root glob witnesses | 888.0 us | 98.7 us |

Graphs have roughly 331/298 source aliases, 400-401/480 lookup-cache paths and
32/26 shared parents for PSS/UBUS. Each unresolved item pays a full witness; this
explains the large resolve sum. The original SDK files were restored byte-for-byte
and all 708 restoration-manifest hashes were checked. Probe binaries, source
copies and traces remain separately identified, and are not the tested candidate.

Next strategies must reduce this work while preserving exact closed bytes,
negative lookup outcomes, directory/symlink identity, cancellation and checks for
every analysis RPC. Candidate work includes sharing regular directory-prefix
proofs within a witness and batching independent filesystem checks. There is no
permission to introduce TTLs, watcher-only freshness or cross-request proof reuse.
Final work still includes resolving this regression, 20 paired first/continuous/
fallback comparisons with the accepted baseline, repeated long memory coverage,
SDK restoration and a normal main push. No push occurred for this milestone.
