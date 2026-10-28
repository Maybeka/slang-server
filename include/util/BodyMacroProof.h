// SPDX-License-Identifier: MIT
#pragma once

#include "util/LineIndependentMacros.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/text/SourceManager.h"
#include <stdexcept>

namespace server {
// This is lexical evidence from the original complete unit, never a successful
// input/version certificate. The caller still authenticates every publication.
class BodyMacroProof {
    struct Effect {
        size_t offset;
        std::string name, text;
        bool operator==(const Effect&) const = default;
    };
    std::shared_ptr<const slang::syntax::SyntaxTree> original;
    std::vector<Effect> baseline;
    std::vector<const slang::syntax::DefineDirectiveSyntax*> ownedExit;
    std::span<const slang::syntax::DefineDirectiveSyntax* const> exit;
    // A static property of the retained immutable graph, not input authentication.
    mutable bool shiftedLookupIdentityProved = false;
    auto exitPosition(std::string_view name) const {
        return std::lower_bound(exit.begin(), exit.end(), name, [](auto definition, auto key) {
            return definition->name.valueText() < key;
        });
    }
    bool exitContains(std::string_view name) const {
        auto found = exitPosition(name);
        return found != exit.end() && (*found)->name.valueText() == name;
    }
    slang::SourceLocation end;
    static void require(bool ok, const char* reason) {
        if (!ok) throw std::runtime_error(reason);
    }
    static bool word(char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '_';
    }
    static std::vector<Effect> effects(std::string_view text) {
        const auto status = inspectRawBodyDirectives(text);
        require(!status.changesEnclosingConditional && !status.openConditionals,
                "body changes enclosing conditional state");
        std::vector<Effect> result;
        for (auto at = text.find('`'); at != text.npos;) {
            auto first = at + 1, last = first;
            while (last < text.size() && word(text[last])) ++last;
            const auto directive = text.substr(first, last - first);
            if (directive == "define" || directive == "undef") {
                auto name = last;
                require(name < text.size() && (text[name] == ' ' || text[name] == '\t'),
                        "macro directive has no plain name");
                while (name < text.size() && (text[name] == ' ' || text[name] == '\t')) ++name;
                auto nameEnd = name;
                while (nameEnd < text.size() && word(text[nameEnd])) ++nameEnd;
                require(nameEnd > name && !(text[name] >= '0' && text[name] <= '9') &&
                            (nameEnd == text.size() || text[nameEnd] != '$'),
                        "macro directive has no plain name");
                auto end = text.find('\n', nameEnd);
                while (end != text.npos) {
                    auto before = end;
                    if (before && text[before - 1] == '\r') --before;
                    if (!before || text[before - 1] != '\\') break;
                    end = text.find('\n', end + 1);
                }
                if (directive == "undef") end = nameEnd;
                if (end == text.npos) end = text.size();
                result.push_back({at, std::string(text.substr(name, nameEnd - name)),
                                  std::string(text.substr(at, end - at))});
            }
            else
                require(!inspectRawBodyDirectives(text.substr(at, last - at)).changesState,
                        "body changes non-macro preprocessing state");
            at = text.find('`', last);
        }
        return result;
    }
    bool fresh(std::string_view name) const {
        const auto& facts = original->getPreprocessorMetadata();
        return name != "__LINE__" && name != "__FILE__" &&
               facts.macroAccessTrackingComplete && facts.macroAccessEndedAtEof &&
               !facts.macroAccessNames.contains(name) && !exitContains(name);
    }
public:
    // The entry span belongs to the overlay; copying this object could leave
    // an exit span pointing into another object's owned vector.
    BodyMacroProof(const BodyMacroProof&) = delete;
    BodyMacroProof& operator=(const BodyMacroProof&) = delete;
    BodyMacroProof(BodyMacroProof&&) = delete;
    BodyMacroProof& operator=(BodyMacroProof&&) = delete;
    BodyMacroProof(std::shared_ptr<const slang::syntax::SyntaxTree> source,
                   std::string_view method, slang::SourceLocation header, slang::SourceLocation end,
                   std::span<const slang::syntax::DefineDirectiveSyntax* const> entry) :
        original(std::move(source)), baseline(effects(method)), exit(entry), end(end) {
        const auto& history = original->getPreprocessorMetadata();
        const slang::parsing::PreprocessorMetadata::SubroutineEntry *begin = nullptr, *boundary = nullptr;
        for (const auto& point : history.subroutineEntries) {
            if (point.location == header) {
                require(!begin, "ambiguous method macro entry");
                begin = &point;
            }
            if (point.location == end) {
                require(!boundary, "ambiguous method macro exit");
                boundary = &point;
            }
        }
        require(begin && boundary, "method macro boundary journal unavailable");
        require(begin->changes <= boundary->changes && boundary->changes <= history.macroChanges.size(),
                "invalid macro boundary journal");
        if (begin->changes == boundary->changes) {
            // Dormant raw directives can be absent from the actual full-unit
            // footprint. Apply the same absence rule to both sides of the proof.
            std::erase_if(baseline, [&](const auto& e) { return fresh(e.name); });
            return; // Borrow the already retained entry vector for the ordinary case.
        }
        ownedExit.assign(entry.begin(), entry.end());
        for (size_t i = begin->changes; i < boundary->changes; ++i) {
            const auto& change = history.macroChanges[i];
            if (change.name.empty()) {
                ownedExit.clear();
                continue;
            }
            auto found = std::lower_bound(ownedExit.begin(), ownedExit.end(), change.name,
                [](auto definition, auto name) { return definition->name.valueText() < name; });
            if (found != ownedExit.end() && (*found)->name.valueText() == change.name) {
                if (change.definition) *found = change.definition;
                else ownedExit.erase(found);
            }
            else if (change.definition) ownedExit.insert(found, change.definition);
        }
        exit = ownedExit;
        std::erase_if(baseline, [&](const auto& e) { return fresh(e.name); });
    }
    bool validateRaw(std::string_view method) const {
        auto current = effects(method);
        const bool changedNames = std::ranges::any_of(current, [&](const auto& e) { return fresh(e.name); });
        std::erase_if(current, [&](const auto& e) { return fresh(e.name); });
        require(current == baseline, "body changes an observed macro definition or its location");
        return changedNames;
    }
    void validateExit(std::span<const slang::syntax::DefineDirectiveSyntax* const> macros) const {
        size_t seen = 0;
        for (auto definition : macros) {
            const auto name = definition->name.valueText();
            if (fresh(name)) continue;
            require(seen < exit.size() && exit[seen]->name.valueText() == name &&
                        (exit[seen] == definition || exit[seen]->toString() == definition->toString()),
                    "body changes an observed macro exit environment");
            ++seen;
        }
        require(seen == exit.size(), "body removes an observed exit macro");
    }
    void validateShiftedLookups(const lsp::RequestContext& ctx) const {
        if (shiftedLookupIdentityProved) {
            ctx.checkpoint("reuse outside macro lookup identity proof");
            return;
        }
        auto& sm = original->sourceManager();
        LineIndependentMacros stable(original->getPreprocessorMetadata(),
            original->options().getOrDefault<slang::parsing::PreprocessorOptions>().trackMacroHistory,
            ctx, true);
        for (const auto& reference : original->getPreprocessorMetadata().macroRefs) {
            ctx.checkpoint("outside macro lookup identity proof");
            auto at = sm.getFullyExpandedLoc(reference.syntax->getFirstToken().location());
            if (sm.getFullPath(at.buffer()) == sm.getFullPath(end.buffer()) && at.offset() >= end.offset())
                require(stable.independent(reference.syntax->toString(), ctx),
                        "line shift can change an outside macro lookup name");
        }
        ctx.checkpoint("before outside macro lookup identity witness commit");
        shiftedLookupIdentityProved = true;
    }
};
} // namespace server
