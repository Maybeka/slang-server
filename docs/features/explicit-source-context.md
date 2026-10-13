# Explicit compilation and package context

Package exploration can use either a complete compilation filelist or source path hints.
Both routes use current editor buffers, including unsaved changes. They do not select a
full RTL build; the existing `build` configuration remains responsible for full-design analysis.

## Complete compilation filelist

Set `compilationFilelist` in `.slang/server.json` to a literal path relative to the workspace:

```json
{"compilationFilelist": "dv/files.f"}
```

The file is processed with Slang's native `-F` semantics. Source order, defines, include
paths, nested `-f` versus `-F`, compilation units and native library selection retain their
native meaning. Existing `flags` still accepts native command-file options. A flags-only
command file does not by itself declare an empty complete source universe; an explicitly
empty `compilationFilelist` does.

The configured sources are authoritative. Opening an included class selects its actual
compiled occurrence. Multiple compatible occurrences are reported as ambiguous. Source
hints cannot silently add a provider outside the configured compilation. Native library
sources and files found through real include directories remain valid compilation inputs.

Package/class queries retain required packages and interface declarations without elaborating
unrelated RTL tops. Direct module/interface editing and full-design analysis keep their
existing behavior. This package boundary intentionally does not diagnose unrelated RTL
instances, binds or top-level overrides.

## Source comments

An included header can name its owner in a leading line comment, before any source token:

```systemverilog
// slang-server: owner "../env/env_pkg.sv"
class transaction;
  // ...
endclass
```

An ordinary active import can name the source of its package on the same line:

```systemverilog
import common_pkg::*; // slang-server: source "../common/common_pkg.sv"
```

Paths are JSON-style quoted literals relative to the file containing the comment. There
is no shell, environment-variable or wildcard expansion. Owner comments are unconditional
leading metadata; import comments bind only to active, complete, nonmacro import syntax.
Comment-looking strings and inactive imports do not supply import context. The package
name comes from the import itself. Ambiguous multi-package imports must be split.

Hints are checked against actual source: an owner must really include the target in package
context, and an imported provider must really declare the named package. Owner chains are
bounded to eight files and cycles are rejected. Duplicate/conflicting directives are errors,
not a precedence rule. Removing a hint returns to ordinary discovery when no complete
compilation filelist applies.

## Errors and diagnostic execution

Unavailable, conflicting or ambiguous explicit context produces an error with code
`context-unavailable`; available syntax/preprocessor errors remain visible. Semantic queries
that require unavailable context return LSP RequestFailed instead of a misleading empty or
stale answer. Correct the path, source or configuration to restore semantic analysis.

Supported package contexts use the independent diagnostic process. Native inherited-macro
units and named-library contexts that cannot be represented by the immutable worker snapshot
use the sequential diagnostic path with an explicit fallback reason. Foreground queries still
use their native context. A later supported context can resume isolated diagnostics. This is
not a claim of independent diagnostics for every native compiler configuration.

## Full-design boundary

Explicit complete-filelist authority and package-only dependency selection apply to
exploration. Existing full-design queries retain their active-instance dependency closure.
Native reparse recipes preserve complete source units during edits, but this does not fix
every existing full-design query limitation: a secondary module root in a multi-root unit
can still lack earlier-root macro context in a standalone active-instance query. Do not
interpret package-first filelist support as general full-design multi-root query acceptance.
