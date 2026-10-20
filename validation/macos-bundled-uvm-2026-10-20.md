# Explicit bundled UVM on Mac

The tested executable SHA-256 is
`0cf6e751d4c41151d5751bbc1599db60bf96f087e099d8c2b8cffb1ca32d11df`.
The candidate source manifest/archive pins this implementation. This milestone
completes optional source packaging and offline selection; it does not complete
integrated performance/memory acceptance or authorize claiming overall success.

## Shipped source and activation

The kit is unchanged UVM 1.2 from the existing clean Accellera checkout at
`71f30a7473118b3cea582d7673c3af8e161fce71`. Its original 155 source files plus
`LICENSE.txt` and `NOTICE.txt` total 2,566,241 bytes. The repository manifest adds
version, upstream commit and SHA-256/size for every upstream file. CMake checks
all hashes and the complete file inventory before staging resources. Formatting
and Git newline conversion exclude this third-party tree so its bytes remain
verifiable. Original source headers and notices remain unchanged.

`uvm.useBundled` defaults to false. It is mutually exclusive with sourceDirectory
and entryFile; the selected bundled entry is fixed to src/uvm_pkg.sv. An existing
matching selected package is retained; a different project provider is rejected
rather than silently mixed or replaced. Normal default project inputs and
explicit user libraries do not require bundled resources. No cache or macro
flags are enabled implicitly, and no runtime network download exists.

CMake build/install uses bin plus share/slang-server/uvm/1.2. Release archives
retain the existing root executable layout expected by editor installers, with
resources/uvm/1.2 beside it. Lookup is anchored to the actual executable rather
than the workspace. Moving either complete layout preserves selection; copying
just a binary without resources leaves ordinary project/user-library loading
available and reports an actionable error only when the kit is requested.
Effective library-directory aliases remain in exact entry-resolution witnesses:
a same-byte symlink retarget cannot silently change the selected provider.

## Evidence

- CTest: 682/682; Python: 16/16; focused group: 6,646 assertions in 94 cases;
  transport: 43/43. The four new dedicated cases pass 3,985 assertions including
  full native semantic diagnostics, cached/native selection, conflicting choices,
  default-off behavior and directory-symlink retargeting.
- The actual CMake install was moved to a new path containing spaces. A separately
  relocated release layout also passed. Each layout runs explicit-path and
  bundled selection with generic caching off/on: four processes, four unsaved
  states per process. Complete ordered completion lists and every resolve
  (120 items per state), definitions, hover, symbols and current-document
  diagnostics match after removing only opaque completion data and normalizing
  outer diagnostic order. An invalid user task appears and disappears after
  repair. Manual refresh preserves the current public completion result.
- Both enabled arms record three real suffix resumes and 25,567,841 charged
  retained bytes within the configured 64 MiB. Off arms retain zero cache bytes.
  These counters are not RSS and do not demonstrate long-session memory bounds.
- A binary with no resource tree successfully handles default project UVM and an
  explicit user UVM. Only an explicit bundled request reports missing resources.
- CMake inventory checks accept the pinned tree and reject separately tested
  modified-source, missing-license and extra-file cases. The test fixture is
  restored after each negative case; production sources are never modified.
- Six real native/enabled SPI/PSS/UBUS processes retain their original explicitly
  selected libraries. All five unsaved states match for hierarchy, completion and
  every resolve, definition, hover, references, highlights, symbols and complete
  current-version diagnostics, including user-error insertion/repair. Each enabled
  project records eight cross-edit hierarchy hits. This proves compatibility with
  existing library choices; it is not a migration of those projects to UVM 1.2.
- Six isolated LazyVim sessions on the same executable pass real unsaved edits,
  matching-version error/repair diagnostics and hierarchy checks with Mac file
  monitoring. User editor configuration and state remain untouched.

All installation checks use local files without network acquisition. Windows and
Linux runtime branches are implemented but this report claims execution on Mac
only. This milestone adds no new timing result; the preceding instance report's
measurements remain pinned to its own executable.

## Remaining integrated acceptance

The experimental caches stay off. The mixed instance-session results and older
PSS/UBUS class bulk-resolve regressions remain open. Next gates are repeated
1,000-edit parent/family RSS and full-design context lifetime/fallback/refresh,
followed by 20 paired baseline/off/on timing, final SDK restoration and complete
validation before a normal main push. No push occurred for this milestone.
