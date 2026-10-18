// SPDX-License-Identifier: MIT
#pragma once

#include "Config.h"
#include "ServerDriver.h"
#include "diagnostics/DiagnosticSnapshotCodec.h"
#include <array>
#include <map>
#include <optional>
#include <unordered_map>

namespace server {

/// Opt-in closed-method experiment. No base AST/symbol pointer survives preparation.
/// Phase one deliberately requires equal-length ASCII edits and unchanged line layout.
class MethodScopeOverlay {
public:
    struct Statistics {
        bool active = false;
        uint64_t accepted = 0;
        int version = 0;
    };
    MethodScopeOverlay(ServerDriver&, const Config&, const URI&, size_t editOffset,
                       const lsp::RequestContext&);
    ~MethodScopeOverlay();
    bool belongsTo(const ServerDriver&) const;
    bool valid(const Config&, const lsp::RequestContext&) const;
    /// synced becomes true before native sync starts. The caller must never replay after it.
    bool apply(const lsp::DidChangeTextDocumentParams&, const Config&, const lsp::RequestContext&,
               bool& synced);
    std::optional<std::vector<lsp::CompletionItem>> complete(const lsp::CompletionParams&,
                                                             const Config&,
                                                             const lsp::RequestContext&) const;
    std::optional<std::vector<lsp::LocationLink>> define(const lsp::DefinitionParams&,
                                                         const Config&,
                                                         const lsp::RequestContext&) const;
    std::optional<lsp::CompletionItem> resolve(const lsp::CompletionItem&, const Config&,
                                               const lsp::RequestContext&) const;
    std::optional<ServerDriver::DiagnosticSnapshot> diagnostics(const Config&,
                                                                const lsp::RequestContext&) const;
    Statistics statistics() const { return {accepted != 0, accepted, version}; }

private:
    struct DTO {
        lsp::CompletionItem item, resolved;
        bool methodOwned = false;
        bool changedDocumentation = false;
    };
    struct Site {
        size_t begin, end, declarationBegin, declarationEnd;
        size_t context;
        bool definitionAtEnd;
    };
    struct DocumentWitness {
        SlangDoc* document;
        uint32_t buffer;
        int version;
        std::string text;
    };
    ServerDriver* driver;
    URI uri;
    std::string text, configKey, optionsKey, packageName, className, functionName, functionPath;
    size_t functionBegin = 0, functionEnd = 0, bodyBegin = 0, bodyEnd = 0, localCount = 0;
    uint32_t originalBuffer = 0, currentBuffer = 0;
    int version = 0;
    uint64_t accepted = 0, inputRevision = 0, indexRevision = 0;
    std::vector<uint32_t> handles;
    std::vector<std::string> outerNames;
    std::map<std::string, std::optional<std::string>> disk;
    std::unordered_map<URI, DocumentWitness> documents;
    ServerDriver::DiagnosticSnapshot frozen;
    std::unique_ptr<diagnostic_snapshot::ReconstructedOptions> treeOptions, compilationOptions;
    // Immutable values only: candidates share unchanged interface DTOs while
    // replacing every method-owned DTO before publication.
    using DTOList = std::vector<std::shared_ptr<const DTO>>;
    std::array<DTOList, 2> lexical;
    std::vector<Site> sites;
    std::unique_ptr<slang::SourceManager> miniManager;
    std::shared_ptr<slang::syntax::SyntaxTree> miniTree;
    std::shared_ptr<ShallowAnalysis> miniAnalysis;

    static std::optional<std::string> read(const std::string&);
    static std::string plain(const SlangDoc&);
    static void require(bool, const char*);
    const Site* site(const lsp::Position&) const;
    lsp::Range range(size_t begin, size_t end) const;
    lsp::CompletionItem rebase(lsp::CompletionItem) const;
};
} // namespace server
