# Capability acceptance for integrated main

This is a capability contract, not byte equivalence to a lost historical tree.
The legal starting point for this round is `1f05b9e` with the verified SDK patch.
Current execution results and raw-evidence hashes are in
[`linux-2026-10-18.json`](linux-2026-10-18.json); older reports describe older runs.
The follow-up [clean-checkout report](clean-checkout-2026-10-18.json) records an
actual full independent build of `9dfd335` and every documented acceptance suite,
including exact reused tools/source dependencies and the unavailable Neovim smoke.

The 2026-10-19 follow-up is in [the macOS report](macos-2026-10-19.json).
It adds a constructor-local completion identity index, immutable value DTO sharing,
and a deterministic regression against replacing a parsed-source witness with
newer lookup bytes. Mac static-SDK option identity and parser-count hooks have
separate platform fixes and tests. Both experimental defaults and the supported
language subset stay unchanged. The table below records the earlier October 18
round; new Mac results do not imply a fresh Linux or production UVM run.

| Capability | Scope / implementation | Acceptance and evidence | Status in this round |
|---|---|---|---|
| Package/include owner and shared compilation | `SlangDoc`, `ServerDriver`, `ShallowCompilation`; preserve main's first-source selection and explicit picker | CTest `IncludedPackageTests`, explicit/source-context and recovery tests | Existing capability; rerun full suite |
| Ordered complete filelist and single-unit preprocessing | Configured native units, SDK `SourceLoader::loadAndParseSources` / `SyntaxTree::fromBuffers` | CTest `CompilationUnitContextTests`, explicit-context suites; all method drivers use three ordered roots | Existing capability; preserve |
| Foreground query priority and safe cancellation | Request scheduling, phase checkpoints, exactly-once editor synchronization | `ForegroundCancellationTests`, method cancellation/publication-failure tests | Existing capability; rerun |
| Isolated complete diagnostics and stale-version rejection | Diagnostic process/snapshot owner, revisions, generations and document versions | Snapshot tests, 32 transport checks, held diagnostic worker test with current multi-header fixture | Existing capability; rerun |
| didOpen delayed full diagnostics | Main process opens authoritative editor bytes, queues complete diagnostics | Foreground/snapshot suites, held-worker protocol test | Existing capability; preserve |
| Limited cache reuse | Same valid owner/shared Compilation; same uninterrupted diagnostic batch; bounded method DTO/certificate | Package suites, semantic batch tests, eight mini-parse count checks plus ninth native fallback | Existing capability; not a general cross-version semantic cache |
| Multi-include method certificate | Every preprocessing buffer/root/include edge certified; unique target occurrence; source/lookup/config witnesses | New sibling+nested+repeated-unrelated C++ positive; 500-class native oracle; counted parse evidence | Added here |
| Dependency/environment change fallback | Reject repeated target, macros, imports, recursive inputs; compare disk bytes with captured parse and before every reuse | C++ refusal/lifecycle cases; dependency-before-certificate and after-certificate tests; counted dependency/repeated-target fallback | Added certification checks; existing guards retained |
| Equal-length declaration and all-reference rename | Rebuild mini bindings and method-owned completion DTOs; reject stale automatic handles | New two-local declaration+use rename oracle and C++ positive; compare current definition, ordered completion and resolved locals with native | Added positive coverage |
| Select real edited method | First changed byte/range locates function; multiple input-int parameters and initialized locals | Native oracle edits earlier, combine and later independently | Added positive coverage |
| Reproducible fork build and full suites | Fork URL, exact SDK patch and dependency hashes, frozen Python, SDK tests explicitly ON | `docs/start/building.md`, dependency helper, independent clean checkout with zero initial product objects and all acceptance commands | Full independent build and suites passed; reused tools/dependency sources explicitly recorded |
| Fork-specific LazyVim use | Absolute cmd, Mason install/activation exclusion, workspace markers, final resolve capabilities | Shipped plugin spec, executable Lua API contract test, editor verification instructions | Added; actual interactive LazyVim session remains unverified here |
| Representative end-to-end timing | Same ordinary binary, preregistered balanced order, timestamp at complete response receipt, native correctness oracle first | 500 classes / five class headers / nested include / two packages / three roots; completion and definition separate; n and failures in JSON | New experiment; no historical F1/F2 equivalence claim |
| Defaults | Both experimentalMethodScopeOverlay and fastCwdSnapshotChecks remain false | `include/Config.h`; baseline config/full suite | Preserved |
| Persistent recovery | Commit + SDK patch/inventory + exact dependency bytes + raw logs/source/binary hashes + bundle; verify upload result | Current report and compact evidence; Library delivery attempted through supported helper only | Delivery result reported separately; local files alone are not persistent storage |
| Variable-length methods, new/deleted locals, broader expressions | Requires offset/range rebasing and stronger semantic dependency certification | Current fallback regressions only | Future work; not implemented |
| Production UVM, GCC 13, Linux arm64 | Needs real design inputs and separate platform builds | No such executed run in this environment | Unverified; no performance/support claim |

Parser counts establish the scope of parent-process parsing, not latency.
Timing uses the uninstrumented executable and excludes cold preparation and
diagnostic waiting from its interactive metric. Every step's count, failure
classification and small sample size must accompany any timing conclusion.
The original source archive is not required to satisfy this contract.
