//------------------------------------------------------------------------------
// SlangExtensions.cpp
// Functions that only deal with slang objects and could potentially go upstream
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "util/SlangExtensions.h"

#include <stdexcept>
#include <unordered_map>

#include "slang/ast/Compilation.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxKind.h"
#include "slang/syntax/SyntaxNode.h"

namespace server {

using namespace slang;

bool hasValidBuffers(const SourceManager& sm, const std::shared_ptr<syntax::SyntaxTree>& tree) {
    if (!tree)
        return false;

    // Check the main buffer and included buffers
    auto buffers = tree->getSourceBufferIds();
    for (auto buf : buffers) {
        if (buf.valid() && !sm.isLatestData(buf)) {
            return false;
        }
    }

    return true;
}

void validateUvmMacroEnvironment(const syntax::SyntaxTree& prefix, const syntax::SyntaxTree& tree) {
    // Contract: user source must not replace or undefine the external/UVM macro environment.
    std::unordered_map<std::string_view, const syntax::DefineDirectiveSyntax*> macros;
    for (auto* d : tree.getDefinedMacros())
        macros[d->name.valueText()] = d;
    for (auto* d : prefix.getDefinedMacros()) {
        auto name = d->name.valueText();
        if (!d->name.location().valid() || d->name.location() == SourceLocation::NoLocation)
            continue;
        auto it = macros.find(name);
        if (it == macros.end() || it->second != d)
            throw std::runtime_error("UVM / external macro overridden by user source");
    }
    for (const auto& ref : tree.getMacroRefs())
        if (ref.syntax && ref.syntax->kind == syntax::SyntaxKind::UndefDirective && ref.definition)
            for (auto* d : prefix.getDefinedMacros())
                if (ref.definition == d)
                    throw std::runtime_error("UVM macro undefined by user source");
}

const ast::Type& unwrapErrorType(const ast::Type& type) {
    auto& canonicalType = type.getCanonicalType();
    if (canonicalType.kind == ast::SymbolKind::ErrorType) {
        if (auto* child = canonicalType.as<ast::ErrorType>().child)
            return child->getCanonicalType();
    }
    return canonicalType;
}

const ast::Type* getTypeParameterTargetType(const ast::Type& type) {
    const ast::Type* result = nullptr;
    for (auto current = &type; auto alias = current->as_if<ast::TypeAliasType>();) {
        auto syntax = alias->getSyntax();
        const bool parameter = syntax && syntax->parent &&
                               syntax->parent->kind == syntax::SyntaxKind::TypeParameterDeclaration;
        auto scope = alias->getParentScope();
        auto view = scope ? scope->getCompilation().getInstanceBodyTypePresentation(*alias)
                          : nullptr;
        const bool transparent = view && view->transparent;
        if (!parameter && !transparent)
            break;
        const auto& target = alias->targetType.getType();
        if (target.isError())
            return nullptr;
        current = &target;
        if (parameter || result)
            result = current;
    }
    return result;
}

} // namespace server
