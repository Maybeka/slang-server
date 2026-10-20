# Mac canonical source identity in live witnesses

Current candidate SHA-256:
`f9267d8f9b85dd4d0b522dd0426382bf77805cb1933680912ec3aaafa5d2f64e`.
All new cache experiments remain off by default. This is not final integrated
acceptance or publication.

## Corrections

The preceding directory-prefix prototype assumed that an ordinary, non-symlink
path already had native canonical spelling. Darwin's canonical resolution also
recovers actual spelling on case-insensitive filesystems: renaming `first` to
`FIRST` changes the recorded identity while the old spelling remains readable
and all bytes remain identical. Directory type checks alone missed that change.

Darwin now opens each distinct requested parent as a directory, obtains its
current resolved path with `F_GETPATH`, then closes the descriptor. The existing
invocation-local parent map avoids repeating a full resolution for every alias.
After closed reads, checks reopen directories and query fresh paths. Missing,
unreadable, unsupported or overlong paths use native weak canonical fallback.
Directory descriptors never persist across witnesses.

Review also exposed the same issue in the older ordinary-leaf shortcut: parent
canonical spelling does not prove the actual leaf spelling after `a.sv` becomes
`A.sv`. Darwin now obtains type and actual leaf name in a single no-follow
`getattrlist` query. Only an ordinary file whose name matches its recorded
canonical leaf can use the parent shortcut. Missing lookups retain their existing
proof; symlinks, unsupported attributes, errors or a name mismatch use full native
resolution. Returned attribute offsets and lengths are checked before accessing
name bytes. A local SDK layout check confirms the actual names before/after a
case-only rename. Positive closed files are still read byte-for-byte and negative
read outcomes are still checked. This is not a metadata-only content shortcut.

Other-platform prefix checks, four bounded read chunks and caller-side
cancellation/draining remain unchanged. No proof is retained across requests.

## Regression evidence

Case-only directory and file rename tests compare outcomes with native canonical
resolution and actual file availability. The directory test also renames after
closed reads on an insensitive volume. Private reproductions link the new test
objects against exact earlier SourceManager implementations, without modifying
the product SDK/build: cdb4 fails two directory assertions; directory-only 4d88
fails the new ordinary-leaf assertion. Both old-source failures are preserved.
With both corrections, 477 live-witness assertions in five cases pass.

A second private build compiles the current SourceManager without
`SLANG_USE_THREADS`, links the current SDK test objects, and passes the same 477
assertions. This verifies the serial source-witness branch, not a whole-SDK
non-threaded build. Temporary reproduction libraries/objects/executables were
removed only after their consumers finished; source, commands, hashes, failure
logs and receipts remain. Reusable build/dependency/editor/workload caches and
frozen candidates remain retained.

## Current verification

After rebuilding all targets, CTest passes 685/685 (including SDK tests), Python
16/16, focused server checks 7,557 assertions in 97 cases, and transport 43/43.
A clean SDK patch reconstruction verifies all 708 manifest hashes and preserves
the four historical added SDK files. Current patch SHA-256:
`c125cb5ec7bfee3e26f080744a8d9640f08af543ece9178508dd5fffa59abfff`.
The SDK gitlink remains deliberately unstaged.

Exact-candidate full-design language replays pass all six native/enabled
processes across five unsaved states, with two forced retirements per enabled
process. Exploration comparisons pass all twelve processes on SPI/PSS/UBUS,
with warning suppression off/on and one forced retirement per enabled process;
complete responses, every resolve, stale handles, inactive regions and latest
diagnostic states match native. Six isolated LazyVim sessions pass with two
enabled retirements and zero disabled retirements. The live user editor is
untouched. Fresh confirmation and separate family-memory results for this exact candidate
are below; preceding cdb4 data remains separate. Historical measurements show a
useful read-batching mitigation versus prior/on, but large native-relative
all-resolve regressions and an approximately 11% first-response regression. They
do not close the performance gate. See
[the historical measurements](macos-frozen-witness-batching-2026-10-20.md).

Remaining work includes lowering per-request alias/input proof costs, selected
and every-resolve comparisons, accepted older-UVM baseline comparisons, repeated
1,000-edit parent/family RSS and footprint, final SDK reconstruction/build/tests,
formatting, remote identity and an ordinary main push. No push occurred here.

## Exact-candidate twenty-pair performance

All 120 sequential fresh processes pass: 20 randomized paired blocks for PSS and
UBUS, with current native/off, prior dependency-prefix/on and current/on arms.
The old UVM-prefix flag is off in all arms. Each process includes setup/preparation,
initial query, two alternating unsaved edits, cached queries, every resolve and
complete matching-version diagnostics. PSS resolves 171 items total, UBUS 393.
No builds, tests, editors, profiling or memory sampling overlap these timings.
Filesystem caches remain warm; every observation is retained.

Milliseconds below are medians; the whole-session metric starts at process launch
and ends at final complete diagnostics, excluding shutdown.

| Case / metric | Native off | Prior on | Current on | Reduction vs prior |
|---|---:|---:|---:|---:|
| W07-pss-multidoc / Whole session | 806.71 | 1867.97 | 1482.83 | 20.6% |
| W07-pss-multidoc / Startup through first response | 107.52 | 119.79 | 120.47 | -0.6% |
| W07-pss-multidoc / First edit | 53.21 | 44.07 | 36.60 | 17.0% |
| W07-pss-multidoc / Every resolve sum | 65.07 | 1284.82 | 930.96 | 27.5% |
| W08-ubus-task / Whole session | 784.43 | 2591.05 | 1943.34 | 25.0% |
| W08-ubus-task / Startup through first response | 101.24 | 110.73 | 108.10 | 2.4% |
| W08-ubus-task / First edit | 53.71 | 32.48 | 27.22 | 16.2% |
| W08-ubus-task / Every resolve sum | 158.44 | 2168.59 | 1536.33 | 29.2% |

Whole-session paired mean savings versus prior/on have percentile-bootstrap 95%
intervals of 340.78–449.44 ms (PSS) and 602.39–728.14 ms (UBUS), with 20,000
paired resamples. Current whole-session medians improve 20.6%/25.0% versus prior.
Against native, however, whole medians regress 83.8%/147.7%; paired mean intervals
are negative, -706.99 to -631.79 ms and -1171.65 to -1096.28 ms. Startup through
first response regresses approximately 12.0%/6.8% versus native. First-edit latency
improves, but cannot substitute for these whole-session findings. Twenty samples
do not establish a tail guarantee. This is a verified mitigation, not a pass of
integrated performance acceptance. Normal selected-resolve sessions must still
be measured separately, and the all-resolve regression remains explicit.

## Separate exact-candidate short family memory

Twenty edits, three repeats per case/arm, 27 processes all pass. Sampling starts
before spawn and stops/joins before orderly close. It measures the parent and
simultaneous diagnostic-child family every 20 ms; every row observes two family
processes. Python is excluded, family sums count shared pages, and sampling can
miss peaks. Values below are median startup-inclusive peaks in MiB. They do not
establish long-session bounds and are not latency observations.

| Case / arm | Parent RSS | Family RSS | Parent footprint | Family footprint |
|---|---:|---:|---:|---:|
| W01-compare / native-off | 161.58 | 243.64 | 127.22 | 200.17 |
| W01-compare / previous-on | 68.23 | 173.80 | 55.95 | 127.78 |
| W01-compare / candidate-on | 68.38 | 173.95 | 56.16 | 139.52 |
| W07-pss-multidoc / native-off | 161.92 | 260.59 | 127.45 | 198.89 |
| W07-pss-multidoc / previous-on | 68.39 | 174.38 | 56.05 | 125.95 |
| W07-pss-multidoc / candidate-on | 68.78 | 174.64 | 56.45 | 132.63 |
| W08-ubus-task / native-off | 176.55 | 272.64 | 142.11 | 228.78 |
| W08-ubus-task / previous-on | 93.81 | 214.38 | 81.42 | 149.53 |
| W08-ubus-task / candidate-on | 93.72 | 214.39 | 81.38 | 149.64 |

Current versus prior family RSS is essentially unchanged in these short runs;
median parent footprint changes by less than 0.5 MiB. Family footprint medians
change by about +9.2% (SPI), +5.3% (PSS), +0.1% (UBUS); this variance must remain
visible rather than treating bounded syntax bytes as a process-memory guarantee.
Compared with native, current median family RSS is lower by about 28.6%/33.0%/21.4%.
All sampled source-cache budgets pass. Three repeated 1,000-edit sessions and
post-retirement/refresh coverage remain required before memory acceptance.

The measurement driver completed timing, offline paired analysis and then memory
sequentially, with all three terminal return codes zero. Exact source/binary
manifests, raw results and raw compressed memory traces remain under the
workspace's untracked area. No main push or goal completion is implied.
