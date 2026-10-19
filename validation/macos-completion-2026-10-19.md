# Class completions and library macro warnings: macOS 2026-10-19

The two completion problems reproduce in **official upstream**, independently of
this fork's performance baseline. An unmodified archive of
[`hudson-trading/slang-server` at `d5116cc`](https://github.com/hudson-trading/slang-server/commit/d5116cc1ed305ffcd49c2500dc97a48563678f4d)
was built separately with its exact pinned slang, reflect-cpp and CTRE sources.
Release uses Homebrew LLVM and disables mimalloc to use available offline
dependencies; no product source patch was applied. The official binary SHA256 is
`e48088b3b9f4ea5e5e28442e04c3df7e7197c1e6c1f18d1927b757f304b55b57`.

| Case | Official upstream | Fixed candidate |
|---|---|---|
| Inherited inline / extern methods | Property (10), no callable insertion | Method (2), callable insertion and resolve |
| Own extern method | Property (10) | Method (2) |
| Own inline method | Function (3) | Method (2) |
| Dot member order | Inherited members precede own members; no sortText | Own class, direct base, then more distant bases |

Slang exposes inherited members through transparent wrappers. Unwrapping those
symbols and recognizing method prototypes repairs both kinds and resolved
documentation/snippets. Inheritance depth is encoded in class-member `sortText`.
The optional nvim-cmp comparator in the shipped LazyVim example respects depth
before fuzzy score or kind, preserving the existing comparators within a depth.
Parameterized receivers resolve against their actual specialization. Ordinary
class items retain logical resolution across edits, covered by a stale-item test.

## Conservative opt-in UVM warning filtering

Use the existing `--suppress-macro-warnings <library-path-glob>` option, scoped to
the UVM source directory. Without the option diagnostic behavior is unchanged.
The previous native implementation could suppress narrowing warnings caused by
user macro arguments along with macro-body warnings. The restored SDK now keeps
warnings with argument-location/range evidence or related notes outside the
filtered library. Direct and nested user arguments, user-defined macros and real
errors have regression coverage. Warning promotion via `-Werror` retains native
suppression semantics.

This is a source-based conservative rule, not a complete semantic relevance
classifier. UBUS W08/W09 warnings fall from **24 to 19** per state; ambiguous and
caller-related warnings remain. No broad width/sign warning category is disabled.
The private SPI/PSS/UBUS launcher enables `--suppress-macro-warnings uvm/**` and
uses the reviewed candidate; the user's global Neovim configuration is unchanged.

## Acceptance

Candidate SHA256:
`562725e1f803430ae458308de84b358f7b25fffbd2d3ae13bed9cf71959be38e`.

- CTest **620/620**, SDK **2,563 cases / 23,731 assertions**, Python **15/15**.
- Focused new regressions: **220 assertions / 4 cases**.
- **56 fresh real-project processes**: 14 cases × macro filtering off/on × UVM
  prefix cache off/on. All 28 native/cache pairs agree on complete public outputs,
  every candidate resolve, repeated completions, current diagnostics, definitions
  and selected stale handles. Error insertion and repair remain observable.
- Known existing W12 UTF-16 oracle failure is excluded; negotiated UTF-8 and CRLF
  cases are included. Outer diagnostic entries are canonicalized for known UBUS
  equal-location/code ordering; relatedInformation order and duplicates are kept.
- Real headless LazyVim attachment and the loaded nvim-cmp comparator pass on
  SPI, PSS and UBUS. This does not claim a GUI visual test. SPI reported an existing
  swapfile warning while attachment and comparator checks passed.
- SDK restoration applies to a clean pinned archive and verifies **708 files**;
  all four preexisting added SDK files are preserved. C++ formatting and both
  parent/SDK whitespace checks pass.

The UVM prefix experiment remains **default off**. This is functional acceptance;
no new performance or memory claim is made. Earlier performance reports remain
separate historical measurements. No Linux rerun was performed.

The [machine-readable record](macos-completion-2026-10-19.json) pins commands,
source/binary hashes and workload summaries. Its hash-verified
[evidence archive](artifacts/macos-completion-2026-10-19.tar.gz) contains compact
logs, summaries and scripts. Private source corpora, upstream checkout, build
objects and binaries are excluded. Scripts retain local paths and references to
the earlier private workload/oracle helpers.
