//------------------------------------------------------------------------------
// SlangDoc.h
// Document container for managing SystemVerilog syntax trees and analysis
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#pragma once

#include "document/ShallowAnalysis.h"
#include "lsp/LspTypes.h"
#include "lsp/RequestContext.h"
#include "lsp/URI.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <sys/types.h>
#include <vector>

#include "slang/ast/Compilation.h"
#include "slang/ast/Symbol.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

namespace server {
/// Container around an open document, syntax tree, and shallow analysis. Isn't aware of any broader
/// compilation context at the moment. Creates a syntax tree at the minimum, and an analysis if
/// required.

using namespace slang;

class DocumentHandle;
class ServerDriver;

class SlangDoc {
private:
    /// Reference to the server driver, for grabbing other dependent documents
    ServerDriver& m_driver;

    /// Reference to the source manager
    slang::SourceManager& m_sourceManager;

    /// Options bag for compilation and analysis
    slang::Bag m_options;

    /// The URI of the document
    URI m_uri;

    size_t m_wsRelativePathOffset;

    /// The buffer of the actual source text (no expansions)
    slang::SourceBuffer m_buffer;

    // Changes with editor text; survives an exact-text driver handoff only.
    uint64_t m_completionTextIdentity;

    /// The syntax tree for this document
    std::shared_ptr<slang::syntax::SyntaxTree> m_tree;

    /// A native tree may contain several ordered source roots and unit-specific
    /// preprocessing options. Keep its recipe independently of the cached tree.
    struct NativeCompilationUnit {
        std::vector<std::filesystem::path> rootPaths;
        std::vector<uint64_t> rootSortKeys;
        slang::Bag options;
        const slang::SourceLibrary* library = nullptr;
        bool isLibraryUnit = false;
        bool requiresNativeReplay = false;
    };
    std::optional<NativeCompilationUnit> m_nativeCompilationUnit;

    std::shared_ptr<syntax::SyntaxTree> m_uvmPrefixTree;
    bool m_uvmPrefixDisabled = false;

    /// Document analysis for syntax and symbol analysis.
    /// Shared so that callers can hold the analysis alive even if getAnalysis() recreates it.
    std::shared_ptr<ShallowAnalysis> m_analysis;
    uint64_t m_analysisIndexRevision = 0;
    uint64_t m_analysisContextRevision = 0;

    // Absent for disk-only buffers or text reloaded independently of the editor.
    std::optional<int> m_version;

    /// Compilation shared with the includer, used to detect context replacement.
    std::shared_ptr<ShallowCompilation> m_includeCompilation;

    /// Latest standalone buffer checked for a file containing only macro definitions.
    slang::BufferID m_macroOnlyBuffer;

    /// Whether the checked buffer defines macros without declarations or includes.
    bool m_macroOnly = false;

    // For testing
    friend class DocumentHandle;

    std::shared_ptr<ShallowAnalysis> refreshAnalysis(const lsp::RequestContext& ctx);

public:
    SlangDoc(ServerDriver& driver, URI uri, slang::SourceBuffer buffer);

    /// Macro-definition-only files use standalone analysis without include-context selection.
    bool isMacroOnly();

    /// Open a document from a native tree. Callers must mark units that can
    /// inherit macros from another tree, even if no inherited definition survives
    /// this particular parse (for example, a -C unit containing `undefineall).
    static std::shared_ptr<SlangDoc> fromTree(ServerDriver& driver,
                                              std::shared_ptr<slang::syntax::SyntaxTree> tree,
                                              bool requiresNativeReplay = false);

    bool isNativeCompilationUnit() const { return m_nativeCompilationUnit.has_value(); }
    void adoptNativeCompilationUnit(const SlangDoc& unit);
    /// Root paths in native preprocessing order; includes are not separate roots.
    std::span<const std::filesystem::path> getRootPaths() const {
        return m_nativeCompilationUnit ? std::span(m_nativeCompilationUnit->rootPaths)
                                       : std::span<const std::filesystem::path>{};
    }

    /// Inherited macros are not recorded as replayable inputs on a SyntaxTree.
    /// The registry must replay these units with the native SourceLoader instead.
    bool requiresNativeReplay() const {
        return m_nativeCompilationUnit && m_nativeCompilationUnit->requiresNativeReplay;
    }

    // Open a Document from text (LSP open)
    static std::shared_ptr<SlangDoc> fromText(ServerDriver& driver, const URI& uri,
                                              std::string_view text);

    // Open a Document from file
    static std::shared_ptr<SlangDoc> open(ServerDriver& driver, const URI& uri);

    SourceManager& getSourceManager() const { return m_sourceManager; }
    const slang::BufferID getBuffer() const { return m_buffer.id; }
    std::optional<int> getVersion() const { return m_version; }
    void setVersion(std::optional<int> version) { m_version = version; }
    uint64_t getCompletionTextIdentity() const { return m_completionTextIdentity; }
    void inheritCompletionTextIdentity(const SlangDoc& previous);
    const std::string_view getText() const;
    const URI& getURI() { return m_uri; }
    std::string_view getPath() const { return m_uri.getPath(); }
    std::string_view getWsRelativePath() const {
        return m_uri.getPath().substr(m_wsRelativePathOffset);
    }

    /// @brief Get the syntax tree, creating it if necessary
    std::shared_ptr<slang::syntax::SyntaxTree> getSyntaxTree(const lsp::RequestContext& ctx = {});

    /// Read-only library artifact; callers must not modify shared syntax.
    std::shared_ptr<slang::syntax::SyntaxTree> getPreparedLibraryPrefix() const {
        return !m_uvmPrefixDisabled && !requiresNativeReplay() ? m_uvmPrefixTree : nullptr;
    }

    /// Inspect an already-parsed tree without loading unrelated documents.
    std::shared_ptr<slang::syntax::SyntaxTree> getCachedSyntaxTree() const { return m_tree; }

    /// Inspect the last analysis context, even if edits have made its buffers stale.
    std::shared_ptr<slang::syntax::SyntaxTree> getCachedAnalysisTree() const {
        return m_analysis ? m_analysis->getSyntaxTree() : nullptr;
    }

    /// @brief Check if analysis exists without creating it
    bool hasAnalysis() const;

    // Syntax-only inputs for a private diagnostic compilation. All trees belong to
    // this document's SourceManager; callers must capture them before returning
    // to the protocol queue.
    struct AnalysisInputs {
        BufferID buffer;
        std::shared_ptr<syntax::SyntaxTree> tree;
        std::vector<std::shared_ptr<syntax::SyntaxTree>> trees;
        uint64_t indexRevision;
    };

private:
    std::shared_ptr<ShallowAnalysis> buildAnalysis(const AnalysisInputs&,
                                                   const lsp::RequestContext&);

public:
    AnalysisInputs getAnalysisInputs(const lsp::RequestContext& ctx = {});
    /// Resolve the source graph once and return an analysis matching that graph.
    std::pair<AnalysisInputs, std::shared_ptr<ShallowAnalysis>> getAnalysisAndInputs(
        const lsp::RequestContext& ctx = {});
    const Bag& getOptions() const { return m_options; }

    /// @brief Get the analysis, creating it if necessary.
    /// Returns a shared_ptr so callers can hold the analysis alive independently of this document.
    std::shared_ptr<ShallowAnalysis> getAnalysis(const lsp::RequestContext& ctx = {});

    /// @brief Drop the cached analysis so the next getAnalysis() rebuilds it.
    /// Used when an external change (e.g. active-instance selection) means cached
    /// goto/hover results must be recomputed.
    void invalidateAnalysis() { m_analysis.reset(); }

    /// At a serialized retirement boundary, release only recomputable bound models.
    /// Source text, versions, syntax and include selections remain available for rollback.
    void releaseSemanticCaches();

    /// Global exploration preprocessing inputs changed, including a file that may
    /// have entered or left the preinclude graph.
    void invalidateSyntax() {
        m_tree.reset();
        m_analysis.reset();
    }

    ////////////////////////////////////////////////
    /// Indexed Syntax Tree Methods
    ////////////////////////////////////////////////

    /// Methods dependent on the indexed syntax tree
    const slang::parsing::Token* getTokenAt(slang::SourceLocation loc) {
        return getAnalysis()->getTokenAt(loc);
    }

    const slang::parsing::Token* getWordTokenAt(slang::SourceLocation loc) {
        return getAnalysis()->getWordTokenAt(loc);
    }

    ////////////////////////////////////////////////
    /// Shallow Compilation Methods
    ////////////////////////////////////////////////

    const std::shared_ptr<slang::ast::Compilation>& getCompilation() {
        return getAnalysis()->getCompilation();
    }

    /// Return the scope at this location, if any. Does not return the root scope.
    const slang::ast::Scope* getScopeAt(slang::SourceLocation loc) {
        return getAnalysis()->getScopeAt(loc);
    }
    ////////////////////////////////////////////////
    /// File Lifecycle
    ////////////////////////////////////////////////
    void onChange(const std::vector<lsp::TextDocumentContentChangeEvent>& contentChanges);

    /// @brief Re-read the buffer from disk (used for external file changes)
    /// @return true if successful, false if the read failed
    bool reloadBuffer();

    bool textMatches(std::string_view text);
    ////////////////////////////////////////////////
    /// Lsp Functions
    ////////////////////////////////////////////////

    void issueParseDiagnostics(DiagnosticEngine& diagEngine);

    /// @brief Issue all diagnostics from this document to the given diagnostic engine
    /// Issue diagnostics to the diagnostic engine
    /// @param diagEngine The diagnostic engine to issue to
    /// @param ctx The request context used for timing logs
    void issueDiagnosticsTo(slang::DiagnosticEngine& diagEngine,
                            const lsp::RequestContext& ctx = {},
                            SemanticDiagnosticBatch* batch = nullptr);

    /// @brief For the document symbols request
    // TODO: should this use the shallow compilation instead of syntax tree?
    std::vector<lsp::DocumentSymbol> getSymbols(const lsp::RequestContext& ctx = {}) {
        return getAnalysis(ctx)->getDocSymbols(ctx);
    }

    /// Map editor coordinates into the selected analysis context. An included file has a
    /// different BufferID there from the editor's backing buffer returned by getBuffer().
    std::optional<slang::SourceLocation> getLocation(const lsp::Position& position,
                                                     const lsp::RequestContext& ctx = {});

    std::vector<lsp::DocumentLink> getDocLinks(const lsp::RequestContext& ctx = {}) {
        return getAnalysis(ctx)->getDocLinks(ctx);
    }

    std::vector<lsp::Range> getInactiveRegions(const lsp::RequestContext& ctx = {});
};

} // namespace server
template<>
struct fmt::formatter<server::SlangDoc> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

    template<typename FormatContext>
    constexpr auto format(const server::SlangDoc& doc, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "{}", doc.getWsRelativePath());
    }
};
