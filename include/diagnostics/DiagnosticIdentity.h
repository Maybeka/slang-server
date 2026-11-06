// SPDX-License-Identifier: MIT
#pragma once
#include <functional>
#include <type_traits>
#include <variant>

#include "slang/diagnostics/Diagnostics.h"
#include "slang/numeric/ConstantValue.h"

// Pure raw-record identity: never binds symbols, formats values or owns models.
namespace server::diagnostic_identity {
using slang::ConstantValue;
using slang::Diagnostic;
inline size_t rawDiagnosticHash(const Diagnostic& value) {
    size_t hash = std::hash<const slang::ast::Symbol*>{}(value.symbol);
    auto add = [&](size_t part) { hash ^= part + size_t(0x9e3779b9) + (hash << 6) + (hash >> 2); };
    add(size_t(value.code.getSubsystem()));
    add(value.code.getCode());
    add(value.location.buffer().getId());
    add(value.location.offset());
    return hash;
}
// Custom arguments have no generic equality, and ConstantValue equality does not
// prove identical formatting (for example signed zero). Reformat these fresh
// diagnostics; hashes only locate candidates and never prove equality.
inline bool scalarDiagnosticEqual(const Diagnostic& a, const Diagnostic& b) {
    if (a.code != b.code || a.location != b.location || a.symbol != b.symbol ||
        a.coalesceCount != b.coalesceCount || a.ranges != b.ranges ||
        a.args.size() != b.args.size() || a.notes.size() != b.notes.size())
        return false;
    for (size_t i = 0; i < a.args.size(); ++i) {
        if (a.args[i].index() != b.args[i].index())
            return false;
        bool same = std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, Diagnostic::CustomArgType> ||
                              std::is_same_v<T, ConstantValue>)
                    return false;
                else
                    return value == std::get<T>(b.args[i]);
            },
            a.args[i]);
        if (!same)
            return false;
    }
    for (size_t i = 0; i < a.notes.size(); ++i)
        if (!scalarDiagnosticEqual(a.notes[i], b.notes[i]))
            return false;
    return true;
}

} // namespace server::diagnostic_identity
