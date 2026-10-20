# Mac UVM persistent syntax restoration — 2026-10-21

Qualification: passed on the sampled Mac workloads. Restoring the UVM syntax
cache is faster than parsing it, and the previously qualified in-process semantic
cache retains its continuous-edit benefit. This does not implement arbitrary
frozen scopes or persistent bound semantic objects.

Candidate executable SHA-256:
`7161a7691913414d041238f7b3596ce8479007014f68cd7d4298c6a3d56510db`.
The accepted semantic reference is main `54f8d44fa188a82b86472d2b91c5040b6646331d`,
executable SHA-256
`261c36a22edbd8447c4188518c4d97616be5029146fad82bfdc30a8b96c6acc3`.
It is this fork's accepted semantic implementation, not upstream slang-server.
SDK base: `151a801ceaf6faf23d79a42d210ff67e50eb4660`; patch SHA-256:
`b739c851c7bfc4b8e504af178ab565c09ccb5e069116a3fcf6c402a58b24ae18`.
A clean base export plus that patch matches all 709 SDK files. The SDK gitlink is
unchanged. All experiments remain disabled by default.

## Implementation and cache policy

The distribution contains the pinned UVM 1.2 sources and exactly one syntax
database, `preparsed/default.cst`. CMake generates and round-trip verifies it
before building/staging server targets, and installs/packages only that file.
Default bundled UVM requires no runtime source parsing, including first startup.
A missing, corrupt, incompatible or mismatched shipped database is rejected
without falling back to source parsing.

Other bundled parse configurations create `.slang/cache/uvm/*.cst` in the project
on first use and reuse it on subsequent server starts. Explicit source-directory
libraries also use project caches when semantic reuse is enabled. The key includes
the actual SDK/schema, paths, cwd, all parse options and ordered include policy.
The full configuration is checked independently of its filename hash.
Custom wrappers with other packages or compilation-unit declarations keep syntax
reuse but decline the single-package semantic owner, preserving nominal type
identities across their additional declarations. A regression case covers passing
a helper-package class handle into a UVM-package function. Library
contents and dependencies are trusted fixed until `slang.rebuildUvmLibrary`;
that command invalidates the current project cache before preparing a new owner.
A changed parse configuration selects another cache. Custom libraries retain
ordinary handling of intentionally repeatable, unguarded macro headers.

The portable, typed archive records node fields and graph identities, values,
source/expansion metadata, macros, guards, diagnostics, parser metadata and parent
relationships. It contains no raw compiler pointers or serialized C++ object
layouts. Bound semantic objects are still built once per process and retained
in-process across edits and ordinary consumer retirement.

The reader uses bulk I/O, the SDK's fast deterministic checksum and bounded memory
views. It avoids per-byte iostream decoding, temporary token-string copies, a
second full in-memory payload copy, and full syntax-tree walks to reconstruct
parser metadata and parent links. Writer scalar output also avoids per-byte stream
sentries. Count, integer, truncation, type, graph and trailing-data checks remain.
Writes use distinct temporary files and publish by rename; project data is not a
cross-machine semantic database. Cache publication does not claim fsync durability.

## Controlled syntax costs

Apple M4 / 24 GiB, macOS 27.0.1, Homebrew clang 23.1.3, Release `-O3`.
Warm filesystem, independent sequential processes, shuffled fixed-seed schedule,
30 samples per configuration/path: 360 successful formal samples plus 12 first
observations. No concurrent builds, tests, memory sampling or parser tracing.
Filesystem caches were not flushed; these are not disk-cold claims.

Milliseconds, median / P95:

| Configuration | Direct preprocessing + syntax parse | Restore, including read and validation |
|---|---:|---:|
| Default | 43.02 / 51.78 | 29.27 / 32.48 |
| `UVM_NO_DPI` | 42.75 / 44.66 | 29.27 / 32.26 |
| `UVM_USE_RESOURCE_CONVERTER` | 43.04 / 48.25 | 29.51 / 32.87 |
| Both | 42.97 / 47.92 | 29.45 / 33.29 |

Restoration medians are about 31–32% below direct parsing. The previous candidate's
roughly 104–105 ms restoration was slower than its roughly 43–44 ms parsing;
that provisional result was never qualified as a performance improvement.
The new default database is 12,125,798 bytes (about 11.56 MiB). Persisting parser
metadata and parent relationships increases the database by roughly 0.72 MiB,
while removing runtime reconstruction work.

First semantic binding remains approximately 29–30 ms in the isolated benchmark.
The syntax-only parse-and-save path is about 86–87 ms. It keeps its original parsed
tree and therefore does not include the extra restore needed to adopt an isolated
project-cache miss into the server's existing source namespace.

A separate traced LSP lifecycle proof has three cold/hit pairs per configuration
(24 processes), plus three manual rebuilds. Default startup performs zero UVM
source parses. Each non-default cold start performs exactly one, and each matching
restart performs zero. Project cold preparation is approximately 45–46 ms parsing,
42–43 ms serialization/publication, and 26–27 ms adoption/restoration. The synthetic
consumer's startup through status is about 196 ms cold versus 110–111 ms on a hit.
These n3 traced observations prove the lifecycle and describe its cost; they are
not substituted for the controlled n30 timing results.

## Continuous edits and startup

Twenty randomized paired blocks per project, ten alternating unsaved edits per
session, three arms: accepted semantic implementation, current semantic mode and
current native mode with caches off. All 180 scheduled fresh-process sessions
succeed. Both semantic arms use the same shipped UVM source inventory; the current
arm restores default data for SPI/PSS and a project cache for UBUS's converter
configuration. The initial UBUS project-cache miss remains in the measurements.
Every edit's target-version diagnostic is awaited. Timing excludes shutdown and
contains no status probes, parser traces or memory sampling.

Medians in milliseconds, accepted semantic mode → current semantic mode:

| Metric | SPI | PSS | UBUS |
|---|---:|---:|---:|
| Startup through first response | 135.03 → 119.17 | 135.16 → 120.79 | 131.44 → 117.16 |
| First edit query | 13.83 → 13.67 | 15.08 → 15.02 | 15.73 → 15.69 |
| Continuous query | 13.48 → 13.52 | 14.81 → 14.83 | 15.27 → 15.27 |
| Edit through diagnostic | 21.92 → 22.07 | 23.88 → 23.88 | 22.32 → 22.36 |
| Whole ten-edit session | 367.58 → 352.51 | 405.84 → 391.37 | 376.17 → 360.81 |

SPI uses definition queries; PSS/UBUS use completion queries with selected resolves.
Changes in continuous diagnostic latency are within the paired bootstrap
uncertainty; no established continuous-query regression is observed. Paired median
whole-session savings versus the accepted semantic implementation are approximately
4.26%, 3.69% and 3.62%. Native-mode continuous diagnostics are approximately
182/226/187 ms, so semantic reuse continues to provide the principal edit-time
benefit. This is not a guarantee for arbitrary workloads or for unmeasured typing
bursts where foreground/background contention may differ.

## Lifetime and memory

Six additional sessions compare both semantic versions at 300 edits per project,
one repeat per arm/project. Every matching diagnostic, error insertion/repair,
suffix-macro change, save and manual reconstruction passes. Each ordinary session
keeps UVM generation 1 and constant library AST bytes: 26,533,888 for SPI/PSS and
27,463,680 for UBUS. SPI/PSS perform three ordinary consumer retirements each;
UBUS performs eleven in the reference and ten in the current version. Explicit
reconstruction creates generation 2.

Sampled parent RSS peaks in MiB, accepted → current:

| Scope | SPI | PSS | UBUS |
|---|---:|---:|---:|
| Through the final ordinary edit | 176.22 → 146.38 | 173.95 → 147.28 | 192.84 → 172.66 |
| Complete run, including manual reconstruction in both arms | 220.34 → 210.94 | 219.95 → 205.81 | 223.97 → 230.56 |

The UBUS manual rebuild has a larger transient peak; it is not presented as a
universal memory improvement. Samples at 201/301 edits in SPI/PSS still vary with
mutable-context history; this experiment does not establish a global RSS plateau.
Sampling is every 20 ms, may miss peaks, and has only one repeat. Both semantic
arms have no diagnostic worker family to add in this sample. Raw samples and
physical footprint are retained in the compact evidence.

## Correctness and distribution acceptance

- Full CTest: 689/689; Python server tests: 16/16; diagnostic transport: 43/43.
- Focused UVM server cases: 4,406 assertions in 15 cases; SDK archive round trip,
  numeric values, source offsets, malformed/truncated/trailing data: 41 assertions.
- Sixteen real SPI/PSS/UBUS processes compare ordered completion lists, all resolves,
  definitions, hover, references, symbols, inactive regions, diagnostics and stale
  handles, including forced retirement. Observable results match ordinary analysis.
  Default UVM parser traces remain zero. UBUS's first project-cache miss is allowed
  exactly once; subsequent edits and retirements reuse the same semantic generation.
- Six full-design processes preserve native root order and compare live width
  edits and PSS bind removal/repair, including complete flow/usage diagnostics.
- Build-tree checks cover all four macro configurations, each with explicit/bundled
  inputs and semantic reuse off/on. Relocated CMake installations and release-style
  archives cover default/converter configurations, complete source inventories,
  library navigation, errors/repair and manual rebuild, with equal public results.
  Each distribution contains only `default.cst`; other data appears in the project.
- Five damaged/missing/incompatible/mismatched-resource cases reject the bundled
  default without UVM source parsing. A project cache's damaged envelope is repaired
  by regeneration, and a changed macro configuration selects a new cache.
- SDK reconstruction matches all 709 source hashes without updating the gitlink.

Secondary-document notices are compared after passive settlement, preserving their
values and ordering within entries. Arrival-order equality is not claimed. The
historical W12 UTF-16 workload issue remains excluded rather than relabelled a
success.

[Compact evidence](data/uvm-preparse-restore-2026-10-21.json) contains executable,
SDK and database hashes, all timing summaries, lifecycle proofs, memory scopes and
acceptance receipts. Raw data, scripts and logs remain in the workspace's untracked
evidence folder. Generated measurement caches and unused installation copies are
cleaned after inventory receipts; useful active project/build caches are retained.
