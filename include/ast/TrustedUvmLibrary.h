// SPDX-License-Identifier: MIT
#pragma once
#include "ast/ScopeBodyCache.h"
#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "slang/ast/Compilation.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

namespace server {
/// One explicitly trusted generation. The source lease owns location storage and
/// library identities, independently of driver loaders and mutable consumer trees.
struct TrustedUvmLibrary {
    std::shared_ptr<const void> sourceOwner;
    std::shared_ptr<slang::ast::Compilation> compilation;
    std::shared_ptr<const slang::SourceManager::ImmutablePrefix> sources;
    std::vector<std::string> packages = {"uvm_pkg"};
    std::vector<std::string> definitions;
    bool hasUnitDeclarations = false;
    std::vector<std::string_view> unitLookupNames;
    std::vector<std::string_view> safeUnitWildcardImports;
    std::vector<std::string_view> unitDpiCNames;
    std::shared_ptr<slang::syntax::SyntaxTree> uvmParseTree;
    uint64_t generation = 0;
    size_t semanticBytes = 0;
    std::optional<std::string> shippedProfile;
    std::set<std::string> preparseDependencies;
    std::filesystem::path parseCachePath;
    std::string preparseKey;
    std::unordered_set<std::string> files;
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
    std::span<const std::shared_ptr<slang::syntax::SyntaxTree>> trees);

inline bool importTrustedUvm(
    slang::ast::Compilation& compilation, slang::SourceManager& sources, const slang::Bag& options,
    std::span<const std::shared_ptr<slang::syntax::SyntaxTree>> trees = {}) {
    auto setting = options.get<TrustedUvmImport>();
    if (!setting || !setting->library || !setting->enabled)
        return false;
    if (!trustedLibraryNamespaceMatches(*setting->library, sources, trees)) {
        ++setting->library->namespaceDeclines;
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
            cache->install(compilation, bodies->sourceLibraries);
    ++setting->library->consumers;
    return true;
}
} // namespace server
