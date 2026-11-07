// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "slang/ast/Compilation.h"

namespace slang::analysis { class AnalysisManager; }

namespace server {
struct TrustedUvmLibrary;
class SourceStorageRegistry;

/// Independent native owners; never a chain of previous editing generations.
class ScopeBodyCache : public std::enable_shared_from_this<ScopeBodyCache> {
public:
    struct Refusal {
        std::string category;
        uint64_t attempts = 0;
        std::vector<std::string> sampleRoots;
    };
    struct Stats {
        bool budgetUnlimited = false;
        size_t budgetBytes = 0, baseBytes = 0, ownerBytes = 0, pinnedBytes = 0, owners = 0;
        uint64_t hits = 0, misses = 0, publications = 0, compactPublications = 0, evictions = 0,
                 declines = 0, ownerBuilds = 0, admissionHits = 0, capacitySkips = 0;
        uint64_t publicationPathsIndexed = 0, publicationPathLookups = 0;
        size_t admissionBytes = 0, admissionRecords = 0;
        size_t analysisBytes = 0;
        uint64_t analysisHits = 0, proceduresReused = 0, proceduresRecomputed = 0;
        std::vector<std::string> retainedRoots;
        // Lifetime attempt counts, not unique bodies or current-model coverage.
        uint64_t refusalAttempts = 0, uncategorizedRefusals = 0;
        size_t refusalReportBytes = 0;
        bool refusalSamplesTruncated = false;
        std::vector<Refusal> refusals;
    };
    ScopeBodyCache(std::shared_ptr<TrustedUvmLibrary> library, size_t budget,
                   std::vector<std::string> modules, std::vector<std::string> instances,
                   std::shared_ptr<SourceStorageRegistry> sourceRegistry = {},
                   bool contextualInitial = false);
    void install(slang::ast::Compilation& compilation,
                 std::shared_ptr<const void> sourceLibraries = {}, bool independentInitial = false);
    Stats status() const;
    bool wantsInitialModel() const;
    std::shared_ptr<const slang::analysis::AnalysisManager> analysisFor(
        const slang::ast::Compilation& current);
    void retainAnalysis(std::shared_ptr<slang::ast::Compilation> current,
                        std::shared_ptr<slang::analysis::AnalysisManager> analysis);
    void noteProcedureReuse(size_t reused, size_t recomputed);
    bool releaseAnalysisSnapshots();

    void trackInitialSources(const std::shared_ptr<slang::SourceManager>& sources);

private:
    struct Group {
        std::shared_ptr<slang::ast::Compilation> owner;
        std::unordered_map<std::string, const slang::ast::InstanceBodySymbol*> bodies;
        size_t bytes;
        std::shared_ptr<const slang::analysis::AnalysisManager> analysis;
        size_t analysisBytes = 0;
    };
    struct Pin {
        std::weak_ptr<const void> owner;
        size_t bytes;
    };
    std::shared_ptr<TrustedUvmLibrary> library;
    std::shared_ptr<SourceStorageRegistry> sourceRegistry;
    std::vector<std::string> modules, instances;
    bool contextualInitial = false;
    std::list<Group> groups;
    std::vector<Pin> pins;
    // Admission advice only: no compiler, type, syntax or consumer ownership.
    std::list<std::string> rejectedContexts;
    Stats counters;
    struct RefusalCount { std::string_view category; uint64_t attempts = 0; };
    struct RefusalSample { std::array<char, 256> path{}; size_t category = 0; };
    struct RefusalReport {
        std::array<RefusalCount, 32> counts{};
        std::array<RefusalSample, 8> samples{};
        size_t countSize = 0, sampleSize = 0;
    } refusalReport;
    Stats accounting() const;
    void recordRefusal(std::string_view category, std::string_view path);
    bool certify(slang::ast::Compilation&, const slang::ast::InstanceBodySymbol&,
                 std::string_view path = {});
    bool selected(const slang::ast::InstanceBodySymbol& body) const;
    void lookup(slang::ast::Compilation&, slang::ast::InstanceBodySymbol&);
    void publish(std::shared_ptr<slang::ast::Compilation>, std::shared_ptr<const void>, bool);
    std::optional<Group> prepareGroup(std::shared_ptr<slang::ast::Compilation>,
                                      std::string_view admittedRoot = {});
    bool retain(Group, bool compact, size_t protectedGroups = 0);
    bool rejected(std::string_view context);
    void rememberRejection(std::string_view context, size_t stagedBytes);
    void trimRejections(size_t reservedBytes);
    bool capacityAdvice(const slang::ast::Compilation&, const slang::ast::InstanceBodySymbol&,
                        const std::vector<Group>& staged, size_t stagedBytes) const;
    bool contains(const slang::ast::Compilation&, const slang::ast::InstanceBodySymbol&) const;
};
struct ScopeBodyImport {
    std::weak_ptr<ScopeBodyCache> cache;
    std::shared_ptr<const void> sourceLibraries;
    bool independentInitial = false;
};
} // namespace server
