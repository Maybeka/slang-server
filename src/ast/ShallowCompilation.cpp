//------------------------------------------------------------------------------
// ShallowCompilation.cpp
// Compilation state shared by a source file and its included-file analyses
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "ast/ShallowCompilation.h"

#include "ast/ServerCompilation.h"
#include "ast/TrustedUvmLibrary.h"
#include "util/SlangExtensions.h"

#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/diagnostics/AnalysisDiags.h"

namespace server {
using namespace slang;

ShallowCompilation::ShallowCompilation(
    SourceManager& sourceManager, std::shared_ptr<syntax::SyntaxTree> tree, const Bag& options,
    const std::vector<std::shared_ptr<syntax::SyntaxTree>>& dependencies,
    const ServerCompilation* design, const lsp::RequestContext& ctx, bool packageOnly,
    BufferID sourceBuffer) :
    m_sourceManager(sourceManager), m_tree(std::move(tree)), m_allTrees{m_tree},
    m_analysisOptions(options.getOrDefault<analysis::AnalysisOptions>()) {
    if (auto setting = options.get<TrustedUvmImport>())
        m_sourceOwnerLease = setting->sourceOwner;
    for (const auto& dependency : dependencies)
        if (std::find(m_allTrees.begin(), m_allTrees.end(), dependency) == m_allTrees.end())
            m_allTrees.push_back(dependency);

    if (auto precompiled = options.get<syntax::PrecompiledUvmSyntax>();
        precompiled && precompiled->tree && !options.get<TrustedUvmImport>()) {
        bool containsLibrary = false;
        for (const auto& tree : m_allTrees)
            for (const auto& [node, _] : tree->getMetadata().nodeMeta)
                containsLibrary |= node->kind == syntax::SyntaxKind::PackageDeclaration &&
                                   node->header->name.valueText() == "uvm_pkg";
        if (!containsLibrary)
            m_allTrees.push_back(precompiled->tree);
    }

    if (auto setting = options.get<TrustedUvmImport>();
        setting && setting->library && setting->enabled &&
        m_tree == setting->library->compilation->getSyntaxTrees().front()) {
        // A package can use its immutable semantic owner directly. A module
        // needs its selected current instance, including current driver context.
        std::vector<BufferID> enclosing;
        if (sourceBuffer.valid()) {
            enclosing.push_back(sourceBuffer);
            for (auto location = sourceManager.getIncludedFrom(sourceBuffer); location;
                 location = sourceManager.getIncludedFrom(location.buffer()))
                enclosing.push_back(location.buffer());
        }
        std::vector<std::string_view> names;
        for (const auto& name : setting->library->definitions) {
            auto definition = setting->library->compilation
                                  ->tryGetDefinition(name, setting->library->compilation->getRoot())
                                  .definition;
            if (definition &&
                (!sourceBuffer.valid() ||
                 std::ranges::find(
                     enclosing, sourceManager.getFullyExpandedLoc(definition->location).buffer()) !=
                     enclosing.end()))
                names.push_back(name);
        }
        if (names.empty()) {
            m_compilation = setting->library->compilation;
            return;
        }
        if (design) {
            auto active = design->createActiveDesignContext(names);
            for (auto name : names) {
                if (auto instance = active.getBoundInstance(name)) {
                    // Native shallow overrides omit invalid arguments. Keep
                    // that private default view while the consumer is broken,
                    // rather than expose its invalid instantiated parameter map.
                    const bool invalid =
                        std::ranges::any_of(instance->body.getParameters(), [](auto parameter) {
                            const auto& symbol = parameter->symbol;
                            if (auto value = symbol.template as_if<ast::ParameterSymbol>())
                                return value->getValue().bad();
                            if (auto type = symbol.template as_if<ast::TypeParameterSymbol>())
                                return type->targetType.getType().isError();
                            return false;
                        });
                    if (invalid)
                        break;
                    m_sourceInstances.push_back(instance);
                }
            }
            if (m_sourceInstances.size() == names.size()) {
                m_activeDesign = std::move(active);
                m_compilation = m_activeDesign->retainCompilation();
                m_editedDefinitionsElaborated = true;
                return;
            }
            m_sourceInstances.clear();
        }
        // Uninstantiated definitions still get a private default elaboration;
        // never add default bodies or overrides to the shared library owner.
    }

    auto cOptions = options.getOrDefault<ast::CompilationOptions>();
    cOptions.flags |= ast::CompilationFlags::AllowTopLevelIfacePorts;
    cOptions.flags |= ast::CompilationFlags::CheckUninstantiated;
    cOptions.flags |= ast::CompilationFlags::AllowInvalidTop;
    // Check the edited module and two levels of instantiated children.
    cOptions.maxInstanceDepth = 3;
    cOptions.topModules.clear();

    if (packageOnly) {
        cOptions.flags |= ast::CompilationFlags::PackageAnalysis;
        cOptions.flags &= ~ast::CompilationFlags::CheckUninstantiated;
    }
    ctx.checkpoint("before compilation construction");
    m_compilation = std::make_shared<ast::Compilation>(cOptions);
    const bool trusted = importTrustedUvm(*m_compilation, sourceManager, options, m_allTrees);
    ctx.checkpoint("after compilation construction");
    for (auto& depTree : m_allTrees) {
        ctx.checkpoint("before adding syntax tree");
        m_compilation->addSyntaxTree(depTree, trusted);
        ctx.checkpoint("after adding syntax tree");
    }

    if (design) {
        std::vector<std::string_view> definitionNames;
        for (auto* symbol : m_compilation->getDefinitions()) {
            if (auto* definition = symbol->as_if<ast::DefinitionSymbol>())
                definitionNames.push_back(definition->name);
        }
        m_activeDesign = design->createActiveDesignContext(definitionNames);
        m_activeDesign->applyOverrides(*m_compilation);
    }
}

ShallowCompilation::ShallowCompilation(SourceManager& sm, std::shared_ptr<syntax::SyntaxTree> tree,
                                       std::shared_ptr<ShallowCompilation> owner,
                                       std::span<const ast::SubroutineSymbol* const> originals) :
    m_bodyQueryOwner(std::move(owner)), m_sourceManager(sm), m_tree(std::move(tree)) {
    m_compilation = std::make_shared<ast::Compilation>(
        Bag{m_bodyQueryOwner->getCompilation()->getOptions()});
    m_compilation->addSyntaxTree(m_tree);
    m_compilation->setBodyQueryContext(m_bodyQueryOwner->getCompilation());
    const syntax::FunctionDeclarationSyntax* syntax = nullptr;
    auto find = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
        if (auto fn = node.as_if<syntax::FunctionDeclarationSyntax>()) {
            if (syntax)
                throw std::runtime_error("multiple query bodies");
            syntax = fn;
            return;
        }
        for (uint32_t i = 0; i < node.getChildCount(); ++i)
            if (auto child = node.childNode(i))
                self(self, *child);
    };
    find(find, m_tree->root());
    if (!syntax)
        throw std::runtime_error("missing query body");
    for (auto original : originals)
        m_bodyQueryRoots.push_back(
            &ast::SubroutineSymbol::createBodyQuery(*m_compilation, *syntax, *original));
}

bool ShallowCompilation::hasValidBuffers() const {
    for (auto& tree : m_allTrees) {
        if (!server::hasValidBuffers(m_sourceManager, tree))
            return false;
    }
    return true;
}

const Diagnostics& ShallowCompilation::getSemanticDiagnostics() {
    if (m_bodyQueryOwner)
        throw std::logic_error("body query diagnostics require current native compilation");
    if (!m_editedDefinitionsElaborated &&
        !m_compilation->hasFlag(ast::CompilationFlags::PackageAnalysis)) {
        auto& root = m_compilation->getRoot();
        for (auto* symbol : m_compilation->getDefinitions()) {
            auto* definition = symbol->as_if<ast::DefinitionSymbol>();
            if (!definition || definition->syntaxTree != m_tree.get())
                continue;

            auto& instance = ast::InstanceSymbol::createDefault(*m_compilation, *definition);
            instance.setParent(root);
            m_compilation->forceElaborate(instance.body);
        }
        m_editedDefinitionsElaborated = true;
    }

    const auto& diagnostics = m_compilation->getSemanticDiagnostics();
    // The full design owns cache admission. Private query views clear tops
    // and cap instance depth; publishing them can evict the active DUT graph
    // with a different parameter / option variant during read-only queries.
    if (!m_activeDesign)
        m_compilation->offerInstanceBodies(m_compilation);
    return diagnostics;
}

Diagnostics ShallowCompilation::getAnalysisDiags() {
    getAnalysisManager();
    if (!m_cachedAnalysisDiags)
        return {};
    return *m_cachedAnalysisDiags;
}

const analysis::AnalysisManager* ShallowCompilation::getAnalysisManager() {
    if (m_bodyQueryOwner)
        return nullptr;
    if (!m_sourceInstances.empty()) {
        auto& manager = m_activeDesign->getAnalysis().getAnalysisManager();
        if (!m_cachedAnalysisDiags)
            m_cachedAnalysisDiags = manager.getDiagnostics().filter(
                {diag::UnusedDefinition, diag::UnusedPackageParameter,
                 diag::UnusedPackageSubroutine, diag::UnusedPackageTypedef,
                 diag::UnusedPackageVar});
        return &manager;
    }
    if (m_driverAnalysis)
        return m_driverAnalysis.get();

    (void)getSemanticDiagnostics();
    if (m_compilation->getRoot().topInstances.empty()) {
        m_cachedAnalysisDiags = Diagnostics{};
        return nullptr;
    }

    auto manager = std::make_unique<analysis::AnalysisManager>(m_analysisOptions);
    m_compilation->freeze();
    manager->analyze(*m_compilation);
    m_compilation->unfreeze();

    // Shallow analysis does not have all references to definitions and package members.
    m_cachedAnalysisDiags = manager->getDiagnostics().filter(
        {diag::UnusedDefinition, diag::UnusedPackageParameter, diag::UnusedPackageSubroutine,
         diag::UnusedPackageTypedef, diag::UnusedPackageVar});
    m_driverAnalysis = std::move(manager);
    return m_driverAnalysis.get();
}

std::vector<const analysis::ValueDriver*> ShallowCompilation::getDrivers(
    const ast::ValueSymbol& symbol) {
    if (m_bodyQueryOwner) {
        // Native procedure analysis does not register automatic variables as
        // value drivers. Fields are selected through their aggregate root and
        // parameters cannot be driven. All other values require a current
        // design-wide driver graph, which the private body view cannot supply.
        if (auto variable = symbol.as_if<ast::VariableSymbol>();
            variable && variable->lifetime == ast::VariableLifetime::Automatic &&
            !symbol.isConnectedToRefPort())
            return {};
        if (symbol.kind == ast::SymbolKind::Field || ast::ParameterSymbol::isKind(symbol.kind))
            return {};
        throw std::logic_error("body hover requires current native driver information");
    }
    if (!m_sourceInstances.empty())
        return m_activeDesign->getAnalysis().getDrivers(symbol);
    if (m_activeDesign) {
        auto* designSymbol = getDesignSymbol(symbol);
        if (auto* designValue = designSymbol ? designSymbol->as_if<ast::ValueSymbol>() : nullptr)
            return m_activeDesign->getAnalysis().getDrivers(*designValue);
    }

    auto* manager = getAnalysisManager();
    if (!manager)
        return {};
    if (!m_analysisQueries)
        m_analysisQueries = std::make_unique<analysis::AnalysisQueries>(*m_compilation, *manager);
    return m_analysisQueries->getDrivers(symbol);
}

} // namespace server
