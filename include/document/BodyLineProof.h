// SPDX-License-Identifier: MIT
#pragma once
#include "lsp/RequestContext.h"

#include "slang/ast/Compilation.h"
#include "slang/parsing/Token.h"
#include "slang/text/SourceManager.h"

namespace server {
// Prove that an unchanged neighbour's expanded syntax differs only in an int32
// runtime input argument to an ordinary instance void function. No neighbour
// semantic graph or driver contribution is replaced by this proof.
class BodyLineProof {
public:
    BodyLineProof(slang::SourceManager&, const slang::ast::Compilation&,
                  const slang::ast::SubroutineSymbol&, slang::BufferID original, size_t editedEnd,
                  std::string_view baseline, std::string path, const lsp::RequestContext&);
    void validate(std::string_view current, size_t newEnd, const lsp::RequestContext&) const;

private:
    struct Neighbour {
        const slang::ast::SubroutineSymbol* symbol;
        size_t begin, end;
        std::vector<slang::parsing::Token> tokens;
        std::vector<bool> runtimeLines;
        std::vector<const slang::syntax::DefineDirectiveSyntax*> macros;
        slang::Bag options;
    };
    slang::SourceManager& sources;
    slang::BufferID original;
    size_t editedEnd;
    std::string path;
    std::vector<Neighbour> neighbours;
    struct Line {
        size_t offset, target;
        std::string name;
        uint8_t level;
    };
    std::vector<Line> lines;
    std::vector<slang::SourceManager::DiagnosticDirectiveInfo> pragmas;
};
} // namespace server
