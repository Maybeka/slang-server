// SPDX-License-Identifier: MIT
#include "document/MacroLineProof.h"

#include "util/DisposableParseRequest.h"
#include "util/QueryText.h"
#include "util/RawBodyDirectives.h"
#include "util/SourceSyntaxReachability.h"
#include "util/UvmPhaseTrace.h"
#include <algorithm>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <tuple>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/ClassSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/diagnostics/LexerDiags.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"

namespace server {
using namespace slang;
namespace {
void need(bool value, const char* reason) {
    if (!value)
        throw std::runtime_error(reason);
}
using Key = std::tuple<uint32_t, size_t, uint32_t, size_t>;
Key key(SourceRange range) {
    return {range.start().buffer().getId(), range.start().offset(), range.end().buffer().getId(),
            range.end().offset()};
}
Key functionKey(const syntax::SyntaxNode& node) {
    return key({node.getFirstToken().location(), node.getLastToken().range().end()});
}
bool lineToken(SourceLocation location, const SourceManager& sm) {
    size_t depth = 0;
    while (location && sm.isMacroLoc(location)) {
        if (sm.getMacroName(location) == "__LINE__")
            return true;
        if (++depth > 1024)
            return false;
        location = sm.getOriginalLoc(location);
    }
    return false;
}
bool signedInt(const ast::Type& type) {
    const auto p = type.getCanonicalType().as_if<ast::PredefinedIntegerType>();
    return p && p->integerKind == ast::PredefinedIntegerType::Int && type.isSigned();
}
std::set<Key> allowedLines(const ast::SubroutineSymbol& fn, const SourceManager& sm) {
    std::set<Key> result;
    auto visitor = ast::makeVisitor([&](auto& visitor, const ast::ExpressionStatement& stmt) {
        visitor.visitDefault(stmt);
        auto call = stmt.expr.as_if<ast::CallExpression>();
        if (!call || call->isSystemCall())
            return;
        auto target = std::get<const ast::SubroutineSymbol*>(call->subroutine);
        auto parent = target->getParentScope();
        if (!parent || parent->asSymbol().kind != ast::SymbolKind::ClassType ||
            target->flags.has(ast::MethodFlags::Static | ast::MethodFlags::Constructor |
                              ast::MethodFlags::BuiltIn | ast::MethodFlags::DPIImport |
                              ast::MethodFlags::InterfaceExtern | ast::MethodFlags::Randomize) ||
            target->subroutineKind != ast::SubroutineKind::Function ||
            !target->getReturnType().isVoid())
            return;
        const auto formals = target->getArguments();
        const auto actuals = call->arguments();
        if (formals.size() != actuals.size())
            return;
        for (size_t i = 0; i < formals.size(); ++i) {
            const auto& value = actuals[i]->unwrapImplicitConversions();
            const auto literal = value.as_if<ast::IntegerLiteral>();
            if (formals[i]->direction == ast::ArgumentDirection::In &&
                signedInt(formals[i]->getType()) && literal && signedInt(*literal->type) &&
                actuals[i]->sourceRange == value.sourceRange && literal->getValue().as<int32_t>() &&
                lineToken(value.sourceRange.start(), sm))
                result.insert(key(value.sourceRange));
        }
    });
    fn.getBody().visit(visitor);
    return result;
}
void triviaIsPure(std::span<const parsing::Trivia> trivia) {
    for (const auto& item : trivia) {
        if (item.kind == parsing::TriviaKind::Directive)
            need(item.syntax()->kind == syntax::SyntaxKind::MacroUsage,
                 "macro replay changes preprocessing state");
        if (item.kind == parsing::TriviaKind::SkippedTokens)
            for (const auto& token : item.getSkippedTokens())
                triviaIsPure(token.trivia());
    }
}
} // namespace

MacroLineProof::MacroLineProof(SourceManager& sm, const ast::Compilation& owner,
                               const analysis::AnalysisManager& analysis, BufferID buffer,
                               size_t stop, std::shared_ptr<const std::string> text,
                               std::string name, const lsp::RequestContext& ctx) :
    sources(sm), original(buffer), editedEnd(stop), baseline(std::move(text)),
    path(std::move(name)) {
    UvmPhaseTrace phases;
    need(bool(baseline), "macro replay baseline unavailable");
    std::string_view old = *baseline;
    if (!old.empty() && old.back() == '\0')
        old.remove_suffix(1);
    need(stop <= old.size(), "macro replay body boundary unavailable");
    const auto suffix = old.substr(stop);
    need(!inspectRawBodyDirectives(suffix).changesState,
         "later source directives require complete analysis");
    static const std::regex branches(R"(`(ifdef|ifndef|elsif|else)\b)");
    need(!std::regex_search(suffix.begin(), suffix.end(), branches),
         "later conditional branches require complete analysis");
    std::map<Key, std::set<Key>> allowed;
    for (auto fn : analysis.getAnalyzedSubroutines()) {
        ctx.checkpoint("native generated macro contexts");
        auto syntax = fn->getSyntax();
        if (!syntax || syntax->kind != syntax::SyntaxKind::FunctionDeclaration)
            continue;
        auto at = sm.getFullyExpandedLoc(syntax->getFirstToken().location());
        if (at.buffer() != original || at.offset() < stop)
            continue;
        auto ranges = allowedLines(*fn, sm);
        auto [it, inserted] = allowed.try_emplace(functionKey(*syntax), ranges);
        if (!inserted)
            std::erase_if(it->second, [&](const Key& r) { return !ranges.contains(r); });
        methods.push_back(fn);
        auto declaration = &syntax->as<syntax::FunctionDeclarationSyntax>();
        if (std::ranges::find(functions, declaration) == functions.end())
            functions.push_back(declaration);
    }
    phases.mark("macro-proof-native-contexts");
    size_t trees = 0;
    for (const auto& tree : owner.getSyntaxTrees()) {
        std::vector<const parsing::PreprocessorMetadata::MacroEntry*> entries;
        const auto& history = tree->getPreprocessorMetadata();
        for (const auto& entry : history.macroEntries) {
            auto range = entry.syntax->sourceRange();
            if (range.start().buffer() != original || range.start().offset() < stop)
                continue;
            need(range.end().buffer() == original && range.end().offset() <= old.size(),
                 "cross-source macro invocation requires complete analysis");
            entries.push_back(&entry);
        }
        if (entries.empty())
            continue;
        for (const auto& diagnostic : tree->diagnostics()) {
            auto at = sm.getFullyExpandedLoc(diagnostic.location);
            need(diagnostic.code == diag::NewlineEOF || at.buffer() != original ||
                     at.offset() < stop,
                 "later macro baseline has syntax diagnostics");
        }
        need(++trees == 1, "ambiguous macro replay tree");
        std::ranges::sort(entries, {},
                          [](auto p) { return p->syntax->sourceRange().start().offset(); });
        for (auto entry : entries) {
            const auto range = entry->syntax->sourceRange();
            if (!expansions.empty())
                need(expansions.back().end <= range.start().offset(),
                     "overlapping macro journal entries");
            Expansion e{.begin = range.start().offset(), .end = range.end().offset()};
            need(entry->changes <= history.macroChanges.size(), "macro journal index unavailable");
            std::map<std::string_view, const syntax::DefineDirectiveSyntax*> environment;
            for (size_t i = 0; i < entry->changes; ++i) {
                if ((i & 1023) == 0)
                    ctx.checkpoint("macro replay environment");
                const auto& c = history.macroChanges[i];
                if (c.name.empty())
                    environment.clear();
                else if (c.definition)
                    environment[c.name] = c.definition;
                else
                    environment.erase(c.name);
            }
            for (const auto& [_, def] : environment)
                e.macros.push_back(def);
            auto pp = tree->options().getOrDefault<parsing::PreprocessorOptions>();
            pp.predefines.clear();
            pp.undefines.clear();
            pp.keywordMapping = {{"*", parsing::KeywordVersion(entry->keywordVersion)}};
            pp.captureUvmPrefix = false;
            pp.trackMacroHistory = true;
            pp.bufferChangeCB = {};
            pp.parseCheckpoint = {};
            e.options = Bag{tree->options().getOrDefault<parsing::LexerOptions>(), pp};
            expansions.push_back(std::move(e));
        }
        phases.mark("macro-proof-entry-journal");
        const SourceSyntaxReachability reachable(sm, original);
        auto walk = [&](auto&& self, const syntax::SyntaxNode& node,
                        std::optional<Key> fn) -> void {
            ctx.checkpoint("native macro expansion token evidence");
            if (reachable.excludes(node)) {
                ctx.checkpoint("native macro evidence excludes unrelated declaration");
                return;
            }
            if (node.kind == syntax::SyntaxKind::ClassDeclaration)
                fn.reset();
            if (node.kind == syntax::SyntaxKind::FunctionDeclaration)
                fn = functionKey(node);
            for (uint32_t i = 0; i < node.getChildCount(); ++i) {
                if (auto child = node.childNode(i))
                    self(self, *child, fn);
                else if (auto token = node.childToken(i);
                         token && sm.isMacroLoc(token.location())) {
                    auto at = sm.getFullyExpandedLoc(token.location());
                    if (at.buffer() != original)
                        continue;
                    auto found = std::upper_bound(expansions.begin(), expansions.end(), at.offset(),
                                                  [](size_t offset, const Expansion& e) {
                                                      return offset < e.begin;
                                                  });
                    if (found == expansions.begin())
                        continue;
                    --found;
                    if (at.offset() >= found->end)
                        continue;
                    found->tokens.push_back(token);
                    bool runtime = token.kind == parsing::TokenKind::IntegerLiteral &&
                                   lineToken(token.location(), sm) && fn && allowed.contains(*fn) &&
                                   allowed.at(*fn).contains(key(token.range()));
                    found->runtimeLines.push_back(runtime);
                }
            }
        };
        walk(walk, tree->root(), std::nullopt);
        phases.mark("macro-proof-native-tokens");
    }
    need(!expansions.empty(), "later macro entry journal unavailable");
    for (const auto& e : expansions) {
        BumpAllocator alloc;
        auto replayed = replay(e, old, stop, ctx, alloc);
        need(replayed.size() == e.tokens.size(), "native macro replay token count differs");
        for (size_t i = 0; i < replayed.size(); ++i)
            need(replayed[i].kind == e.tokens[i].kind && replayed[i].raw == e.tokens[i].rawText(),
                 "native macro replay baseline differs");
    }
    phases.mark("macro-proof-baseline-replay");
}

std::vector<MacroLineProof::ReplayedToken> MacroLineProof::replay(const Expansion& e,
                                                                  std::string_view current,
                                                                  size_t end,
                                                                  const lsp::RequestContext& ctx,
                                                                  BumpAllocator& alloc) const {
    const auto delta = int64_t(end) - int64_t(editedEnd);
    const auto begin = size_t(int64_t(e.begin) + delta), stop = size_t(int64_t(e.end) + delta);
    need(begin <= stop && stop <= current.size(), "macro replay source range moved");
    std::string padded(current.substr(0, stop));
    for (size_t i = 0; i < begin; ++i)
        if (padded[i] != '\n' && padded[i] != '\r')
            padded[i] = ' ';
    padded += '\n';
    QueryText query(sources, path, padded);
    for (const auto& line : sources.getLineDirectives(original)) {
        size_t offset = 0;
        for (size_t i = 1; i < line.lineInFile; ++i) {
            auto next = baseline->find('\n', offset);
            need(next != std::string::npos, "macro replay logical line unavailable");
            offset = next + 1;
        }
        if (offset >= editedEnd)
            offset = size_t(int64_t(offset) + delta);
        if (offset < begin)
            sources.addLineDirective(SourceLocation(query.buffer.id, offset), line.lineOfDirective,
                                     line.name, line.level, true);
    }
    auto options = e.options;
    DisposableParseRequest request(ctx, true);
    request.apply(options);
    Diagnostics diagnostics;
    parsing::Preprocessor pp(sources, alloc, diagnostics, options, e.macros);
    pp.pushSource(query.buffer);
    std::vector<ReplayedToken> result;
    for (;;) {
        ctx.checkpoint("macro replay preprocessing");
        auto token = pp.next();
        triviaIsPure(token.trivia());
        if (token.kind == parsing::TokenKind::EndOfFile)
            break;
        need(!token.isMissing(), "macro replay has missing tokens");
        const bool isLine = token.kind == parsing::TokenKind::IntegerLiteral &&
                            lineToken(token.location(), sources);
        result.push_back({token.kind, std::string(token.rawText()), isLine,
                          isLine ? token.intValue().as<int32_t>() : std::nullopt});
    }
    for (const auto& d : diagnostics)
        need(d.code == diag::NewlineEOF, "macro replay has diagnostics");
    const auto metadata = pp.getMetadata();
    need(metadata.includeDirectives.empty(), "macro replay includes another source");
    std::map<std::string_view, const syntax::DefineDirectiveSyntax*> expected;
    for (auto def : e.macros)
        expected[def->name.valueText()] = def;
    need(metadata.macroChanges.size() == expected.size() + 1 &&
             metadata.macroChanges.front().name.empty(),
         "macro replay changes its environment");
    for (size_t i = 1; i < metadata.macroChanges.size(); ++i) {
        const auto& c = metadata.macroChanges[i];
        const auto found = expected.find(c.name);
        need(found != expected.end() && c.definition, "macro replay changes its environment");
        if (c.definition != found->second) {
            need(c.definition->name.location() == SourceLocation::NoLocation &&
                     found->second->name.location() == SourceLocation::NoLocation,
                 "macro replay replaces a definition");
            auto before = found->second->body, after = c.definition->body;
            need(before.size() == after.size(), "macro replay changes builtin shape");
            for (size_t j = 0; j < before.size(); ++j)
                need(before[j].kind == after[j].kind && before[j].rawText() == after[j].rawText(),
                     "macro replay changes builtin tokens");
        }
        expected.erase(found);
    }
    return result;
}

void MacroLineProof::validateDependencies() const {
    for (auto method : methods)
        need(!method->bodyHasExternalDependents.load(std::memory_order_relaxed),
             "generated macro method acquired semantic dependents");
}

void MacroLineProof::validate(std::string_view current, size_t end,
                              const lsp::RequestContext& ctx) const {
    UvmPhaseTrace phases;
    if (!current.empty() && current.back() == '\0')
        current.remove_suffix(1);
    std::string_view old = *baseline;
    if (!old.empty() && old.back() == '\0')
        old.remove_suffix(1);
    need(end <= current.size() && current.substr(end) == old.substr(editedEnd),
         "macro replay suffix changed");
    validateDependencies();
    for (const auto& e : expansions) {
        BumpAllocator alloc;
        auto fresh = replay(e, current, end, ctx, alloc);
        need(fresh.size() == e.tokens.size(), "macro replay changes token count");
        for (size_t i = 0; i < fresh.size(); ++i) {
            need(fresh[i].kind == e.tokens[i].kind, "macro replay changes token kind");
            if (fresh[i].raw == e.tokens[i].rawText())
                continue;
            need(e.runtimeLines[i] && fresh[i].isLine,
                 "macro line value changes semantic structure");
            auto value = fresh[i].number;
            need(value && *value >= 0, "macro runtime line exceeds int32");
        }
    }
    phases.mark("macro-proof-current-replay");
}
} // namespace server
