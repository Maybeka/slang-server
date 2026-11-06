# Lifecycle and ownership quality evaluation

This evaluation compares the quality refactor with the preceding main revision
`f720af73ceca774ae2ce0877c879139b73b621db`. It is not an upstream comparison.
The implementation order and acceptance contracts are in
[code-quality-plan.md](code-quality-plan.md).

## Implemented boundaries

| Area | Result |
| --- | --- |
| Editor text | A request-local `DocumentEditTransaction` records entry before native mutation. Rejection, exceptions and cancellation cannot replay an incremental range. It owns no model and performs no destructor work. |
| Retirement | `ContextRetirementState` distinguishes a retry request, unadmitted editor input and published replacement history. Private replacement and source-history coordination live in `SlangServerRetirement.cpp`. |
| Diagnostic ownership | `DiagnosticAnalysisOwner` retains the original analysis wrapper together with borrowed complete/procedure/task views. Move operations clear the source's views; task timing requires an analyzed task. |
| Diagnostic lifecycle | `LocalDiagnosticSession` owns capture, preparation, attempt versions, retryable cancellation and original failure provenance. It holds one baseline/cache, not an edit-generation chain. |
| Certification | Physical input proof, preprocessing checks and certificate construction have separate implementation boundaries. Full byte/path validation and cancellation checkpoints are preserved. |
| Queries and rendering | Query response adaptation and diagnostic orchestration are separate units. Immutable rendering identity has no native binding responsibility. The facade and protocol schema remain the same. |

The refactor adds no semantic graph copy, mandatory debounce, new worker or
experimental option. The SDK implementation and restoration patch are unchanged.
All existing experimental defaults remain disabled. Workload qualification uses
unlimited semantic caches; finite-limit regression cases retain their own budgets.

Chip DV exposed a separate rendering defect: command-line macro notes could link
to generated buffer names or an empty URI. The parent diagnostic and real macro
definition notes are preserved. A small C++ test covers generated, replayed and
physical source locations. The final implementation borrows the existing source
name and checks it before formatting an expansion note, avoiding an added path
and string allocation on ordinary macro diagnostics.

## Acceptance evidence

- Ten additional C++ point-test cases cover edit entry/replay, retirement facts,
  analysis ownership/release/move, diagnostic retries/provenance, fixture-root
  termination and generated macro notes.
- Complete normal CTest: 1093 entries passed.
- Complete ASan server suite: 1078 cases / 206828 assertions passed.
- Complete ASan SDK suite: 2650 cases / 37128 assertions passed.
- Seventeen Python system/protocol tests, diagnostic transport, held private
  diagnostics and ordered completion/definition/resolve checks passed.
- Sixteen existing projects pass public hover, definition, completion, resolve,
  reference and diagnostic Native-result comparisons through error/repair,
  close/reopen and manual rebuild.
- UBUS, CVA6 and USB device pass real-edit lifecycles under ASan. Leak detection
  is disabled on these Mac ASan runs; lifetime and footprint checks are separate.
- The installed package passes default-off and default/custom cold/warm UVM
  profile checks. Default UVM uses shipped preparse data with no syntax parse;
  the custom profile creates and then reuses its project cache.
- The 709-file SDK inventory, four offline dependency exports and a fresh SDK
  patch restoration match their recorded identities.

The final versioned data record identifies the tested source files, executable
hashes, inputs, configurations and measurements. Intermediate observations are
kept with their original identities rather than substituted with later runs.
See [the acceptance data](../../validation/macos-code-quality-2026-11-07.json).

Representative real-edit results for the combined arm (SoC/CVA6: medians of three
processes; new DV profiles: one process each):

| Workload | First edited-file diagnostic ms, before → after | Continuous query / diagnostic ms, before → after | Body-6 footprint MiB, before → after |
| --- | --- | --- | --- |
| Large SoC | 66.35 → 66.80 | 16.65 / 31.37 → 16.71 / 31.88 | 1470.22 → 1470.83 |
| CVA6 + UVM | 49.45 → 46.12 | 7.31 / 13.14 → 6.42 / 11.60 | 687.44 → 688.52 |
| USB device DV | 27.29 → 27.47 | 4.58 / 8.03 → 4.63 / 8.08 | 389.56 → 390.47 |
| SPI device DV, RTL import fallback | 45.49 → 45.57 | 8.41 / 15.22 → 8.30 / 15.13 | 526.47 → 528.55 |

The two new DV rows each have one process sample per arm/version. SPI's combined
arm reuses local method results, but does not demonstrate a successful fixed RTL
import. Coverage sets match between code versions in every measured state.

These measurements support preservation of editing performance; they do not
establish a causal speedup from the structural refactor. Startup samples can
contain different amounts of interrupted or repeated analysis. The data retains
the original samples, fixed follow-ups and separately marked phase observations.

Final qualification has twenty scenario comparisons: eighteen from the existing
sixteen-project matrix and two added DV configurations. No first/continuous edit
or memory aggregate crossed the plan's investigation thresholds. Five Native
startup aggregates did; each received one fixed three-repeat follow-up, without
discarding its original samples:

| Native startup control | Original increase ms | Follow-up median ms, before → after |
| --- | --- | --- |
| OpenTitan AES RTL | 82.47 | 361.53 → 359.56 |
| OpenOFDM Wi-Fi | 119.97 | 466.00 → 466.57 |
| OpenTitan AES + UVM | 59.44 | 5232.56 → 5154.12 |
| CVA6 background | 59.76 | 3513.60 → 3540.08 |
| SPI device DV | 51.51 | 5382.45 → 5405.84 |

The controls do not reproduce a stable increase above the threshold; this is not
proof of identical startup time on every run. All original state measurements,
process repetitions and lifetime peaks remain in the data. The final comparison
also checks retained-root sets, current shared-root counts and actual reuse
categories for each measured state, rather than treating a requested freeze as
successful reuse.

Cleanup removed 2387 terminal replay/install/probe and log files (1.220 GiB
logical bytes), including historical replay outputs in the shared RTL harness.
Superseded historical installation trees and executables were also retired.
Canonical projects, pinned EDAM inputs/recipes, the current Release/ASan builds,
offline dependencies and one immutable baseline/candidate pair remain reusable.
The data contains the deletion receipt; the byte count is not a filesystem-space
guarantee for compressed, sparse or cloned files.

## Expanded optional verification workloads

The opt-in preparation and replay tools are documented in
[tests/large_dv/README.md](../../tests/large_dv/README.md). They do not enter CTest
or add a server build/runtime dependency. Sources are the locally pinned
OpenTitan revision `8e7f977585b8e8304feaf688d7e8429059bfd097`; six official DV
targets are configurations of that codebase, not six independent repositories.
No source stubs or diagnostic filtering are used to make a result pass.

USB device covers actual retained DUT roots plus local edits. SPI device covers
a refused fixed import, correct native fallback and local method-body reuse.
Both require complete observed public diagnostic equality, an inserted error and
repair, structural edits and manual rebuilding. The SPI analysis profile starts
with fourteen Native errors in both code versions: thirteen queue/array type
conversion errors and one invalid class-handle binary expression. Its passing
comparison does not certify an error-free simulator configuration.

Loaded physical HDL volume, excluding command-file buffers:

| Profile | Files / lines including UVM | Lexical DV classes excluding UVM | Definition-name groups / instantiated symbols |
| --- | --- | --- | --- |
| USB device | 592 / 162364 | 233 | 63 / 735 |
| SPI device | 559 / 175333 | 204 | 75 / 1641 |

Each includes 142 loaded UVM files, 75331 lines and 350 lexical class declarations.
Class counts describe source text, not elaborated specializations. Hierarchy
groups follow `slang.getScopesByModule`, including module/interface instances
and excluding uninstantiated bodies.

The additional profiles expose limitations rather than universal freeze success:

- DMA selects a different 32/64-bit generic warning representative; OTBN prints
  a different generic type alias. Both differences reproduce before the refactor.
  Their exact diagnostic-payload comparisons remain failures.
- Chip inputs include 659016 physical HDL lines and 1481 lexical DV classes before
  UVM. Fixed preparation enters an invalid constant state in `lc_state_to_bin`
  and declines after `$fatal`. The official `TOP_LEVEL_DV` option does not remove
  that refusal. It is not counted as frozen semantic reuse.
- SPI host/device can prepare a closure but decline its import because of
  `unclosed-global-name`. Preparation, actual frozen-root import and local
  method-body reuse are reported as distinct facts.
- The vendored Ibex core UVM filelist lacks two `pulp_common_cells` inputs. This
  remains a setup failure rather than a successful partial or stubbed test.

## Measurement interpretation

`queryMs` runs from batched edit/hover dispatch through full response receipt;
`diagnosticMs` starts at the same dispatch and ends at the matching edited-file
version publication. They overlap and do not measure global diagnostic completion.
Startup includes a new process, initialization, explicit build/instance selection
and the initial hover. OS caches are not flushed, and project parse caches can
become warm. These figures do not describe typing-burst or p99 latency.

Continuous values use the six successive body edits within each process. SoC and
CVA6 real-edit/background scenarios have three process repetitions per arm/code
version; the rest of the main matrix has one. Additional controls and the exact
reused baseline identities are explicit in the data. All qualified runs are serial
after independent build/correctness work ends; traced probes are marked separately.

Memory reports distinguish boundary RSS, physical footprint and lifetime peak
physical footprint. The lifetime peak includes structural recovery and manual
rebuilding, not just steady editing. A 256-edit UBUS lifetime run checks that
publication results remain equal to Native and records generation, handles and
footprint growth. No universal leak-free or memory-upper-bound claim follows
from this finite sequence.
