// SPDX-License-Identifier: MIT
#include "ServerDriver.h"
#include "ast/TrustedUvmLibrary.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <rfl/to_generic.hpp>

#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"

namespace server {
namespace {
struct PackageClosure {
    std::shared_ptr<ast::Compilation> compilation;
    std::shared_ptr<syntax::SyntaxTree> tree;
    std::vector<std::string> packages;
    std::set<std::string> files;
};

// Bootstrap only: observe native package queries, including suppressed errors.
// Looking at every A::B token would incorrectly freeze an unrelated package named like a local
// class. Each retry adds at least one uniquely declared package, so the number of
// bindings is bounded by the original unit's package declarations.
std::optional<PackageClosure> packageClosure(const std::shared_ptr<syntax::SyntaxTree>& owner,
                                             std::span<const std::string> seeds,
                                             ast::CompilationOptions options,
                                             std::shared_ptr<const void> sourceOwner) {
    if (owner->root().kind != syntax::SyntaxKind::CompilationUnit ||
        std::ranges::any_of(owner->diagnostics(), [](const auto& d) { return d.isError(); }))
        return std::nullopt;
    const auto& unit = owner->root().as<syntax::CompilationUnitSyntax>();
    std::map<std::string, std::vector<const syntax::MemberSyntax*>> candidates;
    for (auto member : unit.members) {
        if (member->kind == syntax::SyntaxKind::PackageImportDeclaration)
            return std::nullopt; // Compilation-unit import routing needs a separate boundary.
        if (member->kind == syntax::SyntaxKind::PackageDeclaration)
            candidates[std::string(
                           member->as<syntax::ModuleDeclarationSyntax>().header->name.valueText())]
                .push_back(member);
    }
    std::set<std::string> wanted(seeds.begin(), seeds.end());
    for (;;) {
        std::vector<const syntax::MemberSyntax*> selected;
        for (const auto& name : wanted) {
            auto found = candidates.find(name);
            if (found == candidates.end() || found->second.size() != 1)
                return std::nullopt;
            selected.push_back(found->second.front());
        }
        auto tree = syntax::SyntaxTree::selectCompilationUnitMembers(owner, selected);
        // A package-only owner cannot bind virtual interface definitions. Keep
        // their existing whole-prefix route without speculatively binding a
        // large package graph that must then be bound again on fallback.
        if (!tree->getMetadata().virtualInterfaceTypes.empty())
            return std::nullopt;
        auto compilation = std::make_shared<ast::Compilation>(options);
        compilation->retainSourceOwner(sourceOwner);
        compilation->beginTrackingMissingPackageLookups();
        compilation->addSyntaxTree(tree);
        const auto& diagnostics = compilation->getSemanticDiagnostics();
        bool added = false;
        for (const auto& name : compilation->endTrackingMissingPackageLookups()) {
            auto found = candidates.find(name);
            if (found == candidates.end())
                continue;
            if (found->second.size() != 1)
                return std::nullopt;
            added |= wanted.emplace(name).second;
        }
        if (added)
            continue;
        if (std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }))
            return std::nullopt;

        auto& sources = owner->sourceManager();
        auto rootPath = [&](SourceLocation location) {
            location = sources.getFullyExpandedLoc(location);
            while (auto parent = sources.getIncludedFrom(location.buffer()))
                location = sources.getFullyExpandedLoc(parent);
            return sources.getFullPath(location.buffer()).lexically_normal().generic_string();
        };
        std::set<std::string> roots, knownRoots;
        for (auto id : owner->getSourceBufferIds())
            if (!sources.getIncludedFrom(id))
                knownRoots.emplace(sources.getFullPath(id).lexically_normal().generic_string());
        for (auto member : selected) {
            roots.emplace(rootPath(member->getFirstToken().location()));
            roots.emplace(rootPath(member->getLastToken().location()));
            auto origins = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
                for (size_t i = 0; i < node.getChildCount(); ++i) {
                    if (auto child = node.childNode(i))
                        self(self, *child);
                    else if (auto token = node.childToken(i); token) {
                        auto path = rootPath(sources.getFullyOriginalLoc(token.location()));
                        if (knownRoots.contains(path))
                            roots.emplace(std::move(path));
                    }
                }
            };
            origins(origins, *member);
        }
        // Whole-file query routing remains valid only when these physical roots
        // contain no unselected siblings. Mixed files await location projection.
        for (auto member : unit.members) {
            if (member->kind == syntax::SyntaxKind::EmptyMember ||
                std::ranges::find(selected, member) != selected.end())
                continue;
            if (roots.contains(rootPath(member->getFirstToken().location())) ||
                roots.contains(rootPath(member->getLastToken().location())))
                return std::nullopt;
        }
        PackageClosure result{std::move(compilation), std::move(tree), {}, {}};
        for (auto member : result.tree->root().as<syntax::CompilationUnitSyntax>().members)
            result.packages.emplace_back(
                member->as<syntax::ModuleDeclarationSyntax>().header->name.valueText());
        for (auto id : owner->getSourceBufferIds())
            if (roots.contains(rootPath(SourceLocation(id, 0)))) {
                const auto& path = sources.getFullPath(id);
                if (!path.empty())
                    result.files.emplace(path.lexically_normal().generic_string());
            }
        return result;
    }
}
} // namespace

bool trustedLibraryNamespaceMatches(const TrustedUvmLibrary& library, const SourceManager& sources,
                                    std::span<const std::shared_ptr<syntax::SyntaxTree>> trees) {
    auto compatible = [&](const syntax::SyntaxNode& member) {
        const bool package = member.kind == syntax::SyntaxKind::PackageDeclaration;
        const bool definition = member.kind == syntax::SyntaxKind::ModuleDeclaration ||
                                member.kind == syntax::SyntaxKind::InterfaceDeclaration ||
                                member.kind == syntax::SyntaxKind::ProgramDeclaration;
        if (!package && !definition) {
            if ((library.definitions.empty() && !library.hasUnitDeclarations) ||
                member.kind == syntax::SyntaxKind::EmptyMember ||
                member.kind == syntax::SyntaxKind::BindDirective)
                return true; // Bind effects are resolved in the current consumer, not its lexical
                             // namespace.
            // Imported definitions keep their provider compilation-unit scope.
            // A later mutable $unit function / type / variable / import can be
            // visible in native single-unit binding but absent from that scope.
            // Lazy bodies prevent proving the absence of every such dependency.
            if (member.kind == syntax::SyntaxKind::DPIImport) {
                const auto& name = *member.as<syntax::DPIImportSyntax>().method->name;
                if (name.kind == syntax::SyntaxKind::IdentifierName) {
                    const auto value =
                        name.as<syntax::IdentifierNameSyntax>().identifier.valueText();
                    const auto cName =
                        member.as<syntax::DPIImportSyntax>().c_identifier.valueText();
                    if (!std::ranges::binary_search(library.unitLookupNames, value) &&
                        (cName.empty() ||
                         !std::ranges::binary_search(library.unitLookupNames, cName)) &&
                        !std::ranges::binary_search(library.unitDpiCNames,
                                                    cName.empty() ? value : cName))
                        return true; // Cannot change any fixed SV or C import binding.
                }
            }
            if (member.kind == syntax::SyntaxKind::PackageImportDeclaration) {
                bool safe = true;
                for (auto item : member.as<syntax::PackageImportDeclarationSyntax>().items) {
                    auto name = item->package.valueText();
                    if (item->item.kind == parsing::TokenKind::Star)
                        safe &= std::ranges::find(library.safeUnitWildcardImports, name) !=
                                library.safeUnitWildcardImports.end();
                    else
                        safe &= library.compilation->getPackage(name) &&
                                !std::ranges::binary_search(library.unitLookupNames,
                                                            item->item.valueText());
                }
                if (safe)
                    return true;
            }
            const auto location = sources.getFullyExpandedLoc(member.getFirstToken().location());
            const auto path = sources.getFullPath(location.buffer()).lexically_normal();
            const bool fixed = !path.empty() && library.files.contains(path.generic_string());
            return fixed;
        }
        const auto& declaration = member.as<syntax::ModuleDeclarationSyntax>();
        const auto name = declaration.header->name.valueText();
        const auto& names = package ? library.packages : library.definitions;
        if (std::ranges::find(names, name) == names.end())
            return true;
        const ast::Symbol* original = nullptr;
        if (package)
            original = library.compilation->getPackage(name);
        else
            original = library.compilation->tryGetDefinition(name, library.compilation->getRoot())
                           .definition;
        if (!original)
            return false;
        const auto* ownerSources = library.compilation->getSourceManager();
        auto provider = ownerSources->getFullPath(
            ownerSources->getFullyExpandedLoc(original->location).buffer());
        auto current = sources.getFullPath(
            sources.getFullyExpandedLoc(declaration.header->name.location()).buffer());
        return original->getSyntax() == &member || (!current.empty() && current == provider);
    };
    for (const auto& tree : trees) {
        const auto& root = tree->root();
        if (root.kind == syntax::SyntaxKind::CompilationUnit) {
            for (auto member : root.as<syntax::CompilationUnitSyntax>().members)
                if (!compatible(*member)) {
                    if (std::getenv("SLANG_SCOPE_NAMESPACE_TRACE")) {
                        auto location = sources.getFullyExpandedLoc(
                            member->getFirstToken().location());
                        std::fprintf(stderr, "scope-unit-decline: %s %s\n",
                                     std::string(syntax::toString(member->kind)).c_str(),
                                     sources.getFullPath(location.buffer()).string().c_str());
                    }
                    return false;
                }
        }
        else if (!compatible(root)) {
            if (std::getenv("SLANG_SCOPE_NAMESPACE_TRACE")) {
                auto location = sources.getFullyExpandedLoc(root.getFirstToken().location());
                std::fprintf(stderr, "scope-root-decline: %s %s\n",
                             std::string(syntax::toString(root.kind)).c_str(),
                             sources.getFullPath(location.buffer()).string().c_str());
            }
            return false;
        }
    }
    return true;
}

bool ServerDriver::hasCompatibleTrustedNamespace() const {
    const auto setting = options.get<TrustedUvmImport>();
    if (!setting || !setting->library)
        return true;
    for (const auto& unit : m_configuredUnits) {
        const std::shared_ptr<syntax::SyntaxTree> current[] = {unit->getSyntaxTree()};
        if (!trustedLibraryNamespaceMatches(*setting->library, sm, current))
            return false;
    }
    return true;
}

bool ServerDriver::prepareScopeLibrary() {
    if (m_trustedScopes) {
        options.set(TrustedUvmImport{m_trustedScopes, m_driverOwner->getSourceOwner()});
        if (m_scopeBodies)
            options.set(ScopeBodyImport{m_scopeBodies});
        m_scopeReason = "retained fixed package semantic generation";
        return true;
    }
    auto decline = [&](std::string reason) {
        m_scopeReason = std::move(reason);
        return false;
    };
    const auto config = m_config.frozenSources.value().value_or(Config::FrozenSourcesConfig{});
    if (m_config.experimentalFrozenSources.value() ||
        m_config.experimentalMethodScopeOverlay.value())
        return decline("scope semantics cannot combine with certified syntax or method overlays");
    auto modules = config.modules.value();
    if (!config.instances.value().empty()) {
        // Resolve concrete hierarchy once. Keep only value selectors, never this
        // temporary probe's AST or the older per-request witness / JSON cache.
        try {
            ast::Compilation planning(options);
            for (const auto& tree : driver.syntaxTrees)
                planning.addSyntaxTree(tree);
            FrozenInstanceCache selection(config.instances.value(), 0, sm, {});
            selection.resolveSelections(planning);
            m_scopeInstanceSelections = selection.selections();
            for (auto& item : m_scopeInstanceSelections) {
                if (!item.resolved || item.modules.empty())
                    return decline(
                        "fixed RTL selection does not resolve to a canonical module subtree: " +
                        item.instance);
                item.reason = "fixed definition closure planned; eligible integral subtrees use "
                              "independent owners";
            }
            for (const auto& name : selection.modules())
                if (std::ranges::find(modules, name) == modules.end())
                    modules.push_back(name);
        }
        catch (const std::exception& error) {
            return decline(std::string("cannot resolve fixed RTL subtree: ") + error.what());
        }
    }
    if (config.packages.value().empty() && modules.empty())
        return decline("no stable scopes selected");
    auto plans = planFrozenSources(driver.syntaxTrees, config.packages.value(), modules);
    if (std::ranges::any_of(plans.selections, [](const auto& s) { return !s.resolved; }) ||
        plans.units.size() != 1 || !plans.units.front().frozenRootCount)
        return decline("package selection must resolve uniquely in one configured unit");
    if (driver.options.singleUnit.value_or(false) && driver.syntaxTrees.size() > 1)
        return decline("inherited compilation-unit preprocessing requires native analysis");
    const auto& plan = plans.units.front();
    for (const auto& tree : driver.syntaxTrees) {
        if (!tree->isLibraryUnit && !tree->getSourceLibrary())
            continue;
        for (auto id : tree->getSourceBufferIds())
            if (std::ranges::find(plan.roots, sm.getFullPath(id)) != plan.roots.end())
                return decline("fixed scope source-library contexts require native analysis");
    }
    std::vector<SourceBuffer> roots;
    for (size_t i = 0; i < plan.frozenRootCount; ++i) {
        auto buffer = sm.readSource(plan.roots[i]);
        if (!buffer)
            return decline("cannot read a fixed package root");
        roots.push_back(*buffer);
    }
    std::shared_ptr<syntax::SyntaxTree> prefix;
    try {
        if (driver.precompiledUvm && !roots.empty() &&
            sm.getFullPath(roots.front().id) == m_uvmEntryPath) {
            prefix = driver.precompiledUvm;
            if (roots.size() > 1) {
                auto extensionOptions = options;
                auto pp = extensionOptions.getOrDefault<parsing::PreprocessorOptions>();
                pp.undefines.clear();
                extensionOptions.set(pp);
                prefix = syntax::SyntaxTree::extendCachedPrefix(prefix, std::span(roots).subspan(1),
                                                                extensionOptions);
            }
        }
        else
            prefix = syntax::SyntaxTree::fromBuffersWithCheckpoint(roots, sm, options);
    }
    catch (const std::exception& error) {
        return decline(std::string("package preprocessing is not reusable: ") + error.what());
    }
    if (!prefix->isPrefixResumeSafe().value_or(false))
        return decline("fixed package prefix has unsupported preprocessing state");
    std::vector<std::string> packages, definitions;
    bool hasUnitDeclarations = false;
    for (const auto* member : prefix->root().as<syntax::CompilationUnitSyntax>().members) {
        if (member->kind == syntax::SyntaxKind::PackageDeclaration)
            packages.emplace_back(
                member->as<syntax::ModuleDeclarationSyntax>().header->name.valueText());
        else if (member->kind == syntax::SyntaxKind::ModuleDeclaration ||
                 member->kind == syntax::SyntaxKind::InterfaceDeclaration ||
                 member->kind == syntax::SyntaxKind::ProgramDeclaration)
            definitions.emplace_back(
                member->as<syntax::ModuleDeclarationSyntax>().header->name.valueText());
        else {
            switch (member->kind) {
                case syntax::SyntaxKind::PackageImportDeclaration:
                case syntax::SyntaxKind::DPIImport:
                case syntax::SyntaxKind::ClassDeclaration:
                case syntax::SyntaxKind::TypedefDeclaration:
                case syntax::SyntaxKind::ForwardTypedefDeclaration:
                case syntax::SyntaxKind::FunctionDeclaration:
                case syntax::SyntaxKind::TaskDeclaration:
                case syntax::SyntaxKind::DataDeclaration:
                case syntax::SyntaxKind::ParameterDeclarationStatement:
                    hasUnitDeclarations = true;
                    break;
                case syntax::SyntaxKind::EmptyMember:
                    break;
                default:
                    return decline(
                        "fixed scope roots contain unsupported compilation-unit declarations");
            }
        }
    }
    if (hasUnitDeclarations && driver.syntaxTrees.size() != 1)
        return decline("fixed compilation-unit declarations require a single configured unit");
    if (std::set<std::string>(packages.begin(), packages.end()).size() != packages.size() ||
        std::set<std::string>(definitions.begin(), definitions.end()).size() != definitions.size())
        return decline("fixed scope dependency declarations are ambiguous; native analysis");
    if (m_config.experimentalUvmSemanticCache.value() && !m_uvmEntryPath.empty() &&
        std::ranges::find(packages, "uvm_pkg") == packages.end())
        return decline("UVM and fixed packages must share one ordered package closure");
    auto compilationOptions = options.getOrDefault<ast::CompilationOptions>();
    compilationOptions.flags |= ast::CompilationFlags::PackageAnalysis;
    compilationOptions.topModules.clear();
    auto compilation = std::make_shared<ast::Compilation>(compilationOptions);
    compilation->retainSourceOwner(m_driverOwner->getSourceOwner());
    compilation->addSyntaxTree(prefix);
    std::optional<PackageClosure> closure;
    auto seeds = config.packages.value();
    if (m_config.experimentalUvmSemanticCache.value() && !m_uvmEntryPath.empty() &&
        std::ranges::find(seeds, "uvm_pkg") == seeds.end())
        seeds.emplace_back("uvm_pkg");
    // A package-only projection cannot discard the fixed $unit namespace that
    // its declarations (including deferred generic bodies) can depend on.
    const bool packageOnly = modules.empty() && !hasUnitDeclarations && !seeds.empty() &&
                             driver.syntaxTrees.size() == 1;
    const bool exactPrefix = definitions.empty() &&
                             std::set<std::string>(packages.begin(), packages.end()) ==
                                 std::set<std::string>(seeds.begin(), seeds.end());
    // Keep the existing exact-prefix fast path, including its UVM distribution
    // policy. Resolve a declaration closure only when a prefix includes extra
    // packages, or native binding requests dependencies after that prefix.
    if (packageOnly && !exactPrefix)
        closure = packageClosure(prefix, seeds, compilationOptions,
                                 m_driverOwner->getSourceOwner());
    if (!closure) {
        const auto& diagnostics = compilation->getSemanticDiagnostics();
        if (std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }) &&
            packageOnly)
            closure = packageClosure(driver.syntaxTrees.front(), seeds, compilationOptions,
                                     m_driverOwner->getSourceOwner());
        if (!closure && std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }))
            return decline("fixed package dependency closure has semantic errors; native analysis");
    }
    bool selective = false;
    auto semanticTree = prefix;
    if (closure) {
        selective = std::set<std::string>(closure->packages.begin(), closure->packages.end()) !=
                        std::set<std::string>(packages.begin(), packages.end()) ||
                    !definitions.empty();
        for (size_t i = 0; i < plan.frozenRootCount; ++i)
            selective |= !closure->files.contains(
                plan.roots[i].lexically_normal().generic_string());
        compilation = closure->compilation;
        semanticTree = closure->tree;
        packages = closure->packages;
        definitions.clear();
    }
    std::vector<std::string_view> unitLookupNames, safeUnitWildcardImports, unitDpiCNames;
    if (!definitions.empty() || hasUnitDeclarations) {
        auto collect = [&](auto&& self, const syntax::SyntaxNode& node,
                           const ast::PackageSymbol* package) -> void {
            if (node.kind == syntax::SyntaxKind::PackageDeclaration)
                package = compilation->getPackage(
                    node.as<syntax::ModuleDeclarationSyntax>().header->name.valueText());
            if (node.kind == syntax::SyntaxKind::ScopedName &&
                node.as<syntax::ScopedNameSyntax>().left->kind == syntax::SyntaxKind::UnitScope)
                package = nullptr; // Explicit $unit lookup bypasses package-local shadows.
            if (node.kind == syntax::SyntaxKind::DPIImport) {
                const auto& dpi = node.as<syntax::DPIImportSyntax>();
                auto name = dpi.c_identifier.valueText();
                if (name.empty() && dpi.method->name->kind == syntax::SyntaxKind::IdentifierName)
                    name =
                        dpi.method->name->as<syntax::IdentifierNameSyntax>().identifier.valueText();
                if (!name.empty())
                    unitDpiCNames.push_back(name);
            }
            for (size_t i = 0; i < node.getChildCount(); ++i) {
                if (auto child = node.childNode(i))
                    self(self, *child, package);
                else if (auto token = node.childToken(i);
                         token && token.kind == parsing::TokenKind::Identifier) {
                    auto name = token.valueText();
                    // Direct package declarations shadow every CU binding,
                    // independent of declaration order. Keep all other names,
                    // including unresolved generic and wildcard dependencies.
                    if (!package || !package->getNameMap().contains(name))
                        unitLookupNames.push_back(name);
                }
            }
        };
        collect(collect, semanticTree->root(), nullptr);
        std::ranges::sort(unitDpiCNames);
        unitDpiCNames.erase(std::unique(unitDpiCNames.begin(), unitDpiCNames.end()),
                            unitDpiCNames.end());
        std::ranges::sort(unitLookupNames);
        auto last = std::unique(unitLookupNames.begin(), unitLookupNames.end());
        std::vector<std::string_view>(unitLookupNames.begin(), last).swap(unitLookupNames);
        auto checkPackage = [&](std::string_view name) {
            if (auto package = compilation->getPackage(name))
                if (std::ranges::none_of(unitLookupNames, [&](auto lookup) {
                        return package->findForImport(lookup) != nullptr;
                    }))
                    safeUnitWildcardImports.push_back(package->name);
        };
        for (const auto& name : packages)
            checkPackage(name);
        checkPackage("std");
    }
    const auto charged = compilation->getTotalBytesAllocated() +
                         semanticTree->getRetainedSyntaxBytes() +
                         semanticTree->getRetainedSourceBytes() + sm.getSourceLocationTableBytes() +
                         sm.getMemoryUsage() +
                         (unitLookupNames.capacity() + safeUnitWildcardImports.capacity() +
                          unitDpiCNames.capacity()) *
                             sizeof(std::string_view);
    if (!config.maxMiB.value() || config.maxMiB.value() > 512 ||
        charged > size_t(config.maxMiB.value()) * 1024 * 1024)
        return decline("fixed package owner exceeds frozenSources.maxMiB");
    auto library = std::make_shared<TrustedUvmLibrary>();
    library->sourceOwner = m_driverOwner->getSourceOwner();
    library->compilation = std::move(compilation);
    library->packages = std::move(packages);
    library->definitions = std::move(definitions);
    library->hasUnitDeclarations = hasUnitDeclarations && !closure;
    library->unitLookupNames = std::move(unitLookupNames);
    library->safeUnitWildcardImports = std::move(safeUnitWildcardImports);
    library->unitDpiCNames = std::move(unitDpiCNames);
    library->uvmParseTree = driver.precompiledUvm;
    library->shippedProfile = m_shippedUvmProfile;
    library->preparseDependencies = m_uvmPreparseDependencies;
    library->parseCachePath = m_uvmParseCachePath;
    library->preparseKey = m_uvmPreparseKey;
    static std::atomic<uint64_t> generation = 0;
    library->generation = ++generation;
    library->semanticBytes = charged;
    if (closure)
        library->files.insert(closure->files.begin(), closure->files.end());
    else
        for (auto id : prefix->getSourceBufferIds()) {
            const auto& path = sm.getFullPath(id);
            if (!path.empty())
                library->files.emplace(path.lexically_normal().generic_string());
        }
    std::vector<BufferID> fixedBuffers;
    for (auto id : semanticTree->getSourceBufferIds())
        if (library->files.contains(sm.getFullPath(id).lexically_normal().generic_string()))
            fixedBuffers.push_back(id);
    // The namespace preserves every existing source ID, but only fixed provider
    // paths enter the inherited lookup cache. Mutable overlays must be assignable
    // after retirement even when the declaration view retains their original CST.
    library->sources = sm.captureImmutablePrefix(fixedBuffers);
    m_scopeRoots = plan.roots;
    m_scopeRootCount = selective ? 0 : plan.frozenRootCount;
    m_scopePrefix = selective ? nullptr : std::move(prefix);
    m_trustedScopes = std::move(library);
    options.set(TrustedUvmImport{m_trustedScopes, m_driverOwner->getSourceOwner()});
    if (!config.modules.value().empty() || !config.instances.value().empty()) {
        m_scopeBodies = std::make_shared<ScopeBodyCache>(
            m_trustedScopes, size_t(config.maxMiB.value()) * 1024 * 1024, config.modules.value(),
            config.instances.value());
        options.set(ScopeBodyImport{m_scopeBodies});
    }
    m_scopeReason =
        m_trustedScopes->definitions.empty()
            ? "prepared whole package closure; dependencies fixed until explicit rebuild"
            : "prepared fixed semantics; eligible integral subtrees use the independent semantic "
              "registry";
    if (selective)
        m_scopeReason =
            "prepared package declaration dependency closure; mutable roots parse natively";
    if (!m_scopeBodies && !m_trustedScopes->definitions.empty())
        m_scopeReason =
            "prepared package semantics and fixed definition identities; no RTL body selectors";
    return true;
}

std::shared_ptr<syntax::SyntaxTree> ServerDriver::resumeScopeLibrary(
    std::span<const std::filesystem::path> paths, std::span<const SourceBuffer> buffers,
    const Bag& parseOptions, bool nativeReplay) {
    if (m_trustedScopes)
        (void)trustedLibrarySourceContext(URI{});
    if (!m_trustedScopes || !m_scopePrefix || nativeReplay ||
        !std::ranges::equal(paths, m_scopeRoots) || buffers.size() < m_scopeRootCount)
        return {};
    // Dependencies are explicitly fixed. Do not certify file contents per edit.
    if (buffers.size() == m_scopeRootCount)
        return m_scopePrefix;
    auto suffixOptions = parseOptions;
    auto pp = suffixOptions.getOrDefault<parsing::PreprocessorOptions>();
    pp.predefines.clear();
    pp.undefines.clear();
    suffixOptions.set(pp);
    auto suffix = syntax::SyntaxTree::afterCachedPrefix(m_scopePrefix,
                                                        buffers.subspan(m_scopeRootCount),
                                                        suffixOptions);
    auto tree = syntax::SyntaxTree::withCachedPrefix(m_scopePrefix, suffix);
    ++m_scopeResumes;
    return tree;
}

bool ServerDriver::isTrustedScopeSource(const std::filesystem::path& path) const {
    return m_trustedScopes &&
           m_trustedScopes->files.contains(path.lexically_normal().generic_string());
}

rfl::Generic ServerDriver::scopeLibraryStatus() const {
    struct Status {
        bool enabled, prepared;
        uint64_t generation, consumers, syntaxResumes, namespaceDeclines;
        size_t chargedBytes, astBytes, fixedSourceFiles;
        std::vector<std::string> packages, definitions;
        bool hasUnitDeclarations = false;
        std::vector<FrozenInstanceCache::Selection> instances;
        bool instanceBodiesShared;
        ScopeBodyCache::Stats bodies;
        std::string reason;
    };
    const auto& library = m_trustedScopes;
    const auto bodies = m_scopeBodies ? m_scopeBodies->status() : ScopeBodyCache::Stats{};
    return rfl::to_generic(
        Status{m_config.experimentalScopeSemanticCache.value(), bool(library),
               library ? library->generation : 0, library ? library->consumers.load() : 0,
               m_scopeResumes, library ? library->namespaceDeclines.load() : 0,
               library ? library->semanticBytes + bodies.ownerBytes + bodies.pinnedBytes +
                             bodies.admissionBytes
                       : 0,
               library ? library->compilation->getTotalBytesAllocated() : 0,
               library ? library->files.size() : 0,
               library ? library->packages : std::vector<std::string>{},
               library ? library->definitions : std::vector<std::string>{},
               library && library->hasUnitDeclarations, m_scopeInstanceSelections, bodies.hits != 0,
               bodies, m_scopeReason});
}
} // namespace server
