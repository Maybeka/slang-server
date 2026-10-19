-- Save as ~/.config/nvim/lua/plugins/slang-fork.lua in a current LazyVim setup.
-- Set SLANG_SERVER_FORK_BIN to the absolute reviewed production binary path.
return {
  {
    "neovim/nvim-lspconfig",
    opts = function(_, opts)
      local binary = vim.env.SLANG_SERVER_FORK_BIN
      assert(binary and binary:sub(1, 1) == "/", "Set an absolute SLANG_SERVER_FORK_BIN")
      assert(vim.fn.executable(binary) == 1, "Fork binary is not executable: " .. binary)
      opts.servers = opts.servers or {}
      opts.servers.slang_server = {
        mason = false,
        cmd = { binary },
        filetypes = { "systemverilog", "verilog" },
        root_markers = { ".slang", ".git" },
      }
      opts.setup = opts.setup or {}
      opts.setup.slang_server = function(_, server_opts)
        -- Final per-server override, after completion-plugin capability merging.
        -- Edits are supplied eagerly; only docs/detail are resolved lazily.
        local caps = server_opts.capabilities or {}
        caps.textDocument = caps.textDocument or {}
        caps.textDocument.completion = caps.textDocument.completion or {}
        caps.textDocument.completion.completionItem = caps.textDocument.completion.completionItem or {}
        caps.textDocument.completion.completionItem.resolveSupport = {
          properties = { "documentation", "detail" },
        }
        server_opts.capabilities = caps
        vim.lsp.config("slang_server", server_opts)
        vim.lsp.enable("slang_server")
        -- LazyVim also excludes this server from Mason's automatic enable list.
        return true
      end
    end,
  },
  {
    "mason-org/mason.nvim",
    opts = function(_, opts)
      opts.ensure_installed = vim.tbl_filter(function(name)
        return name ~= "slang-server"
      end, opts.ensure_installed or {})
    end,
  },
  {
    "mason-org/mason-lspconfig.nvim",
    opts = function(_, opts)
      opts.ensure_installed = vim.tbl_filter(function(name)
        return name ~= "slang_server"
      end, opts.ensure_installed or {})
    end,
  },
  {
    "hrsh7th/nvim-cmp",
    optional = true,
    opts = function(_, opts)
      opts.sorting = opts.sorting or {}
      local comparators = opts.sorting.comparators or require("cmp.config.default")().sorting.comparators
      local function class_members_first(a, b)
        local function depth(entry)
          local text = entry.completion_item.sortText
          return text and tonumber(text:match("^slang%-member:(%d+):"))
        end
        local left, right = depth(a), depth(b)
        if left and right and left ~= right then
          return left < right
        end
      end
      -- Restrict the override to ranked slang class members. Keep the client's
      -- existing scoring and ordering within one inheritance level.
      opts.sorting.comparators = { class_members_first }
      for _, compare in ipairs(comparators) do
        table.insert(opts.sorting.comparators, compare)
      end
    end,
  },
}
