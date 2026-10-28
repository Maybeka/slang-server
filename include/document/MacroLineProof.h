// SPDX-License-Identifier: MIT
#pragma once
#include "lsp/RequestContext.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "slang/parsing/Token.h"
#include "slang/syntax/SyntaxTree.h"

namespace slang::analysis {
class AnalysisManager;
}
namespace slang::ast {
class Compilation;
class SubroutineSymbol;
}
namespace server {
// Native token/semantic evidence for unchanged later macro invocations, including
// invocations that generate only part of a function. File admission is separate.
class MacroLineProof {
public:
    MacroLineProof(slang::SourceManager&, const slang::ast::Compilation&,
                   const slang::analysis::AnalysisManager&, slang::BufferID, size_t editedEnd,
                   std::shared_ptr<const std::string> baseline, std::string path,
                   const lsp::RequestContext&);
    void validate(std::string_view current, size_t newEnd, const lsp::RequestContext&) const;
    void validateDependencies() const;
    const auto& functionContexts() const { return functions; }

private:
    struct Expansion {
        size_t begin, end;
        std::vector<slang::parsing::Token> tokens;
        std::vector<bool> runtimeLines;
        std::vector<const slang::syntax::DefineDirectiveSyntax*> macros;
        slang::Bag options;
    };
    slang::SourceManager& sources;
    slang::BufferID original;
    size_t editedEnd;
    std::shared_ptr<const std::string> baseline;
    std::string path;
    std::vector<Expansion> expansions;
    std::vector<const slang::ast::SubroutineSymbol*> methods;
    std::vector<const slang::syntax::FunctionDeclarationSyntax*> functions;
    struct ReplayedToken {
        slang::parsing::TokenKind kind;
        std::string raw;
        bool isLine;
        std::optional<int32_t> number;
    };
    std::vector<ReplayedToken> replay(const Expansion&, std::string_view, size_t,
                                      const lsp::RequestContext&, slang::BumpAllocator&) const;
};
} // namespace server
