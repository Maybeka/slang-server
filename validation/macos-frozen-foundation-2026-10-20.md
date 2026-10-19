# Frozen-analysis prerequisite: current RTL semantics after unsaved edits

This is the first correctness milestone of the
[frozen-analysis implementation plan](../docs/development/frozen-analysis-plan.md),
not completion of the overall feature. Baseline product is `53abb6b`; explicit UVM
path configuration, package/module/instance freezing, selectable macro hover and
optional bundled UVM remain subsequent work.

The new constructed RTL fixture has two parameterizations of the same module,
an external package type/constant, and a conditional generate branch. Before the
change, 68/72 assertions passed: four failed when an external package or parent
parameter changed without saving. Saved transitions and instance selection passed.
These are native fork observations, not an attribution to official upstream.

Edits now invalidate full-design semantic state. Hierarchy queries, active design
contexts and document semantic consumers refresh it lazily, using current editor
text. A burst of two changes does not eagerly rebuild the design; the first
hierarchy query observes the changed generate branch, and repeated queries retain
the same current context. Active selections survive refresh. Syntax-to-module
lookup is rebuilt with the design. A newly created design is fully elaborated
before driver analysis. Failed refreshes remain eligible for retry.

Semantic freshness is separate from diagnostic publication: rebuilding the AST
cannot make an old saved diagnostic pass cover pending work. A regression forces
a semantic query between an unsaved sibling-macro edit and diagnostic processing,
then verifies the resulting user error is still reported. The parent-local enum
binding regression now compares shallow type identity with the **current** full
compilation, rather than dereferencing a raw pointer from a previous generation.

Final focused tests pass **102 assertions / 6 cases**, including the four new RTL
cases; full **CTest 624/624** and **Python 15/15** pass. CTest includes the SDK suite.
Tests use a workspace-local canonical TMPDIR: an initial default macOS `/var` versus
`/private/var` temporary-path alias caused existing SDK path assertions to fail;
the final run uses one canonical path and does not change those assertions.

The [machine-readable record](macos-frozen-foundation-2026-10-20.json) records the
final source/binary hashes, commands and real-workload off/on results. The real
replay uses 14 existing cases with warning filtering off/on and cache off/on:
56 fresh processes, full public output comparisons and every candidate resolve.
Known original-native W12 UTF-16 remains excluded; negotiated UTF-8 and CRLF are
included. As previously documented, only outer diagnostic entry order is
canonicalized; duplicate counts and ordered relatedInformation remain intact.
All 56 processes pass. Headless LazyVim attaches the candidate and loads the
existing nvim-cmp comparator on SPI, PSS and UBUS; these are not GUI visual tests.
SPI reports an existing swapfile warning while its checks pass.

The unchanged restored SDK verifies all 708 manifest files. No SDK source change
or transport protocol change is part of this milestone. Formatting and whitespace
checks pass. Private baseline binaries, source corpora and working handoff remain
untracked. Compact logs and scripts are archived with hashes in the JSON record.

No new latency or memory result is claimed. Lazy refresh avoids eager work during
text synchronization, but full semantic consumers still require native analysis.
The forthcoming caches must measure the cost of that correct work. Existing UVM
experiments remain default off; no new freeze configuration is implemented here.
