//------------------------------------------------------------------------------
// InstanceIndexer.h
// AST visitor for indexing instance symbols by definition name
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#pragma once

#include <functional>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/util/ScopeGuard.h"

struct InstanceIndexer
    : public slang::ast::ASTVisitor<InstanceIndexer, slang::ast::VisitFlags::Symbols> {
public:
    std::map<std::string, std::vector<const slang::ast::InstanceSymbol*>> moduleToInstances;
    void handle(const slang::ast::InstanceSymbol& symbol) {
        if (m_checkpoint)
            m_checkpoint();
        // $unit modules- unused top modules
        if (symbol.body.flags.has(slang::ast::InstanceFlags::Uninstantiated)) {
            return;
        }
        moduleToInstances[std::string{symbol.getDefinition().name}].push_back(&symbol);
        visitDefault(symbol.body);
    }

    void reset(const slang::ast::Symbol* root, std::function<void()> checkpoint = {}) {
        m_checkpoint = std::move(checkpoint);
        auto clearCheckpoint = slang::ScopeGuard([&] { m_checkpoint = {}; });
        moduleToInstances.clear();
        root->visit(*this);
    }

    void clear() { moduleToInstances.clear(); }

    bool empty() const { return moduleToInstances.empty(); }

private:
    std::function<void()> m_checkpoint;
};
