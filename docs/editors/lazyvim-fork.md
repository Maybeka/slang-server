# LazyVim with the reviewed fork binary

Use `clients/neovim/lazyvim-fork.lua` as
`~/.config/nvim/lua/plugins/slang-fork.lua`. It targets current LazyVim's
`opts.servers` / `opts.setup` interface and Neovim's `vim.lsp.config` /
`vim.lsp.enable` API (Neovim 0.11+). Set the environment variable before launching:

```sh
export SLANG_SERVER_FORK_BIN=/absolute/path/slang-server/build/bin/slang-server
"$SLANG_SERVER_FORK_BIN" --version
sha256sum "$SLANG_SERVER_FORK_BIN"
nvim /absolute/path/design/item.svh
```

The absolute `cmd` pins this executable independently of PATH and Mason. The
path must refer to this fork's build/install directory, outside Mason's managed
directories; do not point it at a Mason binary or symlink. The
spec removes slang-server from Mason installation lists, sets `mason=false`,
and uses LazyVim's setup callback to exclude Mason automatic activation. Mason
can still manage other servers. Remove duplicate slang_server setup from other
user files and any installer that explicitly downloads a server elsewhere.
The optional hierarchy plugin in `clients/neovim/lazy.lua` is not this LSP setup.

Place `.slang/server.json` at the design workspace root; the `.slang` root
marker wins before an enclosing `.git`. For example:

```json
{
  "compilationFilelist": "design.f",
  "flags": "--single-unit --threads 1",
  "fastCwdSnapshotChecks": false,
  "experimentalMethodScopeOverlay": false
}
```

`design.f` must list the actual ordered root sources and include directories,
not just the edited header. For a representative layout:

```text
+incdir+include
rtl/prelude.sv
rtl/owner.sv
rtl/top.sv
```

After attaching to a SystemVerilog buffer, run these checks:

```vim
:set filetype?
:LspInfo
:lua local cs=vim.lsp.get_clients({bufnr=0,name="slang_server"}); assert(#cs==1,"Expected one slang_server"); local c=cs[1]; assert(c.config.cmd[1]==vim.env.SLANG_SERVER_FORK_BIN); print(vim.inspect({cmd=c.config.cmd,root=c.config.root_dir,resolve=c.config.capabilities.textDocument.completion.completionItem.resolveSupport}))
```

Verify the root is the intended design directory, the SHA256 matches the tested
binary, and `resolveSupport.properties` is exactly `documentation`, `detail`.
The server uses eager completion edits in this configuration. Advertising lazy
`insertText`, `insertTextFormat` and `textEdit` together disables the experimental
method certificate, so the final per-server setup intentionally narrows that
capability. This does not enable the experiment; it remains default off.

Exercise completion and `gd` after editing a local, save/reopen, and inspect
`:LspLog` for the actual launched command and configuration/context errors.
Check diagnostics eventually reflect the current document version. Restart
Neovim after a binary replacement; a running process keeps its old executable.
For the restricted experiment, opt in explicitly in a disposable representative
workspace, keep only its target header open, and respect its documented method
shape. Normal UVM workflows should retain the default native path.

Validation scope is recorded in the current capability report. A Lua contract
check is not a claim that the full LazyVim UI, completion engine or user plugins
have been run in this environment.

Primary interface references checked for this configuration:
[LazyVim LSP setup](https://github.com/LazyVim/LazyVim/blob/main/lua/lazyvim/plugins/lsp/init.lua),
[nvim-lspconfig slang_server](https://github.com/neovim/nvim-lspconfig/blob/master/lsp/slang_server.lua),
[Mason LSP configuration](https://github.com/mason-org/mason-lspconfig.nvim/blob/main/doc/mason-lspconfig.txt).
