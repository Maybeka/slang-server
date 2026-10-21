
# Limitations

**Potential Shallow Compilation Issues**

In shallow compilations some nodes may be invalid due to unset parameters, missing defines, etc. Therefore some expressions may not be visited by slang and cause some false positives, especially with unused checking. These can often be fixed, so please raise an issue if you see one. Before raising an issue, please check whether it shows up via a normal slang command to determine which repo to raise the issue in.

**Untaken Ifdef Branches**

Features may be limited in the context of untaken ifdef branches. To minimize this, it's recommended to encode these ifdefs in package parameters, then use generate blocks in the hdl code. Or if these are single branch ifdefs, include the most permissible defines in your server config.

**Deep Hierarchical References**

Shallow compilations only load the directly referenced syntax trees, and only load more transitive trees through packages. This may therefore cause issues with deep hierarchical references.

**Single Unit Compilations**

Selected builds using slang's [`--single-unit`](https://sv-lang.com/command-line-ref.html) provide full-design diagnostics, which are preserved when unchanged build files are opened. Per-file shallow analysis does not infer arbitrary macro state inherited from earlier files in the compilation unit. In exploration mode, [`explorePreincludeFiles`](../start/config.md#explorepreincludefiles) can supply the real ordered shared macro headers, including their unsaved edits and nested includes. Otherwise use explicit includes or configured defines; editing outside the original build context can still show missing-macro errors. See [Single Unit](design/shallow.md#single-unit) for details.

**Some UVM Code**

Classes in included headers can use an owning package's scope and preprocessor context, including unsaved header edits. In explore mode, opening only a header searches the workspace include index for candidate packages, then parses candidates with the configured include paths and defines to validate the actual file and package containment. A uniquely validated package supplies the header's context. Analysis uses that package and its referenced dependencies; it does not require a full RTL design compilation. Opening or editing an owning package, or an included sibling, refreshes diagnostics in affected open headers. Virtual interface definitions referenced by verification classes and typedefs are retained as required dependencies. Navigation and member completion use the bound interface instance, including parameter overrides, aliases, array-element receivers and modport restrictions.

Owner discovery is bounded to 256 reverse-include graph files and 16 candidate packages. Results are cached until relevant editor or index changes. When multiple packages validate, or a discovery budget is exceeded, a warning is shown and header diagnostics are deferred. Open the intended package explicitly to select its context. Among already-known explicit package contexts, the previous context is preferred, then the root path in lexical order; the first matching include occurrence in that root is used. This fallback is a deterministic heuristic, not proof of a unique semantic context. There is no context-selection UI yet.

The lightweight workspace index skips include expansion. Automatic discovery therefore requires indexed intermediate headers and an include directive active under the indexer's default preprocessor state. Locally defined filename macros can work; filename macros supplied only by build configuration or skipped includes, and inactive conditional branches, can require opening the owner explicitly. Basename matches in the reverse graph are only candidates: actual parsing verifies canonical paths and inclusion inside a package. Headers included inside functions or generate blocks are outside this package/class-focused support.

Header syntax indexing skips unrelated class bodies while retaining enclosing include ancestry and leading directives. Required-interface name discovery is reused for unchanged syntax trees; semantic diagnostics are still collected in full. This is not incremental package compilation. Large UVM packages can still be expensive to reparse and analyze, even though stable requests reuse the discovered owner. Full-design build mode retains its configured contexts. Broader ownership discovery and performance work are tracked in [#135](https://github.com/hudson-trading/slang-server/issues/135).

**Frozen Scope Semantic Cache (Experimental)**

With `experimentalScopeSemanticCache`, put stable packages / RTL and editable
consumer code in separate physical files. Selecting a package, module or RTL
instance also freezes other declarations in its provider file. Independent edits
to a sibling scope in that file are unsupported; any fixed-file edit, including
comments or whitespace, requires `slang.rebuildScopeLibrary`. Rebuild preserves
unsaved editor text. Dependency files are assumed fixed until explicit rebuild.

Place stable inputs before editable consumers in the filelist: conservative
prefix reuse can freeze preceding inputs too. Inspect `slang.scopeLibraryStatus`
for the actual reused packages and definitions. The experiment defaults off;
unsupported contexts and over-budget caches use ordinary native analysis. See
[fixed-dependency semantic reuse](frozen-sources.md#fixed-dependency-semantic-experiment-under-development).

**Rapid Editing**

Document changes and requests are applied in arrival order. The `didChange` handler applies text, editor versions and invalidation, then records pending diagnostic work. Full diagnostic collection runs on the same worker only when its foreground queue is empty. New input cancels the idle pass at cooperative checkpoints; pending generations remain available for a later quiet period. Cancelled diagnostic updates are rolled back. Completed document diagnostics carry the editor version, when known. Incidental include diagnostics and buffers reloaded independently from disk can remain unversioned.

Completion and navigation still construct fresh semantic analysis when required. Diagnostics can reuse that analysis only while its buffers, owner context and dependency-index revision remain valid. Requests keep their FIFO position and must resolve against their preceding edits; later edits do not leapfrog them.

This is sequential scheduling, not independent compiler threads. A parsing or semantic-collection call already running cannot be interrupted and can still delay a newly arriving request. Opening, saving, configuration changes and file-watcher handling retain their existing heavier paths. There is no parallel mutation of compiler state or timer-based edit debounce.
