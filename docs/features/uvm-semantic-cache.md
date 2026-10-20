# Trusted UVM semantic reuse

`experimentalUvmSemanticCache` prepares the selected UVM package once and shares
its elaborated package, fixed classes, methods, built-in class identities and
source storage across subsequent user compilations. This is an opt-in in-process
semantic cache. Explicitly selecting the bundled UVM additionally loads the
shipped preparsed syntax database described below; this does not require enabling
the semantic experiment. Bound semantic objects are still process-local.

```json
{
  "uvm": {
    "sourceDirectory": "/path/to/uvm/src",
    "entryFile": "uvm_pkg.sv",
    "semanticMaxMiB": 256
  },
  "experimentalUvmSemanticCache": true
}
```

Choose the actual project entry, including `uvm.sv` for projects using that
wrapper. The existing explicit-path or explicitly selected bundled UVM loading
rules apply. Leave `experimentalFrozenSources` and `experimentalMethodScopeOverlay`
disabled; combining these experiments is rejected. The older
`experimentalUvmPrefixCache` switch is unnecessary in this mode.

## Immutable library contract

The provider's UVM files, transitive includes and external dependencies are
assumed fixed for a generation. Put external UVM macros in the project command
line/configuration. Ordinary user code and user-dependent generic specializations
remain owned by each current compilation and update with unsaved edits. The
server retains the library's semantic objects rather than rebuilding UVM for each
consumer. User-owned specializations are deliberately not added to the retained
library arena.

Library file-watch events do not refresh this trusted generation. After changing
the provider's library, execute `slang.rebuildUvmLibrary` or reload the project
configuration. A successful rebuild replaces the generation and preserves unsaved
user text. If loading a replacement context fails, the previous context remains
active; semantic preparation errors can instead select native analysis, reported
in status. Existing analysis handles retain the source storage they need until released. Context retirement
reuses the prepared library while retiring mutable user state.

For Neovim, execute the rebuild command with:

```lua
vim.lsp.buf.execute_command({ command = "slang.rebuildUvmLibrary", arguments = {} })
```

Do not edit library files while expecting automatic freshness in this mode.
Disable the experiment to restore ordinary library freshness behavior.

## Status, limits and diagnostics

`slang.uvmLibraryStatus` reports preparation, generation, consumer imports,
`astBytes`, `chargedBytes`, fixed source-file count and a reason when preparation
is unavailable. The default is disabled. Unsupported package boundaries,
preparation errors or a retention charge above `semanticMaxMiB` use ordinary
analysis. The limit covers the prepared semantic arena and charged syntax/source
storage; it is not a process RSS limit.

Diagnostics use the shared semantic owner in the server process in this mode.
This avoids rebuilding the library in a separate diagnostic worker, but diagnostic
work can occupy the server request loop. User diagnostics must remain current.
Library diagnostic filtering is configured independently; enabling reuse does not
itself suppress warnings. Queries inside library sources use the prepared owner.

See the [Mac validation report](../../validation/macos-uvm-semantic-reuse-2026-10-20.md)
for measured latency and memory, including preparation and reconstruction costs. Syntax-only cache measurements
do not establish a benefit for this semantic cache.

## Persistent UVM syntax data

```json
{ "uvm": { "useBundled": true }, "experimentalUvmSemanticCache": true }
```

The distribution includes UVM 1.2 sources and **only** `preparsed/default.cst`.
The build generates this database with default parse options, verifies a complete
syntax round trip and binds the restored package to check semantic errors.
Default bundled UVM does not require runtime source parsing, including its first
startup. Missing, damaged or incompatible default data is a configuration error;
the server does not silently parse the bundled sources instead.

Other parse configurations, including command-line UVM macro definitions, build a
project-local `.slang/cache/uvm/*.cst` on first use. Explicit `uvm.sourceDirectory`
installations also use project caches when `experimentalUvmSemanticCache` is on.
The key includes the SDK/data identity, selected library root and entry, current
directory, complete parse options and ordered include paths. The full serialized
configuration is checked as well as the filename hash. A matching cache survives
server restarts. Damaged or incompatible project cache envelopes are regenerated.
Add `.slang/cache/` to the project's ignored paths; these files are not source.

Library contents and dependencies are trusted fixed. Updating them requires
`slang.rebuildUvmLibrary`, which removes the current project's syntax cache and
constructs a new semantic generation. Changing parse configuration selects another
cache. Other configurations' reusable caches remain available; remove the project's
`.slang/cache/uvm` directory when those configurations are no longer needed.
Concurrent writers use distinct temporary files and publish by rename.

This data includes syntax, values, macros, guards, source/expansion locations,
parser previews and parse warnings. Restored bundled paths and `__FILE__` values
relocate to the installed sources. Project caches can retain absolute paths to
fixed dependencies outside the library directory and are specific to that project.
The workspace index consumes restored library metadata.
Custom entries containing additional packages or compilation-unit declarations
retain syntax reuse but use ordinary semantic analysis, preserving cross-package
nominal type identities. They do not activate the single-package semantic owner.

Syntax data does not persist bound semantic objects. First semantic binding still
occurs in each server process; enabling `experimentalUvmSemanticCache` shares that
owner across edits and normal consumer retirement. All experiments remain off by
default. Ordinary explicit source-directory use without semantic caching keeps
its existing source-based behavior.

`slang.uvmLibraryStatus` separates `shippedPreparse` / `shippedProfile` from
`projectCacheHit`, `projectCachePath`, `syntaxParseMs`, `archiveWriteMs` and
`archiveLoadMs`, and from the semantic owner's `prepared` state. Timings describe
this driver's preparation; retirement can reuse the owner without disk access.
Diagnostic work stays in the parent process when precompiled UVM is loaded, so a
worker cannot reparse that library. Restore errors are reported; manually rebuild
a project cache if its syntax payload cannot be restored.

CMake installation and release archives must keep the default database and UVM
source/license inventory with the matching executable. Non-default databases are
never included in the release package.
