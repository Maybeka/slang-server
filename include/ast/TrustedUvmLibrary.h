// SPDX-License-Identifier: MIT
#pragma once
#include "ast/ScopeBodyCache.h"
#include "ast/TrustedImportStatus.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "slang/ast/Compilation.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

namespace server {
/// One explicitly trusted generation. The source lease owns location storage and
/// library identities, independently of driver loaders and mutable consumer trees.
struct TrustedUvmLibrary {
    // Exact immutable syntax / source-lifetime identities, not file freshness
    // certificates. Weak keys never keep an editing namespace or tree alive.
    struct NamespaceWitness {
        std::weak_ptr<const void> sourceOwner;
        const slang::SourceManager* sources;
        std::vector<std::weak_ptr<slang::syntax::SyntaxTree>> trees;
        bool compatible;
        std::string_view refusal;
    };
    mutable std::mutex namespaceWitnessMutex;
    mutable std::vector<NamespaceWitness> namespaceWitnesses;
    mutable std::atomic<uint64_t> namespaceWitnessBuilds = 0;
    mutable std::atomic<uint64_t> namespaceWitnessHits = 0;
    std::shared_ptr<const void> sourceOwner;
    std::shared_ptr<const struct ScopePreparationReport> preparationReport;
    std::shared_ptr<slang::ast::Compilation> compilation;
    std::shared_ptr<const slang::SourceManager::ImmutablePrefix> sources;
    std::vector<std::string> packages = {"uvm_pkg"};
    std::vector<std::string> definitions;
    bool hasUnitDeclarations = false;
    // A projected RTL owner borrows declarations across mutable source roots.
    // Recheck their expanded syntax and parser context in the current native unit.
    bool projectedDefinitions = false;
    // Syntax-only instance names may remain dormant for this parameter frame.
    // Every active descendant still needs the ordinary body certificate.
    std::vector<std::string_view> deferredDefinitionNames;
    // Names appearing only in introductions or labels cannot resolve to a
    // design unit. Package and ordinary unit-name checks use the full inventory.
    std::vector<std::string_view> definitionIndependentNames;
    std::vector<std::string_view> locallyBoundDefinitionNames;
    std::vector<std::string_view> localBindingPackages;
    std::vector<std::string_view> unitLookupNames;
    std::vector<std::string_view> safeUnitWildcardImports;
    // Kept alive by the projected views of their original compilation units.
    // These native packages have structural wildcard exports disjoint from
    // the provider's lookup names. The current declaration must still match.
    std::vector<const slang::syntax::ModuleDeclarationSyntax*> nativeWildcardPackages;
    std::vector<std::string_view> unitDpiCNames;
    std::shared_ptr<slang::syntax::SyntaxTree> uvmParseTree;
    uint64_t generation = 0;
    size_t semanticBytes = 0;
    std::optional<std::string> shippedProfile;
    std::set<std::string> preparseDependencies;
    std::filesystem::path parseCachePath;
    std::string preparseKey;
    std::unordered_set<std::string> files;
    // Text is fixed in these files, but unimported declarations bind natively.
    // URI-only private provider views are incomplete for queries in such files.
    std::unordered_set<std::string> nativeRemainderFiles;
    std::atomic<uint64_t> consumers = 0;
    std::atomic<uint64_t> namespaceDeclines = 0;
};

struct TrustedUvmImport {
    std::shared_ptr<TrustedUvmLibrary> library;
    // Option bags / retained syntax are templates, not editing-generation owners.
    // Semantic consumers acquire their lease when binding the template.
    std::weak_ptr<const void> sourceOwner;
    bool enabled = true;
};

/// Mutable global collisions and compilation-unit bindings outside fixed
/// definition providers require native source-order binding. Imported definitions
/// cannot resolve later mutable $unit declarations through their provider scope.
bool trustedLibraryNamespaceMatches(
    const TrustedUvmLibrary& library, const slang::SourceManager& sources,
    std::span<const std::shared_ptr<slang::syntax::SyntaxTree>> trees,
    std::string_view* refusal = nullptr);

/// Reuse a namespace relation only for the exact ordered trees and live source
/// lifetime that proved it. No cache is used without an owning source lease.
bool cachedTrustedLibraryNamespaceMatches(
    const TrustedUvmImport& setting, const slang::SourceManager& sources,
    std::span<const std::shared_ptr<slang::syntax::SyntaxTree>> trees,
    std::string_view* refusal = nullptr);

inline bool importTrustedUvm(
    slang::ast::Compilation& compilation, slang::SourceManager& sources, const slang::Bag& options,
    std::span<const std::shared_ptr<slang::syntax::SyntaxTree>> trees = {},
    TrustedImportStatus* result = nullptr) {
    if (result)
        *result = {"not-configured", 0, {}, {}};
    auto setting = options.get<TrustedUvmImport>();
    if (!setting)
        return false;
    if (result)
        *result = {"declined", setting->library ? setting->library->generation : 0,
                   "library-unavailable", {}};
    if (!setting->library)
        return false;
    if (!setting->enabled) {
        if (result)
            *result = {"disabled", setting->library->generation, {}, {}};
        return false;
    }
    std::string_view witnessFailure;
    if (!cachedTrustedLibraryNamespaceMatches(*setting, sources, trees,
                                              result ? &witnessFailure : nullptr)) {
        ++setting->library->namespaceDeclines;
        if (result) {
            result->failureCategory = "namespace-witness";
            result->witnessFailure = witnessFailure;
        }
        return false;
    }
    if (auto owner = setting->sourceOwner.lock())
        compilation.retainSourceOwner(std::move(owner));
    for (const auto& name : setting->library->packages)
        compilation.importPackage(setting->library->compilation, name, sources);
    for (const auto& name : setting->library->definitions)
        compilation.importDefinition(setting->library->compilation, name, sources);
    if (setting->library->hasUnitDeclarations)
        compilation.importCompilationUnitDeclarations(setting->library->compilation, sources);
    if (auto bodies = options.get<ScopeBodyImport>())
        if (auto cache = bodies->cache.lock())
            cache->install(compilation, bodies->sourceLibraries, bodies->independentInitial);
    ++setting->library->consumers;
    if (result)
        *result = {"imported", setting->library->generation, {}, {}};
    return true;
}
} // namespace server
