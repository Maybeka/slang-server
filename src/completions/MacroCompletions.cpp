//------------------------------------------------------------------------------
// MacroCompletions.cpp
// SystemVerilog macro completions.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "completions/MacroCompletions.h"

#include "Indexer.h"
#include "completions/CompletionDispatch.h"
#include "document/SlangDoc.h"
#include "lsp/SnippetString.h"
#include "util/Formatting.h"
#include "util/Logging.h"
#include <unordered_set>

#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxVisitor.h"

namespace server::completions {
using namespace slang;

namespace {

constexpr unsigned char foldMacroLetter(unsigned char letter) {
    return letter >= 'A' && letter <= 'Z' ? letter + ('a' - 'A') : letter;
}

// Most macro names share long, identically capitalized prefixes.
int comparePrefix(std::string_view a, std::string_view b) {
    for (size_t i = 0, n = std::min(a.size(), b.size()); i < n; ++i) {
        if (a[i] == b[i])
            continue;
        if (int difference = foldMacroLetter(a[i]) - foldMacroLetter(b[i]))
            return difference;
    }
    return 0;
}

bool matchesPrefix(std::string_view name, std::string_view typedPrefix) {
    if (typedPrefix.starts_with('`'))
        typedPrefix.remove_prefix(1);
    return typedPrefix.size() <= name.size() && comparePrefix(name, typedPrefix) == 0;
}

lsp::CompletionItem getCompletion(std::string name) {
    auto spelling = "`" + name;
    return lsp::CompletionItem{
        .label = spelling,
        .labelDetails =
            lsp::CompletionItemLabelDetails{
                .detail = " Macro",
            },
        .kind = lsp::CompletionItemKind::Constant,
        .filterText = spelling,
    };
}

std::string getCompletionText(const syntax::DefineDirectiveSyntax& macro) {
    SnippetString output;
    output.appendText("`");
    output.appendText(macro.name.valueText());
    if (macro.formalArguments) {
        output.appendText("(");
        auto& args = macro.formalArguments->args;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0)
                output.appendText(", ");
            output.appendPlaceholder(args[i]->name.valueText());
        }
        output.appendText(")");
    }
    return std::string(output.getValue());
}

void resolve(const syntax::DefineDirectiveSyntax& macro, lsp::CompletionItem& item) {
    item.documentation = svCodeBlock(macro);
    if (!item.insertText) {
        item.insertText = getCompletionText(macro);
        item.insertTextFormat = lsp::InsertTextFormat::Snippet;
    }
}

lsp::CompletionItem getCompletion(const syntax::DefineDirectiveSyntax& macro) {
    auto item = getCompletion(std::string(macro.name.valueText()));
    resolve(macro, item);
    return item;
}

} // namespace

// Built lazily from the CST when a user first asks for macro completion.
struct MacroIndex : syntax::SyntaxVisitor<MacroIndex> {
    struct Event {
        SourceLocation location;
        const syntax::DefineDirectiveSyntax* definition;
    };
    struct History {
        const syntax::DefineDirectiveSyntax* initial = nullptr;
        const syntax::DefineDirectiveSyntax* commandLine = nullptr;
        std::vector<Event> changes;
    };
    std::weak_ptr<syntax::SyntaxTree> owner;
    std::unordered_map<std::string_view, History> names;
    std::vector<SourceLocation> resets;
    std::vector<std::string_view> sortedNames;

    explicit MacroIndex(const std::shared_ptr<syntax::SyntaxTree>& tree) : owner(tree) {
        tree->root().visit(*this);
        if (auto eof = tree->getMetadata().eofToken; eof && tree->root().getLastToken() != eof)
            visitToken(eof);
        // Definitions removed with undef remain in the native reference metadata.
        // Command-line buffers identify the bindings that undefineall reinstates.
        std::unordered_set<BufferID> predefines(tree->getPredefineBufferIds().begin(),
                                                tree->getPredefineBufferIds().end());
        auto remember = [&](const syntax::DefineDirectiveSyntax* def) {
            if (!def || def->name.location() == SourceLocation::NoLocation)
                return;
            if (predefines.contains(def->name.location().buffer())) {
                auto& history = names[def->name.valueText()];
                history.commandLine = history.initial = def;
            }
        };
        for (const auto& ref : tree->getMacroRefs())
            remember(ref.definition);
        for (auto* def : tree->getDefinedMacros()) {
            if (def->name.location() != SourceLocation::NoLocation) {
                auto& history = names[def->name.valueText()];
                if (history.changes.empty())
                    history.initial = def;
                remember(def);
            }
        }
        for (const auto& [name, _] : names)
            sortedNames.push_back(name);
        std::ranges::sort(sortedNames, lessName);
    }

    static bool lessName(std::string_view a, std::string_view b) {
        if (int difference = comparePrefix(a, b))
            return difference < 0;
        return a.size() < b.size();
    }

    void visitToken(parsing::Token token) {
        for (auto trivia : token.trivia()) {
            if (trivia.kind != parsing::TriviaKind::Directive)
                continue;
            auto node = trivia.syntax();
            auto loc = node->sourceRange().end();
            if (auto def = node->as_if<syntax::DefineDirectiveSyntax>())
                names[def->name.valueText()].changes.push_back({loc, def});
            else if (auto undef = node->as_if<syntax::UndefDirectiveSyntax>())
                names[undef->name.valueText()].changes.push_back({loc, nullptr});
            else if (node->kind == syntax::SyntaxKind::UndefineAllDirective)
                resets.push_back(loc);
        }
    }

    static bool beforeOrAt(const SourceManager& sm, SourceLocation a, SourceLocation b) {
        if (a.buffer() == b.buffer())
            return a.offset() <= b.offset();
        if (auto before = sm.isBeforeInCompilationUnit(a, b))
            return *before;
        return sm.getSortKey(a.buffer()) < sm.getSortKey(b.buffer());
    }

    const syntax::DefineDirectiveSyntax* active(const SourceManager& sm, const History& history,
                                                SourceLocation cursor, SourceLocation reset) const {
        auto def = reset ? history.commandLine : history.initial;
        for (const auto& event : history.changes) {
            if (!beforeOrAt(sm, event.location, cursor))
                break;
            if (reset && beforeOrAt(sm, event.location, reset))
                continue;
            // A live command-line define has precedence over a source redefinition.
            if (!event.definition || !def || def != history.commandLine)
                def = event.definition;
        }
        return def;
    }
};

namespace {
class MacroCompletionQueryImpl final : public MacroCompletionQuery {
public:
    MacroCompletionQueryImpl(lsp::Range replacementRange, std::string typedPrefix,
                             bool followedByCall, SourceLocation cursor) :
        MacroCompletionQuery(std::move(replacementRange), followedByCall),
        typedPrefix(std::move(typedPrefix)), cursor(cursor) {}

    CompletionQueryKind kind() const final { return CompletionQueryKind::Macro; }

    bool isIncomplete() const final { return true; }

    void getCompletions(std::vector<lsp::CompletionItem>& results, CompletionDispatch& dispatch,
                        const std::shared_ptr<SlangDoc>& doc,
                        const CompletionContext& context) const final {
        auto tree = context.analysis->getSyntaxTree();
        const auto& index = dispatch.getMacroIndex(tree);
        auto& sm = tree->sourceManager();
        SourceLocation reset;
        for (auto it = index.resets.rbegin(); it != index.resets.rend(); ++it) {
            if (index.beforeOrAt(sm, *it, cursor)) {
                reset = *it;
                break;
            }
        }
        auto prefix = std::string_view(typedPrefix);
        if (prefix.starts_with('`'))
            prefix.remove_prefix(1);
        for (auto it = std::ranges::lower_bound(index.sortedNames, prefix, MacroIndex::lessName);
             it != index.sortedNames.end() && matchesPrefix(*it, prefix); ++it) {
            if (auto def = index.active(sm, index.names.at(*it), cursor, reset))
                results.push_back(getCompletion(*def));
        }

        for (const auto& name : dispatch.findIndexedMacros(prefix)) {
            if (index.names.contains(name))
                continue;
            results.push_back(getCompletion(name));
        }
    }

private:
    std::string typedPrefix;
    SourceLocation cursor;
};

} // namespace

std::unique_ptr<CompletionQuery> MacroCompletionQuery::create(lsp::Range replacementRange,
                                                              std::string typedPrefix,
                                                              bool followedByCall,
                                                              SourceLocation cursor) {
    return std::make_unique<MacroCompletionQueryImpl>(std::move(replacementRange),
                                                      std::move(typedPrefix), followedByCall,
                                                      cursor);
}

void MacroCompletionQuery::resolve(CompletionDispatch& dispatch, lsp::CompletionItem& item) {
    if (item.documentation)
        return;
    auto paths = getIndexer(dispatch).getFilesForMacro(item.label.substr(1));
    if (paths.empty()) {
        WARN("No macro files found for {}", item.label);
        return;
    }

    auto maybeTree = getIndexedSyntaxTree(dispatch, paths[0].string());
    if (!maybeTree)
        return;

    for (auto macro : maybeTree->getDefinedMacros()) {
        if (macro->name.valueText() == item.label.substr(1)) {
            completions::resolve(*macro, item);
            updateCompletionEditText(item);
            return;
        }
    }
    WARN("Didn't find macro for {} in {}", item.label, paths[0].string());
}

} // namespace server::completions

namespace server {
const completions::MacroIndex& CompletionDispatch::getMacroIndex(
    const std::shared_ptr<slang::syntax::SyntaxTree>& tree) {
    if (!m_macroIndex || m_macroIndex->owner.lock() != tree)
        m_macroIndex = std::make_shared<completions::MacroIndex>(tree);
    return *m_macroIndex;
}

std::span<const std::string> CompletionDispatch::findIndexedMacros(std::string_view prefix) {
    auto revision = m_indexer.getRevision();
    if (m_indexedMacroRevision != revision) {
        auto names = m_indexer.getAllMacroNames();
        std::ranges::sort(names, completions::MacroIndex::lessName);
        m_indexedMacroNames = std::move(names);
        m_indexedMacroRevision = revision;
    }
    auto start = std::ranges::lower_bound(m_indexedMacroNames, prefix,
                                          completions::MacroIndex::lessName);
    auto end = std::find_if_not(start, m_indexedMacroNames.end(), [&](const auto& name) {
        return completions::matchesPrefix(name, prefix);
    });
    return {start, end};
}
} // namespace server
