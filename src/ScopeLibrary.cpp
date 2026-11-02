// SPDX-License-Identifier: MIT
#include "ServerDriver.h"
#include "util/ModelOwnerTrace.h"
#include "util/SlangExtensions.h"
#include "util/UvmPhaseTrace.h"
#include "ast/TrustedUvmLibrary.h"
#include "ast/ScopePreparationReport.h"
#include "ast/ClosedTypeContext.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <limits>
#include <rfl/to_generic.hpp>

#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/ASTContext.h"
#include "slang/ast/Lookup.h"
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
    std::shared_ptr<ScopePreparationReport> report;
    std::unordered_set<std::string> nativeRemainderFiles;
    bool deferInstanceEdges = false;
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
        return sm.getFullPath(location.buffer()).lexically_normal().generic_string();
    };
    std::set<std::string> roots, knownRoots, files;
    for (auto id : owner->getSourceBufferIds())
        if (sm.isFileLoc(SourceLocation(id, 0)) && !sm.getFullPath(id).empty())
            knownRoots.insert(sm.getFullPath(id).lexically_normal().generic_string());
    for (auto member : selected) {
        roots.insert(rootPath(member->getFirstToken().location()));
        roots.insert(rootPath(member->getLastToken().location()));
        auto origins = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
            for (size_t i = 0; i < node.getChildCount(); ++i) {
                if (auto child = node.childNode(i))
                    self(self, *child);
                else if (auto token = node.childToken(i); token) {
                    auto expanded = rootPath(token.location());
                    if (knownRoots.contains(expanded))
                        roots.insert(std::move(expanded));
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

// A provider can span ordinary compilation units, but never moves declarations
// between them. Compute a greatest closed subset before binding any provider:
// unavailable dependencies and indivisible physical roots propagate rejection
// to parents, leaving independent descendants eligible.
std::optional<PackageClosure> rtlSubsetClosure(
    std::span<const std::shared_ptr<syntax::SyntaxTree>> owners,
    std::span<const std::string> seeds, ast::CompilationOptions options,
    std::shared_ptr<const void> sourceOwner, std::string_view& failure,
    bool isolateNamedUnitDeclarations = false) {
    using K = syntax::SyntaxKind;
    auto decline = [&](std::string_view category) -> std::optional<PackageClosure> {
        failure = category;
        return std::nullopt;
    };
    struct Node {
        size_t unit;
        const syntax::MemberSyntax* syntax;
        std::string name;
        std::set<std::string> dependencies, identifiers, roots, declarationRoots, localNames, scopedNames, instanceDependencies;
        std::set<size_t> imports;
        bool eligible;
        std::string_view refusal, refusalBinding = {};
        bool unitType = false, unitSubroutine = false;
        void reject(std::string_view category, std::string_view binding = {}) {
            if (eligible) { eligible = false; refusal = category; refusalBinding = binding; }
        }
    };
    std::vector<Node> nodes;
    std::map<std::string, std::vector<size_t>> names;
    std::set<std::string> globals, opaqueGlobals, declaredUnitGlobals;
    struct UnitImport {
        size_t unit;
        const syntax::MemberSyntax* syntax;
        std::set<std::string> packages, exports, roots;
        bool valid = true;
    };
    std::vector<UnitImport> unitImports;
    std::map<std::string, std::set<size_t>> rootUnits;
    std::map<std::string, std::set<std::string>> inputContexts;
    std::map<std::string, std::vector<const syntax::MemberSyntax*>> rootMembers;
    // Physical ownership and compilation-input context are distinct: a shared
    // includer does not make every included sibling an immutable dependency.
    auto rootPath = [](const SourceManager& sm, SourceLocation loc) {
        loc = sm.getFullyExpandedLoc(loc);
        return sm.getFullPath(loc.buffer()).lexically_normal().generic_string();
    };
    for (size_t u = 0; u < owners.size(); ++u) {
        const auto& owner = owners[u];
        if (owner->root().kind != K::CompilationUnit)
            return decline("compilation-unit-root");
        auto unitOptions = owner->options().getOrDefault<ast::CompilationOptions>();
        // Native trees may carry only parsing options. Missing compilation
        // options inherit the driver, exactly as Compilation::addSyntaxTree.
        if (!owner->options().get<ast::CompilationOptions>())
            unitOptions = options;
        unitOptions.flags |= ast::CompilationFlags::PackageAnalysis;
        unitOptions.topModules.clear();
        bool unitEligible = !owner->isLibraryUnit && !owner->getSourceLibrary() &&
                            unitOptions == options;
        std::string_view unitReason = unitEligible ? "" :
            (owner->isLibraryUnit || owner->getSourceLibrary() ? "source-library-context" :
                                                               "compilation-unit-options");
        const auto& sm = owner->sourceManager();
        std::set<std::string> knownRoots;
        for (auto id : owner->getSourceBufferIds())
            if (sm.isFileLoc(SourceLocation(id, 0)) && !sm.getFullPath(id).empty()) {
                auto path = rootPath(sm, SourceLocation(id, 0));
                knownRoots.insert(path);
                rootUnits[path].insert(u);
                auto input = SourceLocation(id, 0);
                while (auto from = sm.getIncludedFrom(input.buffer()))
                    input = sm.getFullyExpandedLoc(from);
                inputContexts[path].insert(rootPath(sm, input));
            }
        const auto& members = owner->root().as<syntax::CompilationUnitSyntax>().members;
        std::map<const syntax::MemberSyntax*, std::set<std::string>> declaredNames;
        std::unique_ptr<ast::Compilation> namespaceProbe;
        if (isolateNamedUnitDeclarations) {
            // Inspect names without visiting task / function bodies. These
            // declarations remain native; a retained node must not mention any
            // of their names, including through a deferred generic body.
            namespaceProbe = std::make_unique<ast::Compilation>(options);
            namespaceProbe->retainSourceOwner(sourceOwner);
            namespaceProbe->addSyntaxTree(owner);
            for (auto unit : namespaceProbe->getRoot().compilationUnits)
                for (const auto& symbol : unit->members())
                    if (!symbol.name.empty() && symbol.kind != ast::SymbolKind::Definition &&
                        symbol.kind != ast::SymbolKind::Package &&
                        symbol.kind != ast::SymbolKind::ExplicitImport) {
                        globals.emplace(symbol.name), opaqueGlobals.emplace(symbol.name),
                            declaredUnitGlobals.emplace(symbol.name);
                        if (auto declaration=namespaceProbe->findCompilationUnitDeclaration(symbol,
                                {members.begin(),members.size()}))
                            declaredNames[declaration].emplace(symbol.name);
                    }
        }
        std::set<const ast::PackageSymbol*> visitedImports;
        auto collectImportedNames = [&](auto&& self, const ast::PackageSymbol* package,
                                        std::set<std::string>& exports) -> bool {
            if (!package)
                return false;
            if (!visitedImports.insert(package).second)
                return true;
            for (const auto& symbol : package->members())
                if (!symbol.name.empty())
                    globals.emplace(symbol.name), opaqueGlobals.emplace(symbol.name),
                        exports.emplace(symbol.name);
            if (auto data = package->getWildcardImportData())
                for (auto import : data->wildcardImports)
                    if (!self(self, import->getPackage(), exports))
                        return false;
            return true;
        };
        for (auto member : members) {
            if (member->kind == K::PackageImportDeclaration) {
                UnitImport entry{u, member, {}, {},
                    {rootPath(sm, member->getFirstToken().location()),
                     rootPath(sm, member->getLastToken().location())}};
                for (auto item : member->as<syntax::PackageImportDeclarationSyntax>().items) {
                    entry.packages.emplace(item->package.valueText());
                    if (item->item.kind == parsing::TokenKind::Star) {
                        visitedImports.clear();
                        if (namespaceProbe && collectImportedNames(collectImportedNames,
                                namespaceProbe->getPackage(item->package.valueText()), entry.exports))
                            continue;
                        entry.valid = false;
                        if (unitEligible) unitReason = "unit-wildcard-import";
                        unitEligible = false;
                    }
                    else {
                        auto name = item->item.valueText();
                        globals.emplace(name), opaqueGlobals.emplace(name);
                        entry.exports.emplace(name);
                        if (namespaceProbe) {
                            auto package = namespaceProbe->getPackage(item->package.valueText());
                            entry.valid &= package && package->findForImport(name);
                        }
                    }
                }
                unitImports.push_back(std::move(entry));
            }
            else if (isolateNamedUnitDeclarations &&
                     (member->kind == K::DataDeclaration ||
                      member->kind == K::ParameterDeclarationStatement ||
                      member->kind == K::TypedefDeclaration ||
                      member->kind == K::ForwardTypedefDeclaration ||
                      member->kind == K::ClassDeclaration ||
                      member->kind == K::FunctionDeclaration ||
                      member->kind == K::TaskDeclaration || member->kind == K::DPIImport)) {
                // Only named bindings are omitted. Ambient constructs retain
                // the existing whole-unit refusal below.
            }
            else if (member->kind != K::ModuleDeclaration &&
                     member->kind != K::InterfaceDeclaration &&
                     member->kind != K::ProgramDeclaration &&
                     member->kind != K::PackageDeclaration &&
                     member->kind != K::EmptyMember && member->kind != K::BindDirective)
                { if (unitEligible) unitReason = "unit-declaration-context";
                  unitEligible = false; } // Unmodeled $unit bindings stay native.
        }
        for (auto member : members) {
            if (member->kind == K::EmptyMember)
                continue;
            std::set<std::string> roots{
                rootPath(sm, member->getFirstToken().location()),
                rootPath(sm, member->getLastToken().location())};
            auto declarationRoots = roots;
            auto origins = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
                for (size_t i = 0; i < node.getChildCount(); ++i) {
                    if (auto child = node.childNode(i))
                        self(self, *child);
                    else if (auto token = node.childToken(i); token) {
                        auto expanded = rootPath(sm, token.location());
                        if (knownRoots.contains(expanded)) {
                            declarationRoots.insert(expanded);
                            roots.insert(std::move(expanded));
                        }
                        auto path = rootPath(sm, sm.getFullyOriginalLoc(token.location()));
                        if (knownRoots.contains(path))
                            roots.insert(std::move(path));
                    }
                }
            };
            origins(origins, *member);
            // Macro definition roots are dependencies, not declaration owners.
            // Other consumers of the same macro must remain editable.
            for (const auto& root : declarationRoots)
                rootMembers[root].push_back(member);
            if (member->kind == K::PackageDeclaration) {
                auto name = member->as<syntax::ModuleDeclarationSyntax>().header->name.valueText();
                globals.emplace(name);
                if (!isolateNamedUnitDeclarations)
                    opaqueGlobals.emplace(name);
            }
            const bool unitType=isolateNamedUnitDeclarations &&
                (member->kind==K::ClassDeclaration || member->kind==K::TypedefDeclaration);
            const bool unitSubroutine=isolateNamedUnitDeclarations &&
                (member->kind==K::FunctionDeclaration || member->kind==K::TaskDeclaration);
            if (unitSubroutine && member->as<syntax::FunctionDeclarationSyntax>().prototype->name->kind!=K::IdentifierName)
                continue; // Out-of-line class bodies require their own complete binding unit.
            if (member->kind != K::ModuleDeclaration && member->kind != K::InterfaceDeclaration &&
                member->kind != K::ProgramDeclaration &&
                !(isolateNamedUnitDeclarations && member->kind == K::PackageDeclaration) && !unitType && !unitSubroutine)
                continue;
            auto name=member->kind==K::ClassDeclaration
                ? member->as<syntax::ClassDeclarationSyntax>().name.valueText()
                : member->kind==K::TypedefDeclaration
                ? member->as<syntax::TypedefDeclarationSyntax>().name.valueText()
                : unitSubroutine
                ? member->as<syntax::FunctionDeclarationSyntax>().prototype->name->as<syntax::IdentifierNameSyntax>().identifier.valueText()
                : member->as<syntax::ModuleDeclarationSyntax>().header->name.valueText();
            Node node{u, member, std::string(name),
                      {}, {}, std::move(roots), std::move(declarationRoots), {}, {}, {}, {}, unitEligible, unitReason};
            node.unitType=unitType;
            node.unitSubroutine=unitSubroutine;
            if (unitType || unitSubroutine) {
                node.localNames=declaredNames[member];
                node.localNames.insert(node.name);
                if (std::ranges::any_of(members,[&](const auto* other) {
                        return other->kind==K::ForwardTypedefDeclaration &&
                            other->template as<syntax::ForwardTypedefDeclarationSyntax>().name.valueText()==name;
                    }))
                    node.reject("forward-unit-type-context");
            }
            const auto meta = parsing::ParserMetadata::fromSyntax(*member);
            if (!meta.interfacePorts.empty() || !meta.virtualInterfaceTypes.empty() ||
                (!isolateNamedUnitDeclarations && !meta.namedPortTypes.empty()))
                node.reject("interface-or-named-port-context");
            for (const auto* instance : meta.globalInstances)
                (isolateNamedUnitDeclarations ? node.instanceDependencies : node.dependencies)
                    .emplace(instance->type.valueText());
            {
                // Recovery tokens can survive without a retained diagnostic.
                // Validate every declaration before constructing a combined view.
                const syntax::MemberSyntax* one[] = {member};
                try {
                    (void)syntax::SyntaxTree::selectCompilationUnitMembers(owner, one, true);
                }
                catch (const std::invalid_argument&) {
                    node.reject("syntax-error-root");
                }
            }
            if (member->kind == K::PackageDeclaration) {
                if (auto package = namespaceProbe->getPackage(node.name))
                    for (const auto& symbol : package->members())
                        if (!symbol.name.empty())
                            node.localNames.emplace(symbol.name);
            }
            if (isolateNamedUnitDeclarations) {
                // A::B can name a class (including a type parameter or std
                // class), not just a package. Classify known providers after
                // every native unit has been inventoried below.
                for (auto name : meta.namedPortTypes)
                    node.scopedNames.emplace(name->identifier.valueText());
                for (auto name : meta.classPackageNames)
                    node.scopedNames.emplace(name->identifier.valueText());
                for (auto import : meta.packageImports)
                    for (auto item : import->items)
                        node.dependencies.emplace(item->package.valueText());
            }
            auto inspect = [&](auto&& self, const syntax::SyntaxNode& syntax) -> void {
                if (isolateNamedUnitDeclarations && syntax.kind == K::UnitScope)
                    node.reject("explicit-unit-context");
                if (unitType && syntax.kind==K::ClassMethodPrototype &&
                    std::ranges::any_of(syntax.as<syntax::ClassMethodPrototypeSyntax>().qualifiers,
                        [](const auto& token){return token.kind==parsing::TokenKind::ExternKeyword;}))
                    node.reject("external-unit-method-context");
                if (((syntax.kind == K::ScopedName || syntax.kind == K::PackageImportDeclaration) &&
                     !isolateNamedUnitDeclarations) ||
                    ((syntax.kind == K::ModuleDeclaration || syntax.kind == K::InterfaceDeclaration ||
                      syntax.kind == K::ProgramDeclaration) && &syntax != member))
                    node.reject("scoped-or-nested-context");
                for (size_t i = 0; i < syntax.getChildCount(); ++i) {
                    if (auto child = syntax.childNode(i))
                        self(self, *child);
                    else if (auto token = syntax.childToken(i);
                             token && token.kind == parsing::TokenKind::Identifier)
                        node.identifiers.emplace(token.valueText());
                }
            };
            inspect(inspect, *member);
            if (unitType || unitSubroutine) {
                for (const auto& exported:node.localNames) {
                    globals.insert(exported);
                    names[exported].push_back(nodes.size());
                }
            }
            else {
                globals.insert(node.name);
                names[node.name].push_back(nodes.size());
            }
            nodes.push_back(std::move(node));
        }
    }
    if (isolateNamedUnitDeclarations)
        for (auto& node : nodes)
            for (size_t i=0; i<unitImports.size(); ++i) {
                const auto& import=unitImports[i];
                if (import.unit!=node.unit || std::ranges::none_of(import.exports,[&](const auto& name) {
                        return node.identifiers.contains(name) && !node.localNames.contains(name) &&
                               !declaredUnitGlobals.contains(name);
                    }))
                    continue;
                node.imports.insert(i);
                if (!import.valid) node.reject("unit-import-context");
                for (const auto& name:import.packages)
                    if (name!="std" || names.contains(name)) node.dependencies.insert(name);
                node.roots.insert(import.roots.begin(),import.roots.end());
            }
    for (auto& node : nodes)
        for (const auto& name : node.scopedNames)
            if (names.contains(name) && !node.localNames.contains(name))
                node.dependencies.insert(name);
    for (auto& node:nodes)
        for (const auto& name:node.identifiers) {
            auto found=names.find(name);
            if (!node.localNames.contains(name) && found!=names.end() && found->second.size()==1 &&
                (nodes[found->second.front()].unitType || nodes[found->second.front()].unitSubroutine) &&
                nodes[found->second.front()].unit==node.unit)
                node.dependencies.insert(name);
        }
    std::set<size_t> wanted;
    auto visit = [&](auto&& self, const std::string& name) -> void {
        auto found = names.find(name);
        if (found == names.end() || found->second.size() != 1)
            return;
        auto i = found->second.front();
        if (!wanted.insert(i).second)
            return;
        for (const auto& dependency : nodes[i].dependencies)
            self(self, dependency);
        for (const auto& dependency : nodes[i].instanceDependencies)
            self(self, dependency);
    };
    for (const auto& name : seeds)
        visit(visit, name);
    std::set<std::string> selectedDesignRoots;
    for (auto i:wanted)
        if (!nodes[i].unitType && !nodes[i].unitSubroutine)
            selectedDesignRoots.insert(nodes[i].declarationRoots.begin(),nodes[i].declarationRoots.end());
    std::set<std::string> selectedInputContexts;
    for (const auto& root : selectedDesignRoots)
        selectedInputContexts.insert(inputContexts[root].begin(), inputContexts[root].end());
    for (auto i:wanted)
        if (nodes[i].unitType || nodes[i].unitSubroutine)
            for (const auto& root:nodes[i].roots)
                if (!selectedDesignRoots.contains(root) &&
                    std::ranges::none_of(inputContexts[root], [&](const auto& input) {
                        return selectedInputContexts.contains(input);
                    }) && (nodes[i].unitSubroutine ||
                    std::ranges::any_of(rootMembers[root],[](const auto* member) {
                        return member->kind!=K::ClassDeclaration && member->kind!=K::TypedefDeclaration &&
                               member->kind!=K::PackageImportDeclaration;
                    })))
                    nodes[i].reject("unit-provider-root-context");
    // Dependencies may add pure package files, but must not turn an editable
    // consumer sharing that file into a fixed sibling during root expansion.
    std::set<const syntax::MemberSyntax*> dependencyMembers;
    for (auto i : wanted)
        dependencyMembers.insert(nodes[i].syntax);
    for (auto i : wanted)
        for (auto import:nodes[i].imports)
            dependencyMembers.insert(unitImports[import].syntax);
    for (auto i : wanted)
        for (auto import:nodes[i].imports)
            for (const auto& root:unitImports[import].roots)
                if (std::ranges::any_of(rootMembers[root],[&](const auto* member) {
                        return member->kind!=K::PackageImportDeclaration &&
                               !dependencyMembers.contains(member);
                    }))
                    nodes[i].reject("mixed-unit-import-root");
    for (auto i : wanted) {
        auto& node = nodes[i];
        if (node.syntax->kind != K::PackageDeclaration)
            continue;
        for (const auto& root : node.roots)
            if (std::ranges::any_of(rootMembers[root], [&](const auto* member) {
                    return member->kind != K::PackageDeclaration &&
                           !dependencyMembers.contains(member);
                }))
                node.reject("mixed-package-root");
    }
    // Selecting a stable declaration fixes its physical input roots. Close over
    // other module-like declarations in those same roots instead of treating a
    // harmless sibling as editable code. The root set is fixed before expansion:
    // a sibling must never pull an unrelated mutable provider into the library.
    std::set<std::string> fixedRoots;
    for (auto i : wanted)
        if (nodes[i].eligible)
            fixedRoots.insert(nodes[i].roots.begin(), nodes[i].roots.end());
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto& node = nodes[i];
        if (std::ranges::none_of(node.declarationRoots, [&](const auto& root) { return fixedRoots.contains(root); }))
            continue;
        wanted.insert(i);
        if (std::ranges::any_of(node.roots, [&](const auto& root) { return !fixedRoots.contains(root); }))
            node.reject("outside-fixed-root-context");
        else if (names[node.name].size() != 1)
            node.reject("ambiguous-declaration");
    }
    for (;;) {
        for (;;) {
            std::set<const syntax::MemberSyntax*> selected;
            for (auto i : wanted)
                if (nodes[i].eligible)
                    selected.insert(nodes[i].syntax);
            for (auto i:wanted)
                if (nodes[i].eligible)
                    for (auto import:nodes[i].imports)
                        selected.insert(unitImports[import].syntax);
            bool changed = false;
            for (auto i : wanted) {
                auto& node = nodes[i];
                if (!node.eligible)
                    continue;
                std::string_view refusal, refusedBinding;
                auto require = [&](bool valid, std::string_view category, std::string_view binding = {}) {
                    if (!valid && refusal.empty()) { refusal = category; refusedBinding = binding; }
                };
                for (const auto& name : node.dependencies) {
                    const auto found = names.find(name);
                    if (found == names.end()) require(false, "unavailable-dependency", name);
                    else if (found->second.size() != 1) require(false, "ambiguous-dependency", name);
                    else if (!wanted.contains(found->second.front()))
                        require(false, "outside-fixed-root-dependency", name);
                    else require(nodes[found->second.front()].eligible, "dependency-refused", name);
                }
                for (const auto& name : node.identifiers) {
                    if (!globals.contains(name) || node.localNames.contains(name)) continue;
                    const auto found = names.find(name);
                    if (node.instanceDependencies.contains(name) && !node.dependencies.contains(name) &&
                        !opaqueGlobals.contains(name))
                        continue; // Prove actual child bindings in the parameterized body, not syntax.
                    // An ordinary identifier inside a package cannot acquire
                    // a module-definition binding. Only actual scoped /
                    // instantiated provider references create those edges.
                    // Keep all $unit bindings conservative, including deferred
                    // class bodies; native binding validates the projection.
                    if (node.syntax->kind == K::PackageDeclaration &&
                        found != names.end() && !opaqueGlobals.contains(name) &&
                        !node.scopedNames.contains(name) && !node.dependencies.contains(name) &&
                        std::ranges::all_of(found->second, [&](auto i) {
                            return nodes[i].syntax->kind != K::PackageDeclaration;
                        }))
                        continue;
                    const bool retainedUnitBinding=found!=names.end() && found->second.size()==1 &&
                        nodes[found->second.front()].unit==node.unit &&
                        (nodes[found->second.front()].unitType || nodes[found->second.front()].unitSubroutine) &&
                        selected.contains(nodes[found->second.front()].syntax) && node.dependencies.contains(name);
                    if (retainedUnitBinding) continue;
                    const bool restoredImport = !declaredUnitGlobals.contains(name) &&
                        std::ranges::any_of(node.imports,[&](auto i) {
                            return unitImports[i].valid && unitImports[i].exports.contains(name);
                        });
                    if (restoredImport) continue;
                    if (opaqueGlobals.contains(name)) require(false, "opaque-unit-name", name);
                    else if (found == names.end()) require(false, "unavailable-global-name", name);
                    else if (found->second.size() != 1) require(false, "ambiguous-global-name", name);
                    else require(selected.contains(nodes[found->second.front()].syntax),
                                 "unclosed-global-name", name);
                }
                for (const auto& root : node.roots) {
                    // Macro text can be shared by independent native units;
                    // declaration ownership / nominal bindings cannot be merged.
                    if (node.declarationRoots.contains(root))
                        require(rootUnits[root].size() == 1, "shared-unit-root");
                    if (node.syntax->kind == K::PackageDeclaration)
                        require(std::ranges::all_of(rootMembers[root], [&](const auto* member) {
                            return selected.contains(member);
                        }), "mixed-package-root");
                }
                if (!refusal.empty()) {
                    node.reject(refusal, refusedBinding);
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
        // The native CU-declaration import API currently preserves one unit.
        // Omit affected nodes rather than merging namespaces or throwing away
        // independent providers that do not need those imports.
        std::set<size_t> retainedUnits;
        bool importsNeeded=false;
        for (auto i:wanted)
            if (nodes[i].eligible) {
                retainedUnits.insert(nodes[i].unit);
                importsNeeded |= !nodes[i].imports.empty() || nodes[i].unitType || nodes[i].unitSubroutine;
            }
        if (importsNeeded && (retainedUnits.size()>1 || owners.size()>1)) {
            for (auto i:wanted)
                if (nodes[i].eligible && (!nodes[i].imports.empty() || nodes[i].unitType || nodes[i].unitSubroutine))
                    nodes[i].reject("multi-unit-import-context");
            continue;
        }
        auto compilation = std::make_shared<ast::Compilation>(options);
        compilation->retainSourceOwner(sourceOwner);
        compilation->beginTrackingMissingPackageLookups();
        PackageClosure result{compilation, {}, {}, {}, {}};
        result.deferInstanceEdges=isolateNamedUnitDeclarations;
        std::set<const syntax::MemberSyntax*> retainedMembers;
        for (auto i : wanted)
            if (nodes[i].eligible)
                retainedMembers.insert(nodes[i].syntax);
        for (auto i:wanted)
            if (nodes[i].eligible)
                for (auto import:nodes[i].imports)
                    retainedMembers.insert(unitImports[import].syntax);
        std::set<std::string> partialRoots;
        for (auto i : wanted)
            if (nodes[i].eligible)
                for (const auto& root : nodes[i].roots)
                    if (std::ranges::any_of(rootMembers[root], [&](const auto* member) {
                            return !retainedMembers.contains(member);
                        }))
                        partialRoots.insert(root);
        for (size_t u = 0; u < owners.size(); ++u) {
            std::vector<const syntax::MemberSyntax*> selected;
            std::set<std::string> roots;
            for (auto i : wanted) {
                const auto& node = nodes[i];
                if (node.unit == u && node.eligible) {
                    selected.push_back(node.syntax);
                    roots.insert(node.roots.begin(), node.roots.end());
                    if (node.syntax->kind == K::PackageDeclaration)
                        result.packages.push_back(node.name);
                    else if (!node.unitType && !node.unitSubroutine)
                        result.definitions.push_back(node.name);
                }
            }
            if (selected.empty())
                continue;
            for (const auto& import:unitImports)
                if (import.unit==u && retainedMembers.contains(import.syntax)) {
                    selected.push_back(import.syntax);
                    roots.insert(import.roots.begin(),import.roots.end());
                }
            auto tree = syntax::SyntaxTree::selectCompilationUnitMembers(owners[u], selected, true);
            if (!result.tree)
                result.tree = tree;
            compilation->addSyntaxTree(tree);
            const auto& sm = owners[u]->sourceManager();
            for (auto id : tree->getSourceBufferIds()) {
                const auto root = rootPath(sm, SourceLocation(id, 0));
                if (roots.contains(root)) {
                    auto path = sm.getFullPath(id).lexically_normal().generic_string();
                    result.files.insert(path);
                    if (partialRoots.contains(root))
                        result.nativeRemainderFiles.insert(std::move(path));
                }
            }
        }
        if (!result.tree)
            return decline("no-safe-declarations");
        bool errors = false;
        std::set<std::string> rejectedRoots;
        std::set<size_t> rejectedDeclarations;
        const auto& diagnosticSources = owners.front()->sourceManager();
        auto physicalLocation = [&](SourceLocation location) {
            location = diagnosticSources.getFullyExpandedLoc(location);
            return location;
        };
        const auto& semanticDiagnostics = compilation->getSemanticDiagnostics();
        const auto missingPackages = compilation->endTrackingMissingPackageLookups();
        for (const auto& diagnostic : semanticDiagnostics) {
            if (!diagnostic.isError())
                continue;
            errors = true;
            if (!diagnostic.location || diagnostic.location == SourceLocation::NoLocation)
                return decline("unattributed-semantic-error"); // No safe local attribution.
            // Unique declaration ownership permits independent native remainders.
            // Overlapping macro expansions and global contexts stay root-wide.
            const auto point = physicalLocation(diagnostic.location);
            std::vector<size_t> matches;
            for (auto i : wanted) {
                const auto& node = nodes[i];
                if (!node.eligible && !rejectedDeclarations.contains(i))
                    continue;
                const auto range = node.syntax->sourceRange();
                if (!range.start() || !range.end())
                    continue;
                const auto first = physicalLocation(range.start());
                const auto last = physicalLocation(range.end());
                if (point.buffer() == first.buffer() && point.buffer() == last.buffer() &&
                    point.offset() >= first.offset() && point.offset() <= last.offset())
                    matches.push_back(i);
            }
            if (matches.size() == 1) {
                nodes[matches.front()].reject("semantic-error-declaration");
                rejectedDeclarations.insert(matches.front());
                continue;
            }
            const auto path = rootPath(owners.front()->sourceManager(), diagnostic.location);
            bool attributed = rejectedRoots.contains(path);
            for (auto i : wanted) {
                auto& node = nodes[i];
                if (node.eligible && node.roots.contains(path)) {
                    node.reject("semantic-error-root");
                    attributed = true;
                    rejectedRoots.insert(path);
                }
            }
            if (!attributed)
                return decline("unattributed-semantic-error"); // Never guess a declaration.
        }
        if (!errors) {
            std::vector<const syntax::MemberSyntax*> unitDeclarations;
            for (const auto& tree:compilation->getSyntaxTrees())
                for (auto member:tree->root().as<syntax::CompilationUnitSyntax>().members)
                    if (member->kind==K::ClassDeclaration || member->kind==K::TypedefDeclaration ||
                        member->kind==K::FunctionDeclaration || member->kind==K::TaskDeclaration ||
                        member->kind==K::PackageImportDeclaration)
                        unitDeclarations.push_back(member);
            bool mapped=true;
            if (!unitDeclarations.empty())
                for (auto unit:compilation->getRoot().compilationUnits)
                    for (const auto& symbol:unit->members())
                        if (!symbol.name.empty() && symbol.kind!=ast::SymbolKind::Definition &&
                            symbol.kind!=ast::SymbolKind::Package && symbol.location &&
                            symbol.location!=SourceLocation::NoLocation &&
                            !compilation->findCompilationUnitDeclaration(symbol,unitDeclarations))
                            mapped=false;
            if (!mapped) {
                bool removed=false;
                for (auto i:wanted)
                    if (nodes[i].eligible && (nodes[i].unitType || nodes[i].unitSubroutine ||
                                              !nodes[i].imports.empty())) {
                        nodes[i].reject("unit-export-ownership"); removed=true;
                    }
                if (removed) continue;
                return decline("unit-export-ownership");
            }
        }
        if (!errors) {
            // Native class lookup can speculatively try a package of the same
            // name before resolving a type parameter or class. Unknown guesses
            // are not dependency edges; actual errors were handled above.
            // A known provider that is absent from this projection is different.
            if (std::ranges::any_of(missingPackages, [&](const auto& name) {
                    return names.contains(name);
                }))
                return decline("unclosed-package-dependency");
            result.report = std::make_shared<ScopePreparationReport>();
            result.report->candidateDeclarations = wanted.size();
            for (auto i:wanted)
                if (nodes[i].eligible && nodes[i].unitType) ++result.report->preparedUnitTypes;
            for (auto i : wanted)
                if (!nodes[i].eligible)
                    result.report->exclude(nodes[i].refusal, nodes[i].name, nodes[i].refusalBinding);
            return result;
        }
        // Every retry excludes a declaration or an ambiguously attributed root.
        // Propagate dependency rejection before rebuilding a fresh provider;
        // the current native compilation retains remaining declarations and errors.
    }
}

struct ProjectedBufferWitness {
    bool physical = false;
    bool samePath = false;
};
using ProjectedBufferWitnesses =
    std::map<std::pair<uint64_t, uint64_t>, ProjectedBufferWitness>;

bool sameProjectedSyntax(const syntax::SyntaxNode& left, const syntax::SyntaxNode& right,
                         const SourceManager& aSources, const SourceManager& bSources,
                         bool relocatedParserDirective = false,
                         ProjectedBufferWitnesses* pathMatches = nullptr) {
    if (&left == &right)
        return true;
    if (left.kind != right.kind || left.getChildCount() != right.getChildCount())
        return false;
    // A large declaration can contain millions of tokens from a handful of
    // physical files. Compare each buffer pair's paths once, while retaining
    // the expanded/original location and offset checks for every token. This
    // memo is local to this comparison, so it cannot outlive a source namespace.
    ProjectedBufferWitnesses localPaths;
    if (!pathMatches)
        pathMatches = &localPaths;
    auto buffers = [&](SourceLocation a, SourceLocation b) -> const ProjectedBufferWitness& {
        const auto key = std::pair{uint64_t(a.buffer().getId()), uint64_t(b.buffer().getId())};
        auto [entry, inserted] = pathMatches->try_emplace(key);
        if (inserted) {
            auto& witness = entry->second;
            witness.physical = !aSources.isMacroLoc(a) && !bSources.isMacroLoc(b);
            witness.samePath = witness.physical &&
                               aSources.getFullPath(a.buffer()) == bSources.getFullPath(b.buffer());
        }
        return entry->second;
    };
    auto locationMatches = [&](SourceLocation a, SourceLocation b) {
        if (&aSources == &bSources && a == b)
            return true;
        const auto& witness = buffers(a, b);
        // Physical locations are already fully original and expanded. The
        // buffer kind is immutable, so do not acquire four source-manager locks
        // per physical token merely to rediscover this identity projection.
        if (witness.physical)
            return (relocatedParserDirective || a.offset() == b.offset()) && witness.samePath;
        auto same = [&](SourceLocation x, SourceLocation y) {
            if (!relocatedParserDirective && x.offset() != y.offset())
                return false;
            return buffers(x, y).samePath;
        };
        return same(aSources.getFullyExpandedLoc(a), bSources.getFullyExpandedLoc(b)) &&
               same(aSources.getFullyOriginalLoc(a), bSources.getFullyOriginalLoc(b));
    };
    for (size_t i = 0; i < left.getChildCount(); ++i) {
        if (auto child = left.childNode(i)) {
            auto other = right.childNode(i);
            if (!other || !sameProjectedSyntax(*child, *other, aSources, bSources,
                                                relocatedParserDirective, pathMatches))
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
                if (bool(an) != bool(bn))
                    return false;
                if (an) {
                    // A preceding root's trailing parser-state directive can be
                    // attached to this fixed declaration. Editing that other file
                    // can move the directive without moving any fixed token. Its
                    // spelling, source paths and effective declaration metadata
                    // remain strict witnesses; its byte offset is not a dependency.
                    // Macro, line-map and conditional directives are not admitted.
                    auto parserStateDirective = [](syntax::SyntaxKind kind) {
                        using K = syntax::SyntaxKind;
                        switch (kind) {
                            case K::ResetAllDirective:
                            case K::DefaultNetTypeDirective:
                            case K::TimeScaleDirective:
                            case K::UnconnectedDriveDirective:
                            case K::NoUnconnectedDriveDirective:
                            case K::CellDefineDirective:
                            case K::EndCellDefineDirective:
                                return true;
                            default:
                                return false;
                        }
                    };
                    auto outsideTokenFile = [](const SourceManager& sm,
                                               const syntax::SyntaxNode& directive,
                                               SourceLocation token) {
                        auto path = sm.getFullPath(sm.getFullyExpandedLoc(
                            directive.getFirstToken().location()).buffer());
                        auto tokenPath = sm.getFullPath(sm.getFullyExpandedLoc(token).buffer());
                        return !path.empty() && !tokenPath.empty() && path != tokenPath;
                    };
                    const bool relocate = parserStateDirective(an->kind) &&
                        outsideTokenFile(aSources, *an, a.location()) &&
                        outsideTokenFile(bSources, *bn, b.location());
                    if (!sameProjectedSyntax(*an, *bn, aSources, bSources, relocate, pathMatches))
                        return false;
                }
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
            return sources.getFullPath(location.buffer()).lexically_normal().generic_string();
        };
        std::set<std::string> roots, knownRoots;
        for (auto id : owner->getSourceBufferIds())
            if (sources.isFileLoc(SourceLocation(id, 0)) && !sources.getFullPath(id).empty())
                knownRoots.emplace(sources.getFullPath(id).lexically_normal().generic_string());
        for (auto member : selected) {
            roots.emplace(rootPath(member->getFirstToken().location()));
            roots.emplace(rootPath(member->getLastToken().location()));
            auto origins = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
                for (size_t i = 0; i < node.getChildCount(); ++i) {
                    if (auto child = node.childNode(i))
                        self(self, *child);
                    else if (auto token = node.childToken(i); token) {
                        auto expanded = rootPath(token.location());
                        if (knownRoots.contains(expanded))
                            roots.emplace(std::move(expanded));
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
                                    std::span<const std::shared_ptr<syntax::SyntaxTree>> trees,
                                    std::string_view* refusal) {
    if (refusal) *refusal = {};
    auto decline = [&](std::string_view category) {
        if (refusal) *refusal = category;
        return false;
    };
    std::set<std::string_view> seenDefinitions;
    std::set<const syntax::SyntaxNode*> seenUnitImports;
    std::map<const syntax::SyntaxTree*, const syntax::SyntaxTree*> unitCorrespondence;
    const syntax::SyntaxTree* currentTree = nullptr;
    auto bindUnit = [&](const syntax::SyntaxTree* provider) {
        auto [found, inserted] = unitCorrespondence.emplace(provider, currentTree);
        return inserted || found->second == currentTree || decline("compilation-unit-context");
    };
    auto providerTreeFor = [&](const syntax::SyntaxNode* node) -> const syntax::SyntaxTree* {
        for (const auto& tree : library.compilation->getSyntaxTrees())
            if (tree->root().kind == syntax::SyntaxKind::CompilationUnit &&
                std::ranges::any_of(tree->root().as<syntax::CompilationUnitSyntax>().members,
                                    [&](const auto* member) { return member == node; }))
                return tree.get();
        return nullptr;
    };
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
            if (library.projectedDefinitions) {
                    const auto& providerSources = *library.compilation->getSourceManager();
                    auto point = sources.getFullyExpandedLoc(member.getFirstToken().location());
                    for (const auto& tree : library.compilation->getSyntaxTrees()) {
                        if (tree->root().kind != syntax::SyntaxKind::CompilationUnit) continue;
                        for (auto original : tree->root().as<syntax::CompilationUnitSyntax>().members) {
                            if (original->kind != syntax::SyntaxKind::PackageImportDeclaration &&
                                original->kind != syntax::SyntaxKind::ClassDeclaration &&
                                original->kind != syntax::SyntaxKind::TypedefDeclaration &&
                                original->kind != syntax::SyntaxKind::FunctionDeclaration &&
                                original->kind != syntax::SyntaxKind::TaskDeclaration) continue;
                            auto before = providerSources.getFullyExpandedLoc(
                                original->getFirstToken().location());
                            if (providerSources.getFullPath(before.buffer()) != sources.getFullPath(point.buffer()) ||
                                before.offset() != point.offset()) continue;
                            if (!sameProjectedSyntax(*original, member, providerSources, sources))
                                return decline(original->kind==syntax::SyntaxKind::PackageImportDeclaration
                                    ? "unit-import-syntax" : "unit-type-syntax");
                            if (!bindUnit(tree.get())) return false;
                            seenUnitImports.insert(original);
                            return true;
                        }
                    }

            }
            if (library.projectedDefinitions) {
                using K=syntax::SyntaxKind;
                switch (member.kind) {
                    case K::DataDeclaration: case K::ParameterDeclarationStatement:
                    case K::TypedefDeclaration: case K::ForwardTypedefDeclaration:
                    case K::ClassDeclaration: case K::FunctionDeclaration: case K::TaskDeclaration: {
                        // A token superset also covers deferred class bodies,
                        // enum exports and declarations with several names.
                        // Ambient constructs and DPI's C namespace stay separate.
                        auto touches=[&](auto&& self,const syntax::SyntaxNode& node)->bool {
                            for (size_t i=0;i<node.getChildCount();++i) {
                                if (auto child=node.childNode(i)) {
                                    if (self(self,*child)) return true;
                                }
                                else if (auto token=node.childToken(i);
                                    token && token.kind==parsing::TokenKind::Identifier &&
                                    std::ranges::binary_search(library.unitLookupNames,token.valueText()))
                                    return true;
                            }
                            return false;
                        };
                        if (!touches(touches,member)) return true;
                        return decline("mutable-unit-binding");
                    }
                    default: break;
                }
            }
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
                if (library.projectedDefinitions)
                    return decline("mutable-unit-binding");
            }
            const auto location = sources.getFullyExpandedLoc(member.getFirstToken().location());
            const auto path = sources.getFullPath(location.buffer()).lexically_normal();
            const bool fixed = !path.empty() && library.files.contains(path.generic_string());
            return fixed || decline("mutable-unit-binding");
        }
        const auto& declaration = member.as<syntax::ModuleDeclarationSyntax>();
        const auto name = declaration.header->name.valueText();
        const auto& names = package ? library.packages : library.definitions;
        if (std::ranges::find(names, name) == names.end())
            return (definition && std::ranges::binary_search(library.deferredDefinitionNames,name)) ||
                   !library.projectedDefinitions ||
                   !std::ranges::binary_search(library.unitLookupNames, name) ||
                   decline("unclosed-global-name");
        const ast::Symbol* original = nullptr;
        if (package)
            original = library.compilation->getPackage(name);
        else
            original = library.compilation->tryGetDefinition(name, library.compilation->getRoot())
                           .definition;
        if (!original)
            return decline("missing-provider-declaration");
        const auto* ownerSources = library.compilation->getSourceManager();
        auto provider = ownerSources->getFullPath(
            ownerSources->getFullyExpandedLoc(original->location).buffer());
        auto current = sources.getFullPath(
            sources.getFullyExpandedLoc(declaration.header->name.location()).buffer());
        if (original->getSyntax() != &member && (current.empty() || current != provider))
            return decline("provider-path-conflict");
        if (library.projectedDefinitions) {
            auto tree = providerTreeFor(original->getSyntax());
            if (!tree) return decline("provider-metadata");
            if (!bindUnit(tree)) return false;
        }
        if (library.projectedDefinitions && definition) {
            if (!seenDefinitions.insert(name).second)
                return decline("duplicate-projected-definition");
            if (!original->getSyntax() || !currentTree)
                return decline("provider-metadata");
            const auto& providerTrees = library.compilation->getSyntaxTrees();
            auto providerTree = std::ranges::find_if(providerTrees, [&](const auto& tree) {
                return std::ranges::any_of(tree->getMetadata().nodeMeta, [&](const auto& entry) {
                    return entry.first == original->getSyntax();
                });
            });
            if (providerTree == providerTrees.end())
                return decline("provider-metadata");
            const auto& before = (*providerTree)->getMetadata().nodeMeta;
            const auto& after = currentTree->getMetadata().nodeMeta;
            auto a = std::ranges::find_if(before, [&](const auto& entry) {
                return entry.first == original->getSyntax();
            });
            auto b = std::ranges::find_if(after, [&](const auto& entry) {
                return entry.first == &member;
            });
            if (a == before.end() || b == after.end())
                return decline("provider-metadata");
            if (a->second.defaultNetType != b->second.defaultNetType ||
                a->second.unconnectedDrive != b->second.unconnectedDrive ||
                a->second.cellDefine != b->second.cellDefine ||
                a->second.timeScale != b->second.timeScale)
                return decline("parser-context");
            if (!sameProjectedSyntax(*original->getSyntax(), member, *ownerSources, sources))
                return decline("projected-syntax");
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
        const auto& providerSources = *library.compilation->getSourceManager();
        for (const auto& providerTree : library.compilation->getSyntaxTrees()) {
            if (providerTree->root().kind != syntax::SyntaxKind::CompilationUnit) continue;
            for (auto original : providerTree->root().as<syntax::CompilationUnitSyntax>().members) {
                if ((original->kind != syntax::SyntaxKind::PackageImportDeclaration &&
                     original->kind != syntax::SyntaxKind::ClassDeclaration &&
                     original->kind != syntax::SyntaxKind::TypedefDeclaration &&
                     original->kind != syntax::SyntaxKind::FunctionDeclaration &&
                     original->kind != syntax::SyntaxKind::TaskDeclaration) ||
                    seenUnitImports.contains(original)) continue;
                auto point = providerSources.getFullyExpandedLoc(original->getFirstToken().location());
                auto entry=point;
                while (auto parent=providerSources.getIncludedFrom(entry.buffer()))
                    entry=providerSources.getFullyExpandedLoc(parent);
                for (const auto& tree : trees)
                    for (auto id : tree->getSourceBufferIds())
                        if (sources.getFullPath(id) == providerSources.getFullPath(point.buffer()) ||
                            sources.getFullPath(id) == providerSources.getFullPath(entry.buffer()))
                            return decline(original->kind==syntax::SyntaxKind::PackageImportDeclaration
                                ? "missing-current-unit-import" : "missing-current-unit-type");
            }
        }
        // A direct provider import may precede adding consumer-only syntax.
        // Require the declaration when its physical input is actually present;
        // this also catches an earlier mutable conditional removing that module.
        for (const auto& name : library.definitions) {
            if (seenDefinitions.contains(name))
                continue;
            const auto* original = library.compilation->tryGetDefinition(
                name, library.compilation->getRoot()).definition;
            if (!original)
                return decline("missing-provider-declaration");
            const auto* providerSources = library.compilation->getSourceManager();
            const auto path = providerSources->getFullPath(
                providerSources->getFullyExpandedLoc(original->location).buffer());
            for (const auto& tree : trees)
                for (auto id : tree->getSourceBufferIds())
                    if (sources.getFullPath(id) == path)
                        return decline("missing-current-definition");
        }
    }
    return true;
}

bool cachedTrustedLibraryNamespaceMatches(
    const TrustedUvmImport& setting, const SourceManager& sources,
    std::span<const std::shared_ptr<syntax::SyntaxTree>> trees, std::string_view* refusal) {
    const auto& library = *setting.library;
    auto owner = setting.sourceOwner.lock();
    if (!owner)
        return trustedLibraryNamespaceMatches(library, sources, trees, refusal);
    {
        std::lock_guard lock(library.namespaceWitnessMutex);
        auto& witnesses = library.namespaceWitnesses;
        std::erase_if(witnesses, [](const auto& entry) {
            return entry.sourceOwner.expired() ||
                   std::ranges::any_of(entry.trees, [](const auto& tree) { return tree.expired(); });
        });
        for (const auto& entry : witnesses) {
            if (entry.sources != &sources || entry.sourceOwner.owner_before(owner) ||
                owner.owner_before(entry.sourceOwner) || entry.trees.size() != trees.size())
                continue;
            bool same = true;
            for (size_t i = 0; i < trees.size(); ++i)
                same &= entry.trees[i].lock() == trees[i];
            if (same) {
                ++library.namespaceWitnessHits;
                if (refusal)
                    *refusal = entry.refusal;
                return entry.compatible;
            }
        }
    }
    std::string_view reason;
    const bool compatible = trustedLibraryNamespaceMatches(library, sources, trees, &reason);
    ++library.namespaceWitnessBuilds;
    TrustedUvmLibrary::NamespaceWitness entry{owner, &sources, {}, compatible, reason};
    entry.trees.reserve(trees.size());
    for (const auto& tree : trees)
        entry.trees.emplace_back(tree);
    {
        std::lock_guard lock(library.namespaceWitnessMutex);
        // Only a few full / shallow views coexist. Bound bookkeeping even when
        // a client retains many old trees; eviction merely repeats the proof.
        if (library.namespaceWitnesses.size() >= 8)
            library.namespaceWitnesses.erase(library.namespaceWitnesses.begin());
        library.namespaceWitnesses.push_back(std::move(entry));
    }
    if (refusal)
        *refusal = reason;
    return compatible;
}

bool ServerDriver::hasCompatibleTrustedNamespace() const {
    const auto setting = options.get<TrustedUvmImport>();
    if (!setting || !setting->library)
        return true;
    std::vector<std::shared_ptr<syntax::SyntaxTree>> trees;
    trees.reserve(m_configuredUnits.size());
    for (const auto& unit : m_configuredUnits)
        trees.push_back(unit->getSyntaxTree());
    if (m_trustedNamespaceWitness &&
        m_trustedNamespaceWitness->library.lock() == setting->library &&
        m_trustedNamespaceWitness->units.size() == trees.size()) {
        bool same = true;
        for (size_t i = 0; i < trees.size(); ++i)
            same &= m_trustedNamespaceWitness->units[i].lock() == trees[i];
        if (same)
            return m_trustedNamespaceWitness->compatible;
    }
    TrustedNamespaceWitness witness{setting->library, {}, true};
    witness.units.reserve(trees.size());
    for (const auto& tree : trees) {
        witness.units.emplace_back(tree);
        const std::shared_ptr<syntax::SyntaxTree> current[] = {tree};
        if (witness.compatible &&
            !cachedTrustedLibraryNamespaceMatches(*setting, sm, current))
            witness.compatible = false;
    }
    const bool compatible = witness.compatible;
    m_trustedNamespaceWitness = std::move(witness);
    return compatible;
}

bool ServerDriver::prepareScopeLibrary() {
    if (m_trustedScopes) {
        m_scopePreparationReport = m_trustedScopes->preparationReport;
        options.set(TrustedUvmImport{m_trustedScopes, m_driverOwner->getSourceOwner()});
        if (m_scopeBodies)
            options.set(ScopeBodyImport{m_scopeBodies, driver.sourceLoader.getLibraryOwner()});
        m_scopeReason = "retained fixed package semantic generation";
        return true;
    }
    auto report = std::make_shared<ScopePreparationReport>();
    m_scopePreparationReport = report;
    auto decline = [&](std::string reason, std::string_view category = "preparation-context") {
        report->decline(category);
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
        return decline(reason + diagEngine.formatMessage(error), "native-error");
    };
    const auto config = m_config.frozenSources.value().value_or(Config::FrozenSourcesConfig{});
    if (m_config.experimentalFrozenSources.value() ||
        m_config.experimentalMethodScopeOverlay.value())
        return decline("scope semantics cannot combine with certified syntax or method overlays", "incompatible-experiment");
    auto modules = config.modules.value();
    bool contextualInitial = false;
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
                        item.instance, "unresolved-hierarchy-selection");
                // A closed native type needs provenance, not a second model.
                // Contextual or open types keep the reconstructed-owner route.
                ast::LookupResult lookup;
                ast::ASTContext context(planning.getRoot(), ast::LookupLocation::max);
                ast::Lookup::name(planning.parseName(item.instance), context,
                                 ast::LookupFlags::AllowUnnamedGenerate, lookup);
                if (auto instance = lookup.found ? lookup.found->as_if<ast::InstanceSymbol>() : nullptr)
                    for (auto parameter : instance->body.getParameters())
                        if (auto type = parameter->symbol.as_if<ast::TypeParameterSymbol>())
                            contextualInitial |= !ClosedTypeContext{}.add(
                                type->targetType.getType()).has_value();
                item.reason = "fixed definition closure planned; eligible integral subtrees use "
                              "independent owners";
            }
            for (const auto& name : selection.modules())
                if (std::ranges::find(modules, name) == modules.end())
                    modules.push_back(name);
        }
        catch (const std::exception& error) {
            return decline(std::string("cannot resolve fixed RTL subtree: ") + error.what(), "hierarchy-selection-error");
        }
    }
    if (config.packages.value().empty() && modules.empty())
        return decline("no stable scopes selected", "empty-selection");
    auto plans = planFrozenSources(driver.syntaxTrees, config.packages.value(), modules);
    if (auto unresolved = std::ranges::find_if(plans.selections,
                                               [](const auto& s) { return !s.resolved; });
        unresolved != plans.selections.end())
        return decline("fixed scope selection " +
                       (unresolved->module.empty() ? unresolved->package : unresolved->module) +
                       ": " + unresolved->reason, "unresolved-scope-selection");
    auto compilationOptions = options.getOrDefault<ast::CompilationOptions>();
    compilationOptions.flags |= ast::CompilationFlags::PackageAnalysis;
    compilationOptions.topModules.clear();
    std::shared_ptr<syntax::SyntaxTree> completePrefixTree;
    std::shared_ptr<ast::Compilation> completePrefixCompilation;
    auto supportedPrefixMember = [](const auto* member) {
        using K = syntax::SyntaxKind;
        switch (member->kind) {
            case K::ModuleDeclaration: case K::InterfaceDeclaration:
            case K::ProgramDeclaration: case K::PackageDeclaration:
            case K::PackageImportDeclaration: case K::DPIImport:
            case K::ClassDeclaration: case K::TypedefDeclaration:
            case K::ForwardTypedefDeclaration: case K::FunctionDeclaration:
            case K::TaskDeclaration: case K::DataDeclaration:
            case K::ParameterDeclarationStatement: case K::EmptyMember:
                return true;
            default: return false;
        }
    };
    const bool rtlOnly = config.packages.value().empty() && !modules.empty() && m_uvmEntryPath.empty();
    // A complete fixed physical prefix needs no declaration projection. Prove
    // this route first and keep that exact syntax / declaration compilation.
    // On refusal the isolated-subset routes below retain their existing scope.
    const bool wholeTop = config.modules.value().empty() && config.instances.value().size() == 1 &&
                          config.instances.value().front().find('.') == std::string::npos;
    if (rtlOnly && wholeTop && driver.syntaxTrees.size() == 1 && plans.units.size() == 1 &&
        plans.units.front().frozenRootCount && !driver.syntaxTrees.front()->isLibraryUnit &&
        !driver.syntaxTrees.front()->getSourceLibrary()) {
        try {
            std::vector<SourceBuffer> fixed;
            const auto& plan = plans.units.front();
            for (size_t n = 0; n < plan.frozenRootCount; ++n) {
                auto buffer = sm.readSource(plan.roots[n]);
                if (!buffer)
                    break;
                fixed.push_back(*buffer);
            }
            if (fixed.size() == plan.frozenRootCount) {
                auto candidate = syntax::SyntaxTree::fromBuffersWithCheckpoint(fixed, sm, options);
                if (candidate->isPrefixResumeSafe().value_or(false) &&
                    std::ranges::all_of(candidate->root().as<syntax::CompilationUnitSyntax>().members,
                                        supportedPrefixMember) &&
                    std::ranges::none_of(candidate->diagnostics(), [](const auto& d) { return d.isError(); })) {
                    auto proof = std::make_shared<ast::Compilation>(compilationOptions);
                    proof->retainSourceOwner(m_driverOwner->getSourceOwner());
                    proof->addSyntaxTree(candidate);
                    if (std::ranges::none_of(proof->getSemanticDiagnostics(),
                                            [](const auto& d) { return d.isError(); })) {
                        completePrefixTree = std::move(candidate);
                        completePrefixCompilation = std::move(proof);
                    }
                }
            }
        }
        catch (const std::exception&) {
            // An invalid prefix must still get the existing subset / Native path.
        }
    }
    std::optional<PackageClosure> closure;
    auto attemptIsolatedSubset = [&]() {
        auto packageSeeds = config.packages.value();
        if (m_config.experimentalUvmSemanticCache.value() && !m_uvmEntryPath.empty() &&
            std::ranges::find(packageSeeds, "uvm_pkg") == packageSeeds.end())
            packageSeeds.emplace_back("uvm_pkg");
        auto selected = packageSeeds;
        selected.insert(selected.end(), modules.begin(), modules.end());
        std::string_view failure;
        auto subset = rtlSubsetClosure(driver.syntaxTrees, selected, compilationOptions,
                                       m_driverOwner->getSourceOwner(), failure, true);
        if (subset && std::ranges::any_of(packageSeeds, [&](const auto& name) {
                return std::ranges::find(subset->packages, name) == subset->packages.end();
            })) {
            if (subset->report)
                for (const auto& refusal : subset->report->refusals)
                    if (std::ranges::any_of(packageSeeds, [&](const auto& name) {
                            return std::ranges::find(refusal.sampleDeclarations, name) !=
                                   refusal.sampleDeclarations.end();
                        })) {
                        report->attempt("closed-unit-subset", false, refusal.category);
                        return std::optional<PackageClosure>{};
                    }
            subset.reset();
            failure = "selected-package-dependency-refused";
        }
        report->attempt("closed-unit-subset", bool(subset), failure);
        return subset;
    };
    if (!completePrefixTree && rtlOnly && driver.syntaxTrees.size() == 1) {
        closure = rtlDeclarationClosure(driver.syntaxTrees.front(), modules, compilationOptions,
                                        m_driverOwner->getSourceOwner());
        report->attempt("rtl-closed", bool(closure), closure ? "" : "closed-proof-unavailable");
    }
    if (!completePrefixTree && rtlOnly && !closure) {
        std::string_view failure;
        closure = rtlSubsetClosure(driver.syntaxTrees, modules, compilationOptions,
                                   m_driverOwner->getSourceOwner(), failure);
        report->attempt("rtl-subset", bool(closure), failure);
    }
    bool includedSelection = false;
    for (const auto& tree : driver.syntaxTrees)
        for (const auto& entry : tree->getMetadata().nodeMeta) {
            const auto* node = entry.first;
            if (node->kind != syntax::SyntaxKind::ModuleDeclaration &&
                node->kind != syntax::SyntaxKind::InterfaceDeclaration &&
                node->kind != syntax::SyntaxKind::ProgramDeclaration)
                continue;
            const auto& decl = node->as<syntax::ModuleDeclarationSyntax>();
            if (std::ranges::find(modules, decl.header->name.valueText()) != modules.end()) {
                auto location = sm.getFullyExpandedLoc(decl.header->name.location());
                includedSelection |= bool(sm.getIncludedFrom(location.buffer()));
            }
        }
    if (!completePrefixTree && !closure && (includedSelection ||
        (!rtlOnly && (driver.syntaxTrees.size() > 1 || plans.units.size() != 1))))
        closure = attemptIsolatedSubset();
    if (!closure && driver.options.singleUnit.value_or(false) && driver.syntaxTrees.size() > 1)
        return decline("inherited compilation-unit preprocessing requires native analysis", "compilation-unit-preprocessing");
    if (!closure && (plans.units.size() != 1 || !plans.units.front().frozenRootCount))
        return decline("no closed RTL declaration subset; fixed scopes span configured units", "no-closed-declaration-subset");
    FrozenSourcePlan plan;
    if (plans.units.size() == 1)
        plan = plans.units.front();
    else {
        for (const auto& unit : plans.units)
            for (const auto& root : unit.roots)
                if (std::ranges::find(plan.roots, root) == plan.roots.end())
                    plan.roots.push_back(root);
    }
    if (!closure)
        for (const auto& tree : driver.syntaxTrees) {
            if (!tree->isLibraryUnit && !tree->getSourceLibrary())
                continue;
            for (auto id : tree->getSourceBufferIds())
                if (std::ranges::find(plan.roots, sm.getFullPath(id)) != plan.roots.end())
                    return decline("fixed scope source-library contexts require native analysis", "source-library-context");
        }
    std::vector<SourceBuffer> roots;
    if (!closure)
        for (size_t i = 0; i < plan.frozenRootCount; ++i) {
            auto buffer = sm.readSource(plan.roots[i]);
            if (!buffer)
                return decline("cannot read a fixed package root", "source-read");
            roots.push_back(*buffer);
        }
    if (closure) {
        bool separated = false;
        for (size_t i = 0; i < plan.roots.size(); ++i) {
            const bool fixed = closure->files.contains(plan.roots[i].lexically_normal().generic_string());
            separated |= (i < plan.frozenRootCount) != fixed;
        }
        // A fixed wrapper file can include both fixed providers and separate
        // editable consumers. Prefix promotion must not expand that physical
        // footprint merely because all top-level input paths are fixed.
        for (const auto& tree : driver.syntaxTrees)
            for (auto id : tree->getSourceBufferIds()) {
                if (!sm.isFileLoc(SourceLocation(id, 0)) || sm.getFullPath(id).empty())
                    continue;
                auto origin = SourceLocation(id, 0);
                while (auto from = sm.getIncludedFrom(origin.buffer()))
                    origin = sm.getFullyExpandedLoc(from);
                auto root = std::ranges::find(plan.roots, sm.getFullPath(origin.buffer()));
                if (root != plan.roots.end() &&
                    size_t(root - plan.roots.begin()) < plan.frozenRootCount &&
                    !closure->files.contains(sm.getFullPath(id).lexically_normal().generic_string()))
                    separated = true;
            }
        if (!separated && plans.units.size() == 1 && driver.syntaxTrees.size() == 1) {
            for (size_t i = 0; i < plan.frozenRootCount; ++i) {
                auto buffer = sm.readSource(plan.roots[i]);
                if (!buffer)
                    return decline("cannot read a fixed package root", "source-read");
                roots.push_back(*buffer);
            }
            bool completePrefix = closure->nativeRemainderFiles.empty();
            if (!completePrefix) {
                // Prefer the complete fixed namespace when the ordinary prefix
                // route can prepare it. A subset's conservative closure can
                // omit packages or context-dependent sibling definitions that
                // this route already supports. Never replace it with an invalid
                // prefix: declaration errors must retain native remainders.
                try {
                    auto candidate = syntax::SyntaxTree::fromBuffersWithCheckpoint(roots, sm, options);
                    auto proof = std::make_shared<ast::Compilation>(compilationOptions);
                    proof->retainSourceOwner(m_driverOwner->getSourceOwner());
                    proof->addSyntaxTree(candidate);
                    completePrefix = candidate->isPrefixResumeSafe().value_or(false) &&
                        std::ranges::all_of(candidate->root().as<syntax::CompilationUnitSyntax>().members,
                                            supportedPrefixMember) &&
                        std::ranges::none_of(candidate->diagnostics(), [](const auto& d) { return d.isError(); }) &&
                        std::ranges::none_of(proof->getSemanticDiagnostics(), [](const auto& d) { return d.isError(); });
                    if (completePrefix) {
                        completePrefixTree = std::move(candidate);
                        completePrefixCompilation = std::move(proof);
                    }
                }
                catch (const std::exception&) {
                    completePrefix = false;
                }
            }
            if (completePrefix)
                closure.reset(); // Preserve the existing contiguous-prefix fast path.
            else
                roots.clear();
        }
    }
    bool projectedRtl = closure.has_value();
    std::shared_ptr<syntax::SyntaxTree> prefix;
    try {
        if (projectedRtl)
            prefix = closure->tree;
        else if (completePrefixTree)
            prefix = completePrefixTree;
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
        return decline(std::string("package preprocessing is not reusable: ") + error.what(), "preprocessing-error");
    }
    if (!projectedRtl && !prefix->isPrefixResumeSafe().value_or(false))
        return decline("fixed package prefix has unsupported preprocessing state", "preprocessing-context");
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
                        "fixed scope roots contain unsupported compilation-unit declarations", "unit-declaration-context");
            }
        }
    }
    if (hasUnitDeclarations && driver.syntaxTrees.size() != 1 && !closure)
        return decline("fixed compilation-unit declarations require a single configured unit", "compilation-unit-context");
    if (std::set<std::string>(packages.begin(), packages.end()).size() != packages.size() ||
        std::set<std::string>(definitions.begin(), definitions.end()).size() != definitions.size())
        return decline("fixed scope dependency declarations are ambiguous; native analysis", "ambiguous-declarations");
    if (m_config.experimentalUvmSemanticCache.value() && !m_uvmEntryPath.empty() &&
        std::ranges::find(packages, "uvm_pkg") == packages.end())
        return decline("UVM and fixed packages must share one ordered package closure", "uvm-package-closure");
    auto compilation = projectedRtl ? closure->compilation : completePrefixCompilation
        ? completePrefixCompilation : std::make_shared<ast::Compilation>(compilationOptions);
    if (!projectedRtl && !completePrefixCompilation) {
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
                // An unrelated $unit task / initializer can invalidate a
                // contiguous prefix without being a dependency of any selected
                // declaration. Try the same declaration graph with those named
                // bindings kept native. Identifier collisions and ambient unit
                // contexts still refuse; never suppress their native errors.
                auto subset = attemptIsolatedSubset();
                if (subset) {
                    closure = std::move(subset);
                    projectedRtl = true;
                    prefix = closure->tree;
                }
                else {
                    return declineError("fixed package dependency closure has semantic errors", *error);
                }
            }
        }
    }
    bool selective = false;
    if (closure) {
        selective = std::set<std::string>(closure->packages.begin(), closure->packages.end()) !=
                        std::set<std::string>(packages.begin(), packages.end()) ||
                    !definitions.empty();
        for (size_t i = 0; i < plan.frozenRootCount; ++i)
            selective |= !closure->files.contains(
                plan.roots[i].lexically_normal().generic_string());
        compilation = closure->compilation;
        packages = closure->packages;
        definitions = closure->definitions;
    }
    bool providerHasUnitDeclarations=false;
    size_t retainedUnitTypes=0, retainedUnitSubroutines=0;
    for (const auto& tree:compilation->getSyntaxTrees())
        if (tree->root().kind==syntax::SyntaxKind::CompilationUnit)
            for (auto member:tree->root().as<syntax::CompilationUnitSyntax>().members) {
                using K=syntax::SyntaxKind;
                if (member->kind==K::ClassDeclaration || member->kind==K::TypedefDeclaration ||
                    member->kind==K::ForwardTypedefDeclaration) ++retainedUnitTypes;
                if (member->kind==K::FunctionDeclaration || member->kind==K::TaskDeclaration)
                    ++retainedUnitSubroutines;
                providerHasUnitDeclarations |= member->kind==K::PackageImportDeclaration ||
                    member->kind==K::ClassDeclaration || member->kind==K::TypedefDeclaration ||
                    member->kind==K::FunctionDeclaration || member->kind==K::TaskDeclaration;
            }
    if (providerHasUnitDeclarations && (compilation->getRoot().compilationUnits.size()!=1 ||
                                       driver.syntaxTrees.size()!=1))
        return decline("fixed unit declarations require one configured compilation unit", "compilation-unit-import-context");
    std::vector<std::string_view> deferredDefinitionNames;
    if (closure && closure->deferInstanceEdges) {
        for (const auto& tree:compilation->getSyntaxTrees())
            for (auto instance:tree->getMetadata().globalInstances)
                deferredDefinitionNames.push_back(instance->type.valueText());
        std::ranges::sort(deferredDefinitionNames);
        deferredDefinitionNames.erase(std::unique(deferredDefinitionNames.begin(),deferredDefinitionNames.end()),
                                      deferredDefinitionNames.end());
        std::set<std::string_view> eagerNames;
        auto collectEager=[&](auto&& self,const syntax::SyntaxNode& node)->void {
            for (size_t i=0;i<node.getChildCount();++i) {
                if (auto child=node.childNode(i)) self(self,*child);
                else if (auto token=node.childToken(i);
                    token && token.kind==parsing::TokenKind::Identifier &&
                    node.kind!=syntax::SyntaxKind::HierarchyInstantiation)
                    eagerNames.insert(token.valueText());
            }
        };
        // HierarchyInstantiation has exactly one direct identifier token: its
        // type. Audit all parameter / instance / attribute child nodes normally.
        // Matching token locations is insufficient for repeated macro arguments.
        for (const auto& tree:compilation->getSyntaxTrees()) collectEager(collectEager,tree->root());
        std::erase_if(deferredDefinitionNames,[&](auto name) {return eagerNames.contains(name);});
    }
    std::vector<std::string_view> unitLookupNames, safeUnitWildcardImports, unitDpiCNames;
    if (!definitions.empty() || hasUnitDeclarations || providerHasUnitDeclarations) {
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
        for (const auto& tree : compilation->getSyntaxTrees())
            collect(collect, tree->root(), nullptr);
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
    size_t retainedTreeBytes = 0;
    for (const auto& tree : compilation->getSyntaxTrees())
        retainedTreeBytes += tree->getRetainedSyntaxBytes() + tree->getRetainedSourceBytes();
    if (closure && closure->report) {
        report->candidateDeclarations = closure->report->candidateDeclarations;
        report->excludedDeclarations = closure->report->excludedDeclarations;
        report->uncategorizedDeclarations = closure->report->uncategorizedDeclarations;
        report->samples = closure->report->samples;
        report->bindings = closure->report->bindings;
        report->preparedUnitTypes = closure->report->preparedUnitTypes;
        report->samplesTruncated = closure->report->samplesTruncated;
        report->refusals = std::move(closure->report->refusals);
    }
    report->deferredModuleNames = deferredDefinitionNames.size();
    report->preparedUnitTypes=retainedUnitTypes;
    report->preparedUnitSubroutines=retainedUnitSubroutines;
    report->preparedDeclarations = packages.size() + definitions.size() + retainedUnitTypes + retainedUnitSubroutines;
    if (!report->candidateDeclarations)
        report->candidateDeclarations = report->preparedDeclarations;
    report->choose(projectedRtl ? (closure->report ? "rtl-subset" : "rtl-closed") :
                                 (closure ? "package-projection" : "ordered-prefix"));
    size_t remainderBytes = 0;
    if (closure) {
        const auto& paths = closure->nativeRemainderFiles;
        remainderBytes = sizeof(paths) + paths.bucket_count() * sizeof(void*) +
                         paths.size() * (sizeof(std::string) + 2 * sizeof(void*));
        for (const auto& path : paths)
            remainderBytes += path.capacity();
    }
    const auto charged = compilation->getTotalBytesAllocated() +
                         retainedTreeBytes + sm.getSourceLocationTableBytes() +
                         sm.getMemoryUsage() + report->chargedBytes() + remainderBytes +
                         (unitLookupNames.capacity() + safeUnitWildcardImports.capacity() +
                          unitDpiCNames.capacity() + deferredDefinitionNames.capacity()) *
                             sizeof(std::string_view);
    const bool unlimited = m_config.semanticCacheUnlimited.value();
    const auto semanticBudget = unlimited ? std::numeric_limits<size_t>::max()
                                         : size_t(config.maxMiB.value()) * 1024 * 1024;
    if (!unlimited && (!config.maxMiB.value() || config.maxMiB.value() > 512 ||
                      charged > semanticBudget))
        return decline("fixed package owner exceeds frozenSources.maxMiB", "budget");
    auto library = std::make_shared<TrustedUvmLibrary>();
    library->sourceOwner = m_driverOwner->getSourceOwner();
    library->preparationReport = report;
    if (closure)
        library->nativeRemainderFiles = std::move(closure->nativeRemainderFiles);
    library->compilation = std::move(compilation);
    library->packages = std::move(packages);
    library->definitions = std::move(definitions);
    library->hasUnitDeclarations = (hasUnitDeclarations && !closure) || providerHasUnitDeclarations;
    library->projectedDefinitions = projectedRtl;
    library->deferredDefinitionNames = std::move(deferredDefinitionNames);
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
    for (const auto& tree : library->compilation->getSyntaxTrees())
        for (auto id : tree->getSourceBufferIds())
            if (library->files.contains(sm.getFullPath(id).lexically_normal().generic_string()) &&
                std::ranges::find(fixedBuffers, id) == fixedBuffers.end())
                fixedBuffers.push_back(id);
    // The namespace preserves every existing source ID, but only fixed provider
    // paths enter the inherited lookup cache. Mutable overlays must be assignable
    // after retirement even when the declaration view retains their original CST.
    library->sources = sm.captureImmutablePrefix(fixedBuffers);
    std::shared_ptr<ScopeBodyCache> scopeBodies;
    if (!config.modules.value().empty() || !config.instances.value().empty()) {
        // A module selector covers arbitrary caller type identities. Explicit
        // instance selectors above can use their actual argument provenance.
        for (const auto& name : config.modules.value()) {
            const auto found = library->compilation->tryGetDefinition(
                name, library->compilation->getRoot());
            if (found.definition)
                contextualInitial |= std::ranges::any_of(
                    found.definition->as<ast::DefinitionSymbol>().parameters,
                    [](const auto& parameter) { return parameter.isTypeParam; });
        }
        scopeBodies = std::make_shared<ScopeBodyCache>(
            library, semanticBudget, config.modules.value(),
            config.instances.value(), m_sourceRegistry, contextualInitial);
        if (std::getenv("SLANG_SCOPE_BODY_TRACE"))
            fmt::print(stderr, "scope-body initial-plan\tcontextual={} direct={}\n",
                       contextualInitial, scopeBodies->wantsInitialModel());
        if (scopeBodies->status().baseBytes > semanticBudget)
            return decline("fixed scope owner and bounded report exceed frozenSources.maxMiB", "budget");
    }
    m_scopeRoots = plan.roots;
    m_scopeRootCount = selective ? 0 : plan.frozenRootCount;
    m_scopePrefix = selective ? nullptr : std::move(prefix);
    m_trustedScopes = std::move(library);
    if (modelOwnerTraceEnabled())
        traceModelOwner("scope-provider", m_trustedScopes->compilation.get(), nullptr,
                        m_trustedScopes->compilation->getTotalBytesAllocated());
    options.set(TrustedUvmImport{m_trustedScopes, m_driverOwner->getSourceOwner()});
    m_scopeBodies = std::move(scopeBodies);
    if (m_scopeBodies)
        options.set(ScopeBodyImport{m_scopeBodies, driver.sourceLoader.getLibraryOwner()});
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
    report->commit();
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
    struct Coverage {
        std::vector<std::string> requestedPackages, requestedModules, requestedInstances;
        size_t preparedDeclarations, retainedRoots, lastModelImportedRoots;
    };
    struct Status {
        bool enabled, prepared, budgetUnlimited;
        uint64_t generation, consumers, syntaxResumes, namespaceDeclines;
        size_t chargedBytes, astBytes, fixedSourceFiles;
        std::vector<std::string> packages, definitions;
        bool hasUnitDeclarations = false;
        std::vector<FrozenInstanceCache::Selection> instances;
        std::vector<std::string> lastModelSharedRoots;
        TrustedImportStatus lastModelLibraryImport;
        bool lastModelCurrent;
        bool instanceBodiesShared;
        ScopeBodyCache::Stats bodies;
        std::string reason;
        ScopePreparationReport preparation;
        size_t preparationReportBytes;
        std::vector<std::string> nativeRemainderFiles;
        Coverage coverage;
        SyntaxReplay syntaxReplay;
    };
    const auto& library = m_trustedScopes;
    const auto bodies = m_scopeBodies ? m_scopeBodies->status() : ScopeBodyCache::Stats{};
    const auto config = m_config.frozenSources.value().value_or(Config::FrozenSourcesConfig{});
    const auto& preparation = library && library->preparationReport
                                  ? library->preparationReport : m_scopePreparationReport;
    std::vector<std::string> nativeRemainders;
    if (library) {
        nativeRemainders.assign(library->nativeRemainderFiles.begin(),
                                library->nativeRemainderFiles.end());
        std::ranges::sort(nativeRemainders);
    }
    return rfl::to_generic(
        Status{m_config.experimentalScopeSemanticCache.value(), bool(library),
               m_config.semanticCacheUnlimited.value(),
               library ? library->generation : 0, library ? library->consumers.load() : 0,
               m_scopeResumes, library ? library->namespaceDeclines.load() : 0,
               library ? std::max(library->semanticBytes, bodies.baseBytes) + bodies.ownerBytes + bodies.pinnedBytes +
                             bodies.admissionBytes
                       : 0,
               library ? library->compilation->getTotalBytesAllocated() : 0,
               library ? library->files.size() : 0,
               library ? library->packages : std::vector<std::string>{},
               library ? library->definitions : std::vector<std::string>{},
               library && library->hasUnitDeclarations, m_scopeInstanceSelections,
               comp ? comp->lastModelSharedRoots() : std::vector<std::string>{},
               comp ? comp->lastModelLibraryImport() : TrustedImportStatus{},
               comp && comp->hasCurrentModel(),
               comp && !comp->lastModelSharedRoots().empty(),
               bodies, m_scopeReason,
               preparation ? *preparation : ScopePreparationReport{},
               preparation ? preparation->chargedBytes() : 0,
               std::move(nativeRemainders),
               Coverage{config.packages.value(), config.modules.value(), config.instances.value(),
                   preparation ? preparation->preparedDeclarations : 0,
                   bodies.retainedRoots.size(), comp ? comp->lastModelSharedRoots().size() : 0},
               SyntaxReplay{m_config.experimentalSyntaxPrefixCache.value(),
                   uint64_t(m_config.syntaxPrefixCacheMaxMiB.value()) * 1024 * 1024,
                   m_syntaxPrefixBytes, m_pendingSyntaxRoots, m_syntaxPrefixRoots,
                   m_syntaxPrefixCaptures, m_syntaxPrefixHits, m_syntaxPrefixRefusals,
                   m_syntaxPrefixWitnessFailures, hasStaleSyntaxPrefixInputs()}});
}
} // namespace server
