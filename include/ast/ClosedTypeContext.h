// SPDX-License-Identifier: MIT
#pragma once
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"

namespace server {
// A temporary plan, consumed during independent-owner construction. It retains
// no previous consumer after binding: only closed declarations and origin keys
// survive in the new owner.
struct ClosedTypeContext {
    struct Dependency {
        std::string name;
        const slang::ast::Type* original;
    };
    const slang::ast::Compilation* fixedOwner = nullptr;
    std::string declarations;
    std::vector<Dependency> dependencies;
    std::unordered_map<const slang::ast::Type*, std::string> names;
    std::unordered_set<std::string> enumNames;

    static std::optional<std::string> identifier(std::string_view name) {
        if (name.empty() || name.find_first_of(" \t\r\n") != std::string_view::npos)
            return {};
        return "\\" + std::string(name) + " ";
    }

    std::optional<std::string> add(const slang::ast::Type& original, unsigned depth = 0) {
        using namespace slang;
        using namespace slang::ast;
        if (depth > 64 || names.size() >= 1024)
            return {};
        auto scope = original.getParentScope();
        if (original.kind == SymbolKind::TypeAlias && scope &&
            scope->asSymbol().kind == SymbolKind::Package && &scope->getCompilation() == fixedOwner)
            return original.toString();
        // A class-scoped nominal type also depends on its specialization.
        // Until that provenance is captured, keep this boundary native.
        for (auto parent = scope; parent; parent = parent->asSymbol().getParentScope())
            if (parent->asSymbol().kind == SymbolKind::ClassType ||
                parent->asSymbol().kind == SymbolKind::CovergroupType)
                return {};
        const auto& type = original.getCanonicalType();
        if (auto found = names.find(&original); found != names.end())
            return found->second;
        const auto name = "__slang_scope_closed_type_" + std::to_string(names.size());
        names.emplace(&original, name);
        std::string declaration;
        auto child = [&](const Type& value) { return add(value, depth + 1); };
        if (original.kind == SymbolKind::TypeAlias) {
            auto target = child(original.as<TypeAliasType>().targetType.getType());
            if (!target)
                return {};
            declaration = "typedef " + *target + " " + name + ";\n";
        }
        else if (type.kind == SymbolKind::PackedArrayType) {
            const auto& array = type.as<PackedArrayType>();
            auto element = child(array.elementType);
            if (!element)
                return {};
            declaration = "typedef " + *element + " [" + std::to_string(array.range.left) + ":" +
                          std::to_string(array.range.right) + "] " + name + ";\n";
        }
        else if (type.kind == SymbolKind::FixedSizeUnpackedArrayType) {
            const auto& array = type.as<FixedSizeUnpackedArrayType>();
            auto element = child(array.elementType);
            if (!element)
                return {};
            declaration = "typedef " + *element + " " + name + " [" +
                          std::to_string(array.range.left) + ":" +
                          std::to_string(array.range.right) + "];\n";
        }
        else if (type.kind == SymbolKind::EnumType) {
            const auto& enumeration = type.as<EnumType>();
            auto base = child(enumeration.baseType);
            if (!base)
                return {};
            declaration = "typedef enum " + *base + " {";
            bool first = true;
            for (const auto& value : enumeration.values()) {
                auto label = identifier(value.name);
                if (!label || !enumNames.emplace(value.name).second ||
                    !value.getValue().isInteger())
                    return {};
                if (!first)
                    declaration += ",";
                first = false;
                declaration += *label + " = " +
                               value.getValue().integer().toString(LiteralBase::Binary, true);
            }
            declaration += "} " + name + ";\n";
        }
        else if (type.kind == SymbolKind::PackedStructType ||
                 type.kind == SymbolKind::PackedUnionType) {
            const bool structure = type.kind == SymbolKind::PackedStructType;
            if (!structure &&
                (type.as<PackedUnionType>().isTagged || type.as<PackedUnionType>().isSoft))
                return {};
            const auto& scope = structure ? static_cast<const Scope&>(type.as<PackedStructType>())
                                          : static_cast<const Scope&>(type.as<PackedUnionType>());
            declaration = std::string("typedef ") + (structure ? "struct" : "union") + " packed " +
                          (type.isSigned() ? "signed" : "unsigned") + " {\n";
            for (const auto& field : scope.membersOfType<FieldSymbol>()) {
                auto element = child(field.getType());
                auto label = identifier(field.name);
                if (!element || !label)
                    return {};
                declaration += *element + " " + *label + ";\n";
            }
            declaration += "} " + name + ";\n";
        }
        else if (type.isSimpleBitVector() || type.isString()) {
            declaration = "typedef " + type.toString() + " " + name + ";\n";
        }
        else
            return {};
        declarations += declaration;
        dependencies.push_back({name, &original});
        return name;
    }
};
} // namespace server
