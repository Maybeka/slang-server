# Mac module syntax reuse candidate

The default-off frozen-source experiment now accepts separate package and module
definition names and can resume complete source prefixes in explicit-filelist
full-design mode. Instance semantics are rebuilt; this milestone does not retain
elaborated instance results or implement exact instance-subtree selectors.

The initial tested executable SHA-256 is
`8fd7e1fb742e8b17081d199e852d1ab36ff1747ee312a7e2b9106bf986d0a2d8`.
The private candidate manifest and changed-source archive pin the implementation.
Later documentation updates do not change this executable.

## Correctness evidence

Full CTest passes 669/669, Python passes 16/16, and the frozen-source/hover/library
focused group passes 2,218 assertions in 81 cases. The separate full-design
diagnostic/module/order group passes 308 assertions in five cases. A fresh SDK
patch reconstruction verifies all 708 manifest files.

Small RTL tests cover different parameterizations, interface members, generate
removal, unsaved external package edits, closed dependency changes, current active
instance selection after source-context replacement, filelist order, configuration
and manual refresh. Syntax identity establishes actual reuse in the enabled arm.
Module-only isolated-worker hints are independently validated.

Six isolated LazyVim sessions also pass: native and enabled full-design mode for
each real project, with three unsaved width states, every completion resolve,
definition, matching-version diagnostics and PSS bind removal/repair. SPI/PSS
verify actual suffix resumes; UBUS explicitly allows its observed zero resumes.
These checks use an independent workload copy and isolated editor configuration,
cache and state, without changing the user's running editor. Mac file monitoring
is enabled; the checks run outside the sandbox that previously reproduced native
Neovim watcher failures even without the server.

The real SPI, PSS and UBUS full-filelist replay uses six fresh processes, with
three unsaved states each. All completion items are resolved: 129, 78 and 48
across the three states respectively. Off/on results match for completion,
resolve, definition, references, highlights, symbols, hover and versioned
diagnostics. PSS additionally removes and restores a real bind and its associated
hierarchical reference. No diagnostic filtering or deduplication is used; only
the outer diagnostic list order is normalized, preserving related information.

| Project | Prefix preparations | Suffix resumes | Charged retained bytes |
|---|---:|---:|---:|
| SPI | 1 | 3 | 30,102,396 |
| PSS | 1 | 3 | 30,106,979 |
| UBUS | 3 | 0 | 27,694,721 |

UBUS places its selected module and edited testbench in the same final source
root. Each edit invalidates the group; there is no edit-time prefix reuse in
this scenario. Source ordering was preserved. Retention and a resolved selector
alone therefore do not establish a useful performance gain.

## Diagnostic fixes needed for this replay

The original native path could report an unknown module at a legal PSS bind
after an unsaved edit because it published shallow diagnostics. Covered full-design
documents now receive diagnostics from the current full compilation, preserving
unrelated open-document diagnostics and their versions. Source-context replacement
republishes the new owner's results after the old owner releases its diagnostics.
Tests insert and repair a real user error and preserve that error through owner
replacement; the fix does not suppress user errors.

Generic-class diagnostic traversal also depended on hash-table specialization
order. Native off/off runs exposed different UVM cast-warning representatives.
The SDK now visits each newly discovered specialization batch in expanded type
name order, preserving the existing diagnostic coalescing policy. A regression
test exposes the previous missing warnings and verifies all 12 expected warnings.
The native SPI control and enabled comparison retain the raw warnings.

## Remaining acceptance

This is a correctness milestone, not a performance acceptance or publication.
This executable still needs first/continuous/fallback timing, long-edit
process-family memory and comparison with the accepted baseline.
Earlier selected-resolve speedups and bulk-resolve regressions belong to earlier
frozen executables and are not measurements of this candidate. Exact per-request
input certification remains required; its bulk-resolve cost remains open.

Exact instance selection with useful measured semantic reuse, optional explicitly
enabled bundled UVM and final integrated acceptance remain pending. All new
caches remain disabled by default; main has not been pushed for this milestone.

The subsequent [fallback investigation](macos-frozen-module-fallback-2026-10-20.md)
records separate executable identities, exploratory timings and the latest
671-test/real-project/editor gates. Its results do not relabel this initial binary.
