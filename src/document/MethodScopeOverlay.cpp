// SPDX-License-Identifier: MIT
#include "document/MethodScopeOverlay.h"

#include "ast/TrustedUvmLibrary.h"
#include "completions/InstanceCompletions.h"
#include "completions/MemberCompletions.h"
#include "document/BodyDiagnosticCache.h"
#include "document/DocumentEditTransaction.h"
#include "document/MethodCompletionData.h"
#include "util/BodyMacroProof.h"
#include "util/Converters.h"
#include "util/DirectoryProofPlan.h"
#include "util/LineIndependentMacros.h"
#include "util/MethodInputProof.h"
#include "util/MethodPreprocessingProof.h"
#include "util/QueryText.h"
#include "util/SourceSyntaxReachability.h"
#include "util/UvmPhaseTrace.h"
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/driver/SourceLoader.h"
#include "slang/parsing/Lexer.h"
#include "slang/parsing/Parser.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/util/ScopeGuard.h"

namespace server {
using namespace slang;
using method_completion::Data;
using method_completion::Formatting;
using method_completion::NativePayload;
using method_completion::QueryPayload;
namespace fs = std::filesystem;
using method_input::directoryEntries;
using method_input::readCertificateFile;
using FileProofDirectories = method_input::Directories;
namespace {
void need(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool plainInt(const syntax::DataTypeSyntax* type) {
    auto* value = type ? type->as_if<syntax::IntegerTypeSyntax>() : nullptr;
    return value && value->keyword.kind == parsing::TokenKind::IntKeyword && !value->signing &&
           value->dimensions.empty();
}
struct Shape {
    size_t begin, end, locals;
};
Shape shape(const syntax::FunctionDeclarationSyntax& fn) {
    need(!fn.items.empty(), "empty body");
    size_t locals = 0;
    for (size_t i = 0; i + 1 < fn.items.size(); ++i) {
        auto* declaration = fn.items[i]->as_if<syntax::DataDeclarationSyntax>();
        need(declaration && declaration->attributes.empty() && declaration->modifiers.empty() &&
                 plainInt(declaration->type),
             "only plain automatic int locals");
        for (auto* variable : declaration->declarators) {
            need(variable->dimensions.empty() && variable->initializer, "initialized scalar local");
            ++locals;
        }
    }
    auto* statement = fn.items.back()->as_if<syntax::ReturnStatementSyntax>();
    need(statement && statement->returnValue && statement->attributes.empty() && !statement->label,
         "one final return required");
    return {fn.items.front()->getFirstToken().location().offset(),
            statement->getLastToken().range().end().offset(), locals};
}
// The method interface includes all local declarations. Only their initializer
// expressions may change. Token identity deliberately ignores formatting trivia.
std::vector<std::string> declarations(const syntax::FunctionDeclarationSyntax& fn) {
    std::vector<std::string> result;
    auto collect = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
        if (node.kind == syntax::SyntaxKind::EqualsValueClause)
            return;
        for (uint32_t i = 0; i < node.getChildCount(); ++i) {
            if (auto* child = node.childNode(i))
                self(self, *child);
            else if (auto token = node.childToken(i); token)
                result.emplace_back(std::string(toString(token.kind)) + ":" +
                                    std::string(token.rawText()));
        }
    };
    for (auto* item : fn.items)
        if (item->kind == syntax::SyntaxKind::DataDeclaration)
            collect(collect, *item);
    return result;
}
void verify(const ast::SubroutineSymbol& fn, const std::vector<std::string>& outerNames) {
    const auto& expected = fn.getCompilation().getIntType();
    auto scalar = [&](const ast::ValueSymbol& symbol) {
        auto& type = symbol.getType();
        return type.toString() == "int" && type.getBitWidth() == 32 &&
               type.isSigned() == expected.isSigned() &&
               type.isFourState() == expected.isFourState();
    };
    std::set<const ast::ValueSymbol*> allowed;
    for (auto* argument : fn.getArguments()) {
        need(scalar(*argument), "int formal");
        allowed.insert(argument);
    }
    for (auto& member : fn.members()) {
        if (member.kind == ast::SymbolKind::Variable && &member != fn.thisVar &&
            &member != fn.returnValVar) {
            auto& value = member.as<ast::VariableSymbol>();
            need(value.getParentScope() == &fn && scalar(value) && value.getInitializer(),
                 "flat int local");
            allowed.insert(&value);
        }
    }
    for (auto* symbol : allowed) {
        auto name = symbol->name;
        need(!name.empty() &&
                 (std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_') &&
                 std::all_of(name.begin(), name.end(),
                             [](unsigned char c) { return std::isalnum(c) || c == '_'; }),
             "ordinary local and formal identifiers required");
    }
    for (auto* symbol : allowed)
        need(std::find(outerNames.begin(), outerNames.end(), symbol->name) == outerNames.end(),
             "shadowing outer members is not certified in phase one");
    auto visitor = ast::makeVisitor([&](auto& self,
                                        const std::derived_from<ast::Expression> auto& value) {
        const ast::Expression& e = value;
        need(e.type->toString() == "int" && e.type->getBitWidth() == 32 &&
                 e.type->isSigned() == expected.isSigned() &&
                 e.type->isFourState() == expected.isFourState(),
             "closed int expression");
        if (auto* named = e.as_if<ast::NamedValueExpression>())
            need(allowed.contains(&named->symbol), "nonlocal dependency");
        else if (auto* binary = e.as_if<ast::BinaryExpression>())
            need(binary->op == ast::BinaryOperator::Add ||
                     binary->op == ast::BinaryOperator::Subtract ||
                     binary->op == ast::BinaryOperator::Multiply,
                 "unsupported binary operation");
        else if (auto* unary = e.as_if<ast::UnaryExpression>())
            need(unary->op == ast::UnaryOperator::Plus || unary->op == ast::UnaryOperator::Minus,
                 "unsupported unary operation");
        else if (auto* conversion = e.as_if<ast::ConversionExpression>())
            need(conversion->isImplicit(), "explicit conversion");
        else
            need(e.kind == ast::ExpressionKind::IntegerLiteral, "unsupported expression");
        self.visitDefault(value);
    });
    for (auto* symbol : allowed)
        if (symbol->kind == ast::SymbolKind::Variable)
            symbol->as<ast::VariableSymbol>().getInitializer()->visit(visitor);
    fn.getBody().visit(visitor);
}
const ast::SubroutineSymbol& method(ast::Compilation& c, const std::string& package,
                                    const std::string& cls, const std::string& fn) {
    auto* p = c.getPackage(package);
    need(p, "package missing");
    auto* type = p->find(cls);
    need(type && type->kind == ast::SymbolKind::ClassType, "class missing");
    auto* symbol = type->as<ast::ClassType>().find(fn);
    need(symbol && symbol->kind == ast::SymbolKind::Subroutine, "method missing");
    return symbol->as<ast::SubroutineSymbol>();
}
} // namespace

void MethodScopeOverlay::require(bool yes, const char* message) {
    need(yes, message);
}
std::string MethodScopeOverlay::plain(const SlangDoc& document) {
    auto bytes = document.getText();
    need(!bytes.empty() && bytes.back() == '\0', "source sentinel");
    bytes.remove_suffix(1);
    return std::string(bytes);
}

std::optional<std::string> MethodScopeOverlay::read(const std::string& path, bool fastPath,
                                                    const lsp::RequestContext& ctx) {
    return readCertificateFile(path, fastPath, ctx).contents;
}

bool MethodScopeOverlay::readMatches(const std::string& path,
                                     const std::optional<std::string>& expected, bool fastPath,
                                     const lsp::RequestContext& ctx) {
    return readCertificateFile(path, fastPath, ctx, &expected).matches;
}

MethodScopeOverlay::MethodScopeOverlay(ServerDriver& d, const Config& config, const URI& target,
                                       size_t editOffset, const lsp::RequestContext& ctx) :
    driver(&d), uri(target) {
    UvmPhaseTrace phases;
    UvmPhaseTrace contextPhases;
    ctx.checkpoint("method certificate begin");
    bodyQueries = config.experimentalMethodBodyQueries.value();
    auto [tree, analysis, occurrence, document] = resolveSource(config, ctx, contextPhases);
    contextPhases.mark("body-context-graph");
    ctx.checkpoint("method certificate graph ready");
    if (!analysis)
        analysis = document->getAnalysis(ctx);
    auto& compilation = *analysis->getCompilation();
    if (!bodyQueries)
        need(compilation.getAllDiagnostics().empty(), "base semantic diagnostics");
    contextPhases.mark("body-context-analysis");
    ctx.checkpoint("method certificate semantics ready");
    const syntax::FunctionDeclarationSyntax* selected = nullptr;
    auto find = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
        ctx.checkpoint("method certificate syntax");
        if (auto* fn = node.as_if<syntax::FunctionDeclarationSyntax>();
            fn && fn->getFirstToken().location().buffer() == occurrence &&
            fn->getFirstToken().location().offset() <= editOffset &&
            editOffset < fn->getLastToken().range().end().offset()) {
            need(!selected, "ambiguous target");
            selected = fn;
        }
        for (uint32_t i = 0; i < node.getChildCount(); ++i)
            if (auto* child = node.childNode(i))
                self(self, *child);
    };
    if (bodyQueries) {
        // The per-document index already exists. Do not walk a large package's
        // complete syntax graph to reject an edit outside an eligible method.
        auto* token = analysis->syntaxes.getTokenAt(SourceLocation(occurrence, editOffset));
        for (auto* node =
                 token ? analysis->syntaxes.getTokenParent(token)
                       : analysis->syntaxes.getSyntaxAt(SourceLocation(occurrence, editOffset));
             node; node = node->parent) {
            if (auto* fn = node->as_if<syntax::FunctionDeclarationSyntax>()) {
                selected = fn;
                break;
            }
        }
    }
    else {
        find(find, tree->root());
    }
    need(selected && selected->getFirstToken().location().buffer() == occurrence &&
             selected->getFirstToken().location().offset() <= editOffset &&
             editOffset < selected->getLastToken().range().end().offset(),
         "edit outside function");
    contextPhases.mark("body-context-target");
    if (bodyQueries) {
        auto contexts = analysis->getSubroutines(*selected, ctx, true);
        if (!contexts.empty())
            bodyQueryScopeDisplay = contexts.front()->getLexicalPath();
        contextPhases.mark("body-context-shallow-methods");
        try {
            // A package-only view need not contain a newly referenced package.
            // Reuse the completed Native namespace instead of elaborating every
            // consumer or synthesizing a default generic specialization. Capture
            // requires a current, complete publication and retains its one owner.
            auto baseline =
                BodyDiagnosticCache::capture(d, uri, selected->getFirstToken().location().offset(),
                                             selected->getLastToken().range().end().offset(), ctx);
            contextPhases.mark("body-context-native-capture");
            std::vector<const ast::SubroutineSymbol*> nativeContexts;
            for (auto fn : baseline->originals) {
                // Distinct trees may represent one source occurrence (module
                // query overrides do this). Never combine repeated includes.
                need(fn->getSyntax() == baseline->originals.front()->getSyntax(),
                     "ambiguous completed query occurrence");
                if (fn->getParentScope()->asSymbol().kind != ast::SymbolKind::InstanceBody)
                    nativeContexts.push_back(fn);
            }
            // Module queries must preserve the selected actual instance and its
            // parameter map, even though diagnostics cover all edited instances.
            for (auto fn : contexts)
                if (auto symbol = analysis->getDesignSymbol(*fn))
                    if (auto mapped = symbol->as_if<ast::SubroutineSymbol>();
                        mapped &&
                        std::ranges::find(baseline->originals, mapped) !=
                            baseline->originals.end() &&
                        std::ranges::find(nativeContexts, mapped) == nativeContexts.end())
                        nativeContexts.push_back(mapped);
            if (nativeContexts.empty() && baseline->originals.size() == 1)
                nativeContexts = baseline->originals;
            need(!nativeContexts.empty(), "selected Native query context unavailable");
            contextPhases.mark("body-context-native-contexts");
            // Include already analyzed peers from this physical source so later
            // macro-line certification can still find their original identities.
            std::vector<const ast::SubroutineSymbol*> indexed;
            const auto nativeOccurrence =
                d.sm.getFullyExpandedLoc(
                        baseline->originals.front()->getSyntax()->getFirstToken().location())
                    .buffer();
            for (auto fn : baseline->retainedAnalysis.complete()->getAnalyzedSubroutines()) {
                ctx.checkpoint("collect completed body query contexts");
                if (auto syntax = fn->getSyntax();
                    syntax &&
                    d.sm.getFullyExpandedLoc(syntax->getFirstToken().location()).buffer() ==
                        nativeOccurrence)
                    indexed.push_back(fn);
            }
            need(!indexed.empty(), "completed body query index is empty");
            contextPhases.mark("body-context-native-peers");
            const bool genericRecovery = contexts.empty();
            ctx.checkpoint(genericRecovery ? "before completed generic query context indexing"
                                           : "before completed body query context indexing");
            auto unit = std::make_shared<ShallowCompilation>(
                d.sm, tree, baseline->retainedAnalysis.compilation(), indexed);
            contextPhases.mark("body-context-native-unit");
            // Only immutable token/syntax data is copied, and the constructor
            // checks exact tree and included occurrence identity. Symbols are
            // indexed afresh against the completed Native compilation.
            auto completed = std::make_shared<ShallowAnalysis>(occurrence, std::move(unit), ctx,
                                                               analysis.get());
            contextPhases.mark("body-context-native-index");
            const auto indexedContexts = completed->getSubroutines(*selected);
            for (auto fn : nativeContexts)
                need(std::ranges::find(indexedContexts, fn) != indexedContexts.end(),
                     "incomplete completed body query contexts");
            ctx.checkpoint(genericRecovery ? "after completed generic query context indexing"
                                           : "after completed body query context indexing");
            analysis = std::move(completed);
            contexts = std::move(nativeContexts);
            localDiagnostics.seed(std::move(baseline));
        }
        catch (const lsp::RequestCancelled&) {
            throw;
        }
        catch (const std::exception&) {
            // Query-only or incomplete Native states keep the existing shallow
            // path; do not construct a full design synchronously to obtain this.
            if (contexts.empty())
                throw;
        }
        need(!contexts.empty(), "unbound function / task context");
        for (auto fn : contexts) {
            need(fn->getCompilation().hasFlag(
                     ast::CompilationFlags::TrackSubroutineBodyDependencies),
                 "method context lacks dependency tracking");
            need(!fn->bodyHasExternalDependents.load(std::memory_order_relaxed),
                 "method has external constant dependencies");
        }
        generalBody = true;
        if (generalBody) {
            bodyOwner = analysis;
            bodyOriginals = std::move(contexts);
            if (bodyQueryScopeDisplay.empty())
                bodyQueryScopeDisplay = bodyOriginals.front()->getLexicalPath();
            functionBegin = selected->getFirstToken().location().offset();
            functionEnd = selected->getLastToken().range().end().offset();
            bodyBegin = selected->semi.range().end().offset();
            bodyEnd = selected->end.location().offset();
            originalBodyEnd = bodyEnd;
            originalBuffer = occurrence.getId();
            currentBuffer = document->getBuffer().getId();
            version = *document->getVersion();
            text = plain(*document);
            contextPhases.mark("body-context-methods");
            // Removing an existing directive or state-changing macro is just as
            // unsafe as introducing one: the borrowed suffix used its old state.
            const bool macroProof = config.experimentalMethodBodyMacroProof.value() &&
                                    config.methodBodyMacroProofMaxMiB.value();
            if (!macroProof)
                validateBodyPreprocessing(text.substr(functionBegin, functionEnd - functionBegin));
            // Buffer identities and their paths are immutable during this
            // serialized construction. Resolve each physical source once even
            // when a large library contributes many macro expansion references.
            const auto occurrencePath = d.sm.getFullPath(occurrence);
            std::unordered_map<uint32_t, bool> occurrenceBuffers;
            auto inOccurrence = [&](BufferID buffer) {
                auto [entry, inserted] = occurrenceBuffers.try_emplace(buffer.getId(), false);
                if (inserted)
                    entry->second = d.sm.getFullPath(buffer) == occurrencePath;
                return entry->second;
            };
            if (!macroProof) {
                size_t checkedMacros = 0;
                for (const auto& reference : tree->getPreprocessorMetadata().macroRefs) {
                    if ((checkedMacros++ & 1023) == 0)
                        ctx.checkpoint("original method preprocessing witness");
                    auto at = d.sm.getFullyExpandedLoc(reference.syntax->getFirstToken().location());
                    if (at && functionBegin <= at.offset() && at.offset() < functionEnd &&
                        inOccurrence(at.buffer()))
                        validateMacroPreprocessing(reference);
                }
            }
            contextPhases.mark("body-context-macro-witnesses");
            functionName = bodyOriginals.front()->name;
            functionPath = bodyOriginals.front()->getHierarchicalPath();
            bodyBlocked = bodyOriginals;
            if (localDiagnostics.baseline())
                for (auto fn : localDiagnostics.baseline()->originals)
                    if (std::ranges::find(bodyBlocked, fn) == bodyBlocked.end())
                        bodyBlocked.push_back(fn);
            bodyBufferLimit = uint32_t(d.sm.getSourceLocationEntryCount());
            // Macro expansions after this method can use __LINE__, including through
            // nested macros or token pasting. Conservatively retain their enclosing
            // methods as dependencies; declaration / signature expansions need a full
            // update whenever the method's line count changes.
            std::optional<LineIndependentMacros> declarationMacros;
            for (const auto& [usage, expansion] : analysis->syntaxes.macroExpansions) {
                auto location = d.sm.getFullyExpandedLoc(usage->getFirstToken().location());
                if (!inOccurrence(location.buffer()) || location.offset() < originalBodyEnd)
                    continue;
                const syntax::FunctionDeclarationSyntax* enclosing = nullptr;
                for (auto node = usage; node; node = node->parent)
                    if (auto fn = node->as_if<syntax::FunctionDeclarationSyntax>()) {
                        enclosing = fn;
                        break;
                    }
                if (enclosing && enclosing->prototype->keyword.location().buffer() == occurrence &&
                    location.offset() >= enclosing->semi.range().end().offset() &&
                    location.offset() < enclosing->end.location().offset()) {
                    auto methods = analysis->getSubroutines(*enclosing);
                    if (methods.empty())
                        bodyLineSensitiveDeclaration = true;
                    for (auto method : methods) {
                        if (!method->getCompilation().hasFlag(
                                ast::CompilationFlags::TrackSubroutineBodyDependencies))
                            bodyLineSensitiveDeclaration = true;
                        if (std::ranges::find(bodyLineDependents, method) ==
                            bodyLineDependents.end())
                            bodyLineDependents.push_back(method);
                    }
                }
                else {
                    if (!declarationMacros)
                        declarationMacros.emplace(tree->getPreprocessorMetadata(),
                                                  tree->options()
                                                      .getOrDefault<parsing::PreprocessorOptions>()
                                                      .trackMacroHistory,
                                                  ctx);
                    if (!declarationMacros->independent(usage->toString(), ctx))
                        bodyLineSensitiveDeclaration = true;
                }
            }
            contextPhases.mark("body-context-later-macros");
            phases.mark("body-prepare-context");
            const auto& history = tree->getPreprocessorMetadata();
            const auto header = selected->prototype->keyword.location();
            const parsing::PreprocessorMetadata::SubroutineEntry* entry = nullptr;
            for (const auto& candidate : history.subroutineEntries)
                if (candidate.location == header) {
                    need(!entry, "ambiguous method macro entry");
                    entry = &candidate;
                }
            if (entry) {
                need(entry->changes <= history.macroChanges.size(), "invalid macro entry journal");
                std::map<std::string_view, const syntax::DefineDirectiveSyntax*> macros;
                for (size_t i = 0; i < entry->changes; ++i) {
                    if ((i & 1023) == 0)
                        ctx.checkpoint("method macro environment journal");
                    const auto& change = history.macroChanges[i];
                    if (change.name.empty())
                        macros.clear();
                    else if (change.definition)
                        macros[change.name] = change.definition;
                    else
                        macros.erase(change.name);
                }
                std::vector<BufferID> pins;
                for (const auto& [_, definition] : macros) {
                    bodyMacros.push_back(definition);
                    pins.push_back(definition->name.location().buffer());
                }
                bodyKeywordVersion = entry->keywordVersion;
                macroPins = d.sm.retainBuffers(pins);
            }
            else {
                // Replay preprocessing only once, stopping at the original header. This
                // captures definitions before this occurrence, including textual includes.
                macroArena = std::make_unique<BumpAllocator>();
                Diagnostics diagnostics;
                auto replayOptions = tree->options();
                auto preprocessorOptions =
                    replayOptions.getOrDefault<parsing::PreprocessorOptions>();
                syntax::SyntaxTree::MacroList initialMacros;
                std::filesystem::path cachedEntry;
                if (auto compiled = replayOptions.get<syntax::PrecompiledUvmSyntax>();
                    compiled && compiled->tree) {
                    initialMacros = compiled->tree->getDefinedMacros();
                    cachedEntry = compiled->entry.empty() ? compiled->root / "uvm_pkg.sv"
                                                          : compiled->entry;
                    preprocessorOptions.predefines.clear();
                    preprocessorOptions.undefines.clear();
                    replayOptions.set(preprocessorOptions);
                }
                parsing::Preprocessor pp(d.sm, *macroArena, diagnostics, replayOptions,
                                         initialMacros);
                auto roots = tree->getSourceBufferIds();
                for (auto it = roots.begin(); it != roots.end(); ++it)
                    if (!d.sm.getIncludedFrom(*it) &&
                        (cachedEntry.empty() || d.sm.getFullPath(*it) != cachedEntry))
                        pp.pushSource(
                            SourceBuffer{d.sm.getSourceText(*it), d.sm.getLibraryFor(*it), *it});
                const auto targetPath = d.sm.getFullPath(occurrence);
                bool reached = false;
                size_t replayTokens = 0;
                for (;;) {
                    if ((replayTokens++ & 1023) == 0)
                        ctx.checkpoint("method macro environment replay");
                    auto token = pp.next();
                    if (token.kind == parsing::TokenKind::EndOfFile)
                        break;
                    // Offsets almost always differ. Avoid resolving and copying a
                    // filesystem path for every token of the preceding design.
                    if (token.location().offset() == functionBegin &&
                        d.sm.isFileLoc(token.location()) &&
                        d.sm.getFullPath(token.location().buffer()) == targetPath) {
                        reached = true;
                        break;
                    }
                }
                need(reached, "method preprocessing context unavailable");
                bodyMacros = pp.getDefinedMacros();
                bodyKeywordVersion = int(pp.getCurrentKeywordVersion());
                auto metadata = pp.getMetadata();
                auto pins = metadata.sourceBufferIds;
                for (auto macro : bodyMacros)
                    pins.push_back(macro->name.location().buffer());
                macroPins = d.sm.retainBuffers(pins);
            }
            phases.mark("body-prepare-macros");
            if (macroProof)
                bodyMacroProof = std::make_unique<BodyMacroProof>(tree,
                    std::string_view(text).substr(functionBegin, functionEnd - functionBegin),
                    selected->prototype->keyword.location(), selected->end.location(), bodyMacros);
        }
    }
    if (!generalBody) {
        auto* declaration = selected->parent
                                ? selected->parent->as_if<syntax::ClassMethodDeclarationSyntax>()
                                : nullptr;
        auto* cls = declaration && declaration->parent
                        ? declaration->parent->as_if<syntax::ClassDeclarationSyntax>()
                        : nullptr;
        auto* package = cls && cls->parent ? cls->parent->as_if<syntax::ModuleDeclarationSyntax>()
                                           : nullptr;
        need(cls && package && package->kind == syntax::SyntaxKind::PackageDeclaration &&
                 !cls->parameters &&
                 (bodyQueries || (!cls->extendsClause && !cls->implementsClause)) &&
                 !cls->virtualOrInterface && cls->attributes.empty(),
             "ordinary package class");
        need(declaration->qualifiers.empty() && declaration->attributes.empty() &&
                 selected->attributes.empty() && !selected->endBlockName &&
                 selected->prototype->specifiers.empty() &&
                 selected->prototype->lifetime.kind != parsing::TokenKind::StaticKeyword &&
                 selected->prototype->keyword.kind == parsing::TokenKind::FunctionKeyword &&
                 plainInt(selected->prototype->returnType),
             "ordinary int instance function");
        auto* name = selected->prototype->name->as_if<syntax::IdentifierNameSyntax>();
        need(name, "plain method name");
        packageName = package->header->name.valueText();
        className = cls->name.valueText();
        functionName = name->identifier.valueText();
        auto& fn = method(compilation, packageName, className, functionName);
        need(fn.getSyntax() == selected, "target semantic identity");
        functionPath = fn.getHierarchicalPath();
        need(bool(selected->prototype->portList), "explicit formal list required");
        std::regex identifier("[A-Za-z_][A-Za-z_0-9]*");
        need(std::regex_match(packageName, identifier) && std::regex_match(className, identifier) &&
                 std::regex_match(functionName, identifier),
             "ordinary identifiers required");
        for (auto* port : selected->prototype->portList->ports) {
            auto* p = port->as_if<syntax::FunctionPortSyntax>();
            need(p && p->attributes.empty() &&
                     p->direction.kind == parsing::TokenKind::InputKeyword && !p->constKeyword &&
                     !p->staticKeyword && !p->varKeyword && plainInt(p->dataType) &&
                     p->declarator->dimensions.empty() && !p->declarator->initializer,
                 "plain input int");
        }
        if (!bodyQueries) {
            auto imports = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
                need(node.kind != syntax::SyntaxKind::PackageImportDeclaration,
                     "import environment unsupported");
                for (uint32_t i = 0; i < node.getChildCount(); ++i)
                    if (auto* child = node.childNode(i))
                        self(self, *child);
            };
            imports(imports, *package);
            for (auto* member : cls->items) {
                if (auto* field = member->as_if<syntax::ClassPropertyDeclarationSyntax>()) {
                    auto* value = field->declaration->as_if<syntax::DataDeclarationSyntax>();
                    need(field->attributes.empty() && field->qualifiers.empty() && value &&
                             value->attributes.empty() && value->modifiers.empty() &&
                             plainInt(value->type),
                         "ordinary int field interface");
                    for (auto* declarator : value->declarators)
                        need(declarator->dimensions.empty() && !declarator->initializer,
                             "dependent field interface");
                }
                else {
                    auto* other = member->as_if<syntax::ClassMethodDeclarationSyntax>();
                    need(other && other->qualifiers.empty() && other->attributes.empty(),
                         "ordinary method interface");
                    auto& prototype = *other->declaration->prototype;
                    need(other->declaration->attributes.empty() && prototype.specifiers.empty() &&
                             prototype.keyword.kind == parsing::TokenKind::FunctionKeyword &&
                             prototype.lifetime.kind != parsing::TokenKind::StaticKeyword &&
                             plainInt(prototype.returnType),
                         "explicit int return interface");
                    if (prototype.portList)
                        for (auto* port : prototype.portList->ports) {
                            auto* argument = port->as_if<syntax::FunctionPortSyntax>();
                            need(argument && argument->attributes.empty() &&
                                     argument->direction.kind == parsing::TokenKind::InputKeyword &&
                                     !argument->constKeyword && !argument->staticKeyword &&
                                     !argument->varKeyword && plainInt(argument->dataType) &&
                                     argument->declarator->dimensions.empty() &&
                                     !argument->declarator->initializer,
                                 "dependent method interface");
                        }
                }
            }
        }
        for (const ast::Scope* scope = fn.getParentScope(); scope;
             scope = scope->asSymbol().getParentScope())
            for (auto& member : scope->members())
                if (&member != &fn && !member.name.empty())
                    outerNames.emplace_back(member.name);
        auto body = shape(*selected);
        bodyBegin = body.begin;
        bodyEnd = body.end;
        localCount = body.locals;
        originalBodyEnd = bodyEnd;
        localDeclarations = declarations(*selected);
        verify(fn, outerNames);
        functionBegin = selected->getFirstToken().location().offset();
        functionEnd = selected->getLastToken().range().end().offset();
        text = plain(*document);
        need(std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 128; }),
             "ASCII header only");
        need(text.substr(functionBegin, functionEnd - functionBegin).find('\\') ==
                 std::string::npos,
             "escaped method tokens unsupported");
        if (bodyQueries) {
            need(text.substr(functionBegin, functionEnd - functionBegin).find('`') ==
                     std::string::npos,
                 "macro-free method required");
            auto tokens = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
                for (uint32_t i = 0; i < node.getChildCount(); ++i) {
                    if (auto* child = node.childNode(i))
                        self(self, *child);
                    else if (auto token = node.childToken(i); token)
                        need(token.location().buffer() == occurrence, "expanded method token");
                }
            };
            tokens(tokens, *selected);
        }
        originalBuffer = occurrence.getId();
        currentBuffer = document->getBuffer().getId();
        version = *document->getVersion();
        ctx.checkpoint("method certificate method ready");
        // Completion order still comes from the native queries below. Index only
        // identity resolution, once, instead of scanning every visible member for
        // each item. Pointers live only during this constructor; the retained
        // interface consists of value DTOs. Outer matches overwrite inner matches
        // in the same traversal order as the previous per-item search.
        using Identity = std::tuple<ast::SymbolKind, std::string, std::string, uint32_t, uint64_t>;
        std::map<Identity, const ast::Symbol*> symbols;
        auto indexScope = [&](const ast::Scope& scope) {
            for (auto& member : scope.members()) {
                const ast::Symbol* symbol = &member;
                while (auto* proxy = symbol->as_if<ast::TransparentMemberSymbol>())
                    symbol = &proxy->wrapped;
                if (auto* import = symbol->as_if<ast::ExplicitImportSymbol>())
                    symbol = import->importedSymbol();
                if (symbol && !symbol->name.empty())
                    symbols.insert_or_assign(Identity{symbol->kind, std::string(symbol->name),
                                                      symbol->getHierarchicalPath(),
                                                      symbol->location.buffer().getId(),
                                                      symbol->location.offset()},
                                             symbol);
            }
        };
        for (const ast::Scope* scope = &fn; scope; scope = scope->asSymbol().getParentScope()) {
            ctx.checkpoint("method certificate completion scope");
            indexScope(*scope);
            if (bodyQueries)
                if (auto* imports = scope->getWildcardImportData())
                    for (auto* import : imports->wildcardImports)
                        if (auto* imported = import->getPackage())
                            indexScope(*imported);
        }
        for (size_t kind = 0; kind < 2; ++kind) {
            std::vector<lsp::CompletionItem> items;
            CompletionContext context;
            context.kind = kind ? CompletionContextKind::Expression
                                : CompletionContextKind::Procedural;
            completions::InstanceCompletionQuery::addCompletions(items, d.m_indexer, context);
            Formatting::addCompletions(items, &fn, context.kind, &fn, uri.str(), false, false);
            lexical[kind].reserve(items.size());
            for (auto& item : items) {
                DTO dto{.item = std::move(item)};
                lsp::CompletionItem resolved;
                if (!dto.item.data)
                    resolved = dto.item;
                if (dto.item.data) {
                    auto data = rfl::from_generic<Data, rfl::UnderlyingEnums>(*dto.item.data);
                    need(bool(data), "completion identity");
                    auto symbol = symbols.find(Identity{data->symbolKind, data->symbolName,
                                                        data->symbolPath, data->bufferId,
                                                        data->offset});
                    need(symbol != symbols.end(), "unaccounted completion symbol");
                    const auto* found = symbol->second;
                    dto.methodOwned = found->getParentScope() == &fn;
                    completions::MemberCompletionQuery::resolve(*found, resolved, false);
                    if (!dto.methodOwned && found->getSyntax()) {
                        auto span = found->getSyntax()->sourceRange();
                        dto.changedDocumentation = span.start().buffer() == occurrence &&
                                                   span.start().offset() < bodyEnd &&
                                                   span.end().offset() > bodyBegin;
                    }
                }
                else if (dto.item.kind == lsp::CompletionItemKind::Module) {
                    completions::InstanceCompletionQuery::resolve(*tree, dto.item.label, resolved);
                    need(bool(resolved.documentation), "global module outside frozen tree");
                }
                dto.documentation = std::move(resolved.documentation);
                dto.insertText = std::move(resolved.insertText);
                dto.insertTextFormat = resolved.insertTextFormat;
                if (dto.methodOwned)
                    methodSlots[kind].push_back(lexical[kind].size());
                lexical[kind].push_back(std::move(dto));
            }
        }
        // No AST-backed lookup is needed while capturing the diagnostic snapshot.
        symbols.clear();
        ctx.checkpoint("method certificate completions ready");
    }
    Bag miniOptions = tree->options();
    if (generalBody) {
        auto pp = miniOptions.getOrDefault<parsing::PreprocessorOptions>();
        pp.predefines.clear();
        pp.undefines.clear();
        pp.keywordMapping = {{"*", parsing::KeywordVersion(bodyKeywordVersion)}};
        pp.captureUvmPrefix = false;
        pp.bufferChangeCB = {};
        miniOptions = Bag{miniOptions.getOrDefault<parsing::LexerOptions>(),
                          miniOptions.getOrDefault<parsing::ParserOptions>(), pp};
    }
    if (bodyQueries && !generalBody) {
        // Mini sources have no includes, macros or semantic-library imports.
        // Retain language / lexer behavior without copying live owner options.
        parsing::PreprocessorOptions pp;
        pp.languageVersion =
            miniOptions.getOrDefault<parsing::PreprocessorOptions>().languageVersion;
        miniOptions = Bag{miniOptions.getOrDefault<parsing::LexerOptions>(),
                          miniOptions.getOrDefault<parsing::ParserOptions>(), pp};
    }
    treeOptions = std::make_unique<diagnostic_snapshot::ReconstructedOptions>(
        diagnostic_snapshot::captureOptions(miniOptions));
    compilationOptions = std::make_unique<diagnostic_snapshot::ReconstructedOptions>(
        diagnostic_snapshot::captureOptions(compilation.getOptions()));
    captureWitnesses(config, compilation, occurrence, ctx, phases);
    captureLocalBaseline(config, ctx);
    ctx.checkpoint("after method certificate");
}

MethodScopeOverlay::~MethodScopeOverlay() {
    localDiagnostics.releasePrepared();
    miniAnalysis.reset();
    miniTree.reset();
    miniManager.reset();
}
bool MethodScopeOverlay::belongsTo(const ServerDriver& d) const {
    return driver == &d && frozen.owner == d.m_snapshotOwner;
}
bool MethodScopeOverlay::validContext(const Config& config, const lsp::RequestContext& ctx) const
    try {
    UvmPhaseTrace phases;
    if (generalBody)
        for (auto original : bodyBlocked)
            if (original->bodyHasExternalDependents.load(std::memory_order_relaxed)) {
                return false;
            }
    if (generalBody)
        for (auto syntax : bodyBlockedSyntax)
            if (bodyOwner->getCompilation()->bodySyntaxHasExternalDependents(*syntax)) {
                return false;
            }
    if (bodyQueries && (frozen.revision != driver->completions.getCompletionEpoch() ||
                        immutableUvm.lock() != driver->m_trustedUvm ||
                        immutableScopes.lock() != driver->m_trustedScopes)) {
        return false;
    }
    if (!belongsTo(*driver) || driver->getContextInputRevision() != inputRevision ||
        driver->getDependencyIndexRevision() != indexRevision ||
        fs::current_path().string() != frozen.input->sources.cwd ||
        rfl::json::write(config) != configKey ||
        rfl::json::write(diagnostic_snapshot::captureOptions(
            bodyQueries ? Bag{driver->options.getOrDefault<ast::CompilationOptions>()}
                        : driver->options)) != optionsKey) {
        return false;
    }
    size_t count = 0;
    for (auto& [openUri, document] : driver->docs)
        if (document->getVersion()) {
            ++count;
            auto it = documents.find(openUri);
            if (it == documents.end()) {
                return false;
            }
            const auto& w = it->second;
            const auto current = document->getBuffer();
            bool sameBuffer = current.getId() == w.buffer;
            if (!sameBuffer) {
                // Native replay can assign another sort-key / include alias to
                // the exact same FileData. That does not change this document's
                // input generation. Equal text in a fresh allocation is not enough.
                const auto previous = BufferID(w.buffer, "method source witness");
                if (driver->sm.isLatestData(previous) && driver->sm.isLatestData(current) &&
                    driver->sm.getFullPath(previous) == driver->sm.getFullPath(current)) {
                    const auto before = driver->sm.getSourceText(previous);
                    const auto after = driver->sm.getSourceText(current);
                    sameBuffer = before.data() == after.data() && before.size() == after.size();
                }
            }
            if (document.get() != w.document || !sameBuffer ||
                *document->getVersion() != w.version || plain(*document) != w.text ||
                !driver->sm.isLatestData(current)) {
                return false;
            }
        }
    if (count != documents.size()) {
        return false;
    }
    phases.mark("body-validate-context");
    return true;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return false;
}

bool MethodScopeOverlay::optimizedCertification(const Config& config) const {
    return generalBody && config.experimentalMethodBodyCertification.value();
}
bool MethodScopeOverlay::readProofInputs(std::span<const FileProofInput> inputs,
                                         std::string_view checkpoint,
                                         const lsp::RequestContext& ctx) const {
    ctx.checkpoint("before method input proof");
    if (directoryProofPlan) {
        try {
            DirectoryProofPlan::Session session(*directoryProofPlan, ctx);
            FileProofDirectories directories(&session);
            for (const auto& input : inputs) {
                ctx.checkpoint(checkpoint);
                if (input.expected && !input.expected->has_value() &&
                    session.covers(input.directoryGroup))
                    continue;
                if (input.expected && !input.captured) {
                    if (!readCertificateFile(*input.path, true, ctx, input.expected,
                                             &directories).matches) {
                        return false;
                    }
                }
                else {
                    auto bytes = readCertificateFile(*input.path, true, ctx, nullptr,
                                                      &directories).contents;
                    if (input.expected && bytes != *input.expected) {
                        return false;
                    }
                    if (input.captured)
                        *input.captured = std::move(bytes);
                }
            }
            ctx.checkpoint("after batched " + std::string(checkpoint));
            return session.valid(ctx);
        }
        catch (const DirectoryProofUnavailable&) {
            // The entire optional session (including borrowed access) unwinds
            // before retrying. No partial success is reused by this attempt.
            ctx.checkpoint("before serial directory proof retry");
        }
    }
    FileProofDirectories directories;
    for (const auto& input : inputs) {
        ctx.checkpoint(checkpoint);
        if (input.expected && !input.captured) {
            if (!readCertificateFile(*input.path, true, ctx, input.expected, &directories).matches) {
                return false;
            }
            continue;
        }
        auto bytes = readCertificateFile(*input.path, true, ctx, nullptr, &directories).contents;
        if (input.expected && bytes != *input.expected) {
            return false;
        }
        if (input.captured)
            *input.captured = std::move(bytes);
    }
    ctx.checkpoint("after batched " + std::string(checkpoint));
    return true;
}
bool MethodScopeOverlay::validForWork(const Config& config, const lsp::RequestContext& ctx) const {
    return optimizedCertification(config) ? validContext(config, ctx) : valid(config, ctx);
}
std::optional<std::string> MethodScopeOverlay::changedSourceAlias(
    const lsp::RequestContext& ctx) const {
    for (const auto& [alias, target] : sourceAliases) {
        ctx.checkpoint("method source alias validation");
        std::error_code error;
        // Match SourceManager::openCachedImpl's resolution. Comparing bytes
        // alone misses retargeting to an equal file with a different include base.
        if (fs::weakly_canonical(alias, error).native() != fs::path(target).native() || error)
            return alias;
    }
    return std::nullopt;
}
bool MethodScopeOverlay::valid(const Config& config, const lsp::RequestContext& ctx) const try {
    if (!validContext(config, ctx))
        return false;
    UvmPhaseTrace phases;
    for (const auto& [path, entries] : directories) {
        ctx.checkpoint("method directory validation");
        if (directoryEntries(path) != entries) {
            inputChanged = true;
            return false;
        }
    }
    phases.mark("body-validate-directories");
    if (optimizedCertification(config) && disk.size() >= 64) {
        std::vector<FileProofInput> inputs;
        if (plannedProofInputs.empty()) {
            inputs.reserve(disk.size());
            for (const auto& [path, bytes] : disk)
                inputs.push_back({&path, &bytes, nullptr});
        }
        if (!readProofInputs(plannedProofInputs.empty()
                                 ? std::span<const FileProofInput>(inputs)
                                 : std::span<const FileProofInput>(plannedProofInputs),
                             "method input validation", ctx)) {
            inputChanged = true;
            return false;
        }
    }
    else {
        for (auto& [path, bytes] : disk) {
            ctx.checkpoint("method input validation");
            if (!readMatches(path, bytes, optimizedCertification(config), ctx)) {
                inputChanged = true;
                return false;
            }
        }
    }
    phases.mark("body-validate-files");
    if (changedSourceAlias(ctx)) {
        inputChanged = true;
        return false;
    }
    ctx.checkpoint("method inputs certified");
    return true;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    inputChanged = true;
    return false;
}

bool MethodScopeOverlay::apply(const lsp::DidChangeTextDocumentParams& p, const Config& config,
                               const lsp::RequestContext& ctx, DocumentEditTransaction& transaction,
                               bool deferBinding) {
    need(validForWork(config, ctx) && p.textDocument.uri == uri &&
             p.textDocument.version > version &&
             (accepted < (bodyQueries ? 64u : 8u) ||
              (generalBody && config.experimentalMethodBodyCertificateRenewal.value())) &&
             !p.contentChanges.empty() && (bodyQueries || p.contentChanges.size() == 1),
         "method context/version/bound changed");
    ctx.checkpoint("before method edit work");
    // The original owner and current document witnesses are checked above.
    // Full input contents are rechecked before body / response publication.
    // A renewal is bookkeeping on that same proof, never a new owner based on edited syntax.
    // Commit it only after the edit succeeds (including the deferred-binding text path).
    if (generalBody && certificateEdits == 64)
        ctx.checkpoint("method certificate renewal");
    std::string next = text;
    for (const auto& change : p.contentChanges) {
        if (auto* whole = rfl::get_if<lsp::TextDocumentContentChangeWholeDocument>(&change)) {
            next = whole->text;
            continue;
        }
        const auto& edit = rfl::get<lsp::TextDocumentContentChangePartial>(change);
        auto offset = [&](const lsp::Position& position) {
            size_t start = 0;
            for (uint32_t line = 0; line < position.line; ++line) {
                auto newline = next.find('\n', start);
                need(newline != std::string::npos, "invalid edit line");
                start = newline + 1;
            }
            auto end = next.find('\n', start);
            if (end == std::string::npos)
                end = next.size();
            return start + utf16ColumnToByte(std::string_view(next).substr(start, end - start),
                                             position.character);
        };
        auto begin = offset(edit.range.start), end = offset(edit.range.end);
        need(begin <= end && end <= next.size(), "invalid edit range");
        next.replace(begin, end - begin, edit.text);
    }
    need(next != text && next.size() >= bodyBegin + text.size() - bodyEnd,
         "empty edit or missing method suffix");
    const auto nextBodyEnd = next.size() - (text.size() - bodyEnd);
    const auto nextFunctionEnd = next.size() - (text.size() - functionEnd);
    need(next.substr(0, bodyBegin) == text.substr(0, bodyBegin) &&
             next.substr(nextBodyEnd) == text.substr(bodyEnd),
         "edit crosses body/interface boundary");
    if (!bodyQueries) {
        need(next.size() == text.size(), "phase one requires equal-length edits");
        for (size_t i = 0; i < text.size(); ++i)
            need((next[i] == '\n') == (text[i] == '\n') && (next[i] == '\r') == (text[i] == '\r'),
                 "fixed line layout required");
    }
    if (generalBody) {
        captureLocalBaseline(config, ctx);
        if (deferBinding) {
            // A superseded notification still advances text and its witness. Do
            // not discard the fixed lexical owner or bind obsolete intermediate
            // bodies; the next live query validates and binds the current body.
            commitGeneralText(p, std::move(next), nextBodyEnd, nextFunctionEnd, ctx, transaction);
            return true;
        }
        if (!optimizedCertification(config))
            prepareLocalDiagnostics(config, ctx);
        return generalApply(p, config, ctx, transaction, std::move(next), nextBodyEnd,
                            nextFunctionEnd);
    }
    need(!deferBinding, "deferred binding requires general body queries");
    need(std::all_of(next.begin(), next.end(), [](unsigned char c) { return c < 128; }),
         "ASCII source required");
    auto methodText = next.substr(functionBegin, nextFunctionEnd - functionBegin);
    need((bodyQueries ? methodText : next).find('`') == std::string::npos,
         "method directives unsupported");
    need(methodText.find('\\') == std::string::npos, "escaped method tokens unsupported");
    auto manager = std::make_unique<SourceManager>();
    auto prefix = "package " + packageName + "; class " + className + "; ";
    auto source = manager->assignText(prefix + methodText + " endclass endpackage");
    ctx.checkpoint("before method mini parse");
    auto tree = syntax::SyntaxTree::fromBuffer(source, *manager, treeOptions->bag());
    need(tree->diagnostics().empty(), "method syntax diagnostics");
    auto analysis = std::make_shared<ShallowAnalysis>(
        *manager, source.id, tree, compilationOptions->bag(),
        std::vector<std::shared_ptr<syntax::SyntaxTree>>{tree}, nullptr, ctx, true);
    auto& compilation = *analysis->getCompilation();
    auto& fn = method(compilation, packageName, className, functionName);
    auto currentShape = shape(fn.getSyntax()->as<syntax::FunctionDeclarationSyntax>());
    need(currentShape.locals == localCount, "local additions/removals are a later slice");
    if (bodyQueries)
        need(declarations(fn.getSyntax()->as<syntax::FunctionDeclarationSyntax>()) ==
                 localDeclarations,
             "local declaration changed");
    verify(fn, outerNames);
    need(compilation.getAllDiagnostics().empty(), "method semantic diagnostics");
    std::array<DTOList, 2> fresh;
    for (size_t kind = 0; kind < 2; ++kind) {
        std::vector<lsp::CompletionItem> items;
        Formatting::addCompletions(items, &fn,
                                   kind ? CompletionContextKind::Expression
                                        : CompletionContextKind::Procedural,
                                   &fn, uri.str(), false, false);
        DTOList owned;
        owned.reserve(methodSlots[kind].size());
        for (auto& item : items) {
            if (!item.data)
                continue;
            auto data = rfl::from_generic<Data, rfl::UnderlyingEnums>(*item.data);
            need(bool(data), "mini item data");
            const ast::Symbol* symbol = nullptr;
            for (auto& candidate : fn.members())
                if (candidate.location == SourceLocation(source.id, data->offset) &&
                    candidate.kind == data->symbolKind && candidate.name == data->symbolName &&
                    candidate.getHierarchicalPath() == data->symbolPath) {
                    need(!symbol, "ambiguous mini identity");
                    symbol = &candidate;
                }
            if (!symbol || symbol->getParentScope() != &fn)
                continue;
            DTO dto{.item = std::move(item), .methodOwned = true};
            lsp::CompletionItem resolved;
            completions::MemberCompletionQuery::resolve(*symbol, resolved, false);
            dto.documentation = std::move(resolved.documentation);
            if (data->offset >= prefix.size() &&
                data->offset < prefix.size() + nextFunctionEnd - functionBegin)
                data->offset = functionBegin + data->offset - prefix.size();
            else {
                // `this` denotes the unchanged containing class, outside the method slice.
                auto it = std::find_if(
                    methodSlots[kind].begin(), methodSlots[kind].end(), [&](auto slot) {
                        const auto& old = lexical[kind][slot];
                        if (!old.item.data)
                            return false;
                        auto oldData = rfl::from_generic<Data, rfl::UnderlyingEnums>(
                            *old.item.data);
                        return oldData && oldData->symbolName == data->symbolName &&
                               oldData->symbolPath == data->symbolPath &&
                               oldData->symbolKind == data->symbolKind;
                    });
                need(it != methodSlots[kind].end(), "mini source outside certified method");
                data->offset = rfl::from_generic<Data, rfl::UnderlyingEnums>(
                                   *lexical[kind][*it].item.data)
                                   ->offset;
            }
            data->bufferId = originalBuffer;
            dto.item.data = rfl::to_generic<rfl::UnderlyingEnums>(*data);
            owned.push_back(std::move(dto));
        }
        need(methodSlots[kind].size() == owned.size(), "method member coverage mismatch");
        fresh[kind] = std::move(owned);
    }
    std::vector<Site> freshSites;
    auto collect = ast::makeVisitor([&](auto&, const ast::NamedValueExpression& value) {
        need(value.sourceRange.start().buffer() == source.id &&
                 value.symbol.location.buffer() == source.id,
             "reference outside mini source");
        auto* use = analysis->syntaxes.getWordTokenAt(value.sourceRange.start());
        auto* decl = analysis->syntaxes.getWordTokenAt(value.symbol.location);
        need(use && decl, "reference/declaration token");
        size_t kind = 0;
        for (auto* node = analysis->syntaxes.getTokenParent(use);
             node && node->kind != syntax::SyntaxKind::FunctionDeclaration; node = node->parent)
            if (node->kind == syntax::SyntaxKind::EqualsValueClause ||
                node->kind == syntax::SyntaxKind::ParenthesizedExpression)
                kind = 1;
        auto map = [&](size_t offset) {
            need(offset >= prefix.size() &&
                     offset <= prefix.size() + nextFunctionEnd - functionBegin,
                 "reference mapping bounds");
            return functionBegin + offset - prefix.size();
        };
        freshSites.push_back({map(use->location().offset()), map(use->range().end().offset()),
                              map(decl->location().offset()), map(decl->range().end().offset()),
                              kind,
                              analysis->syntaxes.getWordTokenAt(use->range().end()) != nullptr});
    });
    for (auto& member : fn.members())
        if (member.kind == ast::SymbolKind::Variable && &member != fn.thisVar &&
            &member != fn.returnValVar)
            member.as<ast::VariableSymbol>().getInitializer()->visit(collect);
    fn.getBody().visit(collect);
    // Allocate both publication texts before native mutation. After it starts no fallback replay is
    // legal.
    auto witnessText = next;
    need(valid(config, ctx), "inputs changed before method commit");
    ctx.checkpoint("before method publication");
    transaction.synchronize([&] { driver->onDocDidChange(p, ctx); });
    ctx.checkpoint("after method text synchronization");
    auto document = driver->getDocument(uri);
    auto actual = document->getText();
    need(actual.size() == next.size() + 1 && actual.substr(0, next.size()) == next,
         "native synchronization mismatch");
    miniAnalysis.reset();
    miniTree.reset();
    miniManager.reset();
    miniManager = std::move(manager);
    miniTree = std::move(tree);
    miniAnalysis = std::move(analysis);
    methodValues = std::move(fresh);
    sites = std::move(freshSites);
    text = std::move(next);
    bodyEnd = nextBodyEnd;
    functionEnd = nextFunctionEnd;
    version = p.textDocument.version;
    currentBuffer = document->getBuffer().getId();
    handles.push_back(currentBuffer);
    documents.at(uri) = DocumentWitness{document.get(), currentBuffer, version,
                                        std::move(witnessText)};
    inputRevision = driver->getContextInputRevision();
    ++accepted;
    transaction.commitOverlay();
    return true;
}

bool MethodScopeOverlay::generalApply(const lsp::DidChangeTextDocumentParams& p,
                                      const Config& config, const lsp::RequestContext& ctx,
                                      DocumentEditTransaction& transaction, std::string next,
                                      size_t nextBodyEnd, size_t nextFunctionEnd, bool bindOnly) {
    UvmPhaseTrace phases;
    auto methodText = next.substr(functionBegin, nextFunctionEnd - functionBegin);
    const bool freshMacroEffects = bodyMacroProof ? bodyMacroProof->validateRaw(methodText) : false;
    if (!bodyMacroProof) validateBodyPreprocessing(methodText);
    for (auto original : bodyOriginals)
        need(!original->bodyHasExternalDependents.load(std::memory_order_relaxed),
             "method has constant dependencies");
    // Preserve byte positions and line layout without reparsing any outer declaration.
    std::string padded = next.substr(0, functionBegin);
    for (auto& c : padded)
        if (c != '\n' && c != '\r')
            c = ' ';
    padded += methodText;
    QueryText query(driver->sm,
                    driver->sm.getFullPath(driver->getDocument(uri)->getBuffer()).string(), padded);
    const auto& source = query.buffer;
    for (const auto& line :
         driver->sm.getLineDirectives(BufferID(originalBuffer, "body logical lines"))) {
        auto originalLine = driver->sm.getSourceLocation(
            BufferID(originalBuffer, "body logical lines"), line.lineInFile, 1);
        if (originalLine && originalLine->offset() < functionBegin)
            driver->sm.addLineDirective(SourceLocation(source.id, originalLine->offset()),
                                        line.lineOfDirective, line.name, line.level, true);
    }
    auto diagnosticDirectives = driver->sm.getDiagnosticDirectives(
        BufferID(originalBuffer, "body diagnostic directives"));
    auto inherited = std::vector<SourceManager::DiagnosticDirectiveInfo>(
        diagnosticDirectives.begin(), diagnosticDirectives.end());
    for (const auto& directive : inherited)
        if (directive.offset < functionBegin)
            driver->sm.addDiagnosticDirective(SourceLocation(source.id, directive.offset),
                                              directive.name, directive.severity, true);
    auto blocked = bodyOriginals;
    bool generatedMacrosValidated = false;
    std::vector<const syntax::SyntaxNode*> blockedSyntax;
    // One edited declaration can have other Native instances or future generic
    // specializations. Protect the declaration as well as the selected query
    // symbols, so none of those can evaluate the old body through another path.
    for (auto original : bodyOriginals)
        if (auto syntax = original->getSyntax();
            syntax && std::ranges::find(blockedSyntax, syntax) == blockedSyntax.end())
            blockedSyntax.push_back(syntax);
    const bool shiftedLines = driver->sm.getRawLineNumber(SourceLocation(source.id, nextBodyEnd)) !=
                              driver->sm.getRawLineNumber(
                                  SourceLocation(BufferID(originalBuffer, "original body lines"),
                                                 originalBodyEnd));
    if (freshMacroEffects && shiftedLines) bodyMacroProof->validateShiftedLookups(ctx);
    if (shiftedLines) {
        if (bodyLineSensitiveDeclaration) {
            if (!localDiagnostics.cache())
                prepareLocalDiagnostics(config, ctx);
            need(bool(localDiagnostics.cache()),
                 "body shifts line-dependent enclosing declarations");
            auto contexts = localDiagnostics.cache()->validateGeneratedMacros(next, nextFunctionEnd,
                                                                              ctx);
            generatedMacrosValidated = true;
            std::set<const syntax::SyntaxNode*> missingSyntax;
            std::set<const syntax::SyntaxNode*> witnessedSyntax;
            for (auto declaration : contexts) {
                auto matches = bodyOwner->getSubroutines(*declaration);
                if (matches.empty() && std::ranges::find(bodyMacroSyntaxWitness, declaration) ==
                                           bodyMacroSyntaxWitness.end())
                    missingSyntax.insert(declaration);
                if (!matches.empty() && std::ranges::find(bodyMacroSyntaxWitness, declaration) ==
                                            bodyMacroSyntaxWitness.end()) {
                    ctx.checkpoint("generated macro query syntax identity");
                    for (auto method : matches)
                        need(method->getSyntax() == declaration,
                             "generated macro Native declaration identity differs");
                    witnessedSyntax.insert(declaration);
                }
                need(bodyOwner->getCompilation()->hasFlag(
                         ast::CompilationFlags::TrackSubroutineBodyDependencies) &&
                         !bodyOwner->getCompilation()->bodySyntaxHasExternalDependents(
                             *declaration),
                     "generated macro query syntax has semantic dependents");
                blockedSyntax.push_back(declaration);
                for (auto method : matches) {
                    need(method->getCompilation().hasFlag(
                             ast::CompilationFlags::TrackSubroutineBodyDependencies) &&
                             !method->bodyHasExternalDependents.load(std::memory_order_relaxed),
                         "generated macro query body has semantic dependents");
                    blocked.push_back(method);
                }
            }
            if (!missingSyntax.empty()) {
                // Query and native symbols may instantiate at different times.
                // Verify all still-missing declarations in one pure traversal;
                // bodyOwner retains this same immutable CST for our lifetime.
                const auto witnessed = missingSyntax;
                const SourceSyntaxReachability reachable(
                    driver->sm, BufferID(originalBuffer, "macro query evidence"));
                ctx.checkpoint("generated macro query syntax identity walk");
                auto find = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
                    ctx.checkpoint("generated macro query syntax identity");
                    if (reachable.excludes(node))
                        return;
                    missingSyntax.erase(&node);
                    for (uint32_t i = 0; !missingSyntax.empty() && i < node.getChildCount(); ++i)
                        if (auto child = node.childNode(i))
                            self(self, *child);
                };
                find(find, bodyOwner->getSyntaxTree()->root());
                need(missingSyntax.empty(), "generated macro query syntax differs");
                witnessedSyntax.insert(witnessed.begin(), witnessed.end());
            }
            if (!witnessedSyntax.empty()) {
                ctx.checkpoint("before generated macro query syntax witness commit");
                bodyMacroSyntaxWitness.insert(bodyMacroSyntaxWitness.end(), witnessedSyntax.begin(),
                                              witnessedSyntax.end());
            }
        }
        for (auto method : bodyLineDependents) {
            need(!method->bodyHasExternalDependents.load(std::memory_order_relaxed),
                 "body shifts a method with external semantic dependencies");
            blocked.push_back(method);
        }
    }
    phases.mark("body-apply-text");
    ast::BodyQueryEvaluationGuard guard(blocked, blockedSyntax);
    ctx.checkpoint("before method mini parse");
    auto tree = query.parse(treeOptions->bag(), bodyMacros, bool(bodyMacroProof));
    // The mini source blanks outer code, including comments before the keyword.
    // Keep the native leading trivia for documentation without executing its
    // directives again. Install it before publishing / binding this fresh tree.
    SmallVector<parsing::Trivia> leading;
    for (auto trivia : bodyOriginals.front()->getSyntax()->getFirstToken().trivia()) {
        if (auto raw = trivia.getRawText(); !raw.empty()) {
            auto owned = tree->allocator().copyFrom<char>(std::span(raw.data(), raw.size()));
            leading.emplace_back(trivia.kind, std::string_view(owned.data(), owned.size()));
        }
        else
            leading.push_back(trivia.clone(tree->allocator(), true));
    }
    auto* first = tree->root().getFirstTokenPtr();
    *first = first->withTrivia(tree->allocator(), leading);
    if (!bodyMacroProof)
        for (const auto& reference : tree->getPreprocessorMetadata().macroRefs)
            validateMacroPreprocessing(reference, bodyMacros, ctx);
    phases.mark("body-apply-parse");
    auto unit = std::make_shared<ShallowCompilation>(driver->sm, tree,
                                                     bodyOwner->getShallowCompilation(),
                                                     bodyOriginals);
    for (size_t i = 0; i < unit->getBodyQueryRoots().size(); ++i) {
        auto body = unit->getBodyQueryRoots()[i];
        auto syntax = body->getSyntax()->as_if<syntax::FunctionDeclarationSyntax>();
        need(syntax && syntax->semi.range().end().offset() == bodyBegin &&
                 syntax->end.location().buffer() == source.id &&
                 syntax->end.location().offset() == nextBodyEnd &&
                 syntax->getLastToken().range().end().offset() == nextFunctionEnd,
             "method parse escaped body boundary");
        auto before = bodyOriginals[i]->getArguments(), after = body->getArguments();
        need(before.size() == after.size(), "body changes non-ANSI argument interface");
        need(bodyOriginals[i]->getReturnType().isMatching(body->getReturnType()),
             "body changes return type");
        for (size_t a = 0; a < before.size(); ++a) {
            need(before[a]->name == after[a]->name && before[a]->direction == after[a]->direction &&
                     before[a]->flags == after[a]->flags &&
                     before[a]->getType().isMatching(after[a]->getType()),
                 "body changes non-ANSI argument interface");
            auto previous = before[a]->getDefaultValue(), current = after[a]->getDefaultValue();
            need(previous == current ||
                     (previous && current && previous->syntax && current->syntax &&
                      previous->syntax->toString() == current->syntax->toString()),
                 "body changes non-ANSI argument default");
            if (previous && current) {
                need(previous->bad() == current->bad() &&
                         previous->type->isMatching(*current->type),
                     "body changes argument default validity");
                ast::EvalContext beforeContext(*before[a]), afterContext(*after[a]);
                need(previous->eval(beforeContext) == current->eval(afterContext),
                     "body changes argument default value");
            }
        }
        ctx.checkpoint("before detached body binding");
        (void)body->getBody();
        ctx.checkpoint("after detached body binding");
    }
    phases.mark("body-apply-bind");
    auto resultingMacros = tree->getDefinedMacros();
    if (bodyMacroProof) bodyMacroProof->validateExit(resultingMacros);
    else {
        need(resultingMacros.size() == bodyMacros.size(), "body changes macro environment");
        for (size_t i = 0; i < resultingMacros.size(); ++i)
            need(resultingMacros[i] == bodyMacros[i] ||
                     (resultingMacros[i]->name.valueText() == bodyMacros[i]->name.valueText() &&
                      resultingMacros[i]->toString() == bodyMacros[i]->toString()),
                 "body changes macro environment");
    }
    auto analysis = std::make_shared<ShallowAnalysis>(source.id, unit, ctx);
    phases.mark("body-apply-index");
    // This commits synchronized text and a private provisional slice. Queries,
    // diagnostics and native fallbacks independently perform the complete proof
    // at their publication / admission boundaries; no success is cached across requests.
    need(validForWork(config, ctx), "inputs changed before body publication");
    phases.mark("body-apply-revalidate");
    if (!bindOnly)
        commitGeneralText(p, std::move(next), nextBodyEnd, nextFunctionEnd, ctx, transaction);
    if (generatedMacrosValidated)
        ctx.checkpoint("before current macro proof witness commit");
    miniAnalysis = std::move(analysis);
    miniTree = std::move(tree);
    if (generatedMacrosValidated)
        localDiagnostics.cache()->recordGeneratedMacroValidation(miniTree, nextFunctionEnd);
    bodyBlocked = std::move(blocked);
    bodyBlockedSyntax = std::move(blockedSyntax);
    return true;
}

void MethodScopeOverlay::commitGeneralText(const lsp::DidChangeTextDocumentParams& p,
                                           std::string next, size_t nextBodyEnd,
                                           size_t nextFunctionEnd, const lsp::RequestContext& ctx,
                                           DocumentEditTransaction& transaction) {
    auto witnessText = next;
    ctx.checkpoint("before method publication");
    transaction.synchronize([&] { driver->onDocDidChange(p, ctx); });
    ctx.checkpoint("after method text synchronization");
    // A null miniAnalysis means text is synchronized but binding was deferred.
    miniAnalysis.reset();
    miniTree.reset();
    text = std::move(next);
    bodyEnd = nextBodyEnd;
    functionEnd = nextFunctionEnd;
    version = p.textDocument.version;
    auto document = driver->getDocument(uri);
    currentBuffer = document->getBuffer().getId();

    documents.at(uri) = DocumentWitness{document.get(), currentBuffer, version,
                                        std::move(witnessText)};
    inputRevision = driver->getContextInputRevision();
    acceptGeneralEdit();
    transaction.commitOverlay();
}

void MethodScopeOverlay::acceptGeneralEdit() {
    if (certificateEdits == 64) {
        certificateEdits = 0;
        ++renewals;
    }
    ++certificateEdits;
    ++accepted;
}

void MethodScopeOverlay::ensureGeneralBinding(const Config& config,
                                              const lsp::RequestContext& ctx) {
    if (!miniAnalysis) {
        DocumentEditTransaction unused;
        generalApply({}, config, ctx, unused, text, bodyEnd, functionEnd, true);
    }
}

} // namespace server
