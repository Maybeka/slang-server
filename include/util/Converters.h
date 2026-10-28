//------------------------------------------------------------------------------
// Converters.h
// Type conversion utilities for LSP server, primarily between slang and LSP types
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "lsp/LspTypes.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "slang/ast/SemanticFacts.h"
#include "slang/parsing/Token.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"

namespace slang::ast {
class SubroutineSymbol;
class Symbol;
}

namespace server {

using namespace slang;

std::optional<const parsing::Token> findNameToken(const syntax::SyntaxNode* node,
                                                  std::string_view name);

/// Scoped projection of retained outer source positions after a variable-length body edit.
class BodySourceProjection {
public:
    BodySourceProjection(const SourceManager&, BufferID current, BufferID query, size_t oldEnd,
                         size_t newEnd, uint32_t baselineBufferLimit,
                         std::span<const ast::SubroutineSymbol* const> queryScopes = {},
                         std::string_view displayedScope = {});
    ~BodySourceProjection();
    BodySourceProjection(const BodySourceProjection&) = delete;
    SourceLocation project(SourceLocation, const SourceManager&) const;
    std::string projectLexicalPath(const ast::Symbol&, const SourceManager&) const;

private:
    const BodySourceProjection* previous;
    const SourceManager& sources;
    BufferID current, query;
    size_t oldEnd, newEnd;
    uint32_t baselineBufferLimit;
    std::span<const ast::SubroutineSymbol* const> queryScopes;
    std::string_view displayedScope;
};

std::string bodyQueryLexicalPath(const ast::Symbol&, const SourceManager&);

lsp::Position toPosition(const SourceLocation& loc, const SourceManager& sourceManager);

std::optional<SourceLocation> toSourceLocation(BufferID buffer, const lsp::Position& position,
                                               const SourceManager& sourceManager);

lsp::Range toRange(const SourceRange& range, const SourceManager& sourceManager);

/// Byte offset of a UTF-16 column in a UTF-8 line, clamped before CR, LF, or NUL.
/// Invalid UTF-8 bytes count as one code unit each; columns inside surrogate pairs round up.
size_t utf16ColumnToByte(std::string_view line, uint32_t character);

lsp::Location toOriginalLocation(const SourceRange& range, const SourceManager& sourceManager);

lsp::Range toRange(const SourceLocation& loc, const SourceManager& sourceManager,
                   const size_t length);

lsp::Location toLocation(const SourceRange& range, const SourceManager& sourceManager);

lsp::Location toLocation(const SourceLocation& loc, const SourceManager& sourceManager);

lsp::SymbolKind toSymbolKind(const slang::syntax::SyntaxKind& kind);

lsp::MarkupContent markdown(std::string& md);

std::string portString(ast::ArgumentDirection dir);

std::string subroutineString(ast::SubroutineKind kind);

} // namespace server
