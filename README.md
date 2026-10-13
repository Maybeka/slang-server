# Slang Server

`slang-server` is a [Language Server](https://microsoft.github.io/language-server-protocol/) for SystemVerilog based on the [Slang](https://github.com/MikePopoloski/slang) library, providing useful language features for editors like VS Code and Neovim.

Read about [getting started](https://hudson-trading.github.io/slang-server/start/installing/), [configuring your repo](https://hudson-trading.github.io/slang-server/start/config/), and [learn more about the features](https://hudson-trading.github.io/slang-server/features/features/).

Install on the [VS Code Marketplace](https://marketplace.visualstudio.com/items?itemName=Hudson-River-Trading.vscode-slang), the [OpenVSX Marketplace](https://open-vsx.org/extension/Hudson-River-Trading/vscode-slang), in [Neovim](https://hudson-trading.github.io/slang-server/start/installing/#neovim), or [other editors](https://hudson-trading.github.io/slang-server/start/installing/#other-editors).

Contributions are welcome! See [DEVELOPING.md](DEVELOPING.md)

## Features

Quick, high quality lint messages on every keystroke, with links to the [Slang warning reference](https://sv-lang.com/warning-ref.html).

![](docs/assets/images/lints.gif)

Informative hovers and gotos on nearly every symbol across your workspace and libraries.

![](docs/assets/images/hovers.gif)

Find references across your entire workspace.

![](docs/assets/images/gotorefs.gif)

Configurable inlay hints that provide useful information.

![](docs/assets/images/all_inlays.png)

Intuitive completions for module instances and macros, as well as scope members of packages, modules, structs, and more.

![](docs/assets/images/completions.gif)

HDL-specific features that allow you to set a filelist or top level, browse the elaborated hierarchy, select the active instance of each module, inspect instance-specific values, and go back and forth with a waveform viewer.

![](docs/assets/images/hdl.gif)

For more detailed feature info, see [the docs](https://hudson-trading.github.io/slang-server/features/features/).


### Opt-in fast CWD snapshot checks

The default `fastCwdSnapshotChecks: false` retains strict per-lookup checks.
To opt in for this workspace, add `"fastCwdSnapshotChecks": true` to its
`.slang/server.json` (or its existing local configuration layer).

Fast mode requires the **process working directory and its ancestors** to keep
their names and remain present throughout each analysis. Ordinary source edits,
including unsaved editor buffers, remain supported. This requirement does not
freeze source contents, symlinks, or the rest of the filesystem. Canonicalization,
lookup target and negative-result conflict rules remain enabled.

Fast mode observes CWD at the beginning and end of an analysis phase. A directory
change followed by restoration between those observations is not detected and is
outside this contract. Internal command-file directory writers cannot run during
a fast phase; parser workers borrow the state thread's lease and finish before it
is released. Reads outside fast phases still use strict checks.

A changed or unavailable CWD at either boundary discards the result and reports
`fast cwd snapshot context changed or is unavailable; result discarded; reload configuration`.
That context stays invalid until the server reloads its configuration. It does
not publish a result from the failed phase or silently resume using the old
context. Disable the option and reload to return to strict mode.

These checks apply to native parent analysis. The isolated diagnostic child still
replays its frozen inputs without live source-filesystem reads.
