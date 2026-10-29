// SPDX-License-Identifier: MIT
#include "ServerDriver.h"
#include "util/ModelOwnerTrace.h"
#include "util/SlangExtensions.h"
#include "util/UvmPhaseTrace.h"
#include "ast/TrustedUvmLibrary.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <rfl/to_generic.hpp>

#include "slang/parsing/Preprocessor.h"
#include "slang/parsing/ParserMetadata.h"
#include "slang/syntax/AllSyntax.h"

namespace server {
namespace {
struct PackageClosure {
    std::shared_ptr<ast::Compilation> compilation;
    std::shared_ptr<syntax::SyntaxTree> tree;
    std::vector<std::string> packages;
    std::set<std::string> files;
    std::vector<std::string> definitions;
};

// Pure RTL declaration closure: retain the selected definitions and their global
// child definitions in native input order, without making intervening roots fixed.
// Compilation-unit namespaces, scoped types and interface ports keep the existing
// complete-provider route. Their binding needs more than a definition-name graph.
std::optional<PackageClosure> rtlDeclarationClosure(
    const std::shared_ptr<syntax::SyntaxTree>& owner, std::span<const std::string> seeds,
    ast::CompilationOptions options, std::shared_ptr<const void> sourceOwner) {
    using K = syntax::SyntaxKind;
    if (owner->root().kind != K::CompilationUnit || seeds.empty() ||
        std::ranges::any_of(owner->diagnostics(), [](const auto& d) { return d.isError(); }))
        return std::nullopt;
    const auto& unit = owner->root().as<syntax::CompilationUnitSyntax>();
    std::map<std::string, std::vector<const syntax::MemberSyntax*>> candidates;
    std::set<std::string> globalNames;
    for (auto member : unit.members) {
        if (member->kind == K::EmptyMember || member->kind == K::BindDirective)
            continue;
        if (member->kind == K::PackageDeclaration) {
            globalNames.emplace(member->as<syntax::ModuleDeclarationSyntax>()
                                    .header->name.valueText());
            continue;
        }
        if (member->kind == K::PackageImportDeclaration) {
            for (auto item : member->as<syntax::PackageImportDeclarationSyntax>().items) {
                if (item->item.kind == parsing::TokenKind::Star)
                    return std::nullopt;
                globalNames.emplace(item->item.valueText());
            }
            continue; // Explicit imports are irrelevant only if no selected name uses them.
        }
        if (member->kind != K::ModuleDeclaration && member->kind != K::InterfaceDeclaration &&
            member->kind != K::ProgramDeclaration)
            return std::nullopt;
        const auto name = std::string(member->as<syntax::ModuleDeclarationSyntax>()
                                          .header->name.valueText());
        globalNames.insert(name);
        candidates[name].push_back(member);
    }
    std::set<std::string> wanted(seeds.begin(), seeds.end());
    std::shared_ptr<syntax::SyntaxTree> tree;
    std::vector<const syntax::MemberSyntax*> selected;
    for (;;) {
        selected.clear();
        for (const auto& name : wanted) {
            const auto found = candidates.find(name);
            if (found == candidates.end() || found->second.size() != 1)
                return std::nullopt;
            selected.push_back(found->second.front());
        }
        tree = syntax::SyntaxTree::selectCompilationUnitMembers(owner, selected);
        const auto& meta = tree->getMetadata();
        if (!meta.interfacePorts.empty() || !meta.virtualInterfaceTypes.empty() ||
            !meta.namedPortTypes.empty())
            return std::nullopt;
        bool scoped = false;
        auto inspect = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
            if (node.kind == K::ScopedName || node.kind == K::PackageImportDeclaration ||
                (node.kind == K::ModuleDeclaration &&
                 node.parent && node.parent->kind != K::CompilationUnit))
                scoped = true;
            for (size_t i = 0; i < node.getChildCount(); ++i)
                if (auto child = node.childNode(i))
                    self(self, *child);
        };
        inspect(inspect, tree->root());
        if (scoped)
            return std::nullopt;
        bool added = false;
        for (const auto* instance : meta.globalInstances) {
            const auto name = std::string(instance->type.valueText());
            const auto found = candidates.find(name);
            if (found == candidates.end() || found->second.size() != 1)
                return std::nullopt;
            added |= wanted.insert(name).second;
        }
        if (!added)
            break;
    }
    // Unselected global names must not change a selected body's unqualified
    // lookup, even when native binding would produce an error instead of a value.
    bool globalConflict = false;
    auto checkNames = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
        for (size_t i = 0; i < node.getChildCount(); ++i) {
            if (auto child = node.childNode(i))
                self(self, *child);
            else if (auto token = node.childToken(i);
                     token && token.kind == parsing::TokenKind::Identifier) {
                const auto name = std::string(token.valueText());
                if (globalNames.contains(name) && !wanted.contains(name))
                    globalConflict = true;
            }
        }
    };
    checkNames(checkNames, tree->root());
    if (globalConflict)
        return std::nullopt;
    auto& sm = owner->sourceManager();
    auto rootPath = [&](SourceLocation location) {
        location = sm.getFullyExpandedLoc(location);
        while (auto parent = sm.getIncludedFrom(location.buffer()))
            location = sm.getFullyExpandedLoc(parent);
        return sm.getFullPath(location.buffer()).lexically_normal().generic_string();
    };
    std::set<std::string> roots, knownRoots, files;
    for (auto id : owner->getSourceBufferIds())
        if (!sm.getIncludedFrom(id))
            knownRoots.insert(sm.getFullPath(id).lexically_normal().generic_string());
    for (auto member : selected) {
        roots.insert(rootPath(member->getFirstToken().location()));
        roots.insert(rootPath(member->getLastToken().location()));
        auto origins = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
            for (size_t i = 0; i < node.getChildCount(); ++i) {
                if (auto child = node.childNode(i))
                    self(self, *child);
                else if (auto token = node.childToken(i); token) {
                    auto path = rootPath(sm.getFullyOriginalLoc(token.location()));
                    if (knownRoots.contains(path))
                        roots.insert(std::move(path));
                }
            }
        };
        origins(origins, *member);
    }
    for (auto member : unit.members)
        if (member->kind != K::EmptyMember && std::ranges::find(selected, member) == selected.end())
            if (roots.contains(rootPath(member->getFirstToken().location())) ||
                roots.contains(rootPath(member->getLastToken().location())))
                return std::nullopt; // Mixed physical roots remain indivisible.
    for (auto id : owner->getSourceBufferIds())
        if (roots.contains(rootPath(SourceLocation(id, 0)))) {
            const auto& path = sm.getFullPath(id);
            if (!path.empty())
                files.insert(path.lexically_normal().generic_string());
        }
    auto compilation = std::make_shared<ast::Compilation>(options);
    compilation->retainSourceOwner(std::move(sourceOwner));
    compilation->addSyntaxTree(tree);
    if (std::ranges::any_of(compilation->getSemanticDiagnostics(),
                           [](const auto& d) { return d.isError(); }))
        return std::nullopt;
    PackageClosure result{std::move(compilation), std::move(tree), {}, std::move(files), {}};
    for (auto member : result.tree->root().as<syntax::CompilationUnitSyntax>().members)
        result.definitions.emplace_back(
            member->as<syntax::ModuleDeclarationSyntax>().header->name.valueText());
    return result;
}

bool sameProjectedSyntax(const syntax::SyntaxNode& left, const syntax::SyntaxNode& right,
                         const SourceManager& aSources, const SourceManager& bSources) {
    if (&left == &right)
        return true;
    if (left.kind != right.kind || left.getChildCount() != right.getChildCount())
        return false;
    auto locationMatches = [&](SourceLocation a, SourceLocation b) {
        if (&aSources == &bSources && a == b)
            return true;
        auto same = [&](SourceLocation x, SourceLocation y) {
            return x.offset() == y.offset() &&
                   aSources.getFullPath(x.buffer()) == bSources.getFullPath(y.buffer());
        };
        return same(aSources.getFullyExpandedLoc(a), bSources.getFullyExpandedLoc(b)) &&
               same(aSources.getFullyOriginalLoc(a), bSources.getFullyOriginalLoc(b));
    };
    for (size_t i = 0; i < left.getChildCount(); ++i) {
        if (auto child = left.childNode(i)) {
            auto other = right.childNode(i);
            if (!other || !sameProjectedSyntax(*child, *other, aSources, bSources))
                return false;
        }
        else {
            auto a = left.childToken(i), b = right.childToken(i);
            if (right.childNode(i) || a.kind != b.kind || a.rawText() != b.rawText())
                return false;
            if (!a)
                continue;
            if (!locationMatches(a.location(), b.location()))
                return false;
            auto at = a.trivia(), bt = b.trivia();
            if (at.size() != bt.size())
                return false;
            for (size_t n = 0; n < at.size(); ++n) {
                if (at[n].kind != bt[n].kind || at[n].getRawText() != bt[n].getRawText())
                    return false;
                auto an = at[n].syntax(), bn = bt[n].syntax();
                if (bool(an) != bool(bn) ||
                    (an && !sameProjectedSyntax(*an, *bn, aSources, bSources)))
                    return false;
                // Error-recovery trivia is not a valid provider proof.
                if (!at[n].getSkippedTokens().empty() || !bt[n].getSkippedTokens().empty())
                    return false;
            }
        }
    }
    return true;
}

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
    std::set<std::string_view> seenDefinitions;
    const syntax::SyntaxTree* currentTree = nullptr;
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
                if (library.projectedDefinitions) {
                    bool unrelated = true;
                    for (auto item : member.as<syntax::PackageImportDeclarationSyntax>().items)
                        unrelated &= item->item.kind != parsing::TokenKind::Star &&
                                     !std::ranges::binary_search(library.unitLookupNames,
                                                                item->item.valueText());
                    if (unrelated)
                        return true;
                }
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
            return !library.projectedDefinitions ||
                   !std::ranges::binary_search(library.unitLookupNames, name);
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
        if (original->getSyntax() != &member && (current.empty() || current != provider))
            return false;
        if (library.projectedDefinitions && definition) {
            if (!seenDefinitions.insert(name).second || !original->getSyntax() || !currentTree)
                return false;
            const auto& providerTree = *library.compilation->getSyntaxTrees().front();
            const auto& before = providerTree.getMetadata().nodeMeta;
            const auto& after = currentTree->getMetadata().nodeMeta;
            auto a = std::ranges::find_if(before, [&](const auto& entry) {
                return entry.first == original->getSyntax();
            });
            auto b = std::ranges::find_if(after, [&](const auto& entry) {
                return entry.first == &member;
            });
            if (a == before.end() || b == after.end() ||
                a->second.defaultNetType != b->second.defaultNetType ||
                a->second.unconnectedDrive != b->second.unconnectedDrive ||
                a->second.cellDefine != b->second.cellDefine ||
                a->second.timeScale != b->second.timeScale ||
                !sameProjectedSyntax(*original->getSyntax(), member, *ownerSources, sources))
                return false;
        }
        return true;
    };
    for (const auto& tree : trees) {
        currentTree = tree.get();
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
    if (library.projectedDefinitions) {
        // A direct provider import may precede adding consumer-only syntax.
        // Require the declaration when its physical input is actually present;
        // this also catches an earlier mutable conditional removing that module.
        for (const auto& name : library.definitions) {
            if (seenDefinitions.contains(name))
                continue;
            const auto* original = library.compilation->tryGetDefinition(
                name, library.compilation->getRoot()).definition;
            if (!original)
                return false;
            const auto* providerSources = library.compilation->getSourceManager();
            const auto path = providerSources->getFullPath(
                providerSources->getFullyExpandedLoc(original->location).buffer());
            for (const auto& tree : trees)
                for (auto id : tree->getSourceBufferIds())
                    if (sources.getFullPath(id) == path)
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
            options.set(ScopeBodyImport{m_scopeBodies, driver.sourceLoader.getLibraryOwner()});
        m_scopeReason = "retained fixed package semantic generation";
        return true;
    }
    auto decline = [&](std::string reason) {
        m_scopeReason = std::move(reason);
        return false;
    };
    auto declineError = [&](std::string reason, const Diagnostic& error) {
        reason += "; native analysis; first error: ";
        if (error.location.valid() && error.location != SourceLocation::NoLocation) {
            const auto location = sm.getFullyOriginalLoc(error.location);
            reason += sm.getFullPath(location.buffer()).string() + ":" +
                      std::to_string(sm.getLineNumber(location)) + ": ";
        }
        return decline(reason + diagEngine.formatMessage(error));
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
    auto compilationOptions = options.getOrDefault<ast::CompilationOptions>();
    compilationOptions.flags |= ast::CompilationFlags::PackageAnalysis;
    compilationOptions.topModules.clear();
    std::optional<PackageClosure> closure;
    if (config.packages.value().empty() && !modules.empty() && m_uvmEntryPath.empty() &&
        driver.syntaxTrees.size() == 1)
        closure = rtlDeclarationClosure(driver.syntaxTrees.front(), modules, compilationOptions,
                                        m_driverOwner->getSourceOwner());
    if (closure) {
        bool separated = false;
        for (size_t i = 0; i < plan.roots.size(); ++i) {
            const bool fixed = closure->files.contains(plan.roots[i].lexically_normal().generic_string());
            separated |= (i < plan.frozenRootCount) != fixed;
        }
        if (!separated)
            closure.reset(); // Preserve the existing contiguous-prefix fast path.
    }
    const bool projectedRtl = closure.has_value();
    std::shared_ptr<syntax::SyntaxTree> prefix;
    try {
        if (projectedRtl)
            prefix = closure->tree;
        else if (driver.precompiledUvm && !roots.empty() &&
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
    if (!projectedRtl && !prefix->isPrefixResumeSafe().value_or(false))
        return decline("fixed package prefix has unsupported preprocessing state");
    if (const auto error = std::ranges::find_if(prefix->diagnostics(),
                                               [](const auto& d) { return d.isError(); });
        error != prefix->diagnostics().end())
        return declineError("fixed scope prefix has syntax errors", *error);
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
    auto compilation = projectedRtl ? closure->compilation
                                    : std::make_shared<ast::Compilation>(compilationOptions);
    if (!projectedRtl) {
        compilation->retainSourceOwner(m_driverOwner->getSourceOwner());
        compilation->addSyntaxTree(prefix);
    }
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
        if (!closure) {
            const auto error = std::ranges::find_if(diagnostics,
                                                   [](const auto& d) { return d.isError(); });
            if (error != diagnostics.end()) {
                return declineError("fixed package dependency closure has semantic errors", *error);
            }
        }
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
        definitions = closure->definitions;
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
    library->projectedDefinitions = projectedRtl;
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
    if (modelOwnerTraceEnabled())
        traceModelOwner("scope-provider", m_trustedScopes->compilation.get(), nullptr,
                        m_trustedScopes->compilation->getTotalBytesAllocated());
    options.set(TrustedUvmImport{m_trustedScopes, m_driverOwner->getSourceOwner()});
    if (!config.modules.value().empty() || !config.instances.value().empty()) {
        m_scopeBodies = std::make_shared<ScopeBodyCache>(
            m_trustedScopes, size_t(config.maxMiB.value()) * 1024 * 1024, config.modules.value(),
            config.instances.value(), m_sourceRegistry);
        options.set(ScopeBodyImport{m_scopeBodies, driver.sourceLoader.getLibraryOwner()});
    }
    m_scopeReason =
        m_trustedScopes->definitions.empty()
            ? "prepared whole package closure; dependencies fixed until explicit rebuild"
            : "prepared fixed semantics; eligible integral subtrees use the independent semantic "
              "registry";
    if (selective)
        m_scopeReason = projectedRtl
                            ? "prepared RTL declaration dependency closure; mutable roots parse natively"
                            : "prepared package declaration dependency closure; mutable roots parse natively";
    if (!m_scopeBodies && !m_trustedScopes->definitions.empty())
        m_scopeReason =
            "prepared package semantics and fixed definition identities; no RTL body selectors";
    return true;
}

std::shared_ptr<syntax::SyntaxTree> ServerDriver::resumeScopeLibrary(
    std::span<const std::filesystem::path> paths, std::span<const SourceBuffer> buffers,
    const Bag& parseOptions, bool nativeReplay, const lsp::RequestContext& ctx) {
    if (m_syntaxInputFailure)
        throw lsp::RequestFailed("Syntax prefix inputs changed; fresh input context required: " +
                                 *m_syntaxInputFailure);
    if (m_trustedScopes)
        (void)trustedLibrarySourceContext(URI{});
    if (!m_trustedScopes || !m_scopePrefix || nativeReplay ||
        !std::ranges::equal(paths, m_scopeRoots) || buffers.size() < m_scopeRootCount)
        return {};
    // Dependencies are explicitly fixed. Do not certify file contents per edit.
    if (buffers.size() == m_scopeRootCount)
        return m_scopePrefix;
    auto prefix = m_scopePrefix;
    auto rootCount = m_scopeRootCount;
    size_t captureCount = 0;
    const auto syntaxBudget = uint64_t(m_config.syntaxPrefixCacheMaxMiB.value()) * 1024 * 1024;
    const bool probeEnabled = m_config.experimentalSyntaxPrefixCache.value() &&
                              isCooperativeAnalysisEnabled() && syntaxBudget != 0;
    const auto baseSyntaxBytes = m_scopePrefix->getRetainedSyntaxBytes();
    UvmPhaseTrace probe(probeEnabled);
    auto check = [&] {
        if (const auto* pp = parseOptions.get<parsing::PreprocessorOptions>();
            pp && pp->parseCheckpoint)
            pp->parseCheckpoint();
    };
    if (probeEnabled) {
        if (m_syntaxPrefixBase.lock() != m_scopePrefix) {
            m_syntaxPrefix.reset();
            m_syntaxPrefixBytes = 0;
            m_pendingSyntaxPrefix.reset();
            m_pendingSyntaxRoots = 0;
            m_syntaxPrefixRoots = m_syntaxDeclinedRoots = 0;
            m_syntaxDeclinedInputs.clear();
            m_syntaxPrefixBase = m_scopePrefix;
        }
        if (m_syntaxPrefix && !hasValidBuffers(sm, m_syntaxPrefix)) {
            m_syntaxPrefix.reset();
            m_syntaxPrefixBytes = 0;
            m_syntaxPrefixRoots = 0;
        }
        if (m_syntaxPrefix || m_pendingSyntaxPrefix) {
            std::vector<std::filesystem::path> overlays;
            for (const auto& uri : m_openDocs)
                overlays.emplace_back(uri.getPath());
            if (auto failure = sm.sourceWitnessFailure(overlays, check)) {
                ++m_syntaxPrefixWitnessFailures;
                m_syntaxPrefix.reset();
                m_syntaxPrefixBytes = 0;
                m_syntaxPrefixRoots = 0;
                m_pendingSyntaxPrefix.reset();
                m_pendingSyntaxRoots = 0;
                m_syntaxDeclinedInputs.clear();
                m_syntaxDeclinedRoots = 0;
                m_syntaxInputFailure = std::move(failure);
                // An ordinary parse through this manager would still consult
                // stale bytes and failed lookup entries. Refuse this dispatch;
                // the server's existing input-recovery path creates a fresh
                // manager and preserves every unsaved editor document.
                throw lsp::RequestFailed("Syntax prefix inputs changed; fresh input context required: " +
                                         *m_syntaxInputFailure);
            }
            probe.mark("captured-prefix-physical-witness");
        }
        if (m_pendingSyntaxPrefix) {
            check();
            auto candidate = syntax::SyntaxTree::preparedSourcePrefix(m_pendingSyntaxPrefix);
            const auto baseBytes = m_scopePrefix->getRetainedSyntaxBytes();
            const auto bytes = candidate ? candidate->getRetainedSyntaxBytes() : 0;
            if (candidate && candidate->isPrefixResumeSafe().value_or(false) && bytes >= baseBytes &&
                bytes - baseBytes <= syntaxBudget) {
                ctx.checkpoint("before syntax prefix cache commit");
                check();
                m_syntaxPrefix = std::move(candidate);
                m_syntaxPrefixBytes = bytes - baseBytes;
                m_syntaxPrefixRoots = m_pendingSyntaxRoots;
                m_syntaxDeclinedInputs.clear();
                m_syntaxDeclinedRoots = 0;
            }
            else {
                ++m_syntaxPrefixRefusals;
                m_syntaxPrefixBytes = 0;
                m_syntaxDeclinedRoots = m_pendingSyntaxRoots;
                m_syntaxDeclinedInputs.clear();
                const auto graph = candidate ? candidate->getSourceBufferIds()
                                             : m_pendingSyntaxPrefix->getSourceBufferIds();
                for (auto id : graph) {
                    auto root = id;
                    while (auto from = sm.getIncludedFrom(root))
                        root = sm.getFullyExpandedLoc(from).buffer();
                    auto found = std::ranges::find(paths, sm.getFullPath(root));
                    if (found != paths.end() && size_t(found - paths.begin()) < m_syntaxDeclinedRoots)
                        m_syntaxDeclinedInputs.push_back(id);
                }
            }
            m_pendingSyntaxPrefix.reset();
            m_pendingSyntaxRoots = 0;
            probe.mark("captured-prefix-delayed-extraction");
        }
        if (m_syntaxPrefix) {
            prefix = m_syntaxPrefix;
            rootCount = m_syntaxPrefixRoots;
            ++m_syntaxPrefixHits;
            probe.mark("captured-prefix-use");
        }
        else {
            size_t earliest = paths.size();
            bool changed = false;
            for (const auto& tree : m_currentConfiguredTrees) {
                for (auto id : tree->getSourceBufferIds()) {
                    check();
                    if (sm.isLatestData(id))
                        continue;
                    changed = true;
                    while (auto from = sm.getIncludedFrom(id))
                        id = sm.getFullyExpandedLoc(from).buffer();
                    const auto found = std::ranges::find(paths, sm.getFullPath(id));
                    if (found == paths.end()) {
                        earliest = 0;
                        break;
                    }
                    earliest = std::min(earliest, size_t(found - paths.begin()));
                }
            }
            // Choosing a capture boundary is only a hint for the required full
            // parse; it skips no input. Direct root documents can have already
            // replaced their old native recipe, so the configured-tree registry
            // alone need not identify their edit. Prefer the earliest open root.
            std::optional<size_t> openRoot;
            for (const auto& uri : m_openDocs) {
                const auto found = std::ranges::find(paths, std::filesystem::path(uri.getPath()));
                if (found != paths.end()) {
                    const auto index = size_t(found - paths.begin());
                    if (index >= rootCount && (!openRoot || index < *openRoot))
                        openRoot = index;
                }
            }
            if (openRoot) {
                earliest = *openRoot;
                changed = true;
            }
            const bool refused = earliest == m_syntaxDeclinedRoots &&
                !m_syntaxDeclinedInputs.empty() &&
                std::ranges::all_of(m_syntaxDeclinedInputs, [&](auto id) { return sm.isLatestData(id); });
            if (changed && !refused && earliest > rootCount && earliest < paths.size())
                captureCount = earliest - rootCount;
        }
    }
    auto suffixOptions = parseOptions;
    auto pp = suffixOptions.getOrDefault<parsing::PreprocessorOptions>();
    pp.predefines.clear();
    pp.undefines.clear();
    suffixOptions.set(pp);
    auto suffix = captureCount
                      ? syntax::SyntaxTree::afterCachedPrefixWithCheckpoint(
                            prefix, buffers.subspan(rootCount), captureCount, suffixOptions)
                      : syntax::SyntaxTree::afterCachedPrefix(prefix, buffers.subspan(rootCount),
                                                             suffixOptions);
    probe.mark("captured-prefix-required-parse");
    auto tree = syntax::SyntaxTree::withCachedPrefixOwnedSuffix(prefix, std::move(suffix));
    probe.mark("captured-prefix-compose");
    if (captureCount) {
        check();
        // This tree is already required by the current analysis. Retain the
        // capture without walking its prefix while the first query is waiting.
        const auto retained = tree->getRetainedSyntaxBytes();
        if (retained >= baseSyntaxBytes && retained - baseSyntaxBytes <= syntaxBudget) {
            m_pendingSyntaxPrefix = tree;
            m_pendingSyntaxRoots = rootCount + captureCount;
            m_syntaxPrefixBytes = retained - baseSyntaxBytes;
            ++m_syntaxPrefixCaptures;
            probe.mark("captured-prefix-pending");
        }
        else {
            ++m_syntaxPrefixRefusals;
            m_syntaxDeclinedRoots = rootCount + captureCount;
            m_syntaxDeclinedInputs.clear();
            for (auto id : tree->getSourceBufferIds()) {
                auto root = id;
                while (auto from = sm.getIncludedFrom(root))
                    root = sm.getFullyExpandedLoc(from).buffer();
                auto found = std::ranges::find(paths, sm.getFullPath(root));
                if (found != paths.end() && size_t(found - paths.begin()) < m_syntaxDeclinedRoots)
                    m_syntaxDeclinedInputs.push_back(id);
            }
            probe.mark("captured-prefix-budget-refusal");
        }
    }
    ++m_scopeResumes;
    return tree;
}

bool ServerDriver::isTrustedScopeSource(const std::filesystem::path& path) const {
    return m_trustedScopes &&
           m_trustedScopes->files.contains(path.lexically_normal().generic_string());
}

rfl::Generic ServerDriver::scopeLibraryStatus() const {
    struct SyntaxReplay {
        bool enabled;
        uint64_t budgetBytes;
        size_t chargedBytes, pendingRoots, cachedRoots;
        uint64_t captures, hits, refusals, witnessFailures;
        bool reloadPending;
    };
    struct Status {
        bool enabled, prepared;
        uint64_t generation, consumers, syntaxResumes, namespaceDeclines;
        size_t chargedBytes, astBytes, fixedSourceFiles;
        std::vector<std::string> packages, definitions;
        bool hasUnitDeclarations = false;
        std::vector<FrozenInstanceCache::Selection> instances;
        std::vector<std::string> lastModelSharedRoots;
        bool instanceBodiesShared;
        ScopeBodyCache::Stats bodies;
        std::string reason;
        SyntaxReplay syntaxReplay;
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
               library && library->hasUnitDeclarations, m_scopeInstanceSelections,
               comp ? comp->lastModelSharedRoots() : std::vector<std::string>{}, bodies.hits != 0,
               bodies, m_scopeReason,
               SyntaxReplay{m_config.experimentalSyntaxPrefixCache.value(),
                   uint64_t(m_config.syntaxPrefixCacheMaxMiB.value()) * 1024 * 1024,
                   m_syntaxPrefixBytes, m_pendingSyntaxRoots, m_syntaxPrefixRoots,
                   m_syntaxPrefixCaptures, m_syntaxPrefixHits, m_syntaxPrefixRefusals,
                   m_syntaxPrefixWitnessFailures, hasStaleSyntaxPrefixInputs()}});
}
} // namespace server
