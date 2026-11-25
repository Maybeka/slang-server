rockspec_format = "3.0"
package = "slang-server.nvim"
-- NB: this rockspec is for CI purposes only and is not intended to control
--     versioning or to be released on luarocks
version = "scm-1"
description = { license = "MIT" }
source = {
   url = "git+https://github.com/Maybeka/slang-server",
   dir = "slang-server/clients/neovim",
}
dependencies = {
   -- Add runtime dependencies here
   -- e.g. "plenary.nvim",
}
test_dependencies = {
   "lua >= 5.1",
   "nlua",
   -- this should be a regular (non-test) dependency but that is making lazyvim do things I don't understand
   "nui.nvim",
}
build = {
   type = "builtin",
   copy_directories = {
      "ftplugin",
      -- Add runtimepath directories, like
      -- 'plugin', 'ftplugin', 'doc'
      -- here. DO NOT add 'lua' or 'lib'.
   },
}
