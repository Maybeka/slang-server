// SPDX-License-Identifier: MIT
#pragma once
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "slang/ast/Compilation.h"

namespace server {
struct TrustedUvmLibrary;

/// Independent native owners; never a chain of previous editing generations.
class ScopeBodyCache : public std::enable_shared_from_this<ScopeBodyCache> {
public:
    struct Stats {
        size_t budgetBytes = 0, baseBytes = 0, ownerBytes = 0, pinnedBytes = 0, owners = 0;
        uint64_t hits = 0, misses = 0, publications = 0, compactPublications = 0, evictions = 0,
                 declines = 0, ownerBuilds = 0, admissionHits = 0, capacitySkips = 0;
        size_t admissionBytes = 0, admissionRecords = 0;
        std::vector<std::string> retainedRoots;
    };
    ScopeBodyCache(std::shared_ptr<TrustedUvmLibrary> library, size_t budget,
                   std::vector<std::string> modules, std::vector<std::string> instances);
    void install(slang::ast::Compilation& compilation);
    Stats status() const;

private:
    struct Group {
        std::shared_ptr<slang::ast::Compilation> owner;
        std::unordered_map<std::string, const slang::ast::InstanceBodySymbol*> bodies;
        size_t bytes;
    };
    struct Pin {
        std::weak_ptr<slang::ast::Compilation> owner;
        size_t bytes;
    };
    std::shared_ptr<TrustedUvmLibrary> library;
    std::vector<std::string> modules, instances;
    std::list<Group> groups;
    std::vector<Pin> pins;
    // Admission advice only: no compiler, type, syntax or consumer ownership.
    std::list<std::string> rejectedContexts;
    Stats counters;
    bool selected(const slang::ast::InstanceBodySymbol& body) const;
    void lookup(slang::ast::Compilation&, slang::ast::InstanceBodySymbol&);
    void publish(std::shared_ptr<slang::ast::Compilation>);
    std::optional<Group> prepareGroup(std::shared_ptr<slang::ast::Compilation>);
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
};
} // namespace server
