// SPDX-License-Identifier: MIT
#include "document/BodyLineProof.h"

#include "util/DisposableParseRequest.h"
#include <algorithm>
#include <limits>
#include <map>
#include <regex>
#include <set>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/ClassSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/diagnostics/LexerDiags.h"
#include "slang/parsing/Parser.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"

namespace server {
using namespace slang;
namespace {
void need(bool condition, const char* why) {
    if (!condition)
        throw std::runtime_error(why);
}
void preprocessing(std::string_view text, bool macro) {
    static const std::regex state(
        R"(`(define|undef|undefineall|include|begin_keywords|end_keywords|timescale|default_nettype|resetall|line|celldefine|endcelldefine|pragma|unconnected_drive|nounconnected_drive|default_decay_time|default_trireg_strength|delay_mode_distributed|delay_mode_path|delay_mode_unit|delay_mode_zero|uselib)\b)");
    static const std::regex conditional(R"(`(ifdef|ifndef|elsif|else|endif)\b)");
    const std::string owned(text);
    need(!std::regex_search(owned, state), "later macro method changes preprocessing state");
    size_t depth = 0;
    for (std::sregex_iterator it(owned.begin(), owned.end(), conditional), end; it != end; ++it) {
        need(!macro, "later macro definition changes conditional state");
        const auto name = (*it)[1].str();
        if (name == "ifdef" || name == "ifndef")
            ++depth;
        else {
            need(depth != 0, "later macro method changes enclosing conditional state");
            if (name == "endif")
                --depth;
        }
    }
    need(depth == 0, "later macro method leaves conditional state open");
}
void preprocessing(const parsing::MacroRefMetadata& ref) {
    if (!ref.definition)
        return;
    std::string body;
    for (auto token : ref.definition->body) {
        body += token.rawText();
        body += ' ';
    }
    preprocessing(body, true);
}
void tokens(const syntax::SyntaxNode& node, std::vector<parsing::Token>& out,
            const lsp::RequestContext& ctx) {
    ctx.checkpoint("later macro token proof");
    for (uint32_t i = 0; i < node.getChildCount(); ++i) {
        if (auto child = node.childNode(i))
            tokens(*child, out, ctx);
        else if (auto token = node.childToken(i); token)
            out.push_back(token);
    }
}
bool intrinsicLine(SourceLocation location, const SourceManager& sm) {
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
    auto integer = type.getCanonicalType().as_if<ast::PredefinedIntegerType>();
    return integer && integer->integerKind == ast::PredefinedIntegerType::Int && type.isSigned();
}
const syntax::FunctionDeclarationSyntax* method(const syntax::SyntaxNode& root) {
    if (auto fn = root.as_if<syntax::FunctionDeclarationSyntax>())
        return fn;
    const syntax::FunctionDeclarationSyntax* result = nullptr;
    for (uint32_t i = 0; i < root.getChildCount(); ++i)
        if (auto child = root.childNode(i))
            if (auto found = method(*child)) {
                need(!result, "multiple later macro method fragments");
                result = found;
            }
    return result;
}
size_t lineOffset(std::string_view text, size_t line) {
    size_t offset = 0;
    for (size_t i = 1; i < line; ++i) {
        auto next = text.find('\n', offset);
        need(next != std::string_view::npos, "later macro line directive outside source");
        offset = next + 1;
    }
    return offset;
}
} // namespace

BodyLineProof::BodyLineProof(SourceManager& sm, const ast::Compilation& owner,
                             const ast::SubroutineSymbol& edited, BufferID original,
                             size_t editedEnd, std::string_view baseline, std::string path,
                             const lsp::RequestContext& ctx) :
    sources(sm), original(original), editedEnd(editedEnd), path(std::move(path)) {
    struct Candidate {
        const ast::SubroutineSymbol* symbol;
        const syntax::FunctionDeclarationSyntax* syntax;
        size_t begin, body, end;
        syntax::SyntaxTree* tree = nullptr;
    };
    std::vector<Candidate> candidates;
    auto parent = edited.getParentScope();
    need(parent && parent->asSymbol().kind == ast::SymbolKind::ClassType,
         "later macros require a retained class context");
    std::set<const ast::SubroutineSymbol*> seen;
    for (const auto& member : parent->members()) {
        ctx.checkpoint("later macro native contexts");
        auto fn = member.as_if<ast::SubroutineSymbol>();
        if (auto prototype = member.as_if<ast::MethodPrototypeSymbol>())
            fn = prototype->getSubroutine();
        if (!fn || !seen.insert(fn).second)
            continue;
        auto syntax = fn->getSyntax() ? fn->getSyntax()->as_if<syntax::FunctionDeclarationSyntax>()
                                      : nullptr;
        if (!syntax || syntax->getFirstToken().location().buffer() != original)
            continue;
        auto first = syntax->getFirstToken().location();
        if (first.offset() < editedEnd)
            continue;
        need(fn->getParentScope() == parent, "later macro context differs from original class");
        candidates.push_back({fn, syntax, first.offset(), syntax->semi.range().end().offset(),
                              syntax->getLastToken().range().end().offset()});
    }
    // Intrinsic macros do not appear in macroRefs. Locate every following
    // method by its entry journal, and inspect raw intrinsic uses outside bodies.
    static const std::regex intrinsic(R"(`__(LINE|FILE)__\b)");
    const std::string suffix(baseline.substr(editedEnd));
    for (std::sregex_iterator it(suffix.begin(), suffix.end(), intrinsic), last; it != last; ++it) {
        const auto offset = editedEnd + size_t(it->position());
        need(std::ranges::any_of(candidates,
                                 [&](const auto& c) { return c.body <= offset && offset < c.end; }),
             "line shift affects intrinsic outside retained method bodies");
    }
    for (const auto& tree : owner.getSyntaxTrees()) {
        for (const auto& entry : tree->getPreprocessorMetadata().subroutineEntries)
            for (auto& c : candidates)
                if (entry.location == c.syntax->prototype->keyword.location()) {
                    need(!c.tree || c.tree == tree.get(), "ambiguous later method source context");
                    c.tree = tree.get();
                }
        for (const auto& ref : tree->getPreprocessorMetadata().macroRefs) {
            ctx.checkpoint("later macro provenance");
            if (ref.syntax->kind != syntax::SyntaxKind::MacroUsage)
                continue;
            const auto loc = sm.getFullyExpandedLoc(ref.syntax->getFirstToken().location());
            if (loc.buffer() != original || loc.offset() < editedEnd)
                continue;
            auto found = std::ranges::find_if(candidates, [&](auto& c) {
                return c.body <= loc.offset() && loc.offset() < c.end;
            });
            need(found != candidates.end(),
                 "line shift affects declarations or other class contexts");
            need(!found->tree || found->tree == tree.get(), "ambiguous later macro source context");
            found->tree = tree.get();
            preprocessing(ref);
        }
    }
    for (const auto& c : candidates) {
        need(c.end <= baseline.size(), "later macro source range is unavailable");
        if (baseline.substr(c.begin, c.end - c.begin).find('`') == std::string_view::npos)
            continue;
        need(c.tree != nullptr, "later macro source context unavailable");
        need(c.end <= baseline.size(), "later macro source range is unavailable");
        need(!c.symbol->bodyHasExternalDependents.load(std::memory_order_relaxed),
             "later macro method has external elaboration dependents");
        preprocessing(baseline.substr(c.begin, c.end - c.begin), false);
        for (const auto& diagnostic : c.tree->diagnostics()) {
            auto loc = sm.getFullyExpandedLoc(diagnostic.location);
            need(loc.buffer() != original || loc.offset() < c.begin || loc.offset() >= c.end,
                 "later macro baseline has syntax diagnostics");
        }
        Neighbour n{.symbol = c.symbol, .begin = c.begin, .end = c.end};
        tokens(*c.syntax, n.tokens, ctx);
        std::vector<SourceRange> allowed;
        auto calls = ast::makeVisitor([&](auto& visitor, const ast::ExpressionStatement& stmt) {
            visitor.visitDefault(stmt);
            auto call = stmt.expr.as_if<ast::CallExpression>();
            if (!call || call->isSystemCall())
                return;
            auto target = std::get<const ast::SubroutineSymbol*>(call->subroutine);
            auto scope = target->getParentScope();
            if (!scope || scope->asSymbol().kind != ast::SymbolKind::ClassType ||
                target->flags.has(ast::MethodFlags::Static | ast::MethodFlags::Constructor |
                                  ast::MethodFlags::BuiltIn | ast::MethodFlags::DPIImport |
                                  ast::MethodFlags::InterfaceExtern |
                                  ast::MethodFlags::Randomize) ||
                target->subroutineKind != ast::SubroutineKind::Function ||
                !target->getReturnType().isVoid())
                return;
            const auto formals = target->getArguments();
            const auto actuals = call->arguments();
            if (formals.size() != actuals.size())
                return;
            for (size_t i = 0; i < formals.size(); ++i) {
                const auto& value = actuals[i]->unwrapImplicitConversions();
                auto literal = value.as_if<ast::IntegerLiteral>();
                if (formals[i]->direction == ast::ArgumentDirection::In &&
                    signedInt(formals[i]->getType()) && literal && signedInt(*literal->type) &&
                    actuals[i]->sourceRange == value.sourceRange &&
                    literal->getValue().as<int32_t>() &&
                    intrinsicLine(value.sourceRange.start(), sm))
                    allowed.push_back(value.sourceRange);
            }
        });
        c.symbol->getBody().visit(calls);
        for (const auto& token : n.tokens)
            n.runtimeLines.push_back(token.kind == parsing::TokenKind::IntegerLiteral &&
                                     intrinsicLine(token.location(), sm) &&
                                     std::ranges::find(allowed, token.range()) != allowed.end());
        const auto& history = c.tree->getPreprocessorMetadata();
        const parsing::PreprocessorMetadata::SubroutineEntry* entry = nullptr;
        for (const auto& e : history.subroutineEntries)
            if (e.location == c.syntax->prototype->keyword.location()) {
                need(!entry, "ambiguous later macro environment");
                entry = &e;
            }
        need(entry && entry->changes <= history.macroChanges.size(),
             "later macro environment unavailable");
        std::map<std::string_view, const syntax::DefineDirectiveSyntax*> environment;
        for (size_t i = 0; i < entry->changes; ++i) {
            if ((i & 1023) == 0)
                ctx.checkpoint("later macro environment");
            const auto& change = history.macroChanges[i];
            if (change.name.empty())
                environment.clear();
            else if (change.definition)
                environment[change.name] = change.definition;
            else
                environment.erase(change.name);
        }
        // macroRefs lists top-level uses only. Follow nested definitions too;
        // state-changing expansions cannot be certified from CST tokens alone.
        std::set<const syntax::DefineDirectiveSyntax*> checked;
        static const std::regex use(R"(`([a-zA-Z_][a-zA-Z_0-9]*))");
        auto checkDefinition = [&](auto&& self,
                                   const syntax::DefineDirectiveSyntax* definition) -> void {
            if (!definition || !checked.insert(definition).second)
                return;
            ctx.checkpoint("later nested macro proof");
            std::string body;
            for (auto token : definition->body) {
                body += token.rawText();
                body += ' ';
            }
            preprocessing(body, true);
            need(body.find("``") == std::string::npos,
                 "later macro token concatenation requires full analysis");
            for (std::sregex_iterator it(body.begin(), body.end(), use), last; it != last; ++it)
                if (auto found = environment.find((*it)[1].str()); found != environment.end())
                    self(self, found->second);
        };
        for (const auto& ref : history.macroRefs) {
            const auto location = sm.getFullyExpandedLoc(ref.syntax->getFirstToken().location());
            if (location.buffer() == original && c.begin <= location.offset() &&
                location.offset() < c.end)
                checkDefinition(checkDefinition, ref.definition);
        }
        for (const auto& [_, definition] : environment)
            n.macros.push_back(definition);
        auto pp = c.tree->options().getOrDefault<parsing::PreprocessorOptions>();
        pp.predefines.clear();
        pp.undefines.clear();
        pp.keywordMapping = {{"*", parsing::KeywordVersion(entry->keywordVersion)}};
        pp.captureUvmPrefix = false;
        pp.trackMacroHistory = true;
        pp.bufferChangeCB = {};
        pp.parseCheckpoint = {};
        n.options = Bag{c.tree->options().getOrDefault<parsing::LexerOptions>(),
                        c.tree->options().getOrDefault<parsing::ParserOptions>(), pp};
        neighbours.push_back(std::move(n));
    }
    for (const auto& line : sm.getLineDirectives(original))
        lines.push_back({lineOffset(baseline, line.lineInFile), line.lineOfDirective,
                         std::string(line.name), uint8_t(line.level)});
    auto directives = sm.getDiagnosticDirectives(original);
    pragmas.assign(directives.begin(), directives.end());
}

void BodyLineProof::validate(std::string_view current, size_t newEnd,
                             const lsp::RequestContext& ctx) const {
    const auto delta = int64_t(newEnd) - int64_t(editedEnd);
    for (const auto& n : neighbours) {
        ctx.checkpoint("before later macro proof");
        need(!n.symbol->bodyHasExternalDependents.load(std::memory_order_relaxed),
             "later macro method acquired external dependents");
        const auto first = size_t(int64_t(n.begin) + delta);
        const auto last = size_t(int64_t(n.end) + delta);
        need(first <= last && last <= current.size(), "later macro source range moved");
        std::string padded(current.substr(0, last));
        for (size_t i = 0; i < first; ++i)
            if (padded[i] != '\n' && padded[i] != '\r')
                padded[i] = ' ';
        padded += '\n';
        auto buffer = sources.assignQueryText(path, padded);
        auto move = [&](size_t offset) {
            return offset >= editedEnd ? size_t(int64_t(offset) + delta) : offset;
        };
        for (const auto& line : lines)
            if (move(line.offset) < first)
                sources.addLineDirective(SourceLocation(buffer.id, move(line.offset)), line.target,
                                         line.name, line.level, true);
        for (const auto& pragma : pragmas)
            if (move(pragma.offset) < first)
                sources.addDiagnosticDirective(SourceLocation(buffer.id, move(pragma.offset)),
                                               pragma.name, pragma.severity, true);
        auto options = n.options;
        DisposableParseRequest parse(ctx, true);
        parse.apply(options);
        auto tree = syntax::SyntaxTree::fromBuffer(buffer, sources, options, n.macros);
        for (const auto& diagnostic : tree->diagnostics())
            need(diagnostic.code == diag::NewlineEOF, "current later macro has syntax diagnostics");
        const auto& metadata = tree->getPreprocessorMetadata();
        // Constructor state consists of a reset, builtins, and inherited macros.
        // Every entry after those must be rejected, including nested defines.
        std::map<std::string_view, const syntax::DefineDirectiveSyntax*> expected;
        for (auto definition : n.macros)
            expected.emplace(definition->name.valueText(), definition);
        need(metadata.macroChanges.size() == expected.size() + 1 &&
                 metadata.macroChanges.front().name.empty(),
             "later macro expansion modifies macro environment");
        for (size_t i = 1; i < metadata.macroChanges.size(); ++i) {
            const auto& change = metadata.macroChanges[i];
            const auto found = expected.find(change.name);
            need(found != expected.end() && change.definition,
                 "later macro expansion modifies macro environment");
            if (change.definition != found->second) {
                // Builtins are recreated by each preprocessor. They have no file
                // location; all inherited user definitions retain pointer identity.
                need(change.definition->name.location() == SourceLocation::NoLocation &&
                         found->second->name.location() == SourceLocation::NoLocation,
                     "later macro expansion replaces an inherited definition");
                const auto before = found->second->body;
                const auto after = change.definition->body;
                need(before.size() == after.size(), "later macro builtin body changes");
                for (size_t j = 0; j < before.size(); ++j)
                    need(before[j].kind == after[j].kind &&
                             before[j].rawText() == after[j].rawText(),
                         "later macro builtin value changes");
            }
            expected.erase(found);
        }
        for (const auto& ref : metadata.macroRefs)
            preprocessing(ref);
        auto fn = method(tree->root());
        need(fn && fn->getFirstToken().location().offset() == first &&
                 fn->getLastToken().range().end().offset() == last,
             "later macro method parse boundary changed");
        std::vector<parsing::Token> fresh;
        tokens(*fn, fresh, ctx);
        need(fresh.size() == n.tokens.size(), "later macro expansion changes syntax shape");
        for (size_t i = 0; i < fresh.size(); ++i) {
            const auto& before = n.tokens[i];
            const auto& after = fresh[i];
            need(before.kind == after.kind, "later macro expansion changes token kind");
            if (before.rawText() == after.rawText())
                continue;
            need(n.runtimeLines[i] && intrinsicLine(after.location(), sources),
                 "later macro line value affects semantic structure");
            auto value = after.intValue().as<int32_t>();
            need(value && *value >= 0, "later macro line exceeds int32 input range");
        }
        ctx.checkpoint("after later macro proof");
    }
}
} // namespace server
