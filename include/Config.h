//------------------------------------------------------------------------------
// Config.h
// Provide singleton configuration class debug printing macro.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once
#include "rfl/Deprecated.hpp"
#include "rfl/Description.hpp"
#include "rfl/config.hpp"
#include <optional>
#include <rfl/Result.hpp>
#include <rfl/Skip.hpp>
/// Server configuration, loaded from up to three JSON files:
///
/// 1. `.slang/server.json`       — workspace config (tracked in version control)
/// 2. `~/.slang/server.json`     — user config (personal defaults across projects)
/// 3. `.slang/local/server.json` — local config (untracked, personal overrides)
///
/// Merging rules:
/// - Array fields (e.g. index, incdirs) are appended across all files.
/// - Scalar fields are overwritten by later files (local > user > workspace).
/// - `flags` has special precedence: workspace overrides user (only one is used as the base),
///   and local flags are always appended on top. This means local flags add to whichever
///   of workspace/user flags won, rather than replacing them.
///
/// The "Add define" code action writes `-D` flags to `.slang/local/server.json`.
/// The "Auto-configure" command discovers index and include directories for `.slang/server.json`.
struct Config {
    /// generate json schema from this by running with --config-schema
    // all fields must be optional
    rfl::Description<"Flags to pass to slang", std::string> flags;

    /// Ordered include directories, relative to the workspace root unless absolute.
    rfl::Description<"Include directories, relative to the workspace root. Searched after "
                     "directories supplied through flags and build files.",
                     std::vector<std::string>>
        incdirs;
    rfl::Description<"Complete compilation filelist for package-first exploration. The path is "
                     "workspace-relative and uses native -F semantics; nested -f/-F options "
                     "retain their native meaning. This does not select a full RTL build.",
                     std::optional<std::string>>
        compilationFilelist;

    rfl::Description<"Ordered files prepended to separately parsed exploration-mode sources. "
                     "Use real shared macro headers; relative paths are workspace-relative. "
                     "Full builds retain their own compilation-unit preprocessing.",
                     std::vector<std::string>>
        explorePreincludeFiles;

    struct UvmConfig {
        rfl::Description<"UVM source directory or distribution root, relative to the workspace. "
                         "An existing project UVM is validated and retained; another version is "
                         "never silently substituted.",
                         std::string>
            sourceDirectory;
        rfl::Description<"Optional UVM entry file, relative to sourceDirectory unless absolute. "
                         "Otherwise locate uvm_pkg.sv directly or under src.",
                         std::optional<std::string>>
            entryFile;
        rfl::Description<"Explicitly select the shipped UVM 1.2 sources. Default false. "
                         "Mutually exclusive with sourceDirectory and entryFile. Conflicting "
                         "project UVM inputs are rejected; no automatic injection or download.",
                         bool>
            useBundled = false;
        rfl::Description<"Retention charge limit for the experimental trusted UVM semantic "
                         "owner, in MiB (1..1024). Separate from frozenSources.maxMiB and "
                         "process RSS. An oversized library uses native analysis.",
                         uint32_t>
            semanticMaxMiB = 256;
    };
    rfl::Description<"Explicit UVM library inputs. No library is injected unless configured. "
                     "Source loading is independent of experimentalUvmPrefixCache.",
                     std::optional<UvmConfig>>
        uvm;

    rfl::Description<"Opt-in fast snapshot cwd checks (default false = strict). Requires the "
                     "process working directory and its ancestors not to be moved, deleted "
                     "or renamed during analysis. Ordinary source edits remain allowed. "
                     "Checks phase boundaries; transient changes restored between checks "
                     "are undetected. A changed/unavailable cwd discards the result and "
                     "requires configuration reload. Filesystem lookup rules are unchanged.",
                     bool>
        fastCwdSnapshotChecks = false;

    rfl::Description<"Experimental closed flat int method overlay. Default off. Phase one "
                     "requires equal-length ASCII edits with fixed line layout and local count; "
                     "unknown contexts and unsupported queries use the complete native path.",
                     bool>
        experimentalMethodScopeOverlay = false;

    rfl::Description<
        "Experimental local function/task body queries. Default off. Supports changed local "
        "declarations, types, statements, member access, calls and macro expansions in retained "
        "class, package and module contexts. Signatures and external dependencies remain fixed; "
        "unsafe edits or queries use native analysis. Diagnostics analyze the complete current "
        "snapshot.",
        bool>
        experimentalMethodBodyQueries = false;

    rfl::Description<
        "Experimental full-unit macro dependency facts for body preprocessing proofs. "
        "Default off; requires experimentalMethodBodyQueries. Allows unchanged define/undef "
        "effects and proved unused macro names. Unsafe edits retain native analysis; "
        "private fragments never supply whole-unit absence proofs.", bool>
        experimentalMethodBodyMacroProof = false;
    rfl::Description<
        "Macro proof recording / restore budget per compilation unit in MiB, default 16. "
        "Zero disables runtime recording and macro proof. This is not an RSS limit; "
        "fixed-library artifacts have their own bounded build-time facts.", uint32_t>
        methodBodyMacroProofMaxMiB = 16;

    rfl::Description<"Experimental local method-body diagnostics. Default off. Requires "
                     "experimentalMethodBodyQueries and a completed full-design diagnostic "
                     "context. Unsupported dependency changes use full current analysis.",
                     bool>
        experimentalMethodBodyDiagnostics = false;

    rfl::Description<"Experimental renewal of local method-body certificates. Default off. "
                     "Requires experimentalMethodBodyQueries. Revalidates the existing input "
                     "witnesses and preserves the lexical owner and diagnostic baseline across "
                     "64-edit windows. Unsupported edits and source-storage limits still "
                     "use native analysis and context retirement.",
                     bool>
        experimentalMethodBodyCertificateRenewal = false;

    rfl::Description<"Experimental method-body certification at publication boundaries. Default "
                     "off. Requires experimentalMethodBodyQueries. Skips redundant preflight "
                     "disk reads but still verifies complete input contents before returning "
                     "local results. Retains one completed original diagnostic baseline across "
                     "superseded edits and certifies its local slice on diagnostic demand.",
                     bool>
        experimentalMethodBodyCertification = false;

    // Kept readable for local prototype configurations; it no longer enables work.
    rfl::Description<"Obsolete experimental query/diagnostic transaction key; ignored. "
                     "Queries and background diagnostics certify their inputs independently.",
                     bool>
        experimentalQueryDiagnosticTransactions = false;

    rfl::Description<"Retained unchanged-suffix budget in KiB for the experimental later-macro "
                     "syntax memo. Default 1024, zero disables; capped at 65536. Requires "
                     "method-body queries, diagnostics and certification. Keeps one line "
                     "coordinate, never a history of edits. Semantic dependencies and complete "
                     "physical publication proofs are still checked each time.",
                     uint32_t>
        methodBodyLineSyntaxCacheKiB = 1024;

    rfl::Description<"Charged retained diagnostic display memo budget in KiB. Default 256, "
                     "zero disables; capped at 16384. Requires experimental method-body "
                     "queries, diagnostics and certification. Holds one function-end "
                     "coordinate and pure LSP payloads; no semantic owners or input-proof "
                     "success is cached. Oversized entries use normal rendering.",
                     uint32_t>
        methodBodyRenderingCacheKiB = 256;

    rfl::Description<"Retained negative-input name plan budget in KiB. Default 512, zero "
                     "disables; capped at 16384. Only experimental method-body certification "
                     "uses it. At most 64 directories are pinned during each fresh proof; "
                     "descriptors and successful proofs never survive the invocation. "
                     "Budget or descriptor exhaustion uses the ordinary complete proof.",
                     uint32_t>
        methodBodyDirectoryProofCacheKiB = 512;

    rfl::Description<"Experimental cooperative syntax parsing and background context retirement. "
                     "Default off. Cancelled syntax candidates are discarded; semantic model "
                     "construction retains phase-boundary cancellation. "
                     "Keeps the current query owner until a private replacement is ready; "
                     "new input cancels replacement parsing. A hard source-storage limit "
                     "still forces synchronous retirement to bound retained generations.",
                     bool>
        experimentalCooperativeAnalysis = false;

    rfl::Description<
        "Additional mutable source-history allowance per generation in MiB (1..4096, default "
        "64). Includes text and source-location tables beyond the initial working set. "
        "Experimental context retirement starts at half this allowance and becomes synchronous "
        "at the full allowance. Does not enable experiments or cap process RSS, retained fixed "
        "providers, or source namespaces still held by live views.",
        uint32_t>
        sourceHistoryMaxMiB = 64;

    rfl::Description<"Experimental immutable UVM prefix reuse. Default off. External macros "
                     "affecting UVM must come from command-line/configuration flags. Unsupported "
                     "source changes or preprocessing state use native parsing.",
                     bool>
        experimentalUvmPrefixCache = false;

    rfl::Description<"Experimental semantic reuse of an explicitly configured, user-trusted "
                     "immutable UVM library. Default off. Library files and dependencies are "
                     "assumed unchanged until slang.rebuildUvmLibrary or configuration reload. "
                     "No automatic dependency freshness is promised. Mutable user types and "
                     "generic specializations remain per-consumer.",
                     bool>
        experimentalUvmSemanticCache = false;

    rfl::Description<
        "Experimental complete semantic reuse of explicitly fixed scopes. Default off. "
        "Dependencies are fixed until slang.rebuildScopeLibrary or configuration reload. "
        "Uses frozenSources selectors; unsupported boundaries use native analysis.",
        bool>
        experimentalScopeSemanticCache = false;

    rfl::Description<"Experimental diagnostic delta publication. Default off. Repeated results "
                     "are compared before serialization; changed document versions, errors, "
                     "repairs and URI retirements are still published.",
                     bool>
        experimentalDiagnosticDeltaPublishing = false;

    rfl::Description<"Experimental bounded parallel full-design flow diagnostics. Default off. "
                     "Observers or order-sensitive driver reports retain serial analysis.", bool>
        experimentalParallelDiagnostics = false;
    rfl::Description<"Maximum full diagnostic workers, default2, capped8. Zero or one uses "
                     "ordinary serial analysis. Does not parallelize local-body updates.", uint32_t>
        parallelDiagnosticThreads = 2;

    rfl::Description<
        "Experimental authenticated syntax-prefix reuse for configured single-unit inputs. "
        "Default off. Requires cooperative analysis and a prepared fixed scope prefix. "
        "Mutable preceding roots and includes are rechecked before reuse; this does not "
        "declare them fixed or reuse their semantics.", bool>
        experimentalSyntaxPrefixCache = false;
    rfl::Description<
        "Retained mutable syntax-prefix cache budget in MiB, default 256. Zero disables "
        "this cache. Charges one pending or reusable CST owner, excluding the already "
        "charged fixed prefix. This budget is separate from frozenSources.maxMiB and "
        "sourceHistoryMaxMiB; it is not a process RSS limit.", uint32_t>
        syntaxPrefixCacheMaxMiB = 256;

    struct FrozenSourcesConfig {
        rfl::Description<
            "Exact package names in configured compilation inputs. The complete "
            "ordered roots through each selected package are retained, including "
            "preceding dependencies. Missing or ambiguous names are reported in status.",
            std::vector<std::string>>
            packages;
        rfl::Description<"Exact module, interface or program definition names in configured inputs. "
                         "Retains complete ordered source roots; instance parameterizations "
                         "are elaborated independently. Does not enable the experiment.",
                         std::vector<std::string>>
            modules;
        rfl::Description<"Exact canonical RTL instance paths selecting that instance and its "
                         "descendants. Behavior follows the enabled frozen-source or fixed-scope "
                         "experiment. Requires configured hierarchy and does not itself enable "
                         "caching.",
                         std::vector<std::string>>
            instances;
        rfl::Description<"Maximum charged retained cache size in MiB (0 to 512, default 256). "
                         "Oversized entries use ordinary analysis. The syntax experiment "
                         "reserves one quarter for certified hierarchy values with instance "
                         "selectors; the semantic experiment shares the budget between library "
                         "and body owners. This is not a process RSS limit.",
                         uint32_t>
            maxMiB = 256;
    };
    rfl::Description<"Frozen source selections and cache budget. Does not enable the experiment.",
                     std::optional<FrozenSourcesConfig>>
        frozenSources;
    rfl::Description<
        "Experimental package/module syntax and instance hierarchy reuse. Default off. Exact "
        "source and include lookup certification can cost filesystem I/O; changed "
        "inputs rebuild the native context while preserving editor overlays.",
        bool>
        experimentalFrozenSources = false;

    // Legacy indexing globs, kept for backwards compatibility
    rfl::Deprecated<"Use 'index' instead.",
                    "Globs of what to index. By default will "
                    "index all sv and svh files in the workspace.",
                    std::vector<std::string>>
        indexGlobs;

    struct IndexConfig {
        rfl::Description<"Directories to index", std::vector<std::string>> dirs;
        rfl::Description<
            "Exact directory names to exclude at all path levels, or specific workspace-relative "
            "paths starting with './' (absolute paths are also supported). Wildcards are not "
            "supported.",
            std::optional<std::vector<std::string>>>
            excludeDirs;
    };

    rfl::Description<"Index configurations; by default indexes all .sv, .svh, .v, and .vh files in "
                     "the workspace. External-only roots supplement this default. A workspace "
                     "directory or an empty dirs entry replaces the default scan.",
                     std::vector<IndexConfig>>
        index = {};

    // This can't be defaulted because of how we paint configs over each other and append
    // lists together.
    std::vector<std::string> getIndexGlobs() {
        if (indexGlobs.value().size() == 0) {
            return std::vector<std::string>{"./.../*.sv*"};
        }
        else {
            auto list = indexGlobs.value();
            // dedup
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
            return list;
        }
    }

    rfl::Deprecated<"Use 'index' instead.", "Directories to exclude", std::vector<std::string>>
        excludeDirs;
    rfl::Description<"Thread count to use for indexing", int> indexingThreads = 0;
    rfl::Description<"Build file to use", std::optional<std::string>> build;
    rfl::Description<"Build file glob pattern, e.g. `builds/{}.f`. Used for selecting build "
                     "files. If omitted and no other build source is configured, defaults to "
                     "matching all `.f` files in the workspace.",
                     std::optional<std::string>>
        buildPattern;
    rfl::Description<"Whether build files use paths relative to that file", bool>
        buildRelativePaths = false;
    rfl::Description<"Waveform file glob to open given a build. Name and top variables can be "
                     "passed with {name}, {top})",
                     std::optional<std::string>>
        wavesPattern;
    rfl::Description<"Waveform viewer command ({} will be replaced with the WCP port), used for "
                     "direct wcp connection with neovim and surfer.",
                     std::optional<std::string>>
        wcpCommand;

    struct HoverConfig {
        enum class DocCommentFormat { plaintext, markdown, raw };
        enum class MacroView { source, expanded, both };
        rfl::Description<"Macro code in hovers: 'source' preserves written source, 'expanded' "
                         "shows generated code, 'both' keeps expanded code and its macro origin. "
                         "Default both preserves the existing display.",
                         MacroView>
            macroView = MacroView::both;
        rfl::Description<"How to render leading doc comments in hovers: 'markdown' renders as "
                         "markdown, 'plaintext' escapes markdown characters, 'raw' shows the "
                         "comment text verbatim including comment markers.",
                         DocCommentFormat>
            docCommentFormat = DocCommentFormat::markdown;
    };

    rfl::Description<"Hover behavior settings", HoverConfig> hovers = HoverConfig{};

    struct InlayHints {
        rfl::Description<"Hints for port types", bool> portTypes = false;
        rfl::Description<"Hints for inferred assignment pattern types", bool>
            assignmentPatternTypes = true;
        rfl::Description<"Hints for active instance parameter and localparam values", bool>
            activeParameterValues = true;
        rfl::Description<"Hints for names of ordered ports and params", bool> orderedInstanceNames =
            true;
        rfl::Description<"Hints for port names in wildcard (.*) ports", bool> wildcardNames = true;
        rfl::Description<"Function argument hints: 0=off, N=only calls with >=N args", int>
            funcArgNames = 2;
        rfl::Description<"Macro argument hints: 0=off, N=only calls with >=N args", int>
            macroArgNames = 2;
    };

    rfl::Description<"Inline hints for things like ordered arguments, wildcard ports, and others",
                     InlayHints>
        inlayHints = InlayHints{};

    struct Build {
        rfl::Description<"Optional name used for generated build files and UI labels",
                         std::optional<std::string>>
            name;
        rfl::Description<"Glob pattern to find files, like .f files or makefiles", std::string>
            glob;
        rfl::Description<
            "Optional command that produces .f content on stdout when passed the selected file",
            std::string>
            command;
    };

    rfl::Description<"Builds for direct .f selection or command-based .f generation",
                     std::vector<Build>>
        builds;

    struct FlagSource {
        std::string filePath;
        std::string flags;
    };

    /// Load config from up to three sources:
    /// @param workspaceConf  .slang/server.json (tracked, shared)
    /// @param userConf       ~/.slang/server.json (user-wide)
    /// @param localConf      .slang/local/server.json (untracked, personal)
    /// Non-flag fields are merged (arrays appended, scalars overwritten by later files).
    /// For flags: workspace overrides user (last non-local wins), local always appends.
    static rfl::Result<Config> fromFiles(const std::optional<std::string>& workspaceConf,
                                         const std::optional<std::string>& userConf,
                                         const std::optional<std::string>& localConf);

    /// Per-file flags with correct precedence. Skipped during serialization.
    rfl::Skip<std::vector<FlagSource>> flagsByFile;

    struct InputSource {
        std::string path;
        std::optional<std::string> bytes;
    };
    /// Exact bytes consumed by fromFiles, including absent layers. Runtime only:
    /// method certificates must compare against loaded inputs, not a later read.
    rfl::Skip<std::vector<InputSource>> inputSources;
};

template<>
struct rfl::config::enum_descriptions<Config::HoverConfig::DocCommentFormat> {
    static constexpr bool has_descriptions = true;
    static constexpr std::string_view get(Config::HoverConfig::DocCommentFormat value) {
        switch (value) {
            case Config::HoverConfig::DocCommentFormat::plaintext:
                return "Strip comment markers and escape markdown characters so the text renders "
                       "as-is.";
            case Config::HoverConfig::DocCommentFormat::markdown:
                return "Strip comment markers and render the contents as markdown.";
            case Config::HoverConfig::DocCommentFormat::raw:
                return "Show the comment text verbatim, including comment markers, in a code "
                       "block.";
        }
        return "";
    }
};
