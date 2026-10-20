// SPDX-License-Identifier: MIT
#include "document/FrozenSourceRegistry.h"

#include "diagnostics/DiagnosticSnapshotCodec.h"
#include "util/SlangExtensions.h"
#include <algorithm>
#include <rfl/json.hpp>
#include <stdexcept>
#include <unordered_set>

#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"

namespace server {
using namespace slang;
namespace {
bool hasLiteralMacroOverride(const syntax::SyntaxTree& prefix,
                             std::span<const SourceBuffer> suffix) {
    std::unordered_set<std::string_view> names;
    for (const auto* definition : prefix.getDefinedMacros())
        names.insert(definition->name.valueText());
    if (names.empty())
        return false;
    auto identifier = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '$';
    };
    auto whitespace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    for (const auto& buffer : suffix) {
        const std::string_view text(buffer.data.data(), buffer.data.size());
        size_t offset = 0;
        while ((offset = text.find('`', offset)) != std::string_view::npos) {
            const auto start = ++offset;
            while (offset < text.size() && identifier(text[offset]))
                ++offset;
            const auto directive = text.substr(start, offset - start);
            if (directive == "undefineall")
                return true;
            if (directive != "define" && directive != "undef")
                continue;
            while (offset < text.size() && whitespace(text[offset]))
                ++offset;
            const auto nameStart = offset;
            while (offset < text.size() && identifier(text[offset]))
                ++offset;
            if (names.contains(text.substr(nameStart, offset - nameStart)))
                return true;
        }
    }
    return false;
}
std::vector<std::filesystem::path> orderedRoots(const syntax::SyntaxTree& tree) {
    std::vector<std::filesystem::path> result;
    const auto& sm = tree.sourceManager();
    for (auto id : tree.getSourceBufferIds())
        if (!sm.getIncludedFrom(id).valid())
            result.push_back(sm.getFullPath(id));
    std::reverse(result.begin(), result.end());
    return result;
}
std::filesystem::path containingRoot(const SourceManager& sm, SourceLocation location) {
    location = sm.getFullyExpandedLoc(location);
    while (location.valid() && location != SourceLocation::NoLocation) {
        auto parent = sm.getIncludedFrom(location.buffer());
        if (!parent.valid())
            return sm.getFullPath(location.buffer());
        location = sm.getFullyExpandedLoc(parent);
    }
    return {};
}
std::string cacheKey(const FrozenSourcePlan& plan, std::span<const SourceBuffer> roots,
                     const SourceManager& sm, const Bag& options, bool contextOnly = false) {
    struct Key {
        std::vector<std::string> roots, packages, modules;
        std::vector<uint64_t> sortKeys;
        size_t boundary;
        uintptr_t library;
        bool independent;
        std::string options, cwd;
    } identity{.packages = contextOnly ? std::vector<std::string>{} : plan.packages,
               .modules = contextOnly ? std::vector<std::string>{} : plan.modules,
               .boundary = contextOnly ? 0 : plan.frozenRootCount,
               .library = reinterpret_cast<uintptr_t>(roots.front().library),
               .independent = plan.independent,
               .options = rfl::json::write(diagnostic_snapshot::captureOptions(options)),
               .cwd = std::filesystem::current_path().string()};
    for (size_t i = 0; i < plan.roots.size(); ++i) {
        identity.roots.push_back(plan.roots[i].string());
        if (!plan.independent)
            identity.sortKeys.push_back(sm.getSortKey(roots[i].id));
    }
    return rfl::json::write(identity);
}
} // namespace
FrozenSourcePlans planFrozenSources(std::span<const std::shared_ptr<syntax::SyntaxTree>> units,
                                    std::span<const std::string> packages,
                                    std::span<const std::string> modules) {
    FrozenSourcePlans result;
    std::vector<size_t> unitPlans(units.size(), SIZE_MAX);
    auto select = [&](const std::string& name, bool module) {
        FrozenSelectionStatus status;
        (module ? status.module : status.package) = name;
        const auto kind = module ? syntax::SyntaxKind::ModuleDeclaration
                                 : syntax::SyntaxKind::PackageDeclaration;
        const std::string label = module ? "module" : "package";
        size_t owner = 0, count = 0, boundary = 0;
        bool complete = true;
        for (size_t i = 0; i < units.size(); ++i) {
            auto roots = orderedRoots(*units[i]);
            for (const auto& [node, _] : units[i]->getMetadata().nodeMeta) {
                if (node->kind != kind || node->header->name.valueText() != name)
                    continue;
                ++count;
                owner = i;
                for (auto location :
                     {node->getFirstToken().location(), node->getLastToken().location()}) {
                    auto path = containingRoot(units[i]->sourceManager(), location);
                    auto root = std::find(roots.begin(), roots.end(), path);
                    complete &= root != roots.end();
                    if (root != roots.end())
                        boundary = std::max(boundary, size_t(root - roots.begin()) + 1);
                }
            }
        }
        if (name.empty() || !count)
            status.reason = label + " was not found in configured compilation inputs";
        else if (count != 1)
            status.reason = label + " selection is ambiguous across configured declarations";
        else if (!complete)
            status.reason = label + " does not map to complete native source roots";
        else {
            status.resolved = true;
            status.reason = "complete ordered input roots selected";
            auto& index = unitPlans[owner];
            if (index == SIZE_MAX) {
                index = result.units.size();
                result.units.push_back({.roots = orderedRoots(*units[owner])});
            }
            auto& plan = result.units[index];
            auto& names = module ? plan.modules : plan.packages;
            if (std::find(names.begin(), names.end(), name) == names.end())
                names.push_back(name);
            plan.frozenRootCount = std::max(plan.frozenRootCount, boundary);
        }
        result.selections.push_back(std::move(status));
    };
    for (const auto& name : packages)
        select(name, false);
    for (const auto& name : modules)
        select(name, true);
    return result;
}
void FrozenSourceRegistry::clear(std::string reason) {
    m_entries.clear();
    m_statistics.retainedBytes = 0;
    m_statistics.lastReason = std::move(reason);
}
bool FrozenSourceRegistry::offerNativePrefix(const FrozenSourcePlan& plan,
                                             std::span<const SourceBuffer> roots, SourceManager& sm,
                                             const Bag& options, Tree tree) {
    if (!m_budgetBytes || !tree || !plan.frozenRootCount || plan.frozenRootCount > roots.size() ||
        roots.size() != plan.roots.size() || &tree->sourceManager() != &sm ||
        tree->getSourceLibrary() != roots.front().library ||
        !tree->isPrefixResumeSafe().value_or(false) ||
        tree->getPrefixSourceEffects().value_or(true) || !hasValidBuffers(sm, tree) ||
        std::ranges::any_of(tree->diagnostics(), [](const auto& diag) { return diag.isError(); }))
        return false;
    if (!std::ranges::equal(orderedRoots(*tree),
                            std::span(plan.roots).first(plan.frozenRootCount)) ||
        rfl::json::write(diagnostic_snapshot::captureOptions(tree->options())) !=
            rfl::json::write(diagnostic_snapshot::captureOptions(options)))
        return false;
    for (size_t i = 0; i < roots.size(); ++i)
        if (sm.getFullPath(roots[i].id) != plan.roots[i] ||
            roots[i].library != roots.front().library)
            return false;
    std::vector<BufferID> treeRoots;
    flat_hash_set<BufferID> own;
    for (auto id : tree->getSourceBufferIds()) {
        own.insert(id);
        if (!sm.getIncludedFrom(id).valid())
            treeRoots.push_back(id);
    }
    std::reverse(treeRoots.begin(), treeRoots.end());
    for (size_t i = 0; i < treeRoots.size(); ++i)
        if (sm.getSortKey(treeRoots[i]) != sm.getSortKey(roots[i].id))
            return false;
    for (auto id : tree->getPredefineBufferIds())
        own.insert(id);
    auto external = [&](const syntax::DefineDirectiveSyntax* definition) {
        if (!definition)
            return false;
        auto location = sm.getFullyOriginalLoc(definition->name.location());
        return location.valid() && location != SourceLocation::NoLocation &&
               !own.contains(location.buffer());
    };
    for (auto definition : tree->getDefinedMacros())
        if (external(definition))
            return false;
    for (const auto& ref : tree->getMacroRefs())
        if (external(ref.definition))
            return false;
    auto binding = planFrozenSources(std::vector{tree}, plan.packages, plan.modules);
    if (std::ranges::any_of(binding.selections, [](const auto& item) { return !item.resolved; }))
        return false;
    if (m_owner != &sm) {
        clear("source manager changed");
        m_owner = &sm;
    }
    const auto key = cacheKey(plan, roots, sm, options);
    const auto context = cacheKey(plan, roots, sm, options, true);
    const auto bytes = tree->getRetainedSyntaxBytes() + tree->getRetainedSourceBytes() +
                       key.capacity() + context.capacity() + sizeof(Entry);
    if (bytes > m_budgetBytes)
        return false;
    for (const auto& entry : m_entries)
        if (entry.key == key)
            return false;
    while (!m_entries.empty() && bytes > m_budgetBytes - m_statistics.retainedBytes) {
        m_statistics.retainedBytes -= m_entries.front().bytes;
        m_entries.erase(m_entries.begin());
        ++m_statistics.evictions;
    }
    m_statistics.retainedBytes += bytes;
    ++m_statistics.nativeSeeds;
    m_entries.push_back({key, std::move(tree), bytes, true, context, plan.frozenRootCount});
    m_statistics.lastReason = "native checkpoint staged; current input witness still required";
    return true;
}
FrozenSourceRegistry::Tree FrozenSourceRegistry::prepare(const FrozenSourcePlan& plan,
                                                         std::span<const SourceBuffer> roots,
                                                         SourceManager& sm, const Bag& options,
                                                         const Witness& witness) {
    if (!witness || !plan.frozenRootCount || plan.frozenRootCount > roots.size() ||
        plan.roots.size() != roots.size())
        throw std::invalid_argument(
            "a complete source plan and current input witness are required");
    if (!m_budgetBytes) {
        decline("cache retention disabled by budget");
        return {};
    }
    if (m_owner != &sm) {
        clear("source manager changed");
        m_owner = &sm;
    }
    for (size_t i = 0; i < roots.size(); ++i)
        if (sm.getFullPath(roots[i].id) != plan.roots[i])
            throw std::invalid_argument("source root order differs from the selected plan");
    if (std::ranges::any_of(roots, [&](const auto& root) {
            return root.library != roots.front().library;
        }))
        throw std::invalid_argument("source libraries differ within the selected group");
    const auto key = cacheKey(plan, roots, sm, options);
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->key != key)
            continue;
        if (hasValidBuffers(sm, it->tree) && witness(*it->tree)) {
            if (it->nativeSeed) {
                ++m_statistics.nativeSeedUses;
                it->nativeSeed = false;
            }
            auto entry = std::move(*it);
            auto tree = entry.tree;
            m_entries.erase(it);
            m_entries.push_back(std::move(entry));
            ++m_statistics.hits;
            if (plan.independent)
                ++m_statistics.independentHits;
            m_statistics.lastReason = "current input witness accepted";
            return tree;
        }
        m_statistics.retainedBytes -= it->bytes;
        m_entries.erase(it);
        ++m_statistics.invalidations;
        m_statistics.lastReason = "source or lookup witness changed";
        break;
    }
    Tree tree;
    std::string seedKey;
    if (plan.frozenRootCount > 1) {
        const auto context = cacheKey(plan, roots, sm, options, true);
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->nativeRoots && !hasValidBuffers(sm, it->tree)) {
                m_statistics.retainedBytes -= it->bytes;
                it = m_entries.erase(it);
                ++m_statistics.invalidations;
            }
            else
                ++it;
        }
        const Entry* seed = nullptr;
        for (const auto& entry : m_entries) {
            if (entry.nativeRoots && entry.nativeRoots < plan.frozenRootCount &&
                entry.nativeContext == context && (!seed || entry.nativeRoots > seed->nativeRoots))
                seed = &entry;
        }
        if (seed && witness(*seed->tree)) {
            seedKey = seed->key;
            tree = syntax::SyntaxTree::extendCachedPrefix(
                seed->tree,
                roots.subspan(seed->nativeRoots, plan.frozenRootCount - seed->nativeRoots),
                options);
            ++m_statistics.nativeSeedUses;
        }
    }
    if (!tree)
        tree = syntax::SyntaxTree::fromBuffersWithCheckpoint(roots.first(plan.frozenRootCount), sm,
                                                             options);
    auto binding = planFrozenSources(std::vector{tree}, plan.packages, plan.modules);
    if (std::ranges::any_of(binding.selections, [](const auto& item) { return !item.resolved; })) {
        decline("selected scope declarations changed; native planning required");
        return {};
    }
    if (std::ranges::any_of(tree->diagnostics(),
                            [](const auto& diagnostic) { return diagnostic.isError(); }) ||
        !tree->isPrefixResumeSafe().value_or(false) ||
        tree->getPrefixSourceEffects().value_or(true) || !hasValidBuffers(sm, tree) ||
        !witness(*tree)) {
        ++m_statistics.declines;
        m_statistics.lastReason = "unsupported directives or unproven input witness";
        return {};
    }
    const auto bytes = tree->getRetainedSyntaxBytes() + tree->getRetainedSourceBytes() +
                       key.capacity() + sizeof(Entry);
    if (bytes > m_budgetBytes) {
        ++m_statistics.declines;
        m_statistics.lastReason = "prepared group exceeds the cache budget";
        return {};
    }
    if (!seedKey.empty()) {
        auto seed = std::ranges::find_if(m_entries,
                                         [&](const auto& entry) { return entry.key == seedKey; });
        if (seed != m_entries.end()) {
            m_statistics.retainedBytes -= seed->bytes;
            m_entries.erase(seed);
        }
    }
    while (!m_entries.empty() && bytes > m_budgetBytes - m_statistics.retainedBytes) {
        m_statistics.retainedBytes -= m_entries.front().bytes;
        m_entries.erase(m_entries.begin());
        ++m_statistics.evictions;
    }
    m_statistics.retainedBytes += bytes;
    ++m_statistics.preparations;
    if (plan.independent)
        ++m_statistics.independentPreparations;
    m_statistics.lastReason = "prepared complete input group";
    m_entries.push_back({key, tree, bytes});
    return tree;
}
FrozenSourceRegistry::Tree FrozenSourceRegistry::resume(const FrozenSourcePlan& plan,
                                                        std::span<const SourceBuffer> roots,
                                                        SourceManager& sm, const Bag& options,
                                                        const Witness& witness) {
    if (witness && m_owner == &sm && plan.frozenRootCount && plan.frozenRootCount < roots.size() &&
        roots.size() == plan.roots.size()) {
        const auto key = cacheKey(plan, roots, sm, options);
        const auto context = cacheKey(plan, roots, sm, options, true);
        for (const auto& entry : m_entries) {
            if ((entry.key == key ||
                 (entry.nativeRoots && entry.nativeRoots <= plan.frozenRootCount &&
                  entry.nativeContext == context)) &&
                hasLiteralMacroOverride(*entry.tree, roots.subspan(plan.frozenRootCount))) {
                // This is only an early native decline. Even stale macro names,
                // comments or inactive directives can cause at most a false miss.
                // Nothing is reused without prepare's complete current witness;
                // includes and indirect directives still get normal validation.
                decline("literal source may override a retained macro; native parsing required");
                ++m_statistics.preflightDeclines;
                return {};
            }
        }
    }
    auto prefix = prepare(plan, roots, sm, options, witness);
    if (!prefix || plan.frozenRootCount == roots.size())
        return prefix;
    auto suffixOptions = options;
    auto pp = suffixOptions.getOrDefault<parsing::PreprocessorOptions>();
    pp.predefines.clear();
    pp.captureUvmPrefix = false;
    suffixOptions.set(pp);
    auto suffix = syntax::SyntaxTree::afterCachedPrefix(prefix, roots.subspan(plan.frozenRootCount),
                                                        suffixOptions);
    validateUvmMacroEnvironment(*prefix, *suffix);
    auto tree = syntax::SyntaxTree::withCachedPrefix(prefix, suffix);
    ++m_statistics.resumes;
    return tree;
}
} // namespace server
