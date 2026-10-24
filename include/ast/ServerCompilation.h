//------------------------------------------------------------------------------
// ServerCompilation.h
// Server compilation class that tracks document dependencies
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "ActiveDesignContext.h"
#include "ConeResult.h"
#include "FrozenInstanceCache.h"
#include "HierarchicalView.h"
#include "ServerCompilationAnalysis.h"
#include "document/SlangDoc.h"
#include "lsp/LspClient.h"
#include "lsp/RequestContext.h"
#include "util/Converters.h"
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/util/Bag.h"

namespace server {
using namespace slang;

struct ActiveGenerateLoop {
    std::string activePath;
    std::vector<std::string> iterationPaths;
};

/// @brief A server compilation that is set via top level or a .f file.
/// Manages the specification of the compilation, as well as the analysis state that gets
/// refreshed lazily after edits and on file saves.
class ServerCompilation {
public:
    /// @brief Constructs a new ServerCompilation instance
    /// @param documents Vector of weak pointers to SlangDocuments this compilation is based on
    /// @param options Copy of the options bag for this compilation
    /// @param client LSP client used for user-facing notifications
    /// @param top Optional top module name (owned by this compilation)
    ServerCompilation(std::vector<std::shared_ptr<SlangDoc>> documents, Bag options,
                      SourceManager& sourceManager, lsp::LspClient& client,
                      std::optional<std::string> top = std::nullopt,
                      const ServerCompilation* previous = nullptr,
                      const lsp::RequestContext& ctx = {}, bool disposableRoot = false,
                      std::shared_ptr<const void> sourceOwner = {});

    ~ServerCompilation() = default;

    /// Update the compilation based by requesting all syntax trees from the documents
    void refresh(const lsp::RequestContext& ctx = {});
    /// Prepare complete semantics/dataflow privately before publishing a current model.
    void prepareDiagnostics(const lsp::RequestContext& ctx);
    /// Current syntax only, without committing a root model for diagnostic discovery.
    std::vector<std::shared_ptr<syntax::SyntaxTree>> getCurrentSyntaxTrees(
        const lsp::RequestContext& ctx);
    void invalidateDocuments(const std::vector<std::shared_ptr<SlangDoc>>& documents);
    void refreshDocuments(const std::vector<std::shared_ptr<SlangDoc>>& documents,
                          const lsp::RequestContext& ctx = {});

    void configureFrozenInstances(std::vector<std::string> paths, size_t budgetBytes,
                                  FrozenInstanceCache::Witness witness,
                                  std::function<void(const std::vector<std::string>&)> changed,
                                  FrozenInstanceCache::Witness requestWitness = {},
                                  const lsp::RequestContext& ctx = {});
    const FrozenInstanceCache* frozenInstances() const { return m_frozenInstances.get(); }
    void clearFrozenInstances(std::string reason);

    /// Defer rebuilding until a semantic consumer needs the latest editor text.
    void invalidate() { m_dirty = true; }

    /// Whether the current full compilation covers the latest data for this source.
    bool containsCurrentSource(const std::filesystem::path& path) const;

    InstanceIndexer& getInstances() {
        ensureCurrent();
        return m_analysis->instances;
    }

    /// Get instances by module; Used for the 'instances' view. Includes declaration metadata.
    std::vector<hier::InstanceSet> getScopesByModule(const lsp::RequestContext& ctx = {});

    /// Get instances of a specific module
    const std::vector<const slang::ast::InstanceSymbol*>& getInstancesOfModule(
        const std::string& moduleName, const lsp::RequestContext& ctx = {}) const;

    /// Return the file path that the active compilation resolved for a symbol name, if any.
    std::optional<std::string> getPreferredSymbolPath(std::string_view name,
                                                      const lsp::RequestContext& ctx = {}) const;

    /// Set the active instance and generate scope reached by `hierPath`. The path may be
    /// canonical (`top.bus8`) or an alias that resolves through an interface port
    /// (`top.tap.data_bus`); slang's lookup resolves both. Returns false if the path isn't
    /// contained by an instance.
    bool setActiveInstance(const std::string& hierPath, const lsp::RequestContext& ctx = {});

    /// Return the active instance for a module, falling back to a deterministic default.
    std::optional<hier::QualifiedInstance> getActiveInstance(const std::string& moduleName,
                                                             const lsp::RequestContext& ctx = {});

    /// Return the elaborated active instance symbol for a module, if any.
    const slang::ast::InstanceSymbol* getActiveInstanceSymbol(
        std::string_view moduleName, const lsp::RequestContext& ctx = {}) const;

    /// Capture the selected full-design instances needed by a shallow compilation.
    ActiveDesignContext createActiveDesignContext(std::span<const std::string_view> definitionNames,
                                                  const lsp::RequestContext& ctx = {}) const;

    /// Return the active iteration and available choices for a generate loop in a module.
    std::optional<ActiveGenerateLoop> getActiveGenerateLoop(
        std::string_view moduleName, const slang::syntax::LoopGenerateSyntax& loop,
        const lsp::RequestContext& ctx = {}) const;

    /// Retrun the children of the scope at the given hierarchical path
    std::vector<hier::HierItem_t> getScope(const std::string& hierPath,
                                           const lsp::RequestContext& ctx = {});

    /// Return root-to-focus scope steps with populated child lists for each path segment.
    std::vector<hier::ScopeStep> getScopes(const std::string& hierPath,
                                           const lsp::RequestContext& ctx = {});

    /// Search the elaborated hierarchy for instances, scopes, ports, parameters, and signals.
    hier::HierarchySearchResult searchHierarchy(const std::string& query,
                                                const lsp::RequestContext& ctx = {});

    /// Resolve the document location for a hierarchical path on-demand.
    std::optional<lsp::Location> getHierLocation(const std::string& hierPath,
                                                 const lsp::RequestContext& ctx = {});

    /// Return instances for given doc position
    std::vector<std::string> getInstances(const lsp::TextDocumentPositionParams&,
                                          const lsp::RequestContext& ctx = {});

    /// Prepare cone tracing using LSP call hierarchy API
    std::vector<lsp::CallHierarchyItem> getDocPrepareCallHierarchy(
        const lsp::CallHierarchyPrepareParams& params, const lsp::RequestContext& ctx = {});

    /// Deduce WCP variable vs scope
    // TODO -- remove once not needed for cone tracing
    bool isWcpVariable(const std::string& path, const lsp::RequestContext& ctx = {});

    /// Get document and position params for a given RTL path
    std::optional<lsp::ShowDocumentParams> getHierDocParams(const std::string& path,
                                                            const lsp::RequestContext& ctx = {});

    /// Issue all semantic diagnostics from the compilation to the diagnostic engine
    void issueDiagnosticsTo(
        slang::DiagnosticEngine& diagEngine, const lsp::RequestContext& ctx = {},
        const std::function<void(size_t, const slang::Diagnostic&)>& issue = {});

    /// Return the driver cone for the given RTL path, valid until the compilation is refreshed.
    ConeResult getDriverCone(const std::string& path, const lsp::RequestContext& ctx = {});

    /// Return the load cone for the given RTL path, valid until the compilation is refreshed.
    ConeResult getLoadCone(const std::string& path, const lsp::RequestContext& ctx = {});

private:
    friend class ServerDriver;
    /// Only a new, unpublished ServerDriver may prepare its initial model in place.
    void prepareUnpublishedDiagnostics(const lsp::RequestContext& ctx);
    void refreshImpl(const lsp::RequestContext& ctx, bool completeDiagnostics);
    /// Lazy semantic state is shared by hierarchy and per-document consumers.
    void ensureCurrent(const lsp::RequestContext& ctx = {}) const;
    bool m_dirty = false;
    bool m_refreshing = false;
    std::unique_ptr<FrozenInstanceCache> m_frozenInstances;
    std::function<void(const std::vector<std::string>&)> m_frozenModulesChanged;
    std::optional<FrozenInstanceCache::Certificate> certifyFrozenInstance(
        const std::string& path, const lsp::RequestContext& ctx);

    /// Return the stored active selection for a module, if any.
    std::optional<hier::QualifiedInstance> getActiveInstanceSelection(
        std::string_view moduleName) const;

    // Used when going from hierPath -> compilation symbol -> shallow compilation symbol
    // Therefore we get more up to date source locations
    const slang::ast::Symbol* toShallowSymbol(const slang::ast::Symbol& symbol,
                                              const lsp::RequestContext& ctx = {}) const;

    /// Build m_moduleToDoc from the declared symbols of each document's syntax tree.
    std::unordered_map<std::string, std::shared_ptr<SlangDoc>> indexModuleDocs(
        const lsp::RequestContext& ctx);

    /// The Slang documents this compilation is based on, keyed by their absolute source path so
    /// path-driven lookups (LSP URIs, symbol source locations) avoid scanning every doc.
    std::unordered_map<std::string, std::shared_ptr<SlangDoc>> m_documents;

    /// Maps a declared module (/interface/program/class) name to the document that declares it.
    /// Lets toShallowSymbol() find the owning shallow compilation without scanning all
    /// documents. Rebuilt in the constructor since m_documents is fixed for this compilation's
    /// lifetime.
    std::unordered_map<std::string, std::shared_ptr<SlangDoc>> m_moduleToDoc;

    /// Copy of compilation options
    // Source namespace lifetime, independent of the driver and its old models.
    std::shared_ptr<const void> m_sourceOwner;
    Bag m_options;
    // Execution control is deliberately not part of semantic / snapshot options.
    bool m_disposableRoot = false;

    /// Owned storage for top module name, used with setTopLevel
    /// CompilationOptions::topModules uses string_view, so we need to own the string here
    std::optional<std::string> m_top;

    /// Reference to the source manager for this compilation,
    /// owned by the driver
    SourceManager& m_sourceManager;

    /// LSP client owned by the server
    lsp::LspClient& m_client;

    /// The analysis state, rebuilt on refresh(). Shared by shallow compilations that are using it
    /// for params while we may be rebuilding a new one.
    std::shared_ptr<ServerCompilationAnalysis> m_analysis;

    /// Active instance and generate scope selection by module name for this compilation.
    std::unordered_map<std::string, hier::QualifiedInstance> m_activeInstancesByModule;

    /// Active iteration by fully qualified generate array path.
    std::unordered_map<std::string, std::string> m_activeGenerateScopesByArray;

    /// Flattened hierarchy entries, populated on the first search after each refresh.
    std::vector<hier::HierarchySearchItem> m_hierarchySearchItems;
    bool m_hierarchySearchIndexed = false;

    /// Retain only still-valid active instances after a refresh and seed unique modules.
    std::unordered_map<std::string, hier::QualifiedInstance> syncActiveInstances(
        const ServerCompilationAnalysis& analysis, const lsp::RequestContext& ctx) const;
};

} // namespace server
