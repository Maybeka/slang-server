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
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/diagnostics/AnalysisDiags.h"

namespace server {
using namespace slang;

ShallowCompilation::ShallowCompilation(
    SourceManager& sourceManager, std::shared_ptr<syntax::SyntaxTree> tree, const Bag& options,
    const std::vector<std::shared_ptr<syntax::SyntaxTree>>& dependencies,
    const ServerCompilation* design, const lsp::RequestContext& ctx, bool packageOnly) :
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
        setting && setting->library &&
        m_tree == setting->library->compilation->getSyntaxTrees().front()) {
        // Navigation within a fixed library indexes the prepared semantic owner;
        // no second UVM compilation is constructed merely to open its source.
        m_compilation = setting->library->compilation;
        return;
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
    const bool trusted = importTrustedUvm(*m_compilation, sourceManager, options);
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

bool ShallowCompilation::hasValidBuffers() const {
    for (auto& tree : m_allTrees) {
        if (!server::hasValidBuffers(m_sourceManager, tree))
            return false;
    }
    return true;
}

const Diagnostics& ShallowCompilation::getSemanticDiagnostics() {
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

    return m_compilation->getSemanticDiagnostics();
}

Diagnostics ShallowCompilation::getAnalysisDiags() {
    getAnalysisManager();
    if (!m_cachedAnalysisDiags)
        return {};
    return *m_cachedAnalysisDiags;
}

const analysis::AnalysisManager* ShallowCompilation::getAnalysisManager() {
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
