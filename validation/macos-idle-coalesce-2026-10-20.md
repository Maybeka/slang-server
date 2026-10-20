# Mac idle coalescing and bundled resource staging

Candidate SHA-256: `7e67b0131f9e65b1692f2c7382d6ddf4f7b51b1b3f32d9b524af0dc503cdd989`. SDK patch is unchanged from d678. All new caches remain disabled by default.

## Changes and correctness

The protocol worker waits up to two milliseconds when its foreground queue becomes empty, allowing response-triggered follow-up queries to arrive before starting non-interruptible native parser/binder work. Queued requests take priority; arrival wakes the condition variable. Cancellation, synchronization, pending diagnostic generations, current-input checks and complete diagnostic publication remain unchanged. This is a scheduling window, not an input freshness interval.

An isolated before-change log reproduces native full-design diagnostics starting immediately after a query response. A subsequent repeated hierarchy request waits 54–62 ms before its handler starts; the handler itself finishes immediately. The cancelled diagnostic work is retried after the following response. The two-process reproduction preserves hierarchy equality and logs both binaries separately; it is not formal timing evidence.

Bundled UVM assets now stage as target dependencies using each executable's actual output directory. Configuration expressions and target-specific overrides are resolved by CMake at build time. Every target build stages assets, even without relinking. The verification step still checks the source manifest before staging. Tests and diagnostic/counter executables receive the same layout.

The initial configure-only fixture disproved a suspected plain-relative-path failure: that path already worked. It reproduced a real configuration-expression failure (`$<CONFIG>/bin` staged to a literal directory). Fixed executable fixtures pass relative/configuration-expression/target-override cases and restore removed assets on a no-relink rebuild. These small fixtures test staging, not full installed-product relocation.

The rebuilt candidate passes 685/685 CTest (including SDK), 16/16 Python, 43/43 transport and 7,626 focused assertions in 103 cases, including the protocol scheduler tests. The current candidate also passes six full-design five-state public-response processes with two observed retirements per enabled project, plus twelve exploration processes covering every resolve, stale handles, all latest diagnostics/inactive regions and warning suppression off/on. The initial forced-limit=2 run failed its at-least-two-retirements assertion after observing one; the corrected original forced-limit=1 setup passes without weakening assertions. Failed artifacts remain preserved. Final editor/install/memory/performance/publication gates remain open.

## Exploratory paired RTL pilot

All 180 fresh sequential processes pass: three paired blocks, three real projects, drained/burst, ten before/after arms, ten edits. Every state checks complete hierarchy equality across arms, current diagnostics where required, and generic selection/hit/budget assertions after timing. No other workloads ran during timing. Three observations per cell are exploratory; no confidence, tail or final performance claim follows.

Median whole-session milliseconds. UVM is the dedicated legacy flag alone; generic means exact instance freezing; hybrid means both flags enabled, not a promise that dedicated continuation ran.

| Project | Mode | Native before / after | UVM before / after | Generic before / after | Hybrid before / after |
|---|---|---:|---:|---:|---:|
| spi-block | drained | 1888.02 / 1664.78 | 1520.39 / 1316.05 | 1660.19 / 1656.12 | 1593.43 / 1661.75 |
| spi-block | burst | 1577.29 / 837.24 | 1218.24 / 505.00 | 746.14 / 784.06 | 756.76 / 756.38 |
| spi-pss | drained | 2523.82 / 2304.43 | 2137.28 / 1945.65 | 2225.63 / 2277.63 | 2225.24 / 2276.71 |
| spi-pss | burst | 2268.18 / 1441.58 | 1868.23 / 1077.79 | 911.69 / 912.94 | 899.68 / 937.91 |
| official-ubus | drained | 2010.19 / 1739.24 | 1642.98 / 1371.65 | 1619.13 / 1606.28 | 1610.94 / 1639.44 |
| official-ubus | burst | 1718.23 / 885.65 | 1333.31 / 494.12 | 710.80 / 713.57 | 675.91 / 715.24 |

Native/UVM repeated-scope medians fall from about 50–54 ms to about 0.4–0.5 ms. Generic paths retain their approximately 3–5 ms input-certification cost and show no consistent whole-session gain in this small pilot. Coalescing addresses a separate native scheduling regression; it does not solve generic certification overhead.

## UVM strategy evidence

A separate nine-process, trace-only probe passes on all three projects, comparing dedicated UVM, generic instances and both switches. Dedicated UVM reports one captured/built prefix and three actual prefix reuses per process. Both-switch cases capture/build the dedicated prefix but report zero dedicated reuses: generic continuation currently takes precedence. The trace probe's elapsed times are not used as performance evidence.

The user permits UVM specialization when generic unification adds cost. Keep the dedicated route available, evaluate other stable scopes separately, and measure an actual strategy composition before promising combined gains. Do not silently discard user selections or weaken dependency/macro/diagnostic/budget checks. UVM-only users can explicitly enable `experimentalUvmPrefixCache` while leaving `experimentalFrozenSources` off. Mandatory UVM unification is not an acceptance requirement.

Evidence files under `untracked/frozen-registry-2026-10-20/`: `idle-coalesce-candidate.json`, `idle-coalesce-acceptance.json`, `idle-coalesce-rtl-pilot.json`, `idle-coalesce-rtl-pilot-summary.json`, `idle-coalesce-uvm-routes.json` plus trace TSVs and stderr logs; `bundled-runtime-layout-reproducer/result.json` and `bundled-runtime-layout-fixed/result.json`. Source/archive and raw-driver hashes are retained. No commit or push yet.

## Independent reconstructed SDK result

All 708 source hashes match the reconstruction manifest before and after the
independent build. The first hidden-visibility SDK build succeeds but its tests
fail 28 cases because option identity behavior differs on Mac. Applying the same
static-target visibility properties already used by the production server
(`CXX_VISIBILITY_PRESET=default`, `VISIBILITY_INLINES_HIDDEN=OFF`) via a deferred
CMake validation hook produces a full pass: 24,882 assertions in 2,570 cases.
No reconstructed source bytes changed, and no tests were filtered or excluded.
The original failure, both configurations, compiler flags and hashed phase
receipts are retained. This closes the current independent reconstructed-SDK
build/test gate for the production Mac settings, not an identical-binary-path
claim or an assertion that the SDK's unmodified standalone hidden build works.

Receipts: `alias-plan-fresh-sdk-build.json` (initial failure) and
`alias-plan-fresh-sdk-macos-build.json` (same-source production-settings pass).
The fresh builder initially had a bracket syntax error; its failed log is kept,
and the corrected script was executed. Preparation alone was never counted as
a successful build.
