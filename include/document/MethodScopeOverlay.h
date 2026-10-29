// SPDX-License-Identifier: MIT
#pragma once

#include "Config.h"
#include "ServerDriver.h"
#include "diagnostics/DiagnosticSnapshotCodec.h"
#include <array>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace server {

class BodyDiagnosticCache;
class BodyMacroProof;
class DirectoryProofPlan;
struct BodyDiagnosticBaseline;

/// Opt-in local function/task queries retaining one original lexical context.
/// The legacy closed-int overlay keeps its independent value-only implementation.
class MethodScopeOverlay {
public:
    struct InputsChanged : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    bool observedInputChange() const { return inputChanged; }
    struct Statistics {
        bool active = false;
        uint64_t accepted = 0;
        int version = 0;
        uint64_t renewals = 0;
        size_t retainedHandles = 0;
    };
    MethodScopeOverlay(ServerDriver&, const Config&, const URI&, size_t editOffset,
                       const lsp::RequestContext&);
    ~MethodScopeOverlay();
    bool belongsTo(const ServerDriver&) const;
    bool valid(const Config&, const lsp::RequestContext&) const;
    /// synced becomes true before native sync starts. The caller must never replay after it.
    bool apply(const lsp::DidChangeTextDocumentParams&, const Config&, const lsp::RequestContext&,
               bool& synced, bool deferBinding = false);
    std::optional<std::vector<lsp::CompletionItem>> complete(const lsp::CompletionParams&,
                                                             const Config&,
                                                             const lsp::RequestContext&);
    std::optional<std::vector<lsp::LocationLink>> define(const lsp::DefinitionParams&,
                                                         const Config&, const lsp::RequestContext&);
    // Outer nullopt requests native fallback; inner nullopt is a valid empty hover.
    std::optional<std::optional<lsp::Hover>> hover(
        const URI&, const lsp::Position&, const Config&, const lsp::RequestContext&,
        std::optional<Config::HoverConfig::MacroView> = {});
    std::optional<lsp::CompletionItem> resolve(const lsp::CompletionItem&, const Config&,
                                               const lsp::RequestContext&);
    std::optional<ServerDriver::DiagnosticSnapshot> diagnostics(const Config&,
                                                                const lsp::RequestContext&) const;
    bool isBodyQuery() const { return bodyQueries; }
    bool hasPreparedLocalDiagnostics() const {
        return bodyQueries && accepted && bodyDiagnostics && diagnosticDecline.empty();
    }
    bool publishLocalDiagnostics(const Config&, const lsp::RequestContext&,
                                 bool deferTransport = false);
    uint64_t localDiagnosticUpdates() const;
    std::pair<uint64_t, uint64_t> localDiagnosticProcedureCounts() const;
    std::pair<uint64_t, uint64_t> localDiagnosticRenderingCounts() const;
    std::pair<size_t, size_t> localDiagnosticRenderingCacheUsage() const;
    std::string localDiagnosticDecline() const;
    Statistics statistics() const {
        return {accepted != 0, accepted, version, renewals, handles.size()};
    }

private:
    struct DTO {
        lsp::CompletionItem item;
        // Resolution only supplies these fields. Retaining a second complete
        // CompletionItem duplicates identity JSON and all insertion metadata.
        decltype(lsp::CompletionItem::documentation) documentation;
        decltype(lsp::CompletionItem::insertText) insertText;
        decltype(lsp::CompletionItem::insertTextFormat) insertTextFormat;
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
    mutable bool inputChanged = false;
    std::unique_ptr<BodyDiagnosticCache> bodyDiagnostics;
    std::shared_ptr<const BodyDiagnosticBaseline> diagnosticBaseline;
    bool optimizedCertification(const Config&) const;
    bool validContext(const Config&, const lsp::RequestContext&) const;
    bool validForWork(const Config&, const lsp::RequestContext&) const;
    std::optional<std::vector<lsp::CompletionItem>> completeImpl(const lsp::CompletionParams&,
                                                                 const Config&,
                                                                 const lsp::RequestContext&);
    std::optional<std::vector<lsp::LocationLink>> defineImpl(const lsp::DefinitionParams&,
                                                             const Config&,
                                                             const lsp::RequestContext&);
    std::optional<std::optional<lsp::Hover>> hoverImpl(
        const URI&, const lsp::Position&, const Config&, const lsp::RequestContext&,
        std::optional<Config::HoverConfig::MacroView>);
    std::optional<lsp::CompletionItem> resolveImpl(const lsp::CompletionItem&, const Config&,
                                                   const lsp::RequestContext&);
    std::string diagnosticDecline, diagnosticCaptureDecline;
    uint64_t diagnosticUpdates = 0;
    int diagnosticBaselineVersion = -1, diagnosticCaptureVersion = -1;
    void captureLocalBaseline(const Config&, const lsp::RequestContext&);
    void prepareLocalDiagnostics(const Config&, const lsp::RequestContext&,
                                 bool completeDeferred = false);
    URI uri;
    std::string text, configKey, optionsKey, packageName, className, functionName, functionPath;
    bool bodyQueries = false, generalBody = false;
    int bodyKeywordVersion = 0;
    std::vector<std::shared_ptr<void>> macroPins;
    std::shared_ptr<ShallowAnalysis> bodyOwner;
    std::string bodyQueryScopeDisplay;
    std::vector<const slang::ast::SubroutineSymbol*> bodyOriginals;
    std::vector<const slang::ast::SubroutineSymbol*> bodyLineDependents, bodyBlocked;
    std::vector<const slang::syntax::SyntaxNode*> bodyBlockedSyntax;
    // Static membership in bodyOwner's retained CST, never physical-input admission.
    std::vector<const slang::syntax::SyntaxNode*> bodyMacroSyntaxWitness;
    bool bodyLineSensitiveDeclaration = false;
    std::unique_ptr<BodyMacroProof> bodyMacroProof;
    uint32_t bodyBufferLimit = 0;
    std::unique_ptr<slang::BumpAllocator> macroArena;
    std::vector<const slang::syntax::DefineDirectiveSyntax*> bodyMacros;
    bool generalApply(const lsp::DidChangeTextDocumentParams&, const Config&,
                      const lsp::RequestContext&, bool&, std::string next, size_t nextBodyEnd,
                      size_t nextFunctionEnd, bool bindOnly = false);
    void ensureGeneralBinding(const Config&, const lsp::RequestContext&);
    void commitGeneralText(const lsp::DidChangeTextDocumentParams&, std::string next,
                           size_t nextBodyEnd, size_t nextFunctionEnd, const lsp::RequestContext&,
                           bool& synced);
    bool generalSite(const lsp::Position&) const;
    size_t functionBegin = 0, functionEnd = 0, bodyBegin = 0, bodyEnd = 0, localCount = 0;
    size_t originalBodyEnd = 0;
    std::vector<std::string> localDeclarations;
    uint32_t originalBuffer = 0, currentBuffer = 0;
    int version = 0;
    uint64_t accepted = 0, inputRevision = 0, indexRevision = 0;
    uint64_t renewals = 0;
    uint32_t certificateEdits = 0;
    void acceptGeneralEdit();
    std::vector<uint32_t> handles;
    std::vector<std::string> outerNames;
    std::map<std::string, std::optional<std::string>> disk;
    std::unique_ptr<DirectoryProofPlan> directoryProofPlan;
    struct FileProofInput {
        const std::string* path;
        const std::optional<std::string>* expected = nullptr;
        std::optional<std::string>* captured = nullptr;
        size_t directoryGroup = size_t(-1);
    };
    // Pointers into the immutable disk map above; no successful proof is kept.
    std::vector<FileProofInput> plannedProofInputs;
    bool readProofInputs(std::span<const FileProofInput>, std::string_view checkpoint,
                         const lsp::RequestContext&) const;
    std::map<std::string, std::vector<std::string>> directories;
    std::weak_ptr<TrustedUvmLibrary> immutableUvm, immutableScopes;
    std::unordered_map<URI, DocumentWitness> documents;
    ServerDriver::DiagnosticSnapshot frozen;
    std::unique_ptr<diagnostic_snapshot::ReconstructedOptions> treeOptions, compilationOptions;
    // Immutable outer values and sparse method slots preserve native order.
    // Candidates only rebuild method values, independent of outer item count.
    using DTOList = std::vector<DTO>;
    std::array<DTOList, 2> lexical;
    std::array<std::vector<size_t>, 2> methodSlots;
    std::array<DTOList, 2> methodValues;
    std::vector<Site> sites;
    std::unique_ptr<slang::SourceManager> miniManager;
    std::shared_ptr<slang::syntax::SyntaxTree> miniTree;
    std::shared_ptr<ShallowAnalysis> miniAnalysis;

    static std::optional<std::string> read(const std::string&, bool fastPath = false,
                                           const lsp::RequestContext& ctx = {});
    static bool readMatches(const std::string&, const std::optional<std::string>&, bool fastPath,
                            const lsp::RequestContext&);
    static std::string plain(const SlangDoc&);
    static void require(bool, const char*);
    const DTO& completion(size_t context, size_t index) const;
    const Site* site(const lsp::Position&) const;
    lsp::Range range(size_t begin, size_t end) const;
    lsp::CompletionItem rebase(lsp::CompletionItem, bool methodOwned = false) const;
};
} // namespace server
