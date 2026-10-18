"""Execute the shipped config with Lua and a Neovim API contract double.

This validates merging, pinning and rejection behavior, not a real editor UI.
Uses an existing system Lua library, with an explicit skip if unavailable.
"""

import ctypes
import ctypes.util
from pathlib import Path

import pytest


def test_lazyvim_fork_config():
    library = ctypes.util.find_library("lua5.4") or ctypes.util.find_library("lua5.3")
    if not library:
        pytest.skip(
            "No existing Lua shared library; full editor acceptance is separate"
        )
    lua = ctypes.CDLL(library)
    lua.luaL_newstate.restype = ctypes.c_void_p
    lua.luaL_openlibs.argtypes = [ctypes.c_void_p]
    lua.luaL_loadstring.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lua.lua_pushstring.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lua.lua_pcallk.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_ssize_t,
        ctypes.c_void_p,
    ]
    lua.lua_tolstring.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
    lua.lua_tolstring.restype = ctypes.c_char_p
    lua.lua_close.argtypes = [ctypes.c_void_p]
    state = lua.luaL_newstate()
    assert state
    script = b"""
local path = ...
local configured, enabled
vim = {
  env = { SLANG_SERVER_FORK_BIN = "/reviewed/bin/slang-server" },
  fn = { executable = function(p) return p == "/reviewed/bin/slang-server" and 1 or 0 end },
  tbl_filter = function(fn, list)
    local out = {}; for _, v in ipairs(list) do if fn(v) then out[#out+1]=v end end; return out
  end,
  lsp = {
    config = function(name, opts) assert(name == "slang_server"); configured = opts end,
    enable = function(name) enabled = name end,
  },
}
local specs = dofile(path)
local opts = { servers = { other = {} }, setup = {} }
specs[1].opts(nil, opts)
local server = opts.servers.slang_server
assert(server.cmd[1] == vim.env.SLANG_SERVER_FORK_BIN and #server.cmd == 1)
assert(server.mason == false and opts.servers.other)
assert(server.root_markers[1] == ".slang" and server.root_markers[2] == ".git")
server.capabilities = { textDocument = { completion = { completionItem = {
  snippetSupport = true, resolveSupport = { properties = { "textEdit", "insertText", "insertTextFormat" } },
} } } }
assert(opts.setup.slang_server(nil, server) == true)
assert(enabled == "slang_server" and configured == server)
local item = configured.capabilities.textDocument.completion.completionItem
assert(item.snippetSupport and #item.resolveSupport.properties == 2)
assert(item.resolveSupport.properties[1] == "documentation" and item.resolveSupport.properties[2] == "detail")
local packages = { ensure_installed = { "slang-server", "stylua" } }
specs[2].opts(nil, packages)
assert(#packages.ensure_installed == 1 and packages.ensure_installed[1] == "stylua")
local servers = { ensure_installed = { "slang_server", "lua_ls" } }
specs[3].opts(nil, servers)
assert(#servers.ensure_installed == 1 and servers.ensure_installed[1] == "lua_ls")
for _, bad in ipairs({ "relative/slang-server", "/missing/slang-server" }) do
  vim.env.SLANG_SERVER_FORK_BIN = bad
  assert(not pcall(specs[1].opts, nil, { servers = {}, setup = {} }))
end
"""
    try:
        lua.luaL_openlibs(state)
        code = lua.luaL_loadstring(state, script)
        assert code == 0, lua.lua_tolstring(state, -1, None)
        path = Path(__file__).resolve().parents[2] / "clients/neovim/lazyvim-fork.lua"
        lua.lua_pushstring(state, str(path).encode())
        result = lua.lua_pcallk(state, 1, 0, 0, 0, None)
        assert result == 0, lua.lua_tolstring(state, -1, None)
    finally:
        lua.lua_close(state)
