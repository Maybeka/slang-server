// SPDX-License-Identifier: MIT
#pragma once

#include "ServerDriver.h"
#include <array>
#include <map>
#include <unordered_set>

namespace server {

// A completed native pass captured before synchronizing any edited text. Values
// are copied while current; the one original compilation owns all referenced symbols.
struct BodyDiagnosticBaseline {
    explicit BodyDiagnosticBaseline(const URI& target) : uri(target) {}
    ServerDriver* driver = nullptr;
    std::weak_ptr<const void> context;
    URI uri;
    size_t begin = 0, end = 0;
    uint32_t bufferLimit = 0;
    std::string text;
    std::shared_ptr<slang::ast::Compilation> owner;
    std::vector<const slang::ast::SubroutineSymbol*> originals;
    std::array<slang::Diagnostics, 3> diagnostics;
    std::shared_ptr<const struct NativeDiagnosticSnapshot> publication;
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
                 const lsp::RequestContext&, const std::function<void()>& validate);
    uint64_t updates = 0;

private:
    using References = std::map<const slang::syntax::SyntaxNode*, std::pair<bool, bool>>;
    ServerDriver& driver;
    URI uri;
    size_t begin, end;
    uint32_t bufferLimit;
    bool lineSensitive;
    size_t baselineLine;
    std::shared_ptr<slang::ast::Compilation> owner, baselineBinding;
    std::vector<const slang::ast::ClassType*> specializations;
    std::vector<const slang::ast::SubroutineSymbol*> originals;
    std::array<std::unordered_map<URI, slang::Diagnostics>, 3> retained;
    std::unordered_set<URI> affectedUris;
    References references;
    std::vector<std::string> calls;
    slang::analysis::AnalysisOptions analysisOptions;
    slang::DiagnosticEngine::SettingsSnapshot settings;
    struct Slice {
        std::shared_ptr<slang::ast::Compilation> compilation;
        std::array<slang::Diagnostics, 3> diagnostics;
        std::shared_ptr<const struct NativeDiagnosticSnapshot> publication;
        References references;
        std::vector<std::string> calls;
        std::vector<const slang::ast::ClassType*> specializations;
    };
    void validateSlice(const Slice&) const;
    Slice analyze(const std::shared_ptr<slang::syntax::SyntaxTree>&, const lsp::RequestContext&);
};
} // namespace server
