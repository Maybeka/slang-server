// SPDX-License-Identifier: MIT
#pragma once

#include "ServerDriver.h"
#include <array>
#include <map>
#include <unordered_set>

namespace server {

/// One complete native diagnostic baseline, never a chain of edited owners.
class BodyDiagnosticCache {
public:
    BodyDiagnosticCache(ServerDriver&, const URI&, size_t begin, size_t end,
                        const std::shared_ptr<slang::syntax::SyntaxTree>&, bool lineSensitive,
                        const lsp::RequestContext&);
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
        References references;
        std::vector<std::string> calls;
        std::vector<const slang::ast::ClassType*> specializations;
    };
    Slice analyze(const std::shared_ptr<slang::syntax::SyntaxTree>&, const lsp::RequestContext&);
};
} // namespace server
