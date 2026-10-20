# Trusted UVM semantic reuse

`experimentalUvmSemanticCache` prepares the selected UVM package once and shares
its elaborated package, fixed classes, methods, built-in class identities and
source storage across subsequent user compilations. This is an opt-in in-process
cache; it does not write a portable precompiled database.

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
