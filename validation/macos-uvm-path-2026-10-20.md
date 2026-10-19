# Explicit UVM source selection on macOS

Phase 1 of the [frozen-analysis plan](../docs/development/frozen-analysis-plan.md)
adds `uvm.sourceDirectory` and optional `uvm.entryFile`. Source loading is
independent of `experimentalUvmPrefixCache`, which remains **false by default**.
No UVM is silently injected when the configuration is absent, and no bundled
library is introduced in this milestone.

A selected library already supplied by project inputs retains its original root
order and include wrappers. Otherwise the entry is prepended once. Native parser
metadata verifies exactly one `uvm_pkg` whose include ancestry reaches the selected
entry; conflicting providers, missing sources and ambiguous default entries are
reported. Literal directory/file APIs preserve spaces and wildcard characters.
Directory discovery prefers `uvm_pkg.sv`, then `src/uvm_pkg.sv`, then `uvm.sv`;
coexisting direct/nested package entries require an explicit choice.

The selected entry is part of driver, preprocessor and isolated diagnostic-worker
snapshot identity. The existing prefix experiment can capture explicitly named
entries and reuse a standalone library's native tree for preparation. Source and
macro changes still invalidate the compatible generation. Unsupported root order
or preprocessing boundaries retain native parsing. This is immutable syntax and
macro-checkpoint reuse, not saved compiler pointers or semantic-analysis reuse.

Watched entry changes reload the configuration while preserving unsaved user
text. A newly created missing entry/directory repairs failed configuration. Open
library buffers win over disk notifications, including alternate URI spellings.
A library alone supplies package dependencies to independent user files; a
complete compilation filelist continues to enforce its source boundaries.

The focused group passes **314 assertions in 9 cases**, including root/wrapper
order, injection, standalone consumers, actual cached-tree identity, custom names,
macro and entry replacement, missing-entry repair and disk/overlay transitions.
Full **CTest 632/632**, including SDK tests, **Python 15/15**, and standalone
macOS diagnostic transport **43/43** pass. The SDK patch reconstructs and verifies
all **708 files** against the pinned upstream base. Schema generation, formatting
and whitespace checks pass.

Real SPI/PSS/UBUS acceptance uses the preserved 590-file source inventory:

- 56 fresh processes cover 14 edit scenarios, cache off/on and warning filtering
  off/on. Ordered completion/definition/resolve responses, every completion
  candidate, stale handles and current diagnostics agree. Trace evidence confirms
  actual prefix reuse for every enabled scenario.
- 44 further processes compare unconfigured native inputs, explicit matching
  library selection, and insertion into a filelist with its library root removed.
  UBUS additionally tests automatic entry selection with its original wrapper.
  All outputs agree.
- Six processes compare native results with explicitly selected libraries under
  forced generation retirement on SPI, PSS and UBUS. Three headless LazyVim
  checks verify attachment of the candidate and the inheritance comparator.

Only outer diagnostic entry order is normalized, preserving duplicates and
ordered related information. Known native W12 UTF-16 remains excluded; negotiated
UTF-8 and CRLF scenarios are included. These are Mac arm64 functional checks,
not Linux or GUI visual acceptance. The [machine-readable record](macos-uvm-path-2026-10-20.json)
contains evidence hashes and the frozen candidate identity.

No new latency or memory improvement is claimed here. Insertion can parse the
original inputs before adding a missing library, so initial loading may do extra
work. Preparation remains per compatible compiler context/generation, rather than
once forever. Generic package/module/instance selectors, dependency registry,
status/refresh controls, macro-hover modes, optional bundled UVM and integrated
performance/memory acceptance remain later milestones. The overall goal stays
active.

Disposable source replays, restore verification trees and editor state are removed
once checks finish; current build/dependency caches, canonical workloads, frozen
binaries and compact evidence are retained for reuse. The user's running editor
and its files are preserved.
