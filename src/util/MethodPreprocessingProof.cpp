// SPDX-License-Identifier: MIT
#include "util/MethodPreprocessingProof.h"

#include "util/RawBodyDirectives.h"
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <vector>

#include "slang/parsing/PreprocessorMetadata.h"
#include "slang/syntax/AllSyntax.h"
namespace server {
using namespace slang;
namespace {
void need(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
void validateBodyPreprocessing(const std::string& methodText) {
    const auto directives = inspectRawBodyDirectives(methodText);
    need(!directives.changesState, "body changes preprocessing state");
    need(!directives.changesEnclosingConditional, "body changes enclosing conditional state");
    need(directives.openConditionals == 0, "body leaves conditional preprocessing state open");
}
void validateMacroPreprocessing(const parsing::MacroRefMetadata& reference,
                                std::span<const syntax::DefineDirectiveSyntax* const> inherited,
                                const lsp::RequestContext& ctx) {
    if (!reference.definition)
        return;
    std::map<std::string_view, const syntax::DefineDirectiveSyntax*> environment;
    for (auto definition : inherited)
        environment.emplace(definition->name.valueText(), definition);
    std::vector<const syntax::DefineDirectiveSyntax*> pending{reference.definition};
    std::set<const syntax::DefineDirectiveSyntax*> checked;
    static const std::regex uses(R"(`([a-zA-Z_][a-zA-Z_0-9]*))");
    auto follow = [&](const std::string& text) {
        for (std::sregex_iterator it(text.begin(), text.end(), uses), end; it != end; ++it)
            if (auto found = environment.find((*it)[1].str()); found != environment.end())
                pending.push_back(found->second);
    };
    // Macro actual arguments can invoke another macro even when the wrapper's
    // own definition contains only an ordinary formal parameter.
    if (!inherited.empty())
        follow(reference.syntax->toString());
    while (!pending.empty()) {
        ctx.checkpoint("macro preprocessing proof");
        auto definition = pending.back();
        pending.pop_back();
        if (!checked.insert(definition).second)
            continue;
        need(checked.size() < 4096, "macro preprocessing proof exceeds definition bound");
        std::string expansion;
        for (auto token : definition->body) {
            expansion += token.rawText();
            expansion += ' ';
        }
        const auto directives = inspectRawBodyDirectives(expansion);
        need(!directives.changesState && !directives.hasConditionals,
             "macro expansion can change surrounding preprocessing state");
        if (!inherited.empty()) {
            need(expansion.find("``") == std::string::npos,
                 "macro token concatenation requires full preprocessing");
            follow(expansion);
        }
    }
}

} // namespace server
