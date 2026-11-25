# Installing

### VS Code and compatible editors

Set `slang.path` to the absolute path of the [locally built server](building.md). The extension built from this repository also supports managed downloads from [this project's releases](https://github.com/Maybeka/slang-server/releases) when a matching platform asset is available. Its managed binaries are stored under the release repository name. An existing Marketplace client can use the local server through the same explicit setting.

### Neovim

For LazyVim, use the [project configuration](../editors/lazyvim-fork.md) and set `SLANG_SERVER_FORK_BIN` to this project's executable. For other Neovim setups, point the `slang_server` LSP command at that executable. Check the running command with `:LspInfo`.

#### Code lenses

On Neovim 0.10 and 0.11, code lenses must be refreshed explicitly. It is also
recommended to map `vim.lsp.codelens.run()` so that the lenses on the current
line can be selected and executed. The following configuration keeps lenses
current and maps `<leader>cl` to run them:

```lua
vim.api.nvim_create_autocmd("LspAttach", {
  callback = function(args)
    local client = vim.lsp.get_client_by_id(args.data.client_id)
    if not client or client.name ~= "slang_server"
        or not client:supports_method("textDocument/codeLens") then
      return
    end

    local group = vim.api.nvim_create_augroup("slang-server-codelens", { clear = false })
    vim.api.nvim_clear_autocmds({ group = group, buffer = args.buf })
    vim.api.nvim_create_autocmd({ "BufEnter", "CursorHold", "InsertLeave" }, {
      group = group,
      buffer = args.buf,
      callback = function()
        vim.lsp.codelens.refresh({ bufnr = args.buf })
      end,
    })

    vim.lsp.codelens.refresh({ bufnr = args.buf })
    vim.keymap.set("n", "<leader>cl", vim.lsp.codelens.run, {
      buffer = args.buf,
      desc = "Run code lens",
    })
  end,
})
```

Neovim 0.12 and newer can use `vim.lsp.codelens.enable()` instead of the
refresh autocmds. `vim.lsp.codelens.run()` is still used to execute the lens on
the current line.

#### Enhanced features

The optional [Neovim plugin in this repository](https://github.com/Maybeka/slang-server/tree/main/clients/neovim) provides the design hierarchy and waveform integration. Load that directory as a local plugin to use the matching revision.

### Other editors

Most modern editors can at least point to a language server binary for specific file types. This will provide standard LSP features, but not HDL specific features.

If the editor also allows for executing LSP commands, HDL features like setting a compilation should be available.
