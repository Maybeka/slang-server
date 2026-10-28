// SPDX-License-Identifier: MIT
#pragma once

#include "ServerDriver.h"
#include "document/NativeDiagnosticSnapshot.h"
#include <array>
#include <map>
#include <unordered_set>

#include "slang/ast/Compilation.h"

namespace server {
using NativeReferenceUsage = slang::ast::Compilation::ReferenceUsageMap;

// A completed native pass captured before synchronizing any edited text. Values
// are copied while current; the one original compilation owns all referenced symbols.
struct BodyDiagnosticBaseline {
    explicit BodyDiagnosticBaseline(const URI& target) : uri(target) {}
    ServerDriver* driver = nullptr;
    std::weak_ptr<const void> context;
    URI uri;
    size_t begin = 0, end = 0;
    uint32_t bufferLimit = 0;
    slang::BufferID targetBuffer;
    std::vector<std::shared_ptr<void>> sourcePins;
    std::string text;
    std::shared_ptr<slang::ast::Compilation> owner;
    std::shared_ptr<const NativeReferenceUsage> nativeReferences;
    std::vector<const slang::ast::SubroutineSymbol*> originals;
    std::unordered_set<const slang::syntax::SyntaxNode*> independentReceiverReads;
    std::unordered_set<const slang::syntax::SyntaxNode*> independentCalleeReads;
    std::array<slang::Diagnostics, 3> diagnostics;
    std::shared_ptr<const struct NativeDiagnosticSnapshot> publication;
    // owner aliases the completed ServerCompilationAnalysis and pins this manager.
    const slang::analysis::AnalysisManager* analysis = nullptr;
    const slang::analysis::AnalysisManager* completeAnalysis = nullptr;
    const slang::analysis::AnalysisManager* procedureReuseAnalysis = nullptr;
    std::vector<const slang::analysis::AnalyzedProcedure*> timingCallers;
    slang::analysis::AnalysisOptions analysisOptions;
    slang::DiagnosticEngine::SettingsSnapshot settings;
};

/// One complete native diagnostic baseline, never a chain of edited owners.
class BodyDiagnosticCache {
public:
    static std::shared_ptr<const BodyDiagnosticBaseline> capture(ServerDriver&, const URI&,
                                                                 size_t begin, size_t end,
                                                                 const lsp::RequestContext&);
    BodyDiagnosticCache(ServerDriver&, const URI&, size_t begin, size_t end,
                        const std::shared_ptr<slang::syntax::SyntaxTree>&, bool lineSensitive,
                        const lsp::RequestContext&,
                        std::shared_ptr<const BodyDiagnosticBaseline> = {},
                        const std::shared_ptr<slang::syntax::SyntaxTree>& current = {});
    void publish(const std::shared_ptr<slang::syntax::SyntaxTree>&, size_t newEnd,
                 const lsp::RequestContext&, const std::function<void()>& validate,
                 bool deferTransport = false);
    std::vector<const slang::syntax::FunctionDeclarationSyntax*> validateGeneratedMacros(
        std::string_view, size_t newEnd, const lsp::RequestContext&);
    uint64_t updates = 0;
    uint64_t reusedProcedures = 0, recomputedProcedures = 0;
    uint64_t reusedRenderings = 0, freshRenderings = 0;
    std::pair<size_t, size_t> targetRenderingCacheUsage() const {
        return {targetRenderingBaseCharge + (targetRenderingMemo ? targetRenderingMemo->charge : 0),
                targetRenderingMemo ? targetRenderingMemo->entries.size() : 0};
    }

private:
    friend class MethodScopeOverlay;
    // Immutable syntax / macro replay evidence only. Dynamic dependency and
    // filesystem proofs still run before each publication.
    void recordGeneratedMacroValidation(const std::shared_ptr<slang::syntax::SyntaxTree>& tree,
                                        size_t newEnd) {
        validatedGeneratedTree = tree;
        validatedGeneratedEnd = newEnd;
    }
    std::weak_ptr<slang::syntax::SyntaxTree> validatedGeneratedTree;
    size_t validatedGeneratedEnd = 0;
    using References = std::map<const slang::syntax::SyntaxNode*, std::pair<bool, bool>>;
    ServerDriver& driver;
    URI uri;
    size_t begin, end;
    uint32_t bufferLimit;
    bool lineSensitive;
    size_t baselineLine;
    slang::BufferID originalTarget;
    std::shared_ptr<const std::string> lineText;
    std::vector<std::shared_ptr<void>> linePins;
    std::shared_ptr<class BodyLineProof> lineProof;
    std::shared_ptr<class MacroLineProof> macroLineProof;
    std::shared_ptr<slang::ast::Compilation> owner, baselineBinding;
    std::vector<const slang::ast::ClassType*> specializations;
    std::vector<const slang::ast::SubroutineSymbol*> originals;
    std::unordered_set<const slang::syntax::SyntaxNode*> independentReceiverReads;
    std::unordered_set<const slang::syntax::SyntaxNode*> independentCalleeReads;
    std::array<std::unordered_map<URI, slang::Diagnostics>, 3> retained;
    struct CachedFormatted {
        const slang::Diagnostic* raw;
        const NativeDiagnosticSnapshot::Rendered* rendered;
    };
    std::shared_ptr<const NativeDiagnosticSnapshot> formattedPublication;
    std::vector<CachedFormatted> formattedEntries;
    std::unordered_multimap<size_t, size_t> formattedByHash;
    std::unordered_map<const slang::Diagnostic*, size_t> formattedRetained;
    // One pure-data publication, never a chain of semantic or source owners.
    struct TargetRenderingMemo {
        lsp::Position coordinate;
        bool oversized = false;
        size_t charge = 0;
        std::unordered_map<const slang::Diagnostic*,
                           std::vector<NativeDiagnosticSnapshot::PhysicalDiagnostic>> entries;
    };
    size_t targetRenderingBudget = 0, targetRenderingBaseCharge = 0;
    std::string targetRenderingPrefix, targetRenderingSuffix;
    std::unordered_set<const slang::Diagnostic*> targetRenderingEligible;
    std::unique_ptr<TargetRenderingMemo> targetRenderingMemo;
    std::array<std::unordered_map<URI, std::vector<size_t>>, 3> retainedNativeIndices;
    std::unordered_set<slang::BufferID> nativeTargetBuffers;
    bool formattedPrepared = false;
    void prepareFormatted(const lsp::RequestContext&);
    const CachedFormatted* findFormatted(const slang::Diagnostic&) const;
    std::unordered_set<URI> affectedUris;
    References references;
    std::shared_ptr<const NativeReferenceUsage> nativeReferences;
    std::vector<std::string> calls;
    bool replayEffects = false;
    bool baselineClassLookups = false;
    bool stableExternalDriverLocations = true;
    std::unordered_set<const slang::syntax::SyntaxNode*> effectSensitiveCalls;
    // owner aliases the completed ServerCompilationAnalysis and pins this manager.
    const slang::analysis::AnalysisManager* analysis = nullptr;
    const slang::analysis::AnalysisManager* completeAnalysis = nullptr;
    const slang::analysis::AnalysisManager* procedureReuseAnalysis = nullptr;
    std::vector<const slang::analysis::AnalyzedProcedure*> timingCallers;
    slang::analysis::AnalysisOptions analysisOptions;
    slang::DiagnosticEngine::SettingsSnapshot settings;
    struct Slice {
        std::shared_ptr<slang::ast::Compilation> compilation;
        std::shared_ptr<slang::analysis::AnalysisManager> replay;
        bool fullFlow = false;
        bool fullSemantic = false;
        bool hasClassLookups = false;
        bool classClosureProven = false;
        std::array<slang::Diagnostics, 3> diagnostics;
        std::shared_ptr<const struct NativeDiagnosticSnapshot> publication;
        References references;
        std::vector<std::string> calls;
        std::unordered_set<const slang::syntax::SyntaxNode*> effectSensitiveCalls;
        std::vector<const slang::ast::ClassType*> specializations;
    };
    void validateSlice(const Slice&) const;
    Slice analyze(const std::shared_ptr<slang::syntax::SyntaxTree>&, const lsp::RequestContext&);
};
} // namespace server
