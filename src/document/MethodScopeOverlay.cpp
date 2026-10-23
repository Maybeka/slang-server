// SPDX-License-Identifier: MIT
#include "document/MethodScopeOverlay.h"

#include "ast/TrustedUvmLibrary.h"
#include "completions/InstanceCompletions.h"
#include "completions/MemberCompletions.h"
#include "document/BodyDiagnosticCache.h"
#include "util/Converters.h"
#include "util/UvmPhaseTrace.h"
#include <atomic>
#include <cctype>
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

namespace server {
using namespace slang;
namespace fs = std::filesystem;
namespace {
struct Formatting : completions::MemberCompletionQuery {
    using MemberCompletionQuery::addCompletions;
    using MemberCompletionQuery::updateCompletionEditText;
};
struct Data {
    std::string documentUri, symbolPath, symbolName;
    uint32_t bufferId = 0;
    uint64_t offset = 0;
    ast::SymbolKind symbolKind = ast::SymbolKind::Unknown;
    bool labelOnly = false;
    std::optional<uint32_t> receiverBufferId;
    std::optional<uint64_t> receiverOffset;
    bool operator==(const Data&) const = default;
};
struct QueryPayload {
    uint64_t contextEpoch;
    rfl::Generic payload;
    std::optional<uint64_t> methodBodyCertificate;
    std::optional<int> methodBodyVersion;
};
struct NativePayload {
    uint64_t contextEpoch;
    rfl::Generic payload;
};
std::atomic_uint64_t nextBodyCertificate{1};
void need(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
const std::regex& stateDirectives() {
    static const std::regex directives(
        R"(`(define|undef|undefineall|include|begin_keywords|end_keywords|timescale|default_nettype|resetall|line|celldefine|endcelldefine|pragma|unconnected_drive|nounconnected_drive|default_decay_time|default_trireg_strength|delay_mode_distributed|delay_mode_path|delay_mode_unit|delay_mode_zero|uselib)\b)");
    return directives;
}
const std::regex& bodyConditionals() {
    static const std::regex conditionals(R"(`(ifdef|ifndef|elsif|else|endif)\b)");
    return conditionals;
}
void validateBodyPreprocessing(const std::string& methodText) {
    need(!std::regex_search(methodText, stateDirectives()), "body changes preprocessing state");
    size_t depth = 0;
    for (std::sregex_iterator i(methodText.begin(), methodText.end(), bodyConditionals()), end;
         i != end; ++i) {
        auto directive = (*i)[1].str();
        if (directive == "ifdef" || directive == "ifndef")
            ++depth;
        else {
            need(depth != 0, "body changes enclosing conditional state");
            if (directive == "endif")
                --depth;
        }
    }
    need(depth == 0, "body leaves conditional preprocessing state open");
}
void validateMacroPreprocessing(const parsing::MacroRefMetadata& reference) {
    if (!reference.definition)
        return;
    std::string expansion;
    for (auto token : reference.definition->body) {
        expansion += token.rawText();
        expansion += ' ';
    }
    need(!std::regex_search(expansion, stateDirectives()) &&
             !std::regex_search(expansion, bodyConditionals()),
         "macro expansion can change surrounding preprocessing state");
}
std::vector<std::string> directoryEntries(const fs::path& path) {
    need(fs::canonical(path) == fs::absolute(path).lexically_normal(),
         "symlink directory input unsupported");
    need(fs::is_directory(path), "directory input changed");
    std::vector<std::string> entries;
    for (const auto& entry : fs::directory_iterator(path))
        entries.push_back(entry.path().filename().string() + ":" +
                          std::to_string(int(entry.symlink_status().type())));
    std::sort(entries.begin(), entries.end());
    return entries;
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
std::optional<std::string> MethodScopeOverlay::read(const std::string& path) {
    std::error_code error;
    auto status = fs::status(path, error);
    if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status)))
        return {};
    need(!error && fs::is_regular_file(status), ("nonregular certificate input: " + path).c_str());
    need(fs::canonical(path) == fs::absolute(path).lexically_normal(), "symlink input unsupported");
    // Read into one allocation. Iterator extraction spends most body-query time
    // copying unchanged project bytes a character at a time on large designs.
    // Keep exact byte comparison and reject files that shrink/grow during the read.
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    need(bool(input), "unreadable certificate input");
    const auto size = input.tellg();
    need(size >= 0, "input size unavailable");
    std::string bytes(size_t(size), '\0');
    input.seekg(0);
    input.read(bytes.data(), std::streamsize(bytes.size()));
    need(bool(input) && input.peek() == std::char_traits<char>::eof() && !input.bad(),
         "input changed or read failed");
    return bytes;
}

MethodScopeOverlay::MethodScopeOverlay(ServerDriver& d, const Config& config, const URI& target,
                                       size_t editOffset, const lsp::RequestContext& ctx) :
    driver(&d), uri(target) {
    UvmPhaseTrace phases;
    ctx.checkpoint("method certificate begin");
    bodyQueries = config.experimentalMethodBodyQueries.value();
    std::shared_ptr<syntax::SyntaxTree> tree;
    BufferID occurrence;
    auto document = d.getDocument(uri);
    need(document && document->getVersion(), "open target required");
    if (bodyQueries) {
        // Never force a full previous-generation analysis merely to speculate
        // about an edit. Preparation requires a previously queried live view.
        need(document->hasAnalysis(), "current query context required");
        need(!d.completions.resolveEdits, "deferred callable edits required");
        auto graph = document->getAnalysisInputs(ctx);
        tree = graph.tree;
        occurrence = graph.buffer;
        need(tree && occurrence.valid(), "source context unavailable");
        // This first slice does not choose between repeated textual occurrences.
        size_t occurrences = 0;
        for (const auto& candidate : graph.trees)
            for (auto id : candidate->getPreprocessorMetadata().sourceBufferIds)
                occurrences += d.sm.getFullPath(id) == d.sm.getFullPath(occurrence);
        need(occurrences == 1, "nonunique target occurrence");
    }
    else {
        need(!d.comp && d.hasConfiguredCompilationInputs() && !d.completions.resolveEdits,
             "configured package exploration required");
        need(config.compilationFilelist.value().has_value() &&
                 config.explorePreincludeFiles.value().empty(),
             "literal filelist without preinclude required");
        auto flags = [](const std::string& value) {
            std::istringstream words(value);
            std::string word;
            while (words >> word) {
                if (word == "--single-unit")
                    continue;
                if (word == "--threads") {
                    need(bool(words >> word) && word == "1", "one parse worker");
                    continue;
                }
                need(false, "unsupported source-loading flag");
            }
        };
        flags(config.flags.value());
        for (const auto& source : config.flagsByFile.value())
            flags(source.flags);
        std::set<fs::path> listings;
        std::vector<fs::path> recipe;
        auto literal = [](std::string_view value) {
            return !value.empty() &&
                   value.find_first_of("*?[$`@{}\"'\\") == std::string_view::npos &&
                   value.find("...") == std::string_view::npos;
        };
        auto visit = [&](auto&& self, fs::path path, size_t depth) -> void {
            path = fs::absolute(path).lexically_normal();
            need(depth < 16 && listings.insert(path).second, "cyclic/deep filelist");
            auto bytes = read(path.string());
            need(bool(bytes), "missing filelist");
            disk[path.string()] = bytes;
            std::istringstream lines(*bytes);
            std::string line;
            while (std::getline(lines, line)) {
                std::istringstream tokens(line);
                std::string token;
                while (tokens >> token) {
                    ctx.checkpoint("method certificate filelist");
                    if (token.starts_with("//"))
                        break;
                    need(literal(token), "nonliteral recipe");
                    if (token == "-F") {
                        need(bool(tokens >> token) && literal(token), "nested filelist");
                        self(self, path.parent_path() / token, depth + 1);
                    }
                    else if (token.starts_with("+incdir+"))
                        continue; // Every actual/failed lookup is frozen below.
                    else {
                        need(!token.starts_with('-') && !token.starts_with('+'),
                             "unsupported recipe option");
                        recipe.push_back(
                            fs::absolute(path.parent_path() / token).lexically_normal());
                    }
                }
            }
        };
        auto list = fs::path(*config.compilationFilelist.value());
        if (list.is_relative())
            list = fs::path(d.m_workspacePathPrefix) / list;
        visit(visit, list, 0);
        ctx.checkpoint("method certificate recipe ready");
        std::shared_ptr<SlangDoc> unit;
        for (auto& [_, document] : d.docs)
            if (document->isNativeCompilationUnit()) {
                need(!unit, "one native unit required");
                unit = document;
            }
        need(unit && unit->getOptions().getOrDefault<driver::SourceOptions>().singleUnit &&
                 !unit->requiresNativeReplay(),
             "single-unit without inherited replay required");
        auto roots = unit->getRootPaths();
        need(roots.size() == recipe.size() &&
                 std::equal(roots.begin(), roots.end(), recipe.begin()),
             "ordered recipe mismatch");
        tree = unit->getSyntaxTree(ctx);
        auto& pp = tree->getPreprocessorMetadata();
        need(pp.sourceBufferIds.size() == roots.size() + pp.includeDirectives.size() &&
                 pp.macroRefs.empty() && pp.predefineBufferIds.empty() &&
                 !tree->getSourceLibrary() && !tree->isLibraryUnit,
             "complete include graph without user macro environment");
        for (auto* macro : pp.definedMacros)
            need(macro->name.location() == SourceLocation::NoLocation, "user macro definition");
        // Certify every occurrence, not just the edited header. Each non-root buffer
        // must have exactly one literal include edge and reach a configured root.
        // Repeated physical files are allowed outside the target, but recursive
        // physical paths and unexplained preprocessing inputs are not.
        std::set<BufferID> buffers(pp.sourceBufferIds.begin(), pp.sourceBufferIds.end());
        need(buffers.size() == pp.sourceBufferIds.size(), "duplicate source buffer identity");
        std::map<BufferID, BufferID> parents;
        std::map<BufferID, std::vector<std::pair<size_t, size_t>>> directives;

        for (const auto& include : pp.includeDirectives) {
            need(include.syntax && include.buffer && !include.isSystem, "unsupported include");
            auto range = include.syntax->sourceRange();
            auto parent = range.start().buffer();
            auto child = include.buffer.id;
            need(buffers.contains(parent) && buffers.contains(child) &&
                     range.end().buffer() == parent && parents.emplace(child, parent).second,
                 "ambiguous include graph");
            directives[parent].emplace_back(range.start().offset(), range.end().offset());
            if (URI::fromFile(d.sm.getFullPath(child)) == uri) {
                need(!occurrence.valid(), "repeated target include");
                occurrence = child;
            }
        }
        need(occurrence.valid(), "target must be included");
        need(std::count_if(pp.sourceBufferIds.begin(), pp.sourceBufferIds.end(),
                           [&](auto id) {
                               return d.sm.getFullPath(id) == d.sm.getFullPath(occurrence);
                           }) == 1,
             "repeated header occurrence");
        auto options = tree->options().getOrDefault<parsing::PreprocessorOptions>();
        need(options.predefines.empty() && options.undefines.empty() &&
                 options.additionalIncludePaths.empty() && options.ignoreDirectives.empty() &&
                 options.keywordMapping.empty() && !options.bufferChangeCB,
             "preprocessor override");
        for (auto id : pp.sourceBufferIds) {
            std::string bytes(d.sm.getSourceText(id));
            auto& ranges = directives[id];
            std::sort(ranges.begin(), ranges.end());
            size_t previousEnd = 0;
            for (auto [begin, end] : ranges) {
                need(begin >= previousEnd && begin < end && end <= bytes.size(),
                     "invalid include directive range");
                std::fill(bytes.begin() + begin, bytes.begin() + end, ' ');
                previousEnd = end;
            }
            need(bytes.find('`') == std::string::npos, "other directive");
            std::set<fs::path> ancestors;
            auto current = id;
            while (true) {
                need(ancestors.insert(d.sm.getFullPath(current)).second, "recursive include graph");
                auto parent = parents.find(current);
                if (parent == parents.end())
                    break;
                current = parent->second;
            }
        }
        std::multiset<fs::path> actualRoots, expectedRoots(roots.begin(), roots.end());
        for (auto id : buffers)
            if (!parents.contains(id))
                actualRoots.insert(d.sm.getFullPath(id));
        need(actualRoots == expectedRoots, "include graph root mismatch");

        auto graph = document->getAnalysisInputs(ctx);
        need(graph.tree == tree && graph.buffer == occurrence && graph.trees.size() == 1,
             "nonunique owner graph");
    }
    ctx.checkpoint("method certificate graph ready");
    auto analysis = document->getAnalysis(ctx);
    auto& compilation = *analysis->getCompilation();
    if (!bodyQueries)
        need(compilation.getAllDiagnostics().empty(), "base semantic diagnostics");
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
    if (bodyQueries) {
        auto contexts = analysis->getSubroutines(*selected);
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
            functionBegin = selected->getFirstToken().location().offset();
            functionEnd = selected->getLastToken().range().end().offset();
            bodyBegin = selected->semi.range().end().offset();
            bodyEnd = selected->end.location().offset();
            originalBodyEnd = bodyEnd;
            originalBuffer = occurrence.getId();
            currentBuffer = document->getBuffer().getId();
            version = *document->getVersion();
            text = plain(*document);
            // Removing an existing directive or state-changing macro is just as
            // unsafe as introducing one: the borrowed suffix used its old state.
            validateBodyPreprocessing(text.substr(functionBegin, functionEnd - functionBegin));
            size_t checkedMacros = 0;
            for (const auto& reference : tree->getPreprocessorMetadata().macroRefs) {
                if ((checkedMacros++ & 1023) == 0)
                    ctx.checkpoint("original method preprocessing witness");
                auto at = d.sm.getFullyExpandedLoc(reference.syntax->getFirstToken().location());
                if (at && functionBegin <= at.offset() && at.offset() < functionEnd &&
                    d.sm.getFullPath(at.buffer()) == d.sm.getFullPath(occurrence))
                    validateMacroPreprocessing(reference);
            }
            functionName = bodyOriginals.front()->name;
            functionPath = bodyOriginals.front()->getHierarchicalPath();
            bodyBlocked = bodyOriginals;
            bodyBufferLimit = uint32_t(d.sm.getSourceLocationEntryCount());
            // Macro expansions after this method can use __LINE__, including through
            // nested macros or token pasting. Conservatively retain their enclosing
            // methods as dependencies; declaration / signature expansions need a full
            // update whenever the method's line count changes.
            for (const auto& [usage, expansion] : analysis->syntaxes.macroExpansions) {
                auto location = d.sm.getFullyExpandedLoc(usage->getFirstToken().location());
                if (d.sm.getFullPath(location.buffer()) != d.sm.getFullPath(occurrence) ||
                    location.offset() < originalBodyEnd)
                    continue;
                const syntax::FunctionDeclarationSyntax* enclosing = nullptr;
                for (auto node = usage; node; node = node->parent)
                    if (auto fn = node->as_if<syntax::FunctionDeclarationSyntax>()) {
                        enclosing = fn;
                        break;
                    }
                if (enclosing && location.offset() >= enclosing->semi.range().end().offset() &&
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
                else
                    bodyLineSensitiveDeclaration = true;
            }
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
    if (bodyQueries) {
        // Query certificates do not replay diagnostics or serialize semantic
        // owners. Native diagnostics retain the current UVM / RTL owners.
        auto input = std::make_shared<diagnostic_snapshot::Input>();
        std::vector<BufferID> buffers;
        for (const auto& syntaxTree : compilation.getSyntaxTrees()) {
            const auto& pp = syntaxTree->getPreprocessorMetadata();
            buffers.insert(buffers.end(), pp.sourceBufferIds.begin(), pp.sourceBufferIds.end());
        }
        input->sources = d.sm.captureSnapshot(buffers);
        frozen.owner = d.m_snapshotOwner;
        d.completions.requireCompletionEpoch();
        frozen.revision = d.completions.getCompletionEpoch();
        frozen.indexRevision = nextBodyCertificate.fetch_add(1, std::memory_order_relaxed);
        frozen.input = std::move(input);
    }
    else {
        auto saved = d.m_pendingIncludedDiagnostics;
        d.m_pendingIncludedDiagnostics.emplace(uri, d.m_diagnosticGeneration);
        try {
            auto value = d.captureDiagnosticSnapshot(ctx);
            need(bool(value), "diagnostic freeze unavailable");
            frozen = std::move(*value);
            d.m_pendingIncludedDiagnostics = std::move(saved);
        }
        catch (...) {
            d.m_pendingIncludedDiagnostics = std::move(saved);
            throw;
        }
    }
    if (!bodyQueries)
        need(frozen.metadata.trees.size() == 1, "diagnostic graph is not a single unit");
    phases.mark("body-prepare-snapshot");
    ctx.checkpoint("method certificate snapshot ready");
    for (auto& [openUri, doc] : d.docs)
        if (doc->getVersion() && d.isDocumentOpen(openUri)) {
            need(bodyQueries || openUri == uri, "phase one requires only target header open");
            documents.emplace(openUri, DocumentWitness{doc.get(), doc->getBuffer().getId(),
                                                       *doc->getVersion(), plain(*doc)});
        }
    // Explicit immutable libraries keep their existing manual-rebuild contract.
    // Re-reading those sources on every local query defeats semantic reuse.
    // Hold weak generation identities rather than another live semantic graph.
    std::unordered_set<std::string> immutableFiles;
    if (bodyQueries) {
        immutableUvm = d.m_trustedUvm;
        immutableScopes = d.m_trustedScopes;
        for (const auto& library : {d.m_trustedUvm, d.m_trustedScopes})
            if (library)
                immutableFiles.insert(library->files.begin(), library->files.end());
        need(!immutableFiles.contains(d.sm.getFullPath(occurrence).string()),
             "edited method belongs to an immutable library");
    }
    // A path can appear as a source, lookup and context watch input. Never let
    // a later read replace a witness already checked against the parsed bytes.
    auto witness = [&](const std::string& path) -> const std::optional<std::string>& {
        auto bytes = read(path);
        auto [it, inserted] = disk.try_emplace(path, bytes);
        need(inserted || it->second == bytes, "input changed during certification");
        return it->second;
    };
    for (auto& file : frozen.input->sources.files) {
        if (immutableFiles.contains(file.fullPath))
            continue;
        if (bodyQueries && file.generated)
            continue; // Immutable generated bytes are retained by the snapshot.
        need(!file.generated, "generated input");
        const auto& bytes = witness(file.fullPath);
        if (URI::fromFile(file.fullPath) != uri) {
            need(bool(bytes), "missing source witness");
            auto source = *bytes;
            if (source.empty() || source.back() != '\0')
                source.push_back('\0');
            need(source == file.bytes, "source changed before certification");
        }
    }
    ctx.checkpoint("method certificate sources ready");
    for (auto& lookup : frozen.input->sources.lookups) {
        if (immutableFiles.contains(lookup.path))
            continue;
        need(lookup.fileId || lookup.errorValue == int(std::errc::no_such_file_or_directory),
             "unsupported lookup failure");
        need(bool(witness(lookup.path)) == bool(lookup.fileId), "lookup changed");
    }
    for (auto& [path, _] : d.driver.getCommandFileMetadata())
        witness(path.string());
    for (auto& path : d.getContextWatchPaths()) {
        if (immutableFiles.contains(path.string()))
            continue;
        if (bodyQueries && fs::is_directory(path))
            directories.emplace(path.string(), directoryEntries(path));
        else
            witness(path.string());
    }
    for (auto path : {fs::path(d.m_workspacePathPrefix) / ".slang/server.json",
                      fs::path(d.m_workspacePathPrefix) / ".slang/local/server.json"})
        witness(path.string());
    configKey = rfl::json::write(config);
    optionsKey = rfl::json::write(diagnostic_snapshot::captureOptions(
        bodyQueries ? Bag{d.options.getOrDefault<ast::CompilationOptions>()} : d.options));
    inputRevision = d.getContextInputRevision();
    indexRevision = d.getDependencyIndexRevision();
    if (bodyQueries) {
        // Only the cwd is needed after the witnesses / value DTOs are built.
        // Do not keep a second project-sized source snapshot for query reuse.
        auto compact = std::make_shared<diagnostic_snapshot::Input>();
        compact->sources.cwd = frozen.input->sources.cwd;
        frozen.input = std::move(compact);
    }
    // General-body completion tickets carry an epoch and exact editor version;
    // they never consult the legacy buffer-id list. Keep that list bounded to
    // the independently capped legacy overlay instead of growing per keystroke.
    if (!generalBody) {
        handles.reserve(bodyQueries ? 66 : 10);
        handles.push_back(originalBuffer);
        handles.push_back(currentBuffer);
    }
    phases.mark("body-prepare-witnesses");
    captureLocalBaseline(config, ctx);
    ctx.checkpoint("after method certificate");
}

MethodScopeOverlay::~MethodScopeOverlay() {
    bodyDiagnostics.reset();
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
            if (original->bodyHasExternalDependents.load(std::memory_order_relaxed))
                return false;
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
            if (it == documents.end())
                return false;
            const auto& w = it->second;
            if (document.get() != w.document || document->getBuffer().getId() != w.buffer ||
                *document->getVersion() != w.version || plain(*document) != w.text ||
                !driver->sm.isLatestData(document->getBuffer())) {
                return false;
            }
        }
    if (count != documents.size())
        return false;
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
bool MethodScopeOverlay::validForWork(const Config& config, const lsp::RequestContext& ctx) const {
    return optimizedCertification(config) ? validContext(config, ctx) : valid(config, ctx);
}
bool MethodScopeOverlay::valid(const Config& config, const lsp::RequestContext& ctx) const try {
    if (!validContext(config, ctx))
        return false;
    UvmPhaseTrace phases;
    for (const auto& [path, entries] : directories) {
        ctx.checkpoint("method directory validation");
        if (directoryEntries(path) != entries)
            return false;
    }
    phases.mark("body-validate-directories");
    for (auto& [path, bytes] : disk) {
        ctx.checkpoint("method input validation");
        if (read(path) != bytes)
            return false;
    }
    phases.mark("body-validate-files");
    ctx.checkpoint("method inputs certified");
    return true;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return false;
}

bool MethodScopeOverlay::apply(const lsp::DidChangeTextDocumentParams& p, const Config& config,
                               const lsp::RequestContext& ctx, bool& synced, bool deferBinding) {
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
            commitGeneralText(p, std::move(next), nextBodyEnd, nextFunctionEnd, ctx, synced);
            return true;
        }
        const bool changedLines = std::count(text.begin(), text.begin() + functionEnd, '\n') !=
                                  std::count(next.begin(), next.begin() + nextFunctionEnd, '\n');
        if (config.experimentalMethodBodyDiagnostics.value() && !bodyDiagnostics && changedLines &&
            (bodyLineSensitiveDeclaration || !bodyLineDependents.empty())) {
            diagnosticBaselineVersion = version;
            diagnosticDecline = "line shift affects diagnostics in later macro bodies";
        }
        else if (!optimizedCertification(config))
            prepareLocalDiagnostics(config, ctx);
        return generalApply(p, config, ctx, synced, std::move(next), nextBodyEnd, nextFunctionEnd);
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
    synced = true;
    driver->onDocDidChange(p, ctx);
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
    return true;
}

bool MethodScopeOverlay::generalApply(const lsp::DidChangeTextDocumentParams& p,
                                      const Config& config, const lsp::RequestContext& ctx,
                                      bool& synced, std::string next, size_t nextBodyEnd,
                                      size_t nextFunctionEnd, bool bindOnly) {
    UvmPhaseTrace phases;
    auto methodText = next.substr(functionBegin, nextFunctionEnd - functionBegin);
    validateBodyPreprocessing(methodText);
    for (auto original : bodyOriginals)
        need(!original->bodyHasExternalDependents.load(std::memory_order_relaxed),
             "method has constant dependencies");
    // Preserve byte positions and line layout without reparsing any outer declaration.
    std::string padded = next.substr(0, functionBegin);
    for (auto& c : padded)
        if (c != '\n' && c != '\r')
            c = ' ';
    padded += methodText;
    auto source = driver->sm.assignQueryText(
        driver->sm.getFullPath(driver->getDocument(uri)->getBuffer()).string(), padded);
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
    const bool shiftedLines = driver->sm.getRawLineNumber(SourceLocation(source.id, nextBodyEnd)) !=
                              driver->sm.getRawLineNumber(
                                  SourceLocation(BufferID(originalBuffer, "original body lines"),
                                                 originalBodyEnd));
    if (shiftedLines) {
        need(!bodyLineSensitiveDeclaration, "body shifts line-dependent enclosing declarations");
        for (auto method : bodyLineDependents) {
            need(!method->bodyHasExternalDependents.load(std::memory_order_relaxed),
                 "body shifts a method with external semantic dependencies");
            blocked.push_back(method);
        }
    }
    phases.mark("body-apply-text");
    ast::BodyQueryEvaluationGuard guard(blocked);
    ctx.checkpoint("before method mini parse");
    auto tree = syntax::SyntaxTree::fromBuffer(source, driver->sm, treeOptions->bag(), bodyMacros);
    for (const auto& reference : tree->getPreprocessorMetadata().macroRefs)
        validateMacroPreprocessing(reference);
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
    need(resultingMacros.size() == bodyMacros.size(), "body changes macro environment");
    for (size_t i = 0; i < resultingMacros.size(); ++i)
        need(resultingMacros[i] == bodyMacros[i] ||
                 (resultingMacros[i]->name.valueText() == bodyMacros[i]->name.valueText() &&
                  resultingMacros[i]->toString() == bodyMacros[i]->toString()),
             "body changes macro environment");
    auto analysis = std::make_shared<ShallowAnalysis>(source.id, unit, ctx);
    phases.mark("body-apply-index");
    need(bindOnly && optimizedCertification(config) ? validContext(config, ctx)
                                                    : valid(config, ctx),
         "inputs changed before body publication");
    phases.mark("body-apply-revalidate");
    if (!bindOnly)
        commitGeneralText(p, std::move(next), nextBodyEnd, nextFunctionEnd, ctx, synced);
    miniAnalysis = std::move(analysis);
    miniTree = std::move(tree);
    bodyBlocked = std::move(blocked);
    return true;
}

void MethodScopeOverlay::commitGeneralText(const lsp::DidChangeTextDocumentParams& p,
                                           std::string next, size_t nextBodyEnd,
                                           size_t nextFunctionEnd, const lsp::RequestContext& ctx,
                                           bool& synced) {
    auto witnessText = next;
    ctx.checkpoint("before method publication");
    synced = true;
    driver->onDocDidChange(p, ctx);
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
        bool unused = false;
        generalApply({}, config, ctx, unused, text, bodyEnd, functionEnd, true);
    }
}

bool MethodScopeOverlay::generalSite(const lsp::Position& position) const {
    auto at = toSourceLocation(driver->getDocument(uri)->getBuffer(), position, driver->sm);
    return at && bodyBegin <= at->offset() && at->offset() < bodyEnd;
}

const MethodScopeOverlay::Site* MethodScopeOverlay::site(const lsp::Position& position) const {
    auto at = toSourceLocation(driver->getDocument(uri)->getBuffer(), position, driver->sm);
    if (!at)
        return nullptr;
    for (auto& value : sites)
        if (value.begin <= at->offset() && at->offset() <= value.end)
            return &value;
    return nullptr;
}
lsp::Range MethodScopeOverlay::range(size_t begin, size_t end) const {
    auto id = driver->getDocument(uri)->getBuffer();
    return {toPosition(SourceLocation(id, begin), driver->sm),
            toPosition(SourceLocation(id, end), driver->sm)};
}
lsp::CompletionItem MethodScopeOverlay::rebase(lsp::CompletionItem item, bool methodOwned) const {
    if (item.data) {
        auto data = rfl::from_generic<Data, rfl::UnderlyingEnums>(*item.data);
        need(bool(data), "completion source identity");
        need(!data->receiverBufferId, "receiver handles unsupported");
        if (data->bufferId == originalBuffer) {
            if (bodyQueries && !methodOwned && data->offset >= originalBodyEnd)
                data->offset = bodyEnd + (data->offset - originalBodyEnd);
            data->bufferId = currentBuffer;
        }
        item.data = rfl::to_generic<rfl::UnderlyingEnums>(*data);
    }
    return item;
}
const MethodScopeOverlay::DTO& MethodScopeOverlay::completion(size_t context, size_t index) const {
    const auto& slots = methodSlots[context];
    auto slot = std::lower_bound(slots.begin(), slots.end(), index);
    if (!methodValues[context].empty() && slot != slots.end() && *slot == index)
        return methodValues[context][size_t(slot - slots.begin())];
    return lexical[context][index];
}
std::optional<std::vector<lsp::CompletionItem>> MethodScopeOverlay::complete(
    const lsp::CompletionParams& p, const Config& config, const lsp::RequestContext& ctx) try {
    auto result = completeImpl(p, config, ctx);
    if (result && optimizedCertification(config)) {
        ctx.checkpoint("before method complete certification");
        if (!valid(config, ctx))
            return {};
        ctx.checkpoint("after method complete certification");
    }
    return result;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<std::vector<lsp::LocationLink>> MethodScopeOverlay::define(
    const lsp::DefinitionParams& p, const Config& config, const lsp::RequestContext& ctx) try {
    auto result = defineImpl(p, config, ctx);
    if (result && optimizedCertification(config)) {
        ctx.checkpoint("before method define certification");
        if (!valid(config, ctx))
            return {};
        ctx.checkpoint("after method define certification");
    }
    return result;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<std::optional<lsp::Hover>> MethodScopeOverlay::hover(
    const URI& target, const lsp::Position& position, const Config& config,
    const lsp::RequestContext& ctx, std::optional<Config::HoverConfig::MacroView> view) try {
    auto result = hoverImpl(target, position, config, ctx, view);
    if (result && optimizedCertification(config)) {
        ctx.checkpoint("before method hover certification");
        if (!valid(config, ctx))
            return {};
        ctx.checkpoint("after method hover certification");
    }
    return result;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<lsp::CompletionItem> MethodScopeOverlay::resolve(const lsp::CompletionItem& item,
                                                               const Config& config,
                                                               const lsp::RequestContext& ctx) try {
    auto result = resolveImpl(item, config, ctx);
    if (result && optimizedCertification(config)) {
        ctx.checkpoint("before method resolve certification");
        if (!valid(config, ctx))
            return {};
        ctx.checkpoint("after method resolve certification");
    }
    return result;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<std::vector<lsp::CompletionItem>> MethodScopeOverlay::completeImpl(
    const lsp::CompletionParams& p, const Config& config, const lsp::RequestContext& ctx) try {
    if (generalBody) {
        if (!accepted || p.textDocument.uri != uri || !validForWork(config, ctx) ||
            !generalSite(p.position))
            return {};
        ensureGeneralBinding(config, ctx);
        ast::BodyQueryEvaluationGuard guard(bodyBlocked);
        BodySourceProjection projection(driver->sm, driver->getDocument(uri)->getBuffer(),
                                        miniAnalysis->getBuffer(), originalBodyEnd, bodyEnd,
                                        bodyBufferLimit);
        auto doc = driver->getDocument(uri);
        auto location = toSourceLocation(miniAnalysis->getBuffer(), p.position, driver->sm);
        if (!location)
            return {};
        auto context = CompletionContext::fromLocation(*doc, *location,
                                                       p.context.value_or(lsp::CompletionContext{}),
                                                       ctx, miniAnalysis);
        if (context.query->isIncomplete())
            return {};
        std::vector<lsp::CompletionItem> out;
        driver->completions.getCompletions(out, doc, context);
        for (auto& item : out)
            if (item.data && item.data->to_object()) {
                auto native = rfl::from_generic<NativePayload>(*item.data);
                if (native)
                    item.data = rfl::to_generic(QueryPayload{frozen.revision,
                                                             std::move(native->payload),
                                                             frozen.indexRevision, version});
            }
        return out;
    }
    if (!accepted || p.textDocument.uri != uri || !validForWork(config, ctx) ||
        (p.context && p.context->triggerKind == lsp::CompletionTriggerKind::TriggerCharacter)) {
        return {};
    }
    auto* where = site(p.position);
    if (!where) {
        return {};
    }
    auto edit = completions::MemberCompletionQuery::createLexical(range(where->begin, where->end),
                                                                  false, false);
    std::vector<lsp::CompletionItem> out;
    for (size_t i = 0; i < lexical[where->context].size(); ++i) {
        const auto& dto = completion(where->context, i);
        if (dto.item.documentation && dto.changedDocumentation) {
            return {};
        }
        auto item = rebase(dto.item, dto.methodOwned);
        // CompletionDispatch supplies the requesting document for items whose
        // producer has no symbol handle (including indexed package/module items).
        if (item.kind == lsp::CompletionItemKind::Module)
            item.data = uri.str();
        edit->setCompletionEdit(item);
        if (bodyQueries && item.data && item.data->to_object())
            item.data = rfl::to_generic(QueryPayload{frozen.revision, std::move(*item.data),
                                                     frozen.indexRevision, version});
        out.push_back(std::move(item));
    }
    ctx.checkpoint("before method completion response");
    return out;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<std::vector<lsp::LocationLink>> MethodScopeOverlay::defineImpl(
    const lsp::DefinitionParams& p, const Config& config, const lsp::RequestContext& ctx) try {
    if (!accepted || p.textDocument.uri != uri || !validForWork(config, ctx))
        return {};
    if (generalBody) {
        if (!generalSite(p.position))
            return {};
        ensureGeneralBinding(config, ctx);
        ast::BodyQueryEvaluationGuard guard(bodyBlocked);
        BodySourceProjection projection(driver->sm, driver->getDocument(uri)->getBuffer(),
                                        miniAnalysis->getBuffer(), originalBodyEnd, bodyEnd,
                                        bodyBufferLimit);
        auto info = driver->getDefinitionInfoAt(uri, p.position, ctx, miniAnalysis);
        return info ? info->getDefinitionLspLinks() : std::vector<lsp::LocationLink>{};
    }
    auto* where = site(p.position);
    if (!where)
        return {};
    auto at = toSourceLocation(driver->getDocument(uri)->getBuffer(), p.position, driver->sm);
    if (at->offset() == where->end && !where->definitionAtEnd)
        return std::vector<lsp::LocationLink>{};
    auto target = range(where->declarationBegin, where->declarationEnd);
    ctx.checkpoint("before method definition response");
    return std::vector<lsp::LocationLink>{
        {.targetUri = uri, .targetRange = target, .targetSelectionRange = target}};
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<std::optional<lsp::Hover>> MethodScopeOverlay::hoverImpl(
    const URI& target, const lsp::Position& position, const Config& config,
    const lsp::RequestContext& ctx, std::optional<Config::HoverConfig::MacroView> view) try {
    if (!generalBody || !accepted || target != uri || !validForWork(config, ctx) ||
        !generalSite(position))
        return {};
    ensureGeneralBinding(config, ctx);
    ast::BodyQueryEvaluationGuard guard(bodyBlocked);
    BodySourceProjection projection(driver->sm, driver->getDocument(uri)->getBuffer(),
                                    miniAnalysis->getBuffer(), originalBodyEnd, bodyEnd,
                                    bodyBufferLimit);
    auto result = driver->getDocHover(uri, position, ctx, view, miniAnalysis);
    ctx.checkpoint("before method hover response");
    return std::optional<std::optional<lsp::Hover>>(std::in_place, std::move(result));
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<lsp::CompletionItem> MethodScopeOverlay::resolveImpl(
    const lsp::CompletionItem& item, const Config& config, const lsp::RequestContext& ctx) try {
    if (!accepted || !validForWork(config, ctx))
        return {};
    if (item.data) {
        auto payload = *item.data;
        if (bodyQueries && payload.to_object()) {
            auto owner = rfl::from_generic<QueryPayload>(payload);
            if (!owner)
                return {}; // Untagged handles belong to the native dispatcher.
            if (owner->contextEpoch != frozen.revision ||
                (owner->methodBodyCertificate &&
                 (*owner->methodBodyCertificate != frozen.indexRevision ||
                  owner->methodBodyVersion != version))) {
                auto out = item;
                out.data.reset();
                return out;
            }
            payload = std::move(owner->payload);
        }
        if (generalBody) {
            if (!rfl::from_generic<QueryPayload>(*item.data))
                return {};
            ensureGeneralBinding(config, ctx);
            // A containing class / outer routine's documentation includes this edited
            // body. Resolve that rare enclosing declaration through the current native
            // view rather than print its retained old syntax.
            auto data = rfl::from_generic<Data, rfl::UnderlyingEnums>(payload);
            if (data && data->documentUri == uri.str()) {
                auto position = driver->sm.getFullyExpandedLoc(SourceLocation(
                    BufferID(data->bufferId, "body completion declaration"), data->offset));
                if (driver->sm.getFullPath(position.buffer()) ==
                    driver->sm.getFullPath(driver->getDocument(uri)->getBuffer())) {
                    bool enclosing = false;
                    for (auto original : bodyOriginals)
                        for (auto parent = original->getParentScope(); parent;
                             parent = parent->asSymbol().getParentScope()) {
                            const auto& symbol = parent->asSymbol();
                            if (!symbol.name.empty() && symbol.name == data->symbolName &&
                                driver->sm.getFullyExpandedLoc(symbol.location) == position)
                                enclosing = true;
                        }
                    if (enclosing) {
                        auto native = driver->getDocument(uri)->getAnalysis(ctx);
                        data->bufferId = native->getBuffer().getId();
                        data->offset = position.offset();
                        if (data->receiverBufferId)
                            data->receiverBufferId = native->getBuffer().getId();
                        auto out = item;
                        out.data = rfl::to_generic<rfl::UnderlyingEnums>(*data);
                        completions::MemberCompletionQuery::resolve(driver->completions, out, ctx,
                                                                    native);
                        return out;
                    }
                }
            }
            ast::BodyQueryEvaluationGuard guard(bodyBlocked);
            BodySourceProjection projection(driver->sm, driver->getDocument(uri)->getBuffer(),
                                            miniAnalysis->getBuffer(), originalBodyEnd, bodyEnd,
                                            bodyBufferLimit);
            auto out = item;
            out.data = std::move(payload);
            completions::MemberCompletionQuery::resolve(driver->completions, out, ctx,
                                                        miniAnalysis);
            return out;
        }
        auto data = rfl::from_generic<Data, rfl::UnderlyingEnums>(payload);
        if (!data || data->documentUri != uri.str())
            return {};
        // Automatic locals/formals from any earlier generation never bind a new declaration.
        if (data->bufferId != currentBuffer && data->symbolPath.starts_with(functionPath + ".") &&
            (data->symbolKind == ast::SymbolKind::Variable ||
             data->symbolKind == ast::SymbolKind::FormalArgument)) {
            auto out = item;
            out.data.reset();
            return out;
        }
        // Old automatic locals are rejected above. Variable-length edits also
        // move unchanged declarations after the method: compare current handles
        // against projected DTOs instead of treating offsets as generation-free.
        if (bodyQueries && data->bufferId != currentBuffer &&
            std::find(handles.begin(), handles.end(), data->bufferId) != handles.end())
            return {};
        if (!bodyQueries &&
            std::find(handles.begin(), handles.end(), data->bufferId) != handles.end())
            data->bufferId = originalBuffer;
        for (size_t kind = 0; kind < lexical.size(); ++kind)
            for (size_t i = 0; i < lexical[kind].size(); ++i) {
                const auto& dto = completion(kind, i);
                if (dto.item.data) {
                    auto projected = bodyQueries ? rebase(dto.item, dto.methodOwned) : dto.item;
                    auto expected = rfl::from_generic<Data, rfl::UnderlyingEnums>(*projected.data);
                    if (expected && *data == *expected && item.label == dto.item.label &&
                        item.kind == dto.item.kind) {
                        if (dto.changedDocumentation) {
                            if (!bodyQueries)
                                return {};
                            auto out = item;
                            out.data = rfl::to_generic(NativePayload{frozen.revision, payload});
                            driver->completions.getCompletionItemResolve(out, ctx);
                            return out;
                        }
                        auto out = item;
                        out.data.reset();
                        out.documentation = dto.documentation;
                        Formatting::updateCompletionEditText(out);
                        ctx.checkpoint("before method resolve response");
                        return out;
                    }
                }
            }
    }
    else
        for (size_t kind = 0; kind < lexical.size(); ++kind)
            for (size_t i = 0; i < lexical[kind].size(); ++i) {
                const auto& dto = completion(kind, i);
                if (!dto.item.data && item.label == dto.item.label && item.kind == dto.item.kind) {
                    if (dto.changedDocumentation)
                        return {};
                    auto out = item;
                    out.documentation = dto.documentation;
                    out.insertText = dto.insertText;
                    out.insertTextFormat = dto.insertTextFormat;
                    Formatting::updateCompletionEditText(out);
                    return out;
                }
            }
    return {};
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

void MethodScopeOverlay::captureLocalBaseline(const Config& config,
                                              const lsp::RequestContext& ctx) {
    if (!optimizedCertification(config) || !config.experimentalMethodBodyDiagnostics.value() ||
        bodyDiagnostics || diagnosticBaseline || diagnosticCaptureVersion == version)
        return;
    diagnosticCaptureVersion = version;
    try {
        validateBodyPreprocessing(text.substr(functionBegin, functionEnd - functionBegin));
        UvmPhaseTrace phases;
        diagnosticBaseline = BodyDiagnosticCache::capture(*driver, uri, functionBegin, functionEnd,
                                                          ctx);
        phases.mark("body-diagnostic-capture");
        diagnosticDecline.clear();
    }
    catch (const lsp::RequestCancelled&) {
        diagnosticCaptureVersion = -1;
        throw;
    }
    catch (const std::exception& error) {
        diagnosticDecline = error.what();
    }
}

void MethodScopeOverlay::prepareLocalDiagnostics(const Config& config,
                                                 const lsp::RequestContext& ctx) {
    if (!generalBody || !config.experimentalMethodBodyDiagnostics.value() || bodyDiagnostics ||
        diagnosticBaselineVersion == version)
        return;
    diagnosticBaselineVersion = version;
    try {
        UvmPhaseTrace phases;
        const auto baselineEnd = diagnosticBaseline ? diagnosticBaseline->end : functionEnd;
        if (diagnosticBaseline && (bodyLineSensitiveDeclaration || !bodyLineDependents.empty()) &&
            std::count(diagnosticBaseline->text.begin(),
                       diagnosticBaseline->text.begin() + baselineEnd,
                       '\n') != std::count(text.begin(), text.begin() + functionEnd, '\n')) {
            diagnosticDecline = "line shift affects diagnostics in later macro bodies";
            diagnosticBaseline.reset();
            return;
        }
        auto baselineText = diagnosticBaseline ? std::string_view(diagnosticBaseline->text)
                                               : std::string_view(text);
        validateBodyPreprocessing(
            std::string(baselineText.substr(functionBegin, baselineEnd - functionBegin)));
        std::string padded(baselineText.substr(0, functionBegin));
        for (auto& c : padded)
            if (c != '\n' && c != '\r')
                c = ' ';
        padded += baselineText.substr(functionBegin, baselineEnd - functionBegin);
        auto source = driver->sm.assignQueryText(
            driver->sm.getFullPath(driver->getDocument(uri)->getBuffer()).string(), padded);
        for (const auto& line :
             driver->sm.getLineDirectives(BufferID(originalBuffer, "body lines"))) {
            auto at = driver->sm.getSourceLocation(BufferID(originalBuffer, "body lines"),
                                                   line.lineInFile, 1);
            if (at && at->offset() < functionBegin)
                driver->sm.addLineDirective(SourceLocation(source.id, at->offset()),
                                            line.lineOfDirective, line.name, line.level, true);
        }
        auto directives = driver->sm.getDiagnosticDirectives(
            BufferID(originalBuffer, "body pragmas"));
        auto inherited = std::vector<SourceManager::DiagnosticDirectiveInfo>(directives.begin(),
                                                                             directives.end());
        for (const auto& directive : inherited)
            if (directive.offset < functionBegin)
                driver->sm.addDiagnosticDirective(SourceLocation(source.id, directive.offset),
                                                  directive.name, directive.severity, true);
        auto tree = syntax::SyntaxTree::fromBuffer(source, driver->sm, treeOptions->bag(),
                                                   bodyMacros);
        for (const auto& reference : tree->getPreprocessorMetadata().macroRefs)
            validateMacroPreprocessing(reference);
        bodyDiagnostics = std::make_unique<BodyDiagnosticCache>(
            *driver, uri, functionBegin, baselineEnd, tree,
            bodyLineSensitiveDeclaration || !bodyLineDependents.empty(), ctx, diagnosticBaseline,
            optimizedCertification(config) ? miniTree : nullptr);
        diagnosticBaseline.reset();
        phases.mark("body-diagnostic-certification");
        diagnosticDecline.clear();
    }
    catch (const lsp::RequestCancelled&) {
        diagnosticBaselineVersion = -1;
        throw;
    }
    catch (const std::exception& error) {
        diagnosticDecline = error.what();
        diagnosticBaseline.reset();
    }
}

uint64_t MethodScopeOverlay::localDiagnosticUpdates() const {
    return diagnosticUpdates;
}
std::string MethodScopeOverlay::localDiagnosticDecline() const {
    return diagnosticDecline;
}

bool MethodScopeOverlay::publishLocalDiagnostics(const Config& config,
                                                 const lsp::RequestContext& ctx) {
    if (!generalBody || !accepted || !config.experimentalMethodBodyDiagnostics.value() ||
        (!bodyDiagnostics && !optimizedCertification(config)) || !validForWork(config, ctx))
        return false;
    try {
        ensureGeneralBinding(config, ctx);
        if (optimizedCertification(config) && !bodyDiagnostics)
            prepareLocalDiagnostics(config, ctx);
        if (!bodyDiagnostics)
            return false;
        bodyDiagnostics->publish(miniTree, functionEnd, ctx, [&] {
            need(valid(config, ctx), "inputs changed during local diagnostics");
        });
        ++diagnosticUpdates;
        diagnosticDecline.clear();
        if (driver->client.capabilities.inactiveRegionsSupported) {
            BodySourceProjection projection(driver->sm, driver->getDocument(uri)->getBuffer(),
                                            miniAnalysis->getBuffer(), originalBodyEnd, bodyEnd,
                                            bodyBufferLimit);
            std::vector<lsp::Range> regions;
            for (auto region : bodyOwner->syntaxes.disabledRegions) {
                auto start = driver->sm.getFullyExpandedLoc(region.start());
                if (bodyBegin <= start.offset() && start.offset() < originalBodyEnd)
                    continue;
                regions.push_back(toRange(region, driver->sm));
            }
            for (auto region : miniAnalysis->syntaxes.disabledRegions)
                regions.push_back(toRange(region, driver->sm));
            std::sort(regions.begin(), regions.end(), [](const auto& a, const auto& b) {
                return std::tie(a.start.line, a.start.character) <
                       std::tie(b.start.line, b.start.character);
            });
            driver->client.onTextDocumentInactiveRegions(
                {.uri = uri, .regions = std::move(regions)});
        }
        return true;
    }
    catch (const lsp::RequestCancelled&) {
        throw;
    }
    catch (const std::exception& error) {
        diagnosticDecline = error.what();
        bodyDiagnostics.reset();
        diagnosticBaseline.reset();
        return false;
    }
}

std::optional<ServerDriver::DiagnosticSnapshot> MethodScopeOverlay::diagnostics(
    const Config& config, const lsp::RequestContext& ctx) const {
    if (bodyQueries || !accepted || !valid(config, ctx))
        return {};
    auto out = frozen;
    out.owner = driver->m_snapshotOwner;
    out.revision = driver->m_snapshotRevision;
    out.indexRevision = driver->getDependencyIndexRevision();
    out.diagnosticGeneration = driver->m_diagnosticGeneration;
    auto input = std::make_shared<diagnostic_snapshot::Input>(*frozen.input);
    size_t changed = 0;
    auto document = driver->getDocument(uri);
    auto path = driver->sm.getFullPath(document->getBuffer()).string();
    for (auto& file : input->sources.files)
        if (file.fullPath == path) {
            file.bytes = std::string(document->getText());
            ++changed;
        }
    need(changed == 1, "diagnostic source identity");
    for (auto& id : out.buffers)
        if (driver->sm.getFullPath(id).string() == path)
            id = document->getBuffer();
    for (auto& target : out.metadata.targets)
        target.version = driver->docs.at(target.uri)->getVersion();
    out.input = std::move(input);
    ctx.checkpoint("after method diagnostic freeze");
    return out;
}
} // namespace server
