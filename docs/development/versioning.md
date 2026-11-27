# Independent development and releases

Maybeka/slang-server selects its own features, performance targets and release versions. Hudson River Trading’s slang-server is the reference implementation. Use its features, issue reports and tests to identify behavior worth adopting. Reproduce the relevant behavior locally and implement it using this project's architecture.

Waveform viewer integration is outside the project roadmap and release acceptance criteria. Existing viewer commands are retained for compatibility. Driver and load tracing in the editor remains part of HDL navigation.

The compiler comes directly from the official MikePopoloski/slang repository, pinned by the gitlink. Evaluate official slang releases as they arrive, independently of the SDK selected by the reference implementation. Review the official release, adapt the project-owned patch and validate editor protocols, macro order, diagnostics, incremental editing, frozen scopes, resource use and bundled UVM restoration. See [SDK preparation](https://github.com/Maybeka/slang-server/blob/main/RESTORATION.md).

Linux and macOS are the supported development and release platforms. Keep the
standard build, server, protocol and client tests. Run large external HDL
workloads on demand and retain a small regression case when one reveals a
concrete language or lifecycle issue. Source files have no per-file analysis
size cap. Repository-wide feature scanning remains an on-demand future option.

The [November 2026 reference implementation review](reference-feature-review-2026-11-26.md)
records the product decisions. The [performance roadmap](performance-roadmap-status.md)
sets the priorities for general freeze coverage, incremental editing and native fallback.

## Repository and release contents

- Keep product source, public configuration and design documentation, regression
  tests and small fixtures, SDK patch and restoration inputs, and reusable test
  tooling in Git.
- Keep detailed acceptance results, measurement records and version receipts in
  a local archive. Discard disposable test outputs when their conclusions and
  reusable data have been recorded. Keep large workload checkouts, build products
  and personal configuration in local storage.
- Describe feature changes in the project changelog or feature documentation.
  Publish packaged server binaries and their runtime resources as versioned
  release assets. The bundled UVM package includes one default preparse result.
  Other UVM profiles use project-local caches.

## Version sources

| Component | Project source |
| --- | --- |
| Server | [VERSION](https://github.com/Maybeka/slang-server/blob/main/VERSION) |
| VS Code client | [Extension manifest](https://github.com/Maybeka/slang-server/blob/main/clients/vscode/package.json) |
| Neovim client | [Client capabilities](https://github.com/Maybeka/slang-server/blob/main/clients/neovim/lua/slang-server/_lsp/capabilities.lua) |

The Neovim client ships in this repository. The original project's external plugin mirror and Marketplace publishing workflows remain scoped to their owner.

## Coordinated releases

A major or minor release updates `CLIENT_VERSION` from `VERSION`. Server releases upload platform archives to this repository. The client compatibility versions follow the major and minor numbers. Re-running an
existing tag only rebuilds and replaces its server assets.

Dry-run mode calculates the next version, builds the branch selected in the
**Run workflow** dialog, and prepares archives without pushing or publishing
anything. Select a pull request branch to exercise its release build.

## Compatibility

Editor clients require a server with at least their major and minor version.
The server also warns when a client has an older major or minor version. Patch
differences do not generate compatibility warnings.

Each client advertises its name and version through the
`experimental.slangClient` initialization capability. The server uses this
information to detect an older client during initialization. Neovim 0.11 and
newer receive the capability through `vim.lsp.config`; configurations for older
Neovim versions can use `make_client_capabilities()` from
`slang-server._lsp.capabilities`.

## Server release assets

The server release workflow produces archives for Linux x86-64, Linux ARM64 and
macOS. The canonical filenames are `slang-server-linux-x64.tar.gz`,
`slang-server-linux-arm64.tar.gz`, and `slang-server-macos.tar.gz`.

Releases also include `slang-server-old-linux-x64-gcc.tar.gz` and
`slang-server-linux-arm64-clang.tar.gz` compatibility aliases for clients that
predate the canonical Linux names. Keep these aliases until those client
versions no longer need to install current server releases.
