//------------------------------------------------------------------------------
// SymbolIndexer.h
// Symbol indexer for AST visitors
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/text/SourceLocation.h"
#include "slang/util/SmallVector.h"

namespace server {

using SymbolList = slang::SmallVector<const slang::ast::Symbol*, 2>;

using Symdex = std::unordered_map<const slang::parsing::Token*, SymbolList>;
using Syntex = std::unordered_map<const slang::syntax::SyntaxNode*, SymbolList>;

struct SymbolIndexer
    : public slang::ast::ASTVisitor<SymbolIndexer, slang::ast::VisitFlags::Symbols> {
public:
    /// Token -> Symbol mapping
    Symdex symdex;

    /// Syntax -> Symbol mapping
    Syntex syntex;

    slang::BufferID m_buffer;

    /// Buffers enclosing this header, whose scopes must be traversed to reach its declarations.
    slang::SmallVector<slang::BufferID> enclosingBuffers;

    SymbolIndexer(slang::BufferID buffer);
    void setCheckpoint(std::function<void()> checkpoint) {
        m_checkpoint = std::move(checkpoint);
        m_visits = 0;
    }

    const slang::ast::Symbol* getSymbol(const slang::parsing::Token* node) const;

    const slang::ast::Symbol* getSymbol(const slang::syntax::SyntaxNode* node) const;

    std::span<const slang::ast::Symbol* const> getSymbols(const slang::parsing::Token* node) const;

    const slang::ast::Scope* getScopeForSyntax(const slang::syntax::SyntaxNode& syntax) const;

    slang::SmallVector<const slang::ast::Scope*, 2> getScopesForSyntax(
        const slang::syntax::SyntaxNode& syntax) const;

    // These are not in the buffer, but should be visited
    void handle(const slang::ast::RootSymbol& sym);
    void handle(const slang::ast::CompilationUnitSymbol& sym);

    // Instance-like symbols

    void handle(const slang::ast::PackageSymbol& sym);
    void handle(const slang::ast::InstanceSymbol& sym);
    void handle(const slang::ast::InstanceArraySymbol& sym);

    void handle(const slang::ast::GenerateBlockSymbol& sym) {
        indexSymbolName(sym, sym.isUnnamed);
        visitDefault(sym);
    }
    void handle(const slang::ast::GenerateBlockArraySymbol& sym) {
        indexSymbolName(sym, sym.isUnnamed);
        visitDefault(sym);
    }

    // Types
    void handle(const slang::ast::EnumType& sym) {
        // Enum types' syntax doesn't include the name
        visitDefault(sym);
    }
    void handle(const slang::ast::TypeParameterSymbol& sym) { sym.getTypeAlias().visit(*this); }
    void handle(const slang::ast::TypeAliasType& sym);
    // Anonymous types (no typedef)
    void handle(const slang::ast::TransparentMemberSymbol& sym) { sym.wrapped.visit(*this); }
    // Special case for enum values, since name may not map
    void handle(const slang::ast::EnumValueSymbol& sym);

    /// Generic symbol handler
    template<typename T>
        requires std::is_base_of_v<slang::ast::Symbol, T>
    void handle(const T& astNode) {
        indexSymbolName(astNode);
        if (shouldVisit(astNode.location.buffer())) {
            visitDefault(astNode);
        }
    }

private:
    std::function<void()> m_checkpoint;
    size_t m_visits = 0;
    void checkpoint() {
        if (m_checkpoint && (m_visits++ & 1023u) == 0)
            m_checkpoint();
    }
    /// Whether this scope belongs to the document or one of its includers.
    bool shouldVisit(slang::BufferID buffer) const {
        return buffer == m_buffer ||
               std::ranges::find(enclosingBuffers, buffer) != enclosingBuffers.end();
    }

    static const uint32_t MAX_INSTANCE_DEPTH = 8;
    /// Helper to index instance syntax (shared by InstanceSymbol and InstanceArraySymbol)
    void indexInstanceSyntax(const slang::syntax::HierarchicalInstanceSyntax& instSyntax,
                             const slang::ast::InstanceBodySymbol& instanceSymbol,
                             const slang::ast::DefinitionSymbol& definition,
                             const slang::ast::InstanceSymbol* instance,
                             const slang::ast::Scope* parentScope);

    void indexSymbolName(const slang::ast::Symbol& symbol, bool isUnnamed = false);

    void addSymbol(const slang::parsing::Token* token, const slang::ast::Symbol& symbol);
    void addSymbol(const slang::syntax::SyntaxNode* syntax, const slang::ast::Symbol& symbol);
};

} // namespace server
