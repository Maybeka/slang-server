// SPDX-License-Identifier: MIT
#include "ServerDriver.h"
#include "ast/ServerCompilation.h"
#include "document/SlangDoc.h"
#include <algorithm>
#include <rfl/to_generic.hpp>

#include "slang/util/OS.h"

namespace server {
using namespace slang;
struct FrozenInstanceStatus {
    std::vector<FrozenInstanceCache::Selection> selections;
    FrozenInstanceCache::Statistics statistics;
    size_t budgetBytes = 0;
};
struct FrozenSourceContextStatus {
    size_t sourceBytes;
    size_t locationTableBytes;
    size_t locationEntries;
};

void ServerDriver::configureFrozenSources(
    std::span<const std::shared_ptr<syntax::SyntaxTree>> trees) {
    if (!isFrozenSourcesEnabled())
        return;
    m_frozenDeclinedUnits.clear();
    const auto config = m_config.frozenSources.value().value_or(Config::FrozenSourcesConfig{});
    if (!m_frozenSources) {
        const auto total = config.maxMiB.value() <= 512
                               ? size_t(config.maxMiB.value()) * 1024 * 1024
                               : 0;
        const auto instanceBudget = config.instances.value().empty() ? 0 : total / 4;
        m_frozenSources = std::make_unique<FrozenSourceRegistry>(total - instanceBudget);
    }
    else
        m_frozenSources->clear("native compilation inputs replaced");
    auto modules = config.modules.value();
    for (const auto& name : m_frozenInstanceModules)
        if (std::ranges::find(modules, name) == modules.end())
            modules.push_back(name);
    m_frozenPlans = planFrozenSources(trees, config.packages.value(), modules);
    updateFrozenAllowedPaths(trees);
    m_frozenCommandInputs.clear();
    for (const auto& [path, _] : driver.getCommandFileMetadata()) {
        SmallVector<char> bytes;
        auto error = OS::readFile(path, bytes);
        m_frozenCommandInputs.push_back({path, std::string(bytes.begin(), bytes.end()), error});
    }
    if (config.maxMiB.value() > 512)
        m_frozenSources->decline("maxMiB must be between 0 and 512; cache disabled");
    if (m_captureFrozenNativeSeed) {
        for (const auto& tree : trees) {
            if (tree->isLibraryUnit || tree->getSourceLibrary())
                continue;
            std::vector<SourceBuffer> roots;
            std::vector<std::filesystem::path> paths;
            for (auto id : tree->getSourceBufferIds())
                if (!sm.getIncludedFrom(id).valid())
                    roots.push_back({sm.getSourceText(id), tree->getSourceLibrary(), id});
            std::reverse(roots.begin(), roots.end());
            for (const auto& root : roots)
                paths.push_back(sm.getFullPath(root.id));
            if (std::ranges::none_of(m_frozenPlans.units, [&](const auto& plan) {
                    return plan.roots == paths && plan.frozenRootCount &&
                           (plan.frozenRootCount < paths.size() || paths.size() == 1 ||
                            (paths.size() > 1 && !m_uvmEntryPath.empty() &&
                             paths.front() == m_uvmEntryPath));
                }))
                continue;
            try {
                if (auto seed = syntax::SyntaxTree::preparedUvmPrefix(tree)) {
                    FrozenSourcePlan plan{.packages = {"uvm_pkg"},
                                          .roots = paths,
                                          .frozenRootCount = 1};
                    m_frozenSources->offerNativePrefix(plan, roots, sm, tree->options(),
                                                       std::move(seed));
                }
            }
            catch (const std::exception&) {
                // An unavailable native view does not disable ordinary group preparation.
            }
        }
    }
}
void ServerDriver::configureFrozenInstances() {
    if (!comp || !isFrozenSourcesEnabled() || !m_config.frozenSources.value())
        return;
    const auto config = *m_config.frozenSources.value();
    if (config.instances.value().empty())
        return;
    const auto total = config.maxMiB.value() <= 512 ? size_t(config.maxMiB.value()) * 1024 * 1024
                                                    : 0;
    comp->configureFrozenInstances(
        config.instances.value(), total / 4,
        [this](const lsp::RequestContext& ctx) {
            if (!m_frozenWitnessUsable)
                return false;
            if (auto reason = frozenInputWitnessFailure(
                    [&] { ctx.throwIfCancelled("during instance input certification"); }))
                throw lsp::RequestFailed("Frozen instance inputs changed; retry: " + *reason);
            return true;
        },
        [this](const std::vector<std::string>& modules) {
            if (m_frozenInstanceModules == modules)
                return;
            m_frozenInstanceModules = modules;
            auto context = comp->createActiveDesignContext({});
            configureFrozenSources(context.getAnalysis().compilation.getSyntaxTrees());
        },
        [this](const lsp::RequestContext& ctx) {
            return m_frozenWitnessUsable &&
                   ctx.hasInputWitness(m_snapshotOwner, m_snapshotRevision);
        });
}
std::optional<std::string> ServerDriver::frozenInputWitnessFailure(
    function_ref<void()> checkCancelled) const {
    if (!isFrozenSourcesEnabled())
        return std::nullopt;
    std::vector<std::filesystem::path> overlays;
    for (const auto& uri : m_openDocs) {
        auto found = docs.find(uri);
        if (found != docs.end())
            overlays.push_back(sm.getFullPath(found->second->getBuffer()));
    }
    if (auto reason = sm.sourceWitnessFailure(overlays, checkCancelled))
        return reason;
    if (auto reason = driver.sourceLoader.inputWitnessFailure(overlays, checkCancelled))
        return reason;
    if (checkCancelled)
        checkCancelled();
    if (m_config.uvm.value() && !m_uvmEntryPath.empty()) {
        const auto& library = *m_config.uvm.value();
        const auto& directory = m_uvmDirectory;
        auto available = [&](const std::filesystem::path& path) {
            std::error_code error;
            return sm.isCached(path) || std::filesystem::is_regular_file(path, error);
        };
        std::filesystem::path entry;
        if (library.useBundled.value())
            entry = directory / "uvm_pkg.sv";
        else if (library.entryFile.value()) {
            entry = *library.entryFile.value();
            if (entry.is_relative())
                entry = directory / entry;
        }
        else {
            auto direct = directory / "uvm_pkg.sv", nested = directory / "src/uvm_pkg.sv";
            if (available(direct) && available(nested))
                return "UVM automatic entry selection became ambiguous";
            entry = available(direct) ? direct : available(nested) ? nested : directory / "uvm.sv";
        }
        std::error_code error;
        if (std::filesystem::weakly_canonical(entry, error) != m_uvmEntryPath || error)
            return "configured UVM entry resolution changed";
    }
    for (const auto& input : m_frozenCommandInputs) {
        if (checkCancelled)
            checkCancelled();
        SmallVector<char> bytes;
        auto error = OS::readFile(input.path, bytes);
        if (error != input.error || std::string_view(bytes.data(), bytes.size()) != input.bytes)
            return "compilation command input changed: " + input.path.string();
    }
    return std::nullopt;
}
std::shared_ptr<syntax::SyntaxTree> ServerDriver::resumeFrozenSources(
    std::span<const std::filesystem::path> paths, std::span<const SourceBuffer> buffers,
    const Bag& options, bool nativeReplay) {
    if (!m_frozenSources || !m_frozenWitnessUsable)
        return {};
    auto plan = std::find_if(m_frozenPlans.units.begin(), m_frozenPlans.units.end(),
                             [&](const auto& plan) {
                                 return std::ranges::equal(plan.roots, paths);
                             });
    if (plan == m_frozenPlans.units.end())
        return {};
    if (nativeReplay) {
        m_frozenSources->decline("inherited preprocessing requires native replay");
        return {};
    }
    auto effectivePlan = *plan;
    bool dependencyOnly = false;
    if (comp && plan->frozenRootCount == buffers.size() && buffers.size() > 1 &&
        !m_uvmEntryPath.empty() && plan->roots.front() == m_uvmEntryPath) {
        // The selected root itself needs native parsing, but its explicitly
        // configured earlier UVM dependency can still retain a whole-root
        // checkpoint. Never extract or reorder the selected declaration.
        effectivePlan.packages = {"uvm_pkg"};
        effectivePlan.modules.clear();
        effectivePlan.frozenRootCount = 1;
        dependencyOnly = true;
    }
    if (comp && effectivePlan.frozenRootCount == buffers.size()) {
        // Every edit in this unit changes the selected group itself. Preparing a
        // replacement checkpoint has no suffix to amortize it over; the current
        // full-design document already retains its unchanged native tree.
        const std::string reason = "full-design group has no editable suffix; native parsing";
        m_frozenSources->decline(reason);
        m_frozenDeclinedUnits[plan->roots] = reason;
        return {};
    }
    try {
        auto witness = [&](const syntax::SyntaxTree&) {
            if (auto reason = frozenInputWitnessFailure()) {
                if (reason->starts_with("source context cannot"))
                    throw std::runtime_error(*reason);
                else
                    throw FrozenInputsChanged(*reason);
            }
            return true;
        };
        auto tree = m_frozenSources->resume(effectivePlan, buffers, sm, options, witness);
        // Certify again before publishing a new composition. A concurrent disk
        // change cannot turn a declined cache into a stale native parse.
        if (tree) {
            witness(*tree);
            if (dependencyOnly)
                m_frozenSources->noteDependencyResume();
            m_frozenDeclinedUnits.erase(plan->roots);
        }
        else
            m_frozenDeclinedUnits[plan->roots] = m_frozenSources->statistics().lastReason;
        return tree;
    }
    catch (const lsp::RequestCancelled&) {
        throw;
    }
    catch (const FrozenInputsChanged& error) {
        throw lsp::RequestFailed("Frozen inputs changed during analysis; retry: " +
                                 std::string(error.what()));
    }
    catch (const std::exception& error) {
        m_frozenSources->decline(error.what());
        m_frozenDeclinedUnits[plan->roots] = error.what();
        return {};
    }
}
rfl::Generic ServerDriver::frozenSourcesStatus() const {
    struct Unit {
        std::vector<std::string> packages, modules;
        std::vector<std::string> orderedRoots, selectedRoots;
        std::optional<std::string> workerNativeReason;
    };
    struct Status {
        bool enabled;
        std::vector<FrozenSelectionStatus> selections;
        std::vector<Unit> units;
        FrozenSourceRegistry::Statistics statistics;
        uint32_t configuredMaxMiB;
        bool budgetValid;
        FrozenSourceContextStatus sourceContext;
        std::string mode;
        std::optional<FrozenInstanceStatus> instances;
    } result{.enabled = isFrozenSourcesEnabled(),
             .selections = m_frozenPlans.selections,
             .statistics = m_frozenSources ? m_frozenSources->statistics()
                                           : FrozenSourceRegistry::Statistics{},
             .configuredMaxMiB = m_config.frozenSources.value()
                                     ? m_config.frozenSources.value()->maxMiB.value()
                                     : 64,
             .budgetValid = !m_config.frozenSources.value() ||
                            m_config.frozenSources.value()->maxMiB.value() <= 512,
             .sourceContext = {sm.getMemoryUsage(), sm.getSourceLocationTableBytes(),
                               sm.getSourceLocationEntryCount()},
             .mode = !isFrozenSourcesEnabled()     ? "disabled"
                     : !m_frozenWitnessUsable      ? "native: input witness unavailable"
                     : m_frozenPlans.units.empty() ? "native: no resolved source group"
                     : comp ? "complete-root design syntax; semantics rebound"
                            : "complete-root scope syntax"};
    for (const auto& plan : m_frozenPlans.units) {
        Unit unit{.packages = plan.packages, .modules = plan.modules};
        if (auto declined = m_frozenDeclinedUnits.find(plan.roots);
            declined != m_frozenDeclinedUnits.end())
            unit.workerNativeReason = declined->second;
        for (size_t i = 0; i < plan.roots.size(); ++i) {
            unit.orderedRoots.push_back(plan.roots[i].string());
            if (i < plan.frozenRootCount)
                unit.selectedRoots.push_back(plan.roots[i].string());
        }
        result.units.push_back(std::move(unit));
    }
    if (comp && comp->frozenInstances()) {
        const auto& cache = *comp->frozenInstances();
        result.instances = FrozenInstanceStatus{cache.selections(), cache.statistics(),
                                                cache.budgetBytes()};
    }
    else if (m_config.frozenSources.value() &&
             !m_config.frozenSources.value()->instances.value().empty()) {
        result.instances.emplace();
        for (const auto& path : m_config.frozenSources.value()->instances.value())
            result.instances->selections.push_back(
                {.instance = path,
                 .reason = isFrozenSourcesEnabled() ? "instance selection requires a full design"
                                                    : "instance experiment is disabled"});
    }
    return rfl::to_generic(result);
}
void ServerDriver::refreshFrozenSources() {
    m_frozenDeclinedUnits.clear();
    if (m_frozenSources)
        m_frozenSources->clear("manual refresh");
    completions.invalidateParsedLibraryCache();
    if (comp) {
        comp->clearFrozenInstances("manual refresh or input witness invalidation");
        comp->invalidate();
    }
    for (auto& [_, document] : docs)
        document->invalidateSyntax();
    for (auto& document : m_configuredUnits)
        document->invalidateSyntax();
    m_configuredUnitRevision = UINT64_MAX;
    invalidateAnalysesAndRefreshClient();
}
void ServerDriver::setFrozenWitnessFailure(const std::optional<std::string>& reason) {
    if (reason && m_frozenWitnessUsable) {
        refreshFrozenSources();
        if (m_frozenSources)
            m_frozenSources->decline(*reason);
    }
    m_frozenWitnessUsable = !reason;
}
void ServerDriver::updateFrozenSourcePlans(std::span<const std::filesystem::path> paths,
                                           const std::shared_ptr<syntax::SyntaxTree>& tree) {
    if (!m_frozenSources)
        return;
    std::vector<std::shared_ptr<syntax::SyntaxTree>> current;
    bool replaced = false;
    for (const auto& unit : m_configuredUnits) {
        if (std::ranges::equal(unit->getRootPaths(), paths)) {
            current.push_back(tree);
            replaced = true;
        }
        else if (auto cached = unit->getCachedSyntaxTree())
            current.push_back(std::move(cached));
    }
    if (!replaced)
        return;
    const auto config = m_config.frozenSources.value().value_or(Config::FrozenSourcesConfig{});
    auto modules = config.modules.value();
    for (const auto& name : m_frozenInstanceModules)
        if (std::ranges::find(modules, name) == modules.end())
            modules.push_back(name);
    auto plans = planFrozenSources(current, config.packages.value(), modules);
    const auto same = [](const FrozenSourcePlan& a, const FrozenSourcePlan& b) {
        return a.packages == b.packages && a.modules == b.modules && a.roots == b.roots &&
               a.frozenRootCount == b.frozenRootCount;
    };
    if (!std::ranges::equal(plans.units, m_frozenPlans.units, same))
        m_frozenSources->clear("scope selections changed");
    m_frozenPlans = std::move(plans);
    updateFrozenAllowedPaths(current);
}

void ServerDriver::updateFrozenAllowedPaths(
    std::span<const std::shared_ptr<syntax::SyntaxTree>> trees) {
    std::set<std::string> allowed;
    for (const auto& tree : trees) {
        for (auto buffer : tree->getSourceBufferIds()) {
            auto root = buffer;
            while (auto parent = sm.getIncludedFrom(root))
                root = sm.getFullyExpandedLoc(parent).buffer();
            auto path = sm.getFullPath(root);
            for (const auto& plan : m_frozenPlans.units) {
                auto end = plan.roots.begin() + plan.frozenRootCount;
                if (std::find(plan.roots.begin(), end, path) != end)
                    allowed.insert(sm.getFullPath(buffer).string());
            }
        }
    }
    if (allowed != m_frozenAllowedPaths && m_frozenSources)
        m_frozenSources->clear("selected source dependency set changed");
    m_frozenAllowedPaths = std::move(allowed);
}

std::shared_ptr<syntax::SyntaxTree> ServerDriver::getFrozenIndependentTree(std::string_view path,
                                                                           const Bag& options) {
    if (!m_frozenSources || !m_frozenWitnessUsable || comp)
        return {};
    auto buffer = sm.readSource(std::filesystem::path(path));
    if (!buffer || !m_frozenAllowedPaths.contains(sm.getFullPath(buffer->id).string()))
        return {};
    // This is fromFile's independent CLI macro context, not the package's
    // compilation-unit checkpoint. It has a distinct key and the same total budget.
    FrozenSourcePlan plan{.roots = {sm.getFullPath(buffer->id)},
                          .frozenRootCount = 1,
                          .independent = true};
    try {
        auto witness = [&](const syntax::SyntaxTree&) {
            if (auto reason = frozenInputWitnessFailure()) {
                if (reason->starts_with("source context cannot"))
                    throw std::runtime_error(*reason);
                throw FrozenInputsChanged(*reason);
            }
            return true;
        };
        return m_frozenSources->prepare(plan, std::span(&*buffer, 1), sm, options, witness);
    }
    catch (const lsp::RequestCancelled&) {
        throw;
    }
    catch (const FrozenInputsChanged& error) {
        throw lsp::RequestFailed("Frozen inputs changed during lookup; retry: " +
                                 std::string(error.what()));
    }
    catch (const std::exception& error) {
        m_frozenSources->decline(error.what());
        return {};
    }
}
} // namespace server
