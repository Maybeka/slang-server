# Full-design module fallback investigation on Mac

This is an exploratory performance and correctness milestone. The experiment
remains off by default. Instance-subtree semantic reuse, optional bundled UVM,
accepted-baseline comparison and final integrated acceptance remain open.

## Workloads and evidence

Three separate executable identities prevent relabeling older measurements:

| Candidate | SHA-256 |
|---|---|
| Full-design diagnostic/module candidate | `8fd7e1fb742e8b17081d199e852d1ab36ff1747ee312a7e2b9106bf986d0a2d8` |
| Early literal macro decline | `5d9706629ad3022b207907c2ed5a83ff1b7ab76e5f66b43e0456de183cda2c91` |
| Additionally avoid no-suffix preparation | `c28ee0061bc8bd36b8e049caaae24e2d0ba47fef95ad0bb2fea9fbd186b876fb` |

54 initial processes compare off/on, followed by 81 sequential randomized
reference-paired processes comparing off, early decline on and the preceding
candidate on. Each project/scenario/arm has three samples. A further 27 UBUS
processes compare off, no-suffix on and preceding early-decline on. All 162
processes pass their semantic checks. These are fresh processes with warm OS file
caches, not a cold-filesystem experiment or the final 20-pair acceptance.

The original ordered SPI/PSS/UBUS filelists are retained. Selected and fallback
scenarios perform six alternating unsaved edits, hover/repeat, completion,
selected resolve, definition and matching-version diagnostics. Bulk scenarios
perform two edits and resolve every RTL completion item at all three states.
Fallback prepends a literal undefinition/redefinition of `UVM_MAJOR_REV` to the
editable root. Status is collected after timing. Builds, tests, editors, tracing
and RSS sampling do not overlap primary timing.

The bulk lists here contain RTL members. Earlier UVM-class bulk-resolve
regressions use different, larger completion lists and remain unresolved. These
results cannot replace those measurements. Cached-repeat response time can also
include intervening diagnostic work and is not a pure certification microbenchmark.

## Early native decline

Previously, a direct retained-macro override first certified/prepared the prefix,
attempted suffix composition, then rejected it and parsed natively. A negative-only
literal check now declines before preparation when a compatible retained view
already identifies the macro. Includes and indirect changes still receive the
existing complete validation. Comments or inactive text can cause a conservative
native miss. No successful reuse bypasses current-input certification.

First-edit medians in the 81-process experiment, in milliseconds:

| Macro fallback | Experiment off | Earlier candidate on | Early decline on |
|---|---:|---:|---:|
| SPI | 97.36 | 114.22 | 103.33 |
| PSS | 149.63 | 166.85 | 155.90 |

All seven states in every enabled SPI/PSS fallback session take the early decline;
prefix preparation falls from one to zero. Median complete-session time falls
from 1468.00 to 1377.92 ms on SPI and 1912.32 to 1817.11 ms on PSS. With only
three samples per cell, these establish a promising mechanism, not stable tail
latency or final performance acceptance.

## Groups without an editable suffix

UBUS places its selected module in the same final source root as the edited
testbench. Rebuilding a complete prefix after every edit cannot provide suffix
reuse. Full-design mode now parses such groups natively. Multi-root groups of
this kind also avoid staging an initial UVM view that cannot be consumed.
Ordinary unchanged documents retain their native syntax normally; standalone
single-root library staging and useful SPI/PSS suffix reuse remain available.

The 27-process UBUS follow-up gives these first-edit medians, in milliseconds:

| Scenario | Experiment off | Earlier early-decline on | No-suffix on |
|---|---:|---:|---:|
| Selected resolve | 106.03 | 123.05 | 116.80 |
| Every RTL resolve | 105.10 | 119.26 | 110.03 |
| Macro fallback | 110.41 | 119.08 | 109.50 |

The selected-resolve first edit remains about 10% slower than off in this small
sample. Complete-session medians are 1572.40/1518.39/1458.83 ms for off/earlier/new
in selected resolve, 1193.22/950.21/963.35 ms in bulk, and
1569.93/1487.33/1423.14 ms in fallback. No claim of prefix-reuse speedup is made
for UBUS: it has zero resumes. Exact certification still has a cost.

Its real correctness replay now has zero preparations, zero native seeds and
zero charged retained cache bytes, compared with three preparations and
27,694,721 retained bytes previously. These are cache-accounting values, not RSS
measurements. SPI/PSS each retain three actual resumes and unchanged public output.

## Current correctness and remaining work

The latest candidate passes full CTest 671/671, Python 16/16 and the focused
2,416 assertions in 83 cases. The dedicated macro/preparation group passes 469
assertions in six cases. Six real native/enabled processes match all public
query results and current diagnostics, including PSS bind removal/repair.
Six isolated full-design LazyVim sessions also pass on this exact latest binary,
with real file monitoring, unsaved widths, all RTL resolves and matching-version
diagnostics. The user's running editor/configuration remains untouched.

The SDK patch and manifest remain unchanged from the verified 708-file
reconstruction. Source and executable archives, raw runs, schedules, helper
versions and test logs are retained privately. The first new unit-test run exposed
a relative-path mistake in its synthetic include fixture; correcting the fixture
to absolute in-memory paths makes the indirect-override checks exercise the
intended header. That initial failure is retained separately.

Next priorities are exact instance-subtree selection with useful measured semantic
reuse, the still-open UVM-class resolve cost, optional fixed bundled UVM, then
integrated 20-pair baseline/off/on timing and repeated 1,000-edit family RSS,
retirement/refresh and full publication gates. A hierarchy value-cache prototype
must prove that permitted unrelated edits cannot affect elaboration or returned
locations; identifier-name heuristics alone are insufficient. Deferring work to
diagnostics must not be advertised as a complete-session saving.
