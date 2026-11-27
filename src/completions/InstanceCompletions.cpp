//------------------------------------------------------------------------------
// InstanceCompletions.cpp
// Module, interface, package, and class completions.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "completions/InstanceCompletions.h"

#include "Indexer.h"
#include "document/ShallowAnalysis.h"
#include "document/SlangDoc.h"
#include "lsp/SnippetString.h"
#include "util/Converters.h"
#include "util/Formatting.h"
#include "util/Logging.h"
#include <fmt/format.h>
#include <span>
#include <unordered_set>

#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/parsing/Token.h"
#include "slang/parsing/TokenKind.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxVisitor.h"

namespace server::completions {
using namespace slang;

namespace {

class PortVisitor : public syntax::SyntaxVisitor<PortVisitor> {
public:
    std::vector<std::string_view> names;
    size_t maxLen = 0;
    bool keepNonAnsiSlots = false;

    void handle(const syntax::DeclaratorSyntax& port) {
        names.push_back(port.name.valueText());
        maxLen = std::max(maxLen, port.name.valueText().length());
    }

    void handle(const syntax::ExplicitNonAnsiPortSyntax& portDecl) {
        names.push_back(portDecl.name.valueText());
        maxLen = std::max(maxLen, portDecl.name.valueText().length());
    }

    void handle(const syntax::ImplicitNonAnsiPortSyntax& port) {
        if (!keepNonAnsiSlots)
            return;
        auto* reference = port.expr->as_if<syntax::PortReferenceSyntax>();
        names.push_back(reference ? reference->name.valueText() : std::string_view{});
        maxLen = std::max(maxLen, names.back().size());
    }

    void handle(const syntax::EmptyNonAnsiPortSyntax&) {
        if (keepNonAnsiSlots)
            names.emplace_back();
    }
};

void collectParams(const syntax::ParameterPortListSyntax& paramList,
                   std::vector<std::string_view>& names, std::vector<std::string>& defaults,
                   size_t& maxLen) {
    bool lastLocal = false;
    for (auto declaration : paramList.declarations) {
        if (declaration->keyword)
            lastLocal = declaration->keyword.kind == parsing::TokenKind::LocalParamKeyword;
        if (lastLocal)
            continue;

        if (declaration->kind == syntax::SyntaxKind::ParameterDeclaration) {
            auto& paramSyntax = declaration->as<syntax::ParameterDeclarationSyntax>();
            for (auto decl : paramSyntax.declarators) {
                names.push_back(decl->name.valueText());
                std::string defaultVal = decl->initializer ? decl->initializer->expr->toString()
                                                           : "";
                ltrim(defaultVal);
                defaults.push_back(std::move(defaultVal));
                maxLen = std::max(maxLen, decl->name.valueText().length());
            }
        }
        else {
            auto& paramSyntax = declaration->as<syntax::TypeParameterDeclarationSyntax>();
            for (auto decl : paramSyntax.declarators) {
                names.push_back(decl->name.valueText());
                std::string defaultVal = decl->assignment ? decl->assignment->type->toString() : "";
                ltrim(defaultVal);
                defaults.push_back(std::move(defaultVal));
                maxLen = std::max(maxLen, decl->name.valueText().length());
            }
        }
    }
}

class InstanceCompletionQueryImpl final : public InstanceCompletionQuery {
public:
    InstanceCompletionQueryImpl(lsp::Range replacementRange, const parsing::Token* moduleToken) :
        InstanceCompletionQuery(std::move(replacementRange)), moduleToken(moduleToken) {}

    CompletionQueryKind kind() const final { return CompletionQueryKind::InstantiationSuffix; }

    void getCompletions(std::vector<lsp::CompletionItem>& results, CompletionDispatch& dispatch,
                        const std::shared_ptr<SlangDoc>& doc,
                        const CompletionContext&) const final {
        if (!moduleToken) {
            WARN("No module token found before instantiation suffix");
            return;
        }

        auto name = moduleToken->valueText();
        auto symbolLoc = getIndexer(dispatch).getNearestSymbolLoc(name, doc->getURI().getPath());
        if (!symbolLoc) {
            ERROR("No module found for {}", name);
            return;
        }

        auto completion = getCompletion(std::string(name), symbolLoc->kind);
        resolve(dispatch, completion, *symbolLoc->uri, true);
        results.push_back(std::move(completion));
    }

private:
    const parsing::Token* moduleToken;
};

template<typename Named, typename List>
void addNamedItems(std::vector<lsp::CompletionItem>& results, const List& existing,
                   const syntax::SyntaxNode& site, std::span<const std::string_view> names,
                   std::span<const std::string> details = {}) {
    std::unordered_set<std::string_view> used;
    size_t position = 0;
    for (auto* argument : existing) {
        if (argument == &site)
            continue;
        if (auto* named = argument->template as_if<Named>()) {
            used.insert(named->name.valueText());
        }
        else if (argument->kind != syntax::SyntaxKind::WildcardPortConnection &&
                 position < names.size()) {
            used.insert(names[position++]);
        }
    }
    for (size_t index = 0; index < names.size(); ++index) {
        auto name = names[index];
        if (name.empty() || used.contains(name))
            continue;
        SnippetString snippet;
        snippet.appendText(name);
        snippet.appendText("(");
        snippet.appendPlaceholder(name);
        snippet.appendText(")");
        results.push_back({
            .label = std::string(name),
            .labelDetails = details.empty() ? std::nullopt
                                             : std::optional(lsp::CompletionItemLabelDetails{
                                                   .detail = " " + details[index]}),
            .kind = lsp::CompletionItemKind::Field,
            .insertText = std::string(snippet.getValue()),
            .insertTextFormat = lsp::InsertTextFormat::Snippet,
        });
    }
}

class NamedConnectionQuery final : public InstanceCompletionQuery {
public:
    NamedConnectionQuery(lsp::Range replacementRange, const syntax::SyntaxNode& site,
                         bool followedByCall) :
        InstanceCompletionQuery(std::move(replacementRange), followedByCall), site(site) {}

    CompletionQueryKind kind() const final { return CompletionQueryKind::NamedConnection; }

    void getCompletions(std::vector<lsp::CompletionItem>& results, CompletionDispatch&,
                        const std::shared_ptr<SlangDoc>&,
                        const CompletionContext& context) const final {
        const auto* parent = site.parent.get();
        if (!parent || !parent->parent)
            return;
        if (auto* arguments = parent->as_if<syntax::ArgumentListSyntax>()) {
            const auto* method = context.analysis->getArgumentSubroutine(*arguments);
            if (!method)
                return;
            std::vector<std::string_view> names;
            std::vector<std::string> details;
            for (auto* formal : method->getArguments()) {
                names.push_back(formal->name);
                details.push_back(portString(formal->direction) + " " +
                                  getTypeString(formal->getType()));
            }
            addNamedItems<syntax::NamedArgumentSyntax>(results, arguments->parameters, site, names,
                                                     details);
            return;
        }

        const auto* instantiation = parent->parent->as_if<syntax::HierarchyInstantiationSyntax>();
        if (!instantiation)
            return;
        auto& compilation = *context.analysis->getCompilation();
        const auto& scope = context.scope ? *context.scope
                                          : static_cast<const ast::Scope&>(compilation.getRoot());
        const auto* symbol = compilation.tryGetDefinition(instantiation->type.valueText(), scope)
                                 .definition;
        const auto* definition = symbol ? symbol->as_if<ast::DefinitionSymbol>() : nullptr;
        if (!definition || !definition->getSyntax())
            return;
        const auto* module = definition->getSyntax()->as_if<syntax::ModuleDeclarationSyntax>();
        if (!module)
            return;
        if (auto* instance = parent->as_if<syntax::HierarchicalInstanceSyntax>();
            instance && module->header->ports) {
            PortVisitor visitor;
            visitor.keepNonAnsiSlots = true;
            module->header->ports->visit(visitor);
            addNamedItems<syntax::NamedPortConnectionSyntax>(
                results, instance->connections, site, visitor.names);
        }
        else if (auto* parameters = parent->as_if<syntax::ParameterValueAssignmentSyntax>()) {
            std::vector<std::string_view> names;
            std::vector<std::string> details;
            for (auto& parameter : definition->parameters) {
                if (parameter.isLocalParam)
                    continue;
                names.push_back(parameter.name);
                std::string detail;
                if (parameter.isTypeParam)
                    detail = "type";
                else if (parameter.hasSyntax) {
                    detail = parameter.valueSyntax->type->toString();
                    ltrim(detail);
                }
                details.push_back(detail.empty() ? "parameter" : std::move(detail));
            }
            addNamedItems<syntax::NamedParamAssignmentSyntax>(
                results, parameters->parameters, site, names, details);
        }
    }

private:
    const syntax::SyntaxNode& site;
};

} // namespace

std::unique_ptr<CompletionQuery> InstanceCompletionQuery::create(
    lsp::Range replacementRange, const parsing::Token* moduleToken) {
    return std::make_unique<InstanceCompletionQueryImpl>(std::move(replacementRange), moduleToken);
}

std::unique_ptr<CompletionQuery> InstanceCompletionQuery::createNamed(
    lsp::Range replacementRange, const syntax::SyntaxNode& site, const parsing::Token& dot,
    bool followedByCall) {
    if (auto* port = site.as_if<syntax::NamedPortConnectionSyntax>(); port && &port->dot == &dot)
        return std::make_unique<NamedConnectionQuery>(replacementRange, site, followedByCall);
    if (auto* parameter = site.as_if<syntax::NamedParamAssignmentSyntax>();
        parameter && &parameter->dot == &dot)
        return std::make_unique<NamedConnectionQuery>(replacementRange, site, followedByCall);
    if (auto* argument = site.as_if<syntax::NamedArgumentSyntax>();
        argument && &argument->dot == &dot)
        return std::make_unique<NamedConnectionQuery>(replacementRange, site, followedByCall);
    return nullptr;
}

lsp::CompletionItem InstanceCompletionQuery::getCompletion(std::string name,
                                                           syntax::SyntaxKind kind) {
    std::string detail;
    switch (kind) {
        case syntax::SyntaxKind::ModuleDeclaration:
            detail = " Module";
            break;
        case syntax::SyntaxKind::InterfaceDeclaration:
            detail = " Interface";
            break;
        default:
            detail = std::string(toString(kind));
            break;
    }

    return lsp::CompletionItem{
        .label = name,
        .labelDetails =
            lsp::CompletionItemLabelDetails{
                .detail = detail,
            },
        .kind = lsp::CompletionItemKind::Module,
        .filterText = name,
    };
}

void InstanceCompletionQuery::addCompletions(std::vector<lsp::CompletionItem>& results,
                                             const Indexer& indexer,
                                             const CompletionContext& context) {
    std::unordered_set<std::string_view> seenNames;

    indexer.forEachSymbol([&](const std::string& name, const Indexer::GlobalSymbolLoc& entry) {
        if (!seenNames.insert(name).second)
            return;

        std::string detail;
        std::optional<std::string> insertText;
        switch (entry.kind) {
            case syntax::SyntaxKind::ModuleDeclaration:
                if (context.kind != CompletionContextKind::ModuleMember)
                    return;
                detail = " Module";
                break;
            case syntax::SyntaxKind::InterfaceDeclaration:
                detail = " Interface";
                if (context.kind != CompletionContextKind::ModuleMember)
                    insertText = name;
                break;
            case syntax::SyntaxKind::PackageDeclaration:
                detail = " Package";
                break;
            case syntax::SyntaxKind::ClassDeclaration:
                if (context.kind == CompletionContextKind::Expression)
                    return;
                detail = " Class";
                break;
            default:
                return;
        }
        results.push_back(lsp::CompletionItem{
            .label = name,
            .labelDetails =
                lsp::CompletionItemLabelDetails{
                    .detail = detail,
                },
            .kind = lsp::CompletionItemKind::Module,
            .filterText = name,
            .insertText = insertText,
        });
    });
}

void InstanceCompletionQuery::resolveModuleInstance(const syntax::ModuleHeaderSyntax& header,
                                                    lsp::CompletionItem& item, bool excludeName) {
    item.documentation = svCodeBlock(header);
    if (item.insertText)
        return;

    SnippetString output;
    size_t maxLen = 0;
    std::vector<std::string_view> names;
    std::vector<std::string> defaults;
    if (header.parameters)
        collectParams(*header.parameters, names, defaults, maxLen);

    if (!excludeName)
        output.appendText(header.name.valueText());

    if (!names.empty()) {
        if (!excludeName)
            output.appendText(" #");
        output.appendText("(\n");

        for (size_t i = 0; i < names.size(); ++i) {
            auto name = std::string(names[i]);
            auto nameFmt = name + std::string(maxLen - name.length(), ' ');
            output.appendText("\t." + nameFmt + "(");
            if (defaults[i].empty())
                output.appendPlaceholder(name);
            else
                output.appendPlaceholder(fmt::format("{} /* default {} */", name, defaults[i]));
            output.appendText(")");
            output.appendText(i < names.size() - 1 ? ",\n" : "\n ");
        }
        output.appendText(")");
    }

    output.appendText(" ");
    output.appendPlaceholder(toCamelCase(header.name.valueText()));
    output.appendText(" (");

    maxLen = 0;
    names.clear();
    if (header.ports) {
        PortVisitor visitor;
        header.ports->visit(visitor);
        names = std::move(visitor.names);
        maxLen = visitor.maxLen;
    }

    if (!names.empty()) {
        output.appendText("\n");
        for (size_t i = 0; i < names.size(); ++i) {
            auto name = std::string(names[i]);
            auto nameFmt = name + std::string(maxLen - name.length(), ' ');
            output.appendText("\t." + nameFmt + "(");
            output.appendPlaceholder(name);
            output.appendText(")");
            output.appendText(i < names.size() - 1 ? ",\n" : "\n");
        }
    }

    output.appendText(");");
    item.insertText = output.getValue();
    item.insertTextFormat = lsp::InsertTextFormat::Snippet;
}

void InstanceCompletionQuery::resolve(const syntax::SyntaxTree& tree, std::string_view moduleName,
                                      lsp::CompletionItem& item, bool excludeName) {
    for (auto [module, node] : tree.getMetadata().nodeMeta) {
        auto& header = *module->header;
        if (header.name.valueText() != moduleName)
            continue;

        switch (module->kind) {
            case syntax::SyntaxKind::InterfaceDeclaration:
            case syntax::SyntaxKind::ModuleDeclaration:
                resolveModuleInstance(header, item, excludeName);
                break;
            default:
                item.documentation = svCodeBlock(header);
                item.insertText = header.name.valueText();
                item.insertTextFormat = lsp::InsertTextFormat::PlainText;
                continue;
        }
        break;
    }
}

void InstanceCompletionQuery::resolve(CompletionDispatch& dispatch, lsp::CompletionItem& item,
                                      std::optional<std::filesystem::path> modulePath,
                                      bool excludeName) {
    auto name = item.label;
    if (!modulePath) {
        if (!item.data)
            return;
        auto documentUri = item.data->to_string();
        if (!documentUri)
            return;
        auto symbolLoc = getIndexer(dispatch).getNearestSymbolLoc(name,
                                                                  URI(*documentUri).getPath());
        if (!symbolLoc)
            return;
        modulePath = *symbolLoc->uri;
    }

    auto maybeTree = getIndexedSyntaxTree(dispatch, modulePath->string());
    if (!maybeTree) {
        WARN("Failed to load syntax tree for module {} from {}", name, modulePath->string());
        return;
    }

    resolve(*maybeTree, name, item, excludeName);
    item.data.reset();
    updateCompletionEditText(item);
}

} // namespace server::completions
