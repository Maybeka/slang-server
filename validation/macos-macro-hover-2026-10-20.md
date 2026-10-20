# Mac macro-hover views

The implementation adds `hovers.macroView` (`source`, `expanded`, `both`) and
`slang.previewHover`. Default `both` preserves existing rendering. A temporary
preview copies the hover settings for one request; it does not change the
configuration, write files or replace a valid analysis.

Generated-symbol and driver excerpts honor the mode. Source mode reads retained
editor buffers instead of formatting expanded tokens; nested invocations and
token-pasted identifiers therefore retain the original call. Ordinary declarations
with macros inside their bounds retain written source too. Macro-call source
hovers include the definition and call; expanded views show the call's expansion.
Definition-only positions retain the definition when there is no call to expand.

Frozen executable SHA256:
`53c05ccf30aa3148b2f978e03279f41cb89f25ba3c49bf01f5fd794c156dc5e6`.
The private reproduction area is `untracked/frozen-registry-2026-10-20/`.

Validation:

- CTest 664/664 and Python 16/16 pass.
- The broad hover/frozen/UVM focused group passes 1908 assertions in 76 cases.
- New macro-hover tests pass 57 assertions in two cases, covering all modes,
  nested macros, concatenated identifiers, ordinary syntax with embedded macros,
  temporary-command serialization, invalid mode rejection and unsaved edits.
  Pointer identity proves preview does not rebuild valid current analysis;
  subsequent normal hover and configuration remain unchanged.
- Isolated LazyVim SPI/PSS/UBUS checks exercise real UVM calls, completion/resolve,
  definition/hover, all three temporary modes and source-preview floating windows
  through two unsaved edits. Each test verifies normal hover stays unchanged and
  package reuse still occurs. The user's live editor and configuration are untouched.
- JSON schema and generated TypeScript types include the mode. The SDK patch is
  unchanged from the 708-file reconstruction accepted before this rendering work.

The first sandboxed editor attempts retained their failure logs: native file-event
callbacks report `EMFILE`. The preceding frozen candidate fails the same check,
and a minimal Neovim process without any LSP server reproduces it. An isolated
editor retry outside the sandbox passes all three projects, with watcher errors
still treated as failures. No watcher capability was disabled to obtain a pass.

Raw logs: `macro-hover-acceptance.json`, `macro-hover-*-final.log`,
`macro-hover-focused-1.log`, `macro-hover-editor-3-checks.json`, per-project editor
logs and `macro-hover-candidate.json`. This is correctness/editor evidence; the
earlier native-seed timing and RSS numbers are not relabeled as measurements of
this executable. Integrated module/instance/packaging and performance acceptance
remain part of the active overall goal.

See [configuration and temporary-preview usage](../docs/features/macro-hover.md).
