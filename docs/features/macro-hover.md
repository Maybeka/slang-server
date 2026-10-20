# Macro hover views

Choose the code shown by hover without changing parsing, diagnostics or macro
semantics. The default retains the existing combined display.

```json
{
  "hovers": {
    "macroView": "both"
  }
}
```

- `source`: code excerpts preserve the written macro calls. A macro invocation's
  hover includes its definition and original invocation, without expanded code.
- `expanded`: code excerpts show generated syntax. A macro invocation shows its
  expansion instead of repeating its definition. Definition-only locations still
  show the definition because there is no call-site expansion.
- `both`: preserve the existing expanded excerpts with their macro origin, and
  the definition plus expansion when hovering a macro invocation.

Symbol type, value and driver descriptions remain available in every mode.
Source text comes from current editor buffers, including unsaved edits. This
setting does not write expanded text back to a source file.

## Temporary preview

The `slang.previewHover` server command returns an ordinary LSP `Hover` (or null)
for one position using a supplied mode. Pass one argument object:

```json
{
  "textDocument": {"uri": "file:///project/test.sv"},
  "position": {"line": 10, "character": 4},
  "macroView": "source"
}
```

Positions use the client's negotiated LSP encoding. The command uses current
analysis when it is still valid, honors cancellation and leaves configuration
unchanged. Normal source freshness checks still apply.

For Neovim/LazyVim, this helper opens the returned hover in a floating window:

```lua
local function macro_hover(mode)
  local bufnr = vim.api.nvim_get_current_buf()
  local client = vim.lsp.get_clients({ bufnr = bufnr, name = "slang_server" })[1]
  if not client then return end
  local params = vim.lsp.util.make_position_params(0, client.offset_encoding)
  params.macroView = mode
  client:request("workspace/executeCommand", {
    command = "slang.previewHover",
    arguments = { params },
  }, function(err, hover)
    if err then vim.notify(err.message, vim.log.levels.ERROR); return end
    if not hover then return end
    local lines = vim.lsp.util.convert_input_to_markdown_lines(hover.contents)
    vim.lsp.util.open_floating_preview(lines, "markdown", { border = "rounded" })
  end, bufnr)
end

vim.keymap.set("n", "<leader>hs", function() macro_hover("source") end)
vim.keymap.set("n", "<leader>he", function() macro_hover("expanded") end)
vim.keymap.set("n", "<leader>hb", function() macro_hover("both") end)
```

These mappings are optional examples. Regular `K` continues to use the configured
hover mode; a temporary preview never changes it.
