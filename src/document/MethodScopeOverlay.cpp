// SPDX-License-Identifier: MIT
#include "document/MethodScopeOverlay.h"

#include "completions/InstanceCompletions.h"
#include "completions/MemberCompletions.h"
#include "util/Converters.h"
#include <cctype>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/driver/SourceLoader.h"
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
    need(!error && fs::is_regular_file(status), "nonregular certificate input");
    need(fs::canonical(path) == fs::absolute(path).lexically_normal(), "symlink input unsupported");
    std::ifstream input(path, std::ios::binary);
    need(bool(input), "unreadable certificate input");
    std::string bytes{std::istreambuf_iterator<char>(input), {}};
    need(!input.bad(), "input read failed");
    return bytes;
}

MethodScopeOverlay::MethodScopeOverlay(ServerDriver& d, const Config& config, const URI& target,
                                       size_t editOffset, const lsp::RequestContext& ctx) :
    driver(&d), uri(target) {
    ctx.checkpoint("method certificate begin");
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
        return !value.empty() && value.find_first_of("*?[$`@{}\"'\\") == std::string_view::npos &&
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
                    recipe.push_back(fs::absolute(path.parent_path() / token).lexically_normal());
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
    need(roots.size() == recipe.size() && std::equal(roots.begin(), roots.end(), recipe.begin()),
         "ordered recipe mismatch");
    auto tree = unit->getSyntaxTree(ctx);
    auto& pp = tree->getPreprocessorMetadata();
    need(pp.sourceBufferIds.size() == roots.size() + pp.includeDirectives.size() &&
             pp.macroRefs.empty() && pp.predefineBufferIds.empty() && !tree->getSourceLibrary() &&
             !tree->isLibraryUnit,
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
    BufferID occurrence;
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
    auto document = d.getDocument(uri);
    auto graph = document->getAnalysisInputs(ctx);
    need(graph.tree == tree && graph.buffer == occurrence && graph.trees.size() == 1,
         "nonunique owner graph");
    ctx.checkpoint("method certificate graph ready");
    auto analysis = document->getAnalysis(ctx);
    auto& compilation = *analysis->getCompilation();
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
    find(find, tree->root());
    need(selected, "edit outside function");
    auto* declaration = selected->parent
                            ? selected->parent->as_if<syntax::ClassMethodDeclarationSyntax>()
                            : nullptr;
    auto* cls = declaration && declaration->parent
                    ? declaration->parent->as_if<syntax::ClassDeclarationSyntax>()
                    : nullptr;
    auto* package = cls && cls->parent ? cls->parent->as_if<syntax::ModuleDeclarationSyntax>()
                                       : nullptr;
    need(cls && package && package->kind == syntax::SyntaxKind::PackageDeclaration &&
             !cls->parameters && !cls->extendsClause && !cls->implementsClause &&
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
        need(p && p->attributes.empty() && p->direction.kind == parsing::TokenKind::InputKeyword &&
                 !p->constKeyword && !p->staticKeyword && !p->varKeyword && plainInt(p->dataType) &&
                 p->declarator->dimensions.empty() && !p->declarator->initializer,
             "plain input int");
    }
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
                     value->attributes.empty() && value->modifiers.empty() && plainInt(value->type),
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
    for (const ast::Scope* scope = fn.getParentScope(); scope;
         scope = scope->asSymbol().getParentScope())
        for (auto& member : scope->members())
            if (&member != &fn && !member.name.empty())
                outerNames.emplace_back(member.name);
    auto body = shape(*selected);
    bodyBegin = body.begin;
    bodyEnd = body.end;
    localCount = body.locals;
    verify(fn, outerNames);
    functionBegin = selected->getFirstToken().location().offset();
    functionEnd = selected->getLastToken().range().end().offset();
    text = plain(*document);
    need(std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 128; }),
         "ASCII header only");
    need(text.substr(functionBegin, functionEnd - functionBegin).find('\\') == std::string::npos,
         "escaped method tokens unsupported");
    originalBuffer = occurrence.getId();
    currentBuffer = document->getBuffer().getId();
    version = *document->getVersion();
    ctx.checkpoint("method certificate method ready");
    // Completion order still comes from the native queries below. Index only
    // identity resolution, once, instead of scanning every visible member for
    // each item. Pointers live only during this constructor; the retained
    // interface consists of value DTOs. Outer matches overwrite inner matches
    // in the same traversal order as the previous per-item search.
    using Identity = std::tuple<ast::SymbolKind, std::string, std::string>;
    std::map<Identity, const ast::Symbol*> symbols;
    for (const ast::Scope* scope = &fn; scope; scope = scope->asSymbol().getParentScope()) {
        ctx.checkpoint("method certificate completion scope");
        for (auto& member : scope->members())
            if (!member.name.empty())
                symbols.insert_or_assign(Identity{member.kind, std::string(member.name),
                                                  member.getHierarchicalPath()},
                                         &member);
    }
    for (size_t kind = 0; kind < 2; ++kind) {
        std::vector<lsp::CompletionItem> items;
        CompletionContext context;
        context.kind = kind ? CompletionContextKind::Expression : CompletionContextKind::Procedural;
        completions::InstanceCompletionQuery::addCompletions(items, d.m_indexer, context);
        Formatting::addCompletions(items, &fn, context.kind, &fn, uri.str(), false, false);
        for (auto& item : items) {
            DTO dto{item, item};
            if (item.data) {
                auto data = rfl::from_generic<Data, rfl::UnderlyingEnums>(*item.data);
                need(bool(data), "completion identity");
                auto symbol = symbols.find(
                    Identity{data->symbolKind, data->symbolName, data->symbolPath});
                need(symbol != symbols.end(), "unaccounted completion symbol");
                const auto* found = symbol->second;
                dto.methodOwned = found->getParentScope() == &fn;
                dto.resolved.data.reset();
                completions::MemberCompletionQuery::resolve(*found, dto.resolved, false);
                if (!dto.methodOwned && found->getSyntax()) {
                    auto span = found->getSyntax()->sourceRange();
                    dto.changedDocumentation = span.start().buffer() == occurrence &&
                                               span.start().offset() < bodyEnd &&
                                               span.end().offset() > bodyBegin;
                }
            }
            else if (item.kind == lsp::CompletionItemKind::Module) {
                completions::InstanceCompletionQuery::resolve(*tree, item.label, dto.resolved);
                need(bool(dto.resolved.documentation), "global module outside frozen tree");
            }
            lexical[kind].push_back(std::move(dto));
        }
    }
    ctx.checkpoint("method certificate completions ready");
    treeOptions = std::make_unique<diagnostic_snapshot::ReconstructedOptions>(
        diagnostic_snapshot::captureOptions(tree->options()));
    compilationOptions = std::make_unique<diagnostic_snapshot::ReconstructedOptions>(
        diagnostic_snapshot::captureOptions(compilation.getOptions()));
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
    need(frozen.metadata.trees.size() == 1, "diagnostic graph is not a single unit");
    ctx.checkpoint("method certificate snapshot ready");
    for (auto& [openUri, doc] : d.docs)
        if (doc->getVersion() && d.isDocumentOpen(openUri)) {
            need(openUri == uri, "phase one requires only target header open");
            documents.emplace(openUri, DocumentWitness{doc.get(), doc->getBuffer().getId(),
                                                       *doc->getVersion(), plain(*doc)});
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
        need(lookup.fileId || lookup.errorValue == int(std::errc::no_such_file_or_directory),
             "unsupported lookup failure");
        need(bool(witness(lookup.path)) == bool(lookup.fileId), "lookup changed");
    }
    for (auto& [path, _] : d.driver.getCommandFileMetadata())
        witness(path.string());
    for (auto& path : d.getContextWatchPaths())
        witness(path.string());
    for (auto path : {fs::path(d.m_workspacePathPrefix) / ".slang/server.json",
                      fs::path(d.m_workspacePathPrefix) / ".slang/local/server.json"})
        witness(path.string());
    configKey = rfl::json::write(config);
    optionsKey = rfl::json::write(diagnostic_snapshot::captureOptions(d.options));
    inputRevision = d.getContextInputRevision();
    indexRevision = d.getDependencyIndexRevision();
    handles.reserve(10);
    handles.push_back(originalBuffer);
    handles.push_back(currentBuffer);
    ctx.checkpoint("after method certificate");
}

MethodScopeOverlay::~MethodScopeOverlay() {
    miniAnalysis.reset();
    miniTree.reset();
    miniManager.reset();
}
bool MethodScopeOverlay::belongsTo(const ServerDriver& d) const {
    return driver == &d && frozen.owner == d.m_snapshotOwner;
}
bool MethodScopeOverlay::valid(const Config& config, const lsp::RequestContext& ctx) const try {
    if (!belongsTo(*driver) || driver->getContextInputRevision() != inputRevision ||
        driver->getDependencyIndexRevision() != indexRevision ||
        fs::current_path().string() != frozen.input->sources.cwd ||
        rfl::json::write(config) != configKey ||
        rfl::json::write(diagnostic_snapshot::captureOptions(driver->options)) != optionsKey)
        return false;
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
                !driver->sm.isLatestData(document->getBuffer()))
                return false;
        }
    if (count != documents.size())
        return false;
    for (auto& [path, bytes] : disk) {
        ctx.checkpoint("method input validation");
        if (read(path) != bytes)
            return false;
    }
    return true;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return false;
}

bool MethodScopeOverlay::apply(const lsp::DidChangeTextDocumentParams& p, const Config& config,
                               const lsp::RequestContext& ctx, bool& synced) {
    need(valid(config, ctx) && p.textDocument.uri == uri && p.textDocument.version > version &&
             accepted < 8 && p.contentChanges.size() == 1,
         "method context/version/bound changed");
    std::string next = text;
    if (auto* whole = rfl::get_if<lsp::TextDocumentContentChangeWholeDocument>(
            &p.contentChanges[0]))
        next = whole->text;
    else {
        auto& edit = rfl::get<lsp::TextDocumentContentChangePartial>(p.contentChanges[0]);
        auto document = driver->getDocument(uri);
        auto begin = toSourceLocation(document->getBuffer(), edit.range.start, driver->sm);
        auto end = toSourceLocation(document->getBuffer(), edit.range.end, driver->sm);
        need(begin && end && begin->offset() <= end->offset() && end->offset() <= text.size(),
             "invalid range");
        need(!edit.rangeLength || *edit.rangeLength == end->offset() - begin->offset(),
             "range length mismatch");
        next.replace(begin->offset(), end->offset() - begin->offset(), edit.text);
    }
    need(next.size() == text.size() && next != text, "phase one requires equal-length edits");
    need(next.substr(0, bodyBegin) == text.substr(0, bodyBegin) &&
             next.substr(bodyEnd) == text.substr(bodyEnd),
         "edit crosses body/interface boundary");
    for (size_t i = 0; i < text.size(); ++i)
        need(uint8_t(next[i]) < 128 && (next[i] == '\n') == (text[i] == '\n') &&
                 (next[i] == '\r') == (text[i] == '\r'),
             "ASCII and fixed line layout required");
    need(next.find('`') == std::string::npos, "directives unsupported");
    need(next.substr(functionBegin, functionEnd - functionBegin).find('\\') == std::string::npos,
         "escaped method tokens unsupported");
    auto manager = std::make_unique<SourceManager>();
    auto prefix = "package " + packageName + "; class " + className + "; ";
    auto source = manager->assignText(
        prefix + next.substr(functionBegin, functionEnd - functionBegin) + " endclass endpackage");
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
    verify(fn, outerNames);
    need(compilation.getAllDiagnostics().empty(), "method semantic diagnostics");
    std::array<std::vector<DTO>, 2> fresh = lexical;
    for (size_t kind = 0; kind < 2; ++kind) {
        std::vector<lsp::CompletionItem> items;
        Formatting::addCompletions(items, &fn,
                                   kind ? CompletionContextKind::Expression
                                        : CompletionContextKind::Procedural,
                                   &fn, uri.str(), false, false);
        std::vector<DTO> owned;
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
            DTO dto{item, item, true, false};
            dto.resolved.data.reset();
            completions::MemberCompletionQuery::resolve(*symbol, dto.resolved, false);
            if (data->offset >= prefix.size() &&
                data->offset < prefix.size() + functionEnd - functionBegin)
                data->offset = functionBegin + data->offset - prefix.size();
            else {
                // `this` denotes the unchanged containing class, outside the method slice.
                auto it =
                    std::find_if(lexical[kind].begin(), lexical[kind].end(), [&](const DTO& old) {
                        if (!old.methodOwned || !old.item.data)
                            return false;
                        auto oldData = rfl::from_generic<Data, rfl::UnderlyingEnums>(
                            *old.item.data);
                        return oldData && oldData->symbolName == data->symbolName &&
                               oldData->symbolPath == data->symbolPath &&
                               oldData->symbolKind == data->symbolKind;
                    });
                need(it != lexical[kind].end(), "mini source outside certified method");
                data->offset =
                    rfl::from_generic<Data, rfl::UnderlyingEnums>(*it->item.data)->offset;
            }
            data->bufferId = originalBuffer;
            dto.item.data = rfl::to_generic<rfl::UnderlyingEnums>(*data);
            owned.push_back(std::move(dto));
        }
        size_t index = 0;
        for (auto& dto : fresh[kind])
            if (dto.methodOwned) {
                need(index < owned.size(), "method member slot mismatch");
                dto = std::move(owned[index++]);
            }
        need(index == owned.size(), "method member coverage mismatch");
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
            need(offset >= prefix.size() && offset <= prefix.size() + functionEnd - functionBegin,
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
    lexical = std::move(fresh);
    sites = std::move(freshSites);
    text = std::move(next);
    version = p.textDocument.version;
    currentBuffer = document->getBuffer().getId();
    handles.push_back(currentBuffer);
    documents.at(uri) = DocumentWitness{document.get(), currentBuffer, version,
                                        std::move(witnessText)};
    inputRevision = driver->getContextInputRevision();
    ++accepted;
    return true;
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
lsp::CompletionItem MethodScopeOverlay::rebase(lsp::CompletionItem item) const {
    if (item.data) {
        auto data = rfl::from_generic<Data, rfl::UnderlyingEnums>(*item.data);
        need(bool(data), "completion source identity");
        need(!data->receiverBufferId, "receiver handles unsupported");
        if (data->bufferId == originalBuffer)
            data->bufferId = currentBuffer;
        item.data = rfl::to_generic<rfl::UnderlyingEnums>(*data);
    }
    return item;
}
std::optional<std::vector<lsp::CompletionItem>> MethodScopeOverlay::complete(
    const lsp::CompletionParams& p, const Config& config, const lsp::RequestContext& ctx) const {
    if (!accepted || p.textDocument.uri != uri || !valid(config, ctx) ||
        (p.context && p.context->triggerKind == lsp::CompletionTriggerKind::TriggerCharacter))
        return {};
    auto* where = site(p.position);
    if (!where)
        return {};
    auto edit = completions::MemberCompletionQuery::createLexical(range(where->begin, where->end),
                                                                  false, false);
    std::vector<lsp::CompletionItem> out;
    for (auto& dto : lexical[where->context]) {
        if (dto.item.documentation && dto.changedDocumentation)
            return {};
        auto item = rebase(dto.item);
        // CompletionDispatch supplies the requesting document for items whose
        // producer has no symbol handle (including indexed package/module items).
        if (item.kind == lsp::CompletionItemKind::Module)
            item.data = uri.str();
        edit->setCompletionEdit(item);
        out.push_back(std::move(item));
    }
    ctx.checkpoint("before method completion response");
    return out;
}
std::optional<std::vector<lsp::LocationLink>> MethodScopeOverlay::define(
    const lsp::DefinitionParams& p, const Config& config, const lsp::RequestContext& ctx) const {
    if (!accepted || p.textDocument.uri != uri || !valid(config, ctx))
        return {};
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
std::optional<lsp::CompletionItem> MethodScopeOverlay::resolve(
    const lsp::CompletionItem& item, const Config& config, const lsp::RequestContext& ctx) const {
    if (!accepted || !valid(config, ctx))
        return {};
    if (item.data) {
        auto data = rfl::from_generic<Data, rfl::UnderlyingEnums>(*item.data);
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
        // Header aliases are generation-specific; unchanged outer sources keep
        // their exact frozen identity and must match the complete DTO below.
        if (std::find(handles.begin(), handles.end(), data->bufferId) != handles.end())
            data->bufferId = originalBuffer;
        for (auto& list : lexical)
            for (auto& dto : list)
                if (dto.item.data) {
                    auto expected = rfl::from_generic<Data, rfl::UnderlyingEnums>(*dto.item.data);
                    if (expected && *data == *expected && item.label == dto.item.label &&
                        item.kind == dto.item.kind) {
                        if (dto.changedDocumentation)
                            return {};
                        auto out = item;
                        out.data.reset();
                        out.documentation = dto.resolved.documentation;
                        Formatting::updateCompletionEditText(out);
                        ctx.checkpoint("before method resolve response");
                        return out;
                    }
                }
    }
    else
        for (auto& list : lexical)
            for (auto& dto : list)
                if (!dto.item.data && item.label == dto.item.label && item.kind == dto.item.kind) {
                    if (dto.changedDocumentation)
                        return {};
                    auto out = item;
                    out.documentation = dto.resolved.documentation;
                    out.insertText = dto.resolved.insertText;
                    out.insertTextFormat = dto.resolved.insertTextFormat;
                    Formatting::updateCompletionEditText(out);
                    return out;
                }
    return {};
}
std::optional<ServerDriver::DiagnosticSnapshot> MethodScopeOverlay::diagnostics(
    const Config& config, const lsp::RequestContext& ctx) const {
    if (!accepted || !valid(config, ctx))
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
