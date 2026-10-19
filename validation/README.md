# Validation records

## macOS 2026-10-20: frozen-analysis correctness prerequisite

The [RTL freshness report](macos-frozen-foundation-2026-10-20.md) records lazy
semantic refresh for unsaved external-package and instance-parameter changes,
with separate diagnostic-publication freshness. It is the initial milestone of
the [overall implementation plan](../docs/development/frozen-analysis-plan.md),
not completion of the planned caches or a new performance result.

## macOS 2026-10-19: official upstream completion reproduction and fixes

The [completion and macro-warning report](macos-completion-2026-10-19.md) pins an
independent official upstream build and reproduces the method-kind and member
ordering problems there. The fixes pass 620 CTest tests, 2,563 SDK cases, 15
Python tests, 56 real-project processes and three headless LazyVim checks. The
opt-in library macro warning filter retains explicit caller evidence; it does
not classify all semantic relevance. This is functional acceptance, with no new
performance claim. The UVM prefix experiment remains default off.

## macOS 2026-10-19: worker and retirement follow-up

The [follow-up acceptance report](macos-uvm-followup-2026-10-19.md) and
[machine-readable results](macos-uvm-followup-2026-10-19.json) evaluate unused
capture removal, retirement seeded with the incoming edit, and worker-local UVM
prefix reuse against `2903a08`. They retain cold-start, complete resolve,
fallback, natural retirement and separate parent/process-family RSS evidence.
Complete diagnostic content is preserved; equal-location/code diagnostic entry
order can change in two UBUS cases, as documented with the exact comparison.
The experiment remains **default off**. The initial report below remains a
separate historical result.

## macOS 2026-10-19: parsed UVM prefix reuse

The opt-in UVM prefix implementation is evaluated in the
[acceptance report](macos-uvm-prefix-2026-10-19.md), with
[machine-readable results](macos-uvm-prefix-2026-10-19.json) and hash-verified raw
evidence. It covers first and continuous edits, every completion resolve, native
fallback, natural context retirement and separate short/long memory replay on
SPI/PSS and official UBUS workloads. The report includes cold-start overhead,
unsupported-input penalties and retirement spikes. The experiment remains
**default off**. This evaluation supersedes the earlier method-overlay fallback
observations below for the new prefix experiment; the historical records remain
separate.

## macOS 2026-10-19: sparse method values and real UVM

Product commit `bd4cf37` passed CTest **609/609**, Python **15/15**, and
transport **35/35**. The 10,000-class full ordered native oracle, all resolves,
definition rebinding, stale handles, eight-mini/ninth-full parser counts,
held diagnostics, real-UVM oracle and SDK/dependency integrity also passed.
See [the report](macos-uvm-2026-10-19.json) for hashes, exact commands,
randomized schedules, paired block analysis and a verified raw archive.

The change retains only the fields supplied by resolution and keeps outer
completion values in one contiguous vector. Accepted edits publish a sparse
replacement vector, preserving native order without copying/scanning the outer
completion array. No source freshness check or unsupported-syntax guard was
removed. Both experimental flags remain **default off**.

New and previous binaries were interleaved in 20 randomized complete blocks per
fixture/query, with native and method arms in every block. **n=20** per timing
cell; p95 is withheld. All 480 fresh processes and 4,480 timed requests succeeded.
The previous binary is tested product `719359b`; the new binary is `bd4cf37`.
This comparison is separate from the earlier n=100 single-version experiment.

The synthetic fixture retains the same seven include edges. Values are medians
in ms. First-edit columns compare the **previous / new method** binaries; the
other columns compare **new native / new method**. All nine response sums include
first certification and ninth fallback. Whole sequence adds diagnostic waits.

| Classes / query | First edit, previous / new method | Nine responses, native / method | Whole sequence, native / method |
|---|---:|---:|---:|
| 5,000 / completion | 44.124 / 42.306 | 127.149 / 109.344 | 838.972 / 840.493 |
| 5,000 / definition | 44.082 / 42.196 | 126.724 / 108.709 | 838.777 / 841.501 |
| 10,000 / completion | 82.410 / 78.281 | 234.164 / 178.192 | 1090.543 / 1054.668 |
| 10,000 / definition | 82.077 / 77.546 | 229.778 / 176.292 | 1085.414 / 1052.537 |

First certification improves modestly and remains slower than native. The
10,000-class nine-response sums are about 23–24% below new native; whole sequences
improve about 3%. The report includes complete-block bootstrap intervals for
paired mean differences, including a comparison adjusted by the native-arm
version difference. These exploratory intervals are not median or tail bounds.
After adjusting by the native-arm version difference, the additional new-version
nine-response saving has a positive exploratory interval only for 10,000-class
completion; the other three intervals include zero. First-edit paired savings
are positive in all four sampled cells. Full source validation and response
construction still depend on project size.

Separate memory runs provide three independent observations per cell. The table
shows median Darwin `wait4` server-child `ru_maxrss` in MiB. This includes the test
status collection and does not measure simultaneous server + diagnostic worker +
client total, nor retained heap at a fixed version. Timings from these runs are
discarded; exact values and ranges remain in the report.

| Classes / query | Previous / new method peak | New native peak |
|---|---:|---:|
| 5,000 / completion | 98.14 / 90.86 | 79.36 |
| 5,000 / definition | 98.12 / 90.84 | 79.28 |
| 10,000 / completion | 180.42 / 166.14 | 140.94 |
| 10,000 / definition | 180.55 / 166.20 | 140.92 |

The real workload is the local educational SPI/APB/AHB UVM laboratory with the
original Makefile order, include directories and timescale, plus official UVM
1.2 commit `71f30a7473118b3cea582d7673c3af8e161fce71`. Its input inventory is
209 files / 77,665 lines; this counts the source inventory, not only reached
includes. Static compilation has 0 errors and 2 UVM-library warnings. No design
source changes, synthetic extra classes or planted certifiable methods were used.
This is an actual educational UVM project, not a production design or simulation
acceptance.

All ordered completion JSON, every resolve, definition and diagnostics match
across ten versions, both arms and both binaries. Physical handles are checked
against source-byte witnesses; unknown expansion handles remain exact opaque
values, establishing deterministic equality rather than macro freshness.
Project sources and full response bodies stay local; public evidence retains
hashes, timings and equality summaries.

The real-project table is **new native / new experiment**, n=20, medians in ms.
The first request includes `didOpen`; startup additionally includes initialize.
Its nine-edit sum is steps 1–9, so these first-query metrics differ from the
synthetic driver's prewarmed first-edit metric above.

| Query | First query response | Startup through first response | Nine edit responses |
|---|---:|---:|---:|
| completion | 13.120 / 13.069 | 103.975 / 104.494 | 456.314 / 461.574 |
| definition | 10.195 / 10.195 | 95.797 / 97.818 | 434.984 / 427.125 |

Every real-UVM edit declines at the configured source-loading flag and takes the
native full parse, as independently confirmed by the counted executable. Macros
and inheritance also remain outside the certificate contract. There is no
consistent UVM speedup. New-binary completion actually has an observed paired
mean nine-response **penalty of 27.105 ms** (exploratory 95% interval
4.504–59.179 ms); slow outliers are retained. Definition's interval includes
zero. This is an observed enabled-experiment cost, not proof that the new DTO
changes cause it: their accepted path is never reached here. The ninth UVM edit
is another ordinary fallback and does not test certificate reuse exhaustion. Keep the experiment default off.

These Mac runs use fresh processes without flushing filesystem caches, CPU
pinning or frequency controls. Builds, other tests and memory sampling were not
concurrent with latency replays. Other applications were not stopped. New code
has not been rerun on Linux or tested in a GUI editor.

## Earlier macOS 2026-10-19 follow-up

Product commit `719359b` passed CTest 609/609 (including SDK unit, tidy and
regressions), Python 14/14 with no skips, transport 35/35, and the 10,000-class
full native oracle, parser counts and held-diagnostic checks. The ordinary
binary hash, exact commands, environment, raw archive and paired block analysis
are in [the report](macos-2026-10-19.json). New code has not been rerun on Linux.

The table gives medians in ms, **native / method**, n=100 per arm/query/step.
Each nine-response sum is computed within a run and includes the first
certificate and ninth native fallback. Whole sequence additionally includes
versioned diagnostic waits. Two scales together contain 800 fresh processes
and 7,200 successful timed requests; no timeout, cancellation, crash or error.

| Classes / query | First response | All nine response latencies | Whole sequence with diagnostics |
|---|---:|---:|---:|
| 5,000 / completion | 15.385 / 45.122 | 132.515 / 113.318 | 854.448 / 856.734 |
| 5,000 / definition | 14.991 / 44.404 | 128.930 / 112.365 | 847.070 / 847.784 |
| 10,000 / completion | 29.686 / 83.197 | 235.871 / 189.332 | 1098.016 / 1073.928 |
| 10,000 / definition | 29.557 / 83.242 | 234.409 / 188.190 | 1097.253 / 1074.200 |

Nine-response median sums are 13–20% lower in these two synthetic fixtures.
Paired balanced-block mean savings have positive exploratory 95% bootstrap
intervals for both queries at both scales (50 blocks each). These intervals
refer to paired mean differences, not to the medians above or a tail bound.
At 5,000 classes the whole-sequence medians are effectively unchanged;
at 10,000 classes they improve only about 2%. First responses and fallback
remain slower. The separate 500-class n=6 experiment has no net response-sum
gain. Separate n=1 child-RSS observations are higher for the method arm; these
are not aggregate simultaneous parent/worker/client memory measurements.
Both experimental flags remain default off. A fixed seven-edge include graph
with synthetic classes is not production UVM or arbitrary large-graph evidence.

The first-edit hotspot was per-item repeated scope scanning. The implementation
now indexes identities once and shares immutable outer completion values across
edits, replacing all method-owned values before publication. It also rejects
inconsistent repeated disk witnesses; the new deterministic race failed before
the fix and passes afterward. Raw before/pilot samples remain separate and do
not establish a paired binary-version speedup or a Linux comparison.

## Earlier Linux records

The October 18 capability acceptance is in [capability-acceptance.md](capability-acceptance.md),
with executed results in [linux-2026-10-18.json](linux-2026-10-18.json).
The follow-up [independent clean-checkout report](clean-checkout-2026-10-18.json)
closes the earlier configuration-only limitation: commit `9dfd335` was actually
compiled from zero product object/library files in a separate checkout and build
directory (887 build steps, 576 seconds). It passed CTest 607/607, Python 11/11,
transport 32/32, all oracle/count/held-diagnostic checks and static checks, plus a
fresh execution of the documented 216-request benchmark recipe. SDK/dependency
integrity and unchanged source state were checked again afterward.

That build reused the installed GCC/CMake/Ninja/Python tools, existing frozen-version
Python test packages and four hash-verified dependency source directories. It
reused no prior product objects, libraries or binaries. It was not a fresh OS/tool
installation or a new `uv sync`. No existing Neovim executable was available, so
real headless LSP attach/initialize/query smoke was not run; the Lua configuration
contract passed and interactive LazyVim remains unverified. See the report's exact
paths, versions, commands and limitations.

The [independent raw evidence archive](artifacts/clean-checkout-2026-10-18-9dfd335.tar.gz)
is retained in Git (400,026 bytes, SHA256
`b0ad3cf7e47d2d3a0094738e6729e69bee21c3cbe575a8b0add44a120a2dfd7b`).

The compact [raw evidence archive](artifacts/linux-2026-10-18-b194c8a.tar.gz)
is retained in Git (480,843 bytes, SHA256
`53633240ed8561e4ffaa1042e9f1d7b62830b05b9dba7574dfc9381f064fa502`).
[delivery-2026-10-18.json](delivery-2026-10-18.json) records source/bundle hashes
and the Library upload's network blocker. No Library success was returned.
The `b194c8a` binary passed CTest 607/607, Python 11/11 (including the shipped
Lua configuration contract), transport 32/32, the native oracle, parser counts,
and held diagnostics on the multi-header fixture. At the time of that earlier
report, the independent checkout had only configured successfully. The separate
follow-up report above records its actual full build and acceptance.

The 500-class experiment uses five class headers, a nested include, two packages
and three ordered roots. Each cell below is a median in milliseconds, n=6 per
arm per step; the middle column is the range of the seven per-step medians.

| Query / arm | First edit | Edits 2–8 | Ninth edit (native fallback) |
|---|---:|---:|---:|
| Completion / native | 1.842 | 1.522–1.690 | 1.594 |
| Completion / overlay | 6.269 | 0.712–0.842 | 1.943 |
| Definition / native | 1.745 | 1.384–1.513 | 1.469 |
| Definition / overlay | 6.281 | 0.657–0.787 | 1.778 |

All 216 requests succeeded in 24 fresh processes. Accepted repeated edits have
lower medians in this fixture; certification and fallback cost more. Complete
sequence times include diagnostic waits and do not consistently improve. This
small synthetic experiment does not establish general or UVM responsiveness.
Both experimental flags remain default off. Raw outcomes and preregistered
binary/driver/fixture hashes accompany the report.

Use [the complete fork checkout/build/acceptance recipe](../docs/start/building.md),
including `SLANG_SERVER_INCLUDE_SDK_TESTS=ON`, for new environments.

`linux-2026-10-17.json` records the previous rebuild. The
`testedProductCommit` identifies the C++ product tree; a later evidence-only
commit may contain the report and additional executed test drivers. No historical
pass or performance sample is included as a current result.

The method experiment remains default off. The October 17 synthetic Linux measurement had slower medians
at every recorded step for completion and definition. The first edit and
the ninth-edit native fallback are recorded separately from edits 2–8. Each arm
has six samples per step; this is not production or UVM performance evidence.

Canonical commands (after SDK/dependency preparation and CMake configuration):

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure --no-tests=error -j4
python3 -m pytest tests/system tests/method_scope --binary "$PWD/build/bin/slang-server" -q
CXX=g++-14 bash tests/transport/run-diagnostic-transport-tests.sh
python3 tests/method_scope/check.py --binary build/bin/slang-server --output oracle.json
python3 tests/method_scope/count.py --binary build/bin/slang_server_counted --output counts.json
python3 tests/method_scope/held_diagnostics.py --binary build/bin/diagnostic_test_server --output held.json
python3 tests/method_scope/benchmark.py --binary build/bin/slang-server --plan plan.json --output timing.json --seed 20261017 --blocks 3 --plan-only
python3 tests/method_scope/benchmark.py --binary build/bin/slang-server --plan plan.json --output timing.json
```

The benchmark plan binds the executable, driver, fixture, and arm configuration
hashes before measuring. The counted executable is exclusively for scope
observations; timings use the ordinary executable. Logs and raw measurements
are retained outside Git, with their hashes in the report.

GCC 13 and Linux arm64 are unverified here. CMake used the existing Python 3.12.14
for source generation; the final Python/LSP suite and benchmark used a scoped
Python 3.12.6 matching the project's Python pin. Tool installation did not change
user settings or the system Python installation.
