//------------------------------------------------------------------------------
// ServerDriver.h
// Server driver class for processing syntax trees with indexing support
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once
#include "Config.h"
#include "ServerDiagClient.h"
#include "SlangLspClient.h"
#include "ast/ServerCompilation.h"
#include "codeactions/CodeActionDispatch.h"
#include "completions/CompletionDispatch.h"
#include "diagnostics/DiagnosticWorker.h"
#include "document/DefinitionInfo.h"
#include "document/FrozenSourceRegistry.h"
#include "lsp/RequestContext.h"
#include "lsp/URI.h"
#include "util/SourceContextHints.h"
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include "slang/driver/Driver.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"
#include "slang/util/FlatMap.h"
namespace server {
using namespace slang;

/// @brief Manages the document handles, which include open and referenced symbols/documents.
/// Syntax trees and options are used to build one, after flags are processed via a slang driver.
/// Compilations can be created either from a document (using the index to populate) or the existing
/// options passed in a filelist
class ServerDriver {
    friend class MethodScopeOverlay;

public:
    /// Create an explore driver; failed loads return null when requireValidConfig is set.
    static std::unique_ptr<ServerDriver> createForExplore(
        Indexer& indexer, SlangLspClient& client, const Config& config,
        std::optional<std::string_view> workspaceFolder = std::nullopt,
        const ServerDriver* oldDriver = nullptr, bool requireValidConfig = false,
        bool deferOpenDiagnostics = false);

    /// Create a build driver; failed loads return null when requireValidConfig is set.
    static std::unique_ptr<ServerDriver> createFromFileLists(
        Indexer& indexer, SlangLspClient& client, const Config& config,
        std::vector<std::string> buildfiles,
        std::optional<std::string_view> workspaceFolder = std::nullopt,
        const ServerDriver* oldDriver = nullptr, bool requireValidConfig = false);
    /// Mapping of URI to SlangDoc, which may hold a shallow analysis of the document
    std::unordered_map<URI, std::shared_ptr<SlangDoc>> docs;

    // Owned by the Slang Driver
    SourceManager& sm;
    DiagnosticEngine& diagEngine;
    SlangLspClient& client;

    driver::Driver driver;

    /// Options parsed from the flags on creation
    Bag options;

    // References m_client, receives diags from the engine
    std::shared_ptr<ServerDiagClient> diagClient;
    // The current compilation, if one has been created
    std::unique_ptr<ServerCompilation> comp;

    CompletionDispatch completions;
    CodeActionDispatch codeActions;

    /// Release disposable models before constructing a replacement at a quiescent boundary.
    void prepareForContextRetirement();

    bool isUvmPrefixCacheEnabled() const {
        return m_config.experimentalUvmPrefixCache.value() &&
               (!m_config.experimentalFrozenSources.value() || m_frozenWitnessUsable);
    }
    bool isFrozenSourcesEnabled() const { return m_config.experimentalFrozenSources.value(); }
    std::optional<std::string> frozenInputWitnessFailure(
        slang::function_ref<void()> checkCancelled = {}) const;
    /// Only call after the complete current-input witness succeeds in this dispatch.
    void noteSuccessfulFrozenInputWitness(const lsp::RequestContext& ctx) const {
        ctx.recordInputWitness(m_snapshotOwner, m_snapshotRevision);
    }
    std::shared_ptr<syntax::SyntaxTree> resumeFrozenSources(
        std::span<const std::filesystem::path> paths, std::span<const SourceBuffer> buffers,
        const Bag& options, bool nativeReplay);
    rfl::Generic frozenSourcesStatus() const;
    void refreshFrozenSources();
    void setFrozenWitnessFailure(const std::optional<std::string>& reason);
    void updateFrozenSourcePlans(std::span<const std::filesystem::path> paths,
                                 const std::shared_ptr<syntax::SyntaxTree>& tree);
    std::shared_ptr<syntax::SyntaxTree> getFrozenIndependentTree(std::string_view path,
                                                                 const Bag& options);

    /// @brief Destructor
    ~ServerDriver() {
        // Clear diags from this driver
        diagClient->clearAndPush();
        docs.clear(); // Release document owners before the SDK Driver / SourceManager.
    };

    /// @brief Gets a document by URI, creating it if it doesn't exist
    void openDocument(const URI& uri, const std::string_view text,
                      std::optional<int> version = std::nullopt,
                      bool deferConfiguredDiagnostics = false);

    /// @brief Close a document and remove it from the open docs set
    void closeDocument(const URI& uri);

    /// @brief Reload a document from disk, used when external tools modify open files
    void reloadDocument(const URI& uri);

    uint64_t getDependencyIndexRevision() const { return m_indexer.getRevision(); }

    struct DiagnosticSnapshot {
        std::shared_ptr<const void> owner;
        uint64_t revision;
        uint64_t indexRevision;
        uint64_t diagnosticGeneration;
        std::vector<BufferID> buffers;
        DiagnosticJobMetadata metadata;
        std::shared_ptr<const diagnostic_snapshot::Input> input;
    };
    std::optional<DiagnosticSnapshot> captureDiagnosticSnapshot(const lsp::RequestContext& ctx);
    bool isDiagnosticSnapshotCurrent(const DiagnosticSnapshot&) const;
    void commitDiagnosticSnapshot(const DiagnosticSnapshot&, DiagnosticJobOutput,
                                  const lsp::RequestContext& ctx);

    bool hasPendingDiagnostics() const { return !m_pendingIncludedDiagnostics.empty(); }
    void runPendingDiagnostics(const lsp::RequestContext& ctx = {});

    void onDocDidChange(const lsp::DidChangeTextDocumentParams& params,
                        const lsp::RequestContext& ctx = {});

    /// @brief Update the index and refresh relevant diagnostics after a document is saved
    void onDocDidSave(SlangDoc& doc);

    /// @brief Checks if a document is open
    bool isDocumentOpen(const URI& uri);

    /// @brief Handle workspace file change notifications from the file watcher
    /// Reloads disk-backed dependencies before analysis, preserving editor-owned buffers.
    void onWorkspaceDidChangeWatchedFiles(const lsp::DidChangeWatchedFilesParams& params);

    std::shared_ptr<SlangDoc> getDocument(const URI& uri);

    /// Record the active instance for the module reached by `instPath`, then invalidate
    /// every open document's cached analysis so subsequent hovers/gotos see the new
    /// selection. Refreshes client-side code lenses and inlay hints.
    bool setActiveInstance(std::string_view hierPath);

    /// Load nearest indexed dependencies, recursing through packages and interfaces.
    /// When fullHierarchy is set, also recurse through instantiated modules.
    std::vector<std::shared_ptr<syntax::SyntaxTree>> getDependentTrees(
        std::shared_ptr<syntax::SyntaxTree> tree, bool fullHierarchy = false);

    /// Infer include paths for this syntax tree only, then refresh its include relationships.
    std::shared_ptr<syntax::SyntaxTree> parseShallowTree(SourceBuffer buffer, Bag& documentOptions);

    /// Identifies a file's selected enclosing source and occurrence within that source.
    struct IncludeContextSelection {
        /// File whose analysis should use this context.
        URI uri;
        /// Root source containing the include, possibly through other headers.
        URI source;
        /// Zero-based occurrence of this file in the root source's include list.
        size_t occurrence = 0;
    };

    /// A parsed include occurrence and the source that owns its compilation.
    struct IncludeContext {
        /// Root document providing the compilation and preprocessor context.
        std::shared_ptr<SlangDoc> source;
        /// Buffer for this particular include occurrence.
        BufferID buffer;
        /// Immediate include directive, used for navigation.
        lsp::Location location;
        /// Stable ordinal among this file's occurrences in the root source.
        size_t occurrence;
    };

    /// Find active include occurrences, with the selected context first.
    /// selectedOnly stops after finding a usable context for document analysis.
    std::vector<IncludeContext> getIncludeContexts(const std::filesystem::path& path,
                                                   bool selectedOnly = false);

    /// Select a validated include occurrence and refresh open documents and annotations.
    bool setIncludeContext(const IncludeContextSelection& selection);

    struct PackageIncludeContext {
        std::shared_ptr<syntax::SyntaxTree> tree;
        BufferID buffer;
    };

    /// Find an already-known including root without parsing the rest of the workspace.
    std::optional<PackageIncludeContext> getIncludeContext(
        const URI& uri, const syntax::SyntaxTree* previous = nullptr);

    bool isPackageContextDeferred(const URI& uri) const;
    bool hasExplicitContextError(const URI& uri) const;
    void requireExplicitContext(const URI& uri) const;
    void issueExplicitContextDiagnostics(const URI& uri);
    bool isPackageOnlyContext(const URI& uri) const;
    void updatePackageContextKind(const URI& uri, const syntax::SyntaxTree& tree);
    uint64_t getExplicitContextRevision(const URI& uri) const;
    bool hasConfiguredCompilationInputs() const {
        return m_config.compilationFilelist.value().has_value() ||
               m_config.uvm.value().has_value() || !m_configuredUnits.empty();
    }
    bool hasSourceContextHints() const { return !m_explicitContexts.empty(); }
    uint64_t getContextInputRevision() const { return m_snapshotRevision; }
    std::vector<std::filesystem::path> getContextWatchPaths() const;
    bool configuredInputLoadFailed() const { return m_nativeInputsFailed; }
    bool isUvmConfigurationInput(const std::filesystem::path& path) const;
    uint64_t getOwnerDiscoveryAttemptCount() const { return m_ownerDiscoveryAttempts; }
    const SemanticDiagnosticBatch::Statistics& getSemanticDiagnosticBatchStatistics() const {
        return m_semanticDiagnosticStatistics;
    }
    void refreshOpenIncludeContexts();

    /// Ordered preprocessing sources for an exploration document. Macro-header
    /// documents themselves are parsed normally, including nested preinclude files.
    std::vector<SourceBuffer> getExplorePreincludeBuffers(BufferID target);
    bool isExplorePreinclude(const URI& uri) const;
    void issueExplorePreincludeDiagnostics(BufferID target);

    std::vector<std::shared_ptr<syntax::SyntaxTree>> getDependentTrees(
        std::shared_ptr<syntax::SyntaxTree> tree, const URI* target);

    std::vector<std::string> getModulesInFile(const std::string& path);

    /// @brief Gets definition information for a symbol at an LSP position, used for
    /// hovers and definitions
    /// @param uri The URI of the document
    /// @param position The LSP position to query
    /// @return Optional definition information
    std::optional<DefinitionInfo> getDefinitionInfoAt(const URI& uri, const lsp::Position& position,
                                                      const lsp::RequestContext& ctx = {});

    /// Return the concrete full-design instance referenced by an instantiation token.
    std::optional<std::string> getDesignInstancePathAt(const URI& uri,
                                                       const lsp::Position& position,
                                                       const lsp::RequestContext& ctx = {});

    /// @brief Gets hover information for a symbol at an LSP position
    /// @param uri The URI of the document
    /// @param position The LSP position to query
    /// @return Optional hover information, or nullopt if none available
    std::optional<lsp::Hover> getDocHover(
        const URI& uri, const lsp::Position& position, const lsp::RequestContext& ctx = {},
        std::optional<Config::HoverConfig::MacroView> macroView = {});

    /// @brief Gets highlight positions for a symbol and all its references in a document
    /// @param uri The URI of the document
    /// @param position The LSP position to query
    /// @return Optional highlight information, or nullopt if no symbol found
    std::optional<std::vector<lsp::DocumentHighlight>> getDocDocumentHighlight(
        const URI& uri, const lsp::Position& position);

    /// @brief Gets all references to a symbol in a document
    /// @param uri The URI of the document
    /// @param position The LSP position to query
    /// @param includeDeclaration Whether to include the declaration in results
    /// @return Optional vector of locations, or nullopt if no symbol found
    std::optional<std::vector<lsp::Location>> getDocReferences(const URI& uri,
                                                               const lsp::Position& position,
                                                               bool includeDeclaration,
                                                               const lsp::RequestContext& ctx = {});

    /// @brief Renames a symbol in a document
    /// @param uri The URI of the document
    /// @param position The LSP position to query
    /// @param newName The new name for the symbol
    /// @return Optional workspace edit with all rename changes, or nullopt if no symbol found
    std::optional<lsp::WorkspaceEdit> getDocRename(const URI& uri, const lsp::Position& position,
                                                   std::string_view newName);

    static std::unique_ptr<ServerDriver> createFromTop(
        Indexer& indexer, SlangLspClient& client, const Config& config, const URI& topUri,
        std::optional<std::string_view> workspaceFolder, const ServerDriver* oldDriver,
        bool requireValidConfig = false);

    /// @brief Constructs a new ServerDriver instance by creating and configuring a driver
    /// internally
    /// @param indexer Reference to an indexer for symbol/macro indexing
    /// @param client Reference to the slang client for error reporting
    /// @param config Reference to the configuration object
    /// @param buildfiles List of build files to process
    /// @param requireValidConfig Log load errors without sending them to the editor.
    ServerDriver(Indexer& indexer, SlangLspClient& client, const Config& config,
                 std::vector<std::string> buildfiles,
                 std::optional<std::string_view> workspaceFolder, bool requireValidConfig = false,
                 const ServerDriver* overlays = nullptr);

    size_t getWsRelativePathOffset(std::string_view path) const {
        return path.starts_with(m_workspacePathPrefix) ? m_workspacePathPrefix.size() : 0;
    }

    /// Map from macro name to the config/build file that defined it
    flat_hash_map<std::string, std::filesystem::path> m_defineSources;

    /// When true (default in `SLANG_DEBUG` builds), `getDocHover` returns a synthetic
    /// `**Token:** ...` hover for tokens with no resolved definition. Tests turn this off so
    /// goldens don't diverge between Debug and Release builds.
    static bool s_debugHoversEnabled;

private:
    SemanticDiagnosticBatch::Statistics m_semanticDiagnosticStatistics;

    // This syntax-only probe discovers nested macro headers; no compilation is built.
    // Actual document trees parse the preincludes again and own all expanded tokens.
    std::shared_ptr<syntax::SyntaxTree> m_explorePreincludeTree;
    flat_hash_set<URI> m_explorePreincludeUris;
    flat_hash_set<std::string> m_reportedPreincludeFailures;
    uint64_t m_explorePreincludeRevision = 0;
    std::optional<uint64_t> m_publishedPreincludeRevision;
    flat_hash_set<URI> m_publishedPreincludeUris;

    /// Reference to the indexer for module/macro indexing
    Indexer& m_indexer;

    /// Configuration owned by this driver, including while a replacement is being validated.
    Config m_config;
    std::unique_ptr<FrozenSourceRegistry> m_frozenSources;
    FrozenSourcePlans m_frozenPlans;
    std::vector<std::string> m_frozenInstanceModules;
    std::map<std::vector<std::filesystem::path>, std::string> m_frozenDeclinedUnits;
    std::set<std::string> m_frozenAllowedPaths;
    bool m_frozenWitnessUsable = true;
    bool m_captureFrozenNativeSeed = false;
    void updateFrozenAllowedPaths(std::span<const std::shared_ptr<syntax::SyntaxTree>> trees);
    struct FrozenCommandInput {
        std::filesystem::path path;
        std::string bytes;
        std::error_code error;
    };
    std::vector<FrozenCommandInput> m_frozenCommandInputs;
    void configureFrozenSources(std::span<const std::shared_ptr<syntax::SyntaxTree>> trees);
    void configureFrozenInstances();

    /// Whether config flags, command files, and source files loaded successfully.
    bool m_configLoaded = false;

    std::string m_workspacePathPrefix;

    /// Parse config flags and build files, load sources, create documents
    bool parseAndLoadSources(const std::vector<std::string>& buildfiles, bool requireValidConfig);

    /// Drop analysis state derived from the active design and refresh client-side annotations.
    void invalidateAnalysesAndRefreshClient();

    std::optional<DefinitionInfo> getMacroDefinitionInfo(const ShallowAnalysis& analysis,
                                                         const parsing::Token& token,
                                                         const syntax::SyntaxNode& referenceSyntax,
                                                         const lsp::RequestContext& ctx);

    /// Set of URIs for documents that are explicitly opened by the client
    flat_hash_set<URI> m_openDocs;

    /// Included-header diagnostics awaiting refresh after an owner edit. Keep these
    /// across superseded didChange notifications until analysis actually completes.
    std::shared_ptr<const void> m_snapshotOwner = std::make_shared<int>(0);
    uint64_t m_snapshotRevision = 0;
    uint64_t m_diagnosticGeneration = 0;
    uint64_t m_fullDiagnosticGeneration = 0;
    flat_hash_map<URI, uint64_t> m_pendingIncludedDiagnostics;

    enum class OwnerDiscoveryState { None, Unique, Ambiguous, LimitExceeded };
    struct OwnerDiscoveryCache {
        uint64_t indexRevision = 0;
        uint64_t ownerEpoch = 0;
        OwnerDiscoveryState state = OwnerDiscoveryState::None;
        bool oneCandidate = false;
        std::optional<URI> root;
    };
    std::unordered_map<URI, OwnerDiscoveryCache> m_ownerDiscovery;
    flat_hash_set<URI> m_automaticOwnerRoots;
    uint64_t m_ownerEpoch = 0;
    uint64_t m_ownerDiscoveryAttempts = 0;
    // Weak keys never retain obsolete parse trees after an edit. Cached names are
    // valid only for the immutable tree identity that produced them.
    using TreeIdentity = std::weak_ptr<syntax::SyntaxTree>;
    std::map<TreeIdentity, std::vector<std::string>, std::owner_less<TreeIdentity>>
        m_virtualInterfaceReferences;

    /// Every source file covered by the build, including secondary single-unit buffers.
    flat_hash_set<URI> m_buildSourceUris;
    bool m_designUsesConfiguredUnits = false;
    std::vector<std::shared_ptr<SlangDoc>> m_configuredUnits;
    bool m_nativeInputsFailed = false;
    bool m_configurationInputsFailed = false;
    std::filesystem::path m_uvmEntryPath;
    std::filesystem::path m_uvmDirectory;
    bool m_uvmLibraryOnly = false;
    std::vector<std::filesystem::path> m_uvmWatchPaths;
    bool configureUvmLibrary(const std::filesystem::path& base, bool requireValidConfig);
    bool completeUvmLibraryInputs(bool requireValidConfig);
    bool uvmConfigurationError(std::string_view message, bool requireValidConfig);
    uint64_t m_configuredUnitRevision = UINT64_MAX;
    std::vector<std::shared_ptr<syntax::SyntaxTree>> m_currentConfiguredTrees;
    struct CachedSourceHints {
        const char* data = nullptr;
        size_t size = 0;
        SourceContextHints hints;
    };
    std::unordered_map<std::string, CachedSourceHints> m_sourceHints;
    std::map<TreeIdentity, SourceContextImportHints, std::owner_less<TreeIdentity>>
        m_boundSourceHints;
    uint64_t m_boundHintsRevision = UINT64_MAX;
    std::unordered_map<URI, uint64_t> m_explicitResolutionRevision;
    std::unordered_map<URI, std::vector<lsp::Diagnostic>> m_explicitContextErrors;
    flat_hash_set<URI> m_explicitContexts;
    flat_hash_set<URI> m_packageOnlyContexts;
    void refreshConfiguredUnits();
    void installConfiguredUnits(std::vector<std::shared_ptr<syntax::SyntaxTree>> trees);
    SourceContextHints getSourceHints(BufferID buffer);
    const SourceContextImportHints& getImportHints(const std::shared_ptr<syntax::SyntaxTree>& tree);
    std::optional<PackageIncludeContext> getExplicitIncludeContext(const URI& uri, bool& handled);
    std::optional<BufferID> findPackageInclude(const syntax::SyntaxTree& tree,
                                               const std::filesystem::path& target) const;
    void addExplicitContextError(const URI& target, std::string message,
                                 std::optional<SourceContextHintRange> hint = {});
    std::vector<std::shared_ptr<syntax::SyntaxTree>> getExplicitDependencies(
        const std::shared_ptr<syntax::SyntaxTree>& tree, const URI& target, bool& handled);
    std::shared_ptr<syntax::SyntaxTree> getHintedPackageTree(
        const std::shared_ptr<syntax::SyntaxTree>& tree, std::string_view name, const URI& target,
        bool& handled);

    /// Explicit include selections by canonical header path, preserved across driver reloads.
    std::unordered_map<std::filesystem::path, IncludeContextSelection> m_includeSelections;

    void copyOpenDocumentsFrom(const ServerDriver* oldDriver,
                               bool deferConfiguredDiagnostics = false);

    /// @brief Run shallow analysis and publish diagnostics and inactive regions for one document
    void analyzeDocument(SlangDoc& doc, const lsp::RequestContext& ctx = {},
                         SemanticDiagnosticBatch* batch = nullptr);

    /// @brief Publish diagnostics for the active full compilation, optionally prioritizing a URI
    void publishCompilationDiagnostics(const URI* priorityUri = nullptr,
                                       const lsp::RequestContext* ctx = nullptr);

    /// Helper to add member references to the references vector
    void addMemberReferences(std::vector<lsp::Location>& references,
                             const ast::Symbol& parentSymbol, const ast::Symbol& targetSymbol,
                             bool isTypeMember = false, const lsp::RequestContext& ctx = {});

    void publishInactiveRegions(SlangDoc& doc, const lsp::RequestContext& ctx = {});
};
} // namespace server
