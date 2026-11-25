# slang-server.nvim

A Neovim plugin to support non-LSP features of [Slang Server](../../README.md).

## Features

Note that it is not necessary to install this plugin in order to use Slang Server.
Neovim supports all standard [LSP](https://microsoft.github.io/language-server-protocol/) commands.
This plugin provides features which extend the standard LSP interface, such as:

* Browse the elaborated design in hierarchy and cell views.
* Search the hierarchy with FzfLua, Telescope, Snacks Picker, or `vim.ui`.
* Select active instances and iterations from code lenses or the
  hierarchy views.
* Open waveforms and add signals to them (experimental).

See the [Neovim feature guide](../../docs/features/hdl/neovim.md).

Code-lens support is required to select active instances or generate-loop
iterations in the source; see
[installation guide](../../docs/start/installing.md#code-lenses)
for an example configuration.

## Requirements

* Neovim 0.10.0 or newer
* `slang-server` configured as a Neovim language server
* [Nerd Font](https://www.nerdfonts.com/) is recommended

### Plugin dependencies

If installing with lazy.nvim, plugin dependencies are resolved automatically.

* [nui.nvim](https://github.com/MunifTanjim/nui.nvim)

FzfLua, Telescope, or Snacks Picker are optional dependencies. If installed,
the configured picker will be used for the `searchHierarchy` command;
otherwise, a slightly degraded two-step search is provided by native
`vim.ui.input` and `vim.ui.select`.

## Installation

For lazy.nvim, point the plugin at your project checkout in `~/.config/nvim/lua/plugins/slang-server.lua`:

```lua
return {
  {
    name = "slang-server.nvim",
    dir = "/absolute/path/to/slang-server/clients/neovim",
    lazy = false,
  },
}
```

The plugin defers command and mapping initialization until a Verilog or
SystemVerilog ftplugin is loaded. Its lazy.nvim package specification therefore
sets `lazy = false`; adding another plugin-manager lazy-loading trigger is neither
required nor recommended. To install without a plugin manager, simply clone and
place the plugin directory in your Neovim runtimepath.

## Configuration

The default configuration can be found in [config.lua](./lua/slang-server/_core/config.lua). Override options can be defined in the global `vim.g.slang_server_config`, or passed to `opts = {...}` in the lazy.nvim plugin spec.

Global key mappings for plugin commands are disabled by default. Set
`keymaps.enable_defaults = true` to enable them all; individual mappings
can still override `enabled` or `key`.

`search.query_delay` debounces requests made through the picker. This delay is
added to any input or query delay applied by the selected picker engine itself;
set it to `0` to rely solely on the picker's behaviour.

A custom `searchHierarchy` picker can be supplied as a function accepting a
`ctx`. Call `ctx.search` whenever its query changes; results arrive
asynchronously and retain the server's fuzzy-match ordering. Call `ctx.select`
with the chosen result item. E.g.:

```lua
require("slang-server").setup({
  search = {
    picker = function(ctx)
      vim.ui.input({ prompt = "Hierarchy query: " }, function(query)
        if not query then
          return
        end

        ctx.search(query, function(result)
          vim.ui.select(result.matches, {
            prompt = ("Select result (%d total)"):format(result.totalResults),
            format_item = function(item)
              return item.path
            end,
          }, function(item)
            if item then
              ctx.select(item)
            end
          end)
        end)
      end)
    end,
  },
})
```

`ctx.search(query, callback)` debounces requests and discards stale responses.
Each result item contains `name`, `path`, `kind`, and optional `description` and
`containerName` fields. The custom picker should pass the original item to
`ctx.select(item)` so the plugin can reveal its path.

## Project

The plugin and server are maintained together in [Maybeka/slang-server](https://github.com/Maybeka/slang-server).
