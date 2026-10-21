# CVA6 full DUT semantic reuse (Mac, 2026-10-22)

This extends the [namespace evaluation](cva6-uvm-namespace-evaluation.md) with
pre-serialization diagnostic deduplication and closed RTL type dependencies.
Experiments remain disabled by default.

## Workload and acceptance

The same local CVA6 `cv64a6_imafdc_sv39` and complete core-v-verif UVM environment
are used without `VERILATOR`, source repairs, stubs, or warning suppression.
There are 258 explicit roots (259 including bundled UVM), 612 diagnostic URIs,
and 404 instances in the selected DUT out of 575 in the full design.
The selected path is
`uvmt_cva6_tb.cva6_dut_wrap.cva6_tb_wrapper_i.i_cva6`.
Source pins and file census are unchanged from the linked evaluation. All
workload and dependency sources were reused locally.

A successful import of that root now shares its entire bound semantic subtree,
including the tracer's closed class specializations. Current outside parameter /
port connections, reference use and diagnostic analysis remain current. This
is an LSP test, not a simulator or ISS run.

Each edit's complete accumulated client diagnostic state is compared with the
Native oracle, including all empty lists, notes, locations and versions. The
existing 164 strict-language errors are retained; injecting a missing user symbol
raises this to 165 and repair restores 164. Public acceptance includes hover,
definition, references, highlights, symbols, 171 completions and every resolution.
Additional cases cover frozen RTL parameters / ports, forwarded anonymous types,
macro source / expanded / combined views, and captured-field navigation.

Read-only views cannot publish a shallower replacement for an active design.
Compiler options own their top-name text independently of command-line storage,
so retiring the driver does not corrupt cache compatibility. A six-edit sequence
with all 171 completion resolutions after every edit must retain and import the
whole DUT across retirement, without eviction.

## Measurement

Fresh sequential processes run in randomized paired blocks on the same Mac and
warm filesystem. Four arms separate the current binary with all relevant
experiments off, diagnostic delta publishing alone, the previous qualified
partial-RTL binary, and the current combined implementation. All use the same
default UVM syntax preparse. The old installed UVM URI prefix is mapped to the
current build prefix only after checking every resource source file byte hash.

Five latency blocks contain six states each: opening, ordinary edits, error
injection and repair. Continuous medians exclude opening and the injected-error
state, and include repair and retirement. First a median is taken within each
session, then across sessions. Memory is sampled separately at 20 ms over three
blocks of twelve edits; those runs are not latency evidence. RSS sums the server
and its diagnostic children; physical footprint and final process cleanup are
also checked. Cache budget accounting is separate from process RSS.

Per-request pooled P50/P95 are shown separately from the session medians defined above.

| Mode | Query P50 ms | Query P95 ms | Diagnostics P50 ms | Peak RSS MiB | Peak footprint MiB | Final footprint MiB |
|---|---:|---:|---:|---:|---:|---:|
| Current, experiments off | 989.1 | 3535.8 | 2080.1 | 1354.2 | 746.4 | 650.6 |
| Current, diagnostic delta only | 986.4 | 1024.5 | 1402.9 | 1365.6 | 765.5 | 657.8 |
| Previous UVM + partial RTL | 681.4 | 3186.2 | 1801.4 | 1403.5 | 1028.2 | 892.5 |
| Current UVM + whole DUT + delta | 671.9 | 1481.9 | 989.3 | 1433.8 | 1087.9 | 1087.9 |

The predeclared session-median query statistic is 1373.4 ms for the previous version and 673.0 ms for the combined version. Ordinary query P50 changes little; fewer recovery pauses explain the larger session-level difference. Session-median diagnostic updates improve from 1808.4 to 991.5 ms.

Setup plus first-open query improves from 4078.7 to 3495.5 ms. The combined version still reaches 3262.0 ms for an individual query and 3762.0 ms for diagnostics in these runs.

An ordinary publication batch drops from 612 notifications to one. Approximate compact JSON traffic drops from about 13 MB to 42,881 bytes; the changed current-document version is still published. All scope runs retain one whole-DUT owner, hit the root for every edit, and report no evictions. Charged immutable base plus body ownership is about 244 MiB within the 256 MiB cache budget.

Memory is a tradeoff: peak RSS rises by 30.3 MiB, and final footprint is higher in this twelve-edit observation window. No memory-reduction claim is made; retention and diagnostic publication history cost memory. All sampled process families exit normally, and these measurements do not establish an unlimited-session memory bound.

Qualified binary SHA256: `4d19ae17e6f6b869f3a82a86c0f14ceab73878bf712fd7f5206f251cd66c2b2b`.

## Scope and remaining costs

Only the stable providers and their dependencies are fixed. Unfrozen verification
sources still parse and bind after edits; current dataflow / diagnostic work also
remains. Whole-DUT graph reuse does not imply a tens-of-milliseconds response or
elimination of all pauses.

Type capture preserves nominal identity, alias presentation, declaration syntax
and immutable source ancestry without retaining old consumer compilations.
Changed layouts / provenance, unsupported type dependencies, outside binds and
unproven calls use native analysis. Class-scoped nominal dependencies remain
native until specialization provenance is supported. If independent owners would
assign conflicting identities to one current nominal type, the later body stays
native. Fixed-provider changes require `slang.rebuildScopeLibrary`.

Enable the two semantic experiments plus
`experimentalDiagnosticDeltaPublishing` explicitly to reproduce the combined
mode. `frozenSources.maxMiB` defaults to 256 MiB and can be configured. Status
`bodies.retainedRoots` distinguishes whole-DUT retention from leaf-only hits;
one root hit can cover all 404 instances.

## Validation

745 CTest entries (including 2,588 SDK cases), 17 Python tests, 43 transport cases,
and 11 headless Neovim project/mode checks pass. Relocated installation passes
explicit and bundled UVM comparisons for default, `UVM_NO_DPI`, and converter
profiles (12 sessions). The SDK patch reconstructs all 709 inventory files exactly.
All 612 workload source hashes remain unchanged.

Local receipts and repeatable measurement helpers are collected under
`untracked/dut-reuse-2026-10-22/`; that directory is not tracked.
