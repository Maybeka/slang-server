// SPDX-License-Identifier: MIT
#pragma once

#include "lsp/RequestContext.h"
#include "util/RawBodyDirectives.h"
#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "slang/parsing/PreprocessorMetadata.h"
#include "slang/syntax/AllSyntax.h"

namespace server {
// Overapproximate every definition in the complete native macro journal, not
// merely the final environment. A closed graph with no line intrinsic, paste,
// or state-changing directive cannot change its expansion when lines move.
class LineIndependentMacros {
public:
    LineIndependentMacros(const slang::parsing::PreprocessorMetadata& history, bool complete,
                          const lsp::RequestContext& ctx, bool allowLineIntrinsic = false) :
        complete(complete && !history.macroChanges.empty()), allowLineIntrinsic(allowLineIntrinsic) {
        size_t visited = 0;
        for (const auto& change : history.macroChanges) {
            if ((visited++ & 255) == 0)
                ctx.checkpoint("later macro definition journal");
            if (change.definition) {
                auto& entries = definitions[change.name];
                if (std::ranges::find(entries, change.definition) == entries.end())
                    entries.push_back(change.definition);
            }
        }
    }

    bool independent(std::string_view text, const lsp::RequestContext& ctx,
                     std::span<const std::string_view> dynamicNames = {}, size_t depth = 0) {
        if (!complete || depth >= 128 || states.size() > 4096 ||
            inspectRawBodyDirectives(text).changesState ||
            text.find("``") != std::string_view::npos)
            return false;
        for (auto at = text.find('`'); at != std::string_view::npos;) {
            auto start = at + 1, end = start;
            while (end < text.size() && word(text[end]))
                ++end;
            if (end == start && (start == text.size() || text[start] != '"'))
                return false; // Do not overlook escaped or constructed directive names.
            if (end != start) {
                ctx.checkpoint("later declaration macro dependencies");
                const auto name = text.substr(start, end - start);
                if (name == "__LINE__" && allowLineIntrinsic) {
                    at = text.find('`', end);
                    continue;
                }
                if (name == "__LINE__" ||
                    std::ranges::find(dynamicNames, name) != dynamicNames.end())
                    return false;
                const auto found = definitions.find(name);
                if (found == definitions.end()) {
                    if (name != "__FILE__")
                        return false; // Includes dynamic macro formal substitution.
                }
                else
                    for (auto definition : found->second)
                        if (!independent(*definition, ctx, depth))
                            return false;
            }
            at = text.find('`', end);
        }
        return true;
    }

private:
    enum class State { Active, Independent, Dependent };
    bool complete;
    bool allowLineIntrinsic;
    std::unordered_map<std::string_view, std::vector<const slang::syntax::DefineDirectiveSyntax*>>
        definitions;
    std::unordered_map<const slang::syntax::DefineDirectiveSyntax*, State> states;
    static bool word(char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
               ch == '_';
    }
    bool independent(const slang::syntax::DefineDirectiveSyntax& definition,
                     const lsp::RequestContext& ctx, size_t depth) {
        const auto [it, inserted] = states.try_emplace(&definition, State::Active);
        if (!inserted)
            return it->second == State::Independent; // Recursive definitions are not proved.
        std::string text;
        std::vector<std::string_view> dynamicNames;
        if (definition.formalArguments) {
            text = definition.formalArguments->toString(); // Include defaults, not only the body.
            for (auto argument : definition.formalArguments->args)
                dynamicNames.push_back(argument->name.valueText());
        }
        for (auto token : definition.body) {
            text += token.rawText();
            text += ' ';
        }
        const bool result = independent(text, ctx, dynamicNames, depth + 1);
        states.at(&definition) = result ? State::Independent : State::Dependent;
        return result;
    }
};
} // namespace server
