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
    std::shared_ptr<const void> sourceOwner;
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
    if (setting->sourceOwner)
        compilation.retainSourceOwner(setting->sourceOwner);
    for (const auto& name : setting->library->packages)
        compilation.importPackage(setting->library->compilation, name, sources);
    for (const auto& name : setting->library->definitions)
        compilation.importDefinition(setting->library->compilation, name, sources);
    if (auto bodies = options.get<ScopeBodyImport>())
        if (auto cache = bodies->cache.lock())
            cache->install(compilation);
    ++setting->library->consumers;
    return true;
}
} // namespace server
