// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "slang/syntax/SyntaxTree.h"

namespace server {
class FrozenInputsChanged : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
/// A plan names scopes but freezes complete ordered input roots. Earlier roots are
/// dependencies, never reordered or extracted from their preprocessing context.
struct FrozenSourcePlan {
    std::vector<std::string> packages;
    std::vector<std::filesystem::path> roots;
    size_t frozenRootCount = 0;
    bool independent = false;
    std::vector<std::string> modules;
};
struct FrozenSelectionStatus {
    std::string package;
    std::string reason;
    bool resolved = false;
    std::string module;
};
struct FrozenSourcePlans {
    std::vector<FrozenSourcePlan> units;
    std::vector<FrozenSelectionStatus> selections;
};
FrozenSourcePlans planFrozenSources(
    std::span<const std::shared_ptr<slang::syntax::SyntaxTree>> units,
    std::span<const std::string> packages, std::span<const std::string> modules = {});

/// Driver-local owner for immutable syntax groups; no semantic pointers or global
/// SourceManager registry. A caller MUST provide an exact current-source and include
/// lookup witness. Buffer freshness alone is insufficient; no default witness exists.
/// ServerDriver supplies its exact recorded filesystem/overlay/command-input witness.
class FrozenSourceRegistry {
public:
    using Tree = std::shared_ptr<slang::syntax::SyntaxTree>;
    using Witness = std::function<bool(const slang::syntax::SyntaxTree&)>;
    struct Statistics {
        uint64_t preparations = 0, hits = 0, invalidations = 0, evictions = 0, declines = 0;
        uint64_t independentPreparations = 0, independentHits = 0, resumes = 0;
        uint64_t nativeSeeds = 0, nativeSeedUses = 0;
        uint64_t preflightDeclines = 0;
        uint64_t dependencyResumes = 0;
        size_t retainedBytes = 0;
        std::string lastReason;
    };
    explicit FrozenSourceRegistry(size_t budgetBytes = 64 * 1024 * 1024) :
        m_budgetBytes(budgetBytes) {}
    /// Stage a bounded native checkpoint without certifying filesystem freshness.
    /// It is never returned / extended until prepare's mandatory witness accepts it.
    bool offerNativePrefix(const FrozenSourcePlan& plan, std::span<const slang::SourceBuffer> roots,
                           slang::SourceManager& sourceManager, const slang::Bag& options,
                           Tree tree);
    Tree prepare(const FrozenSourcePlan& plan, std::span<const slang::SourceBuffer> roots,
                 slang::SourceManager& sourceManager, const slang::Bag& options,
                 const Witness& witness);
    Tree resume(const FrozenSourcePlan& plan, std::span<const slang::SourceBuffer> roots,
                slang::SourceManager& sourceManager, const slang::Bag& options,
                const Witness& witness);
    void clear(std::string reason);
    void noteDependencyResume() {
        ++m_statistics.dependencyResumes;
        m_statistics.lastReason = "configured UVM dependency prefix reused; selected root reparsed";
    }
    void decline(std::string reason) {
        ++m_statistics.declines;
        m_statistics.lastReason = std::move(reason);
    }
    const Statistics& statistics() const { return m_statistics; }

private:
    struct Entry {
        std::string key;
        Tree tree;
        size_t bytes;
        bool nativeSeed = false;
        std::string nativeContext;
        size_t nativeRoots = 0;
    };
    size_t m_budgetBytes;
    slang::SourceManager* m_owner = nullptr;
    std::vector<Entry> m_entries;
    Statistics m_statistics;
};
} // namespace server
