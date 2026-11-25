# Independent development and releases

Maybeka/slang-server selects its own features, performance targets and release versions. Hudson River Trading’s slang-server is the reference implementation. Review its release notes, issue reports, performance evidence and test changes. Use fixtures and assertions to understand input conditions, expected behavior and gaps in coverage. Write a local case for the observable behavior, check whether the issue or opportunity exists here, and implement selected changes against this project's architecture. Record the regression test or before/after measurement for acceptance. Track candidates by behavior and local acceptance criteria.

The compiler comes directly from the official MikePopoloski/slang repository, pinned by the gitlink. Evaluate official slang releases as they arrive, independently of the SDK selected by the reference implementation. Review the official release, adapt the project-owned patch and validate editor protocols, macro order, diagnostics, incremental editing, frozen scopes, resource use and bundled UVM restoration. See [SDK preparation](https://github.com/Maybeka/slang-server/blob/main/RESTORATION.md).

The first feature candidates from the November review are recovery after source deletion and recreation, processing source events while the build configuration is invalid, stable document iteration during analysis, and diagnostics for active instance parameter overrides. Each starts with an editor-visible reproduction. The existing cross-file macro and save tests define this project's single-compilation-unit behavior.

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

The server release workflow publishes one archive per supported platform. The
VS Code installer uses the canonical names, such as
`slang-server-linux-x64.tar.gz`, `slang-server-macos.tar.gz`, and
`slang-server-windows-x64.zip`.

Releases also include `slang-server-old-linux-x64-gcc.tar.gz` and
`slang-server-linux-arm64-clang.tar.gz` compatibility aliases for clients that
predate the canonical Linux names. Keep these aliases until those client
versions no longer need to install current server releases.
