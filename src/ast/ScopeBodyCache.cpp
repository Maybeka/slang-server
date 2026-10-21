// SPDX-License-Identifier: MIT
#include "ast/ScopeBodyCache.h"

#include "ast/ClosedTypeContext.h"
#include "ast/TrustedUvmLibrary.h"
#include <algorithm>
#include <cstdlib>
#include <fmt/format.h>
#include <fstream>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/syntax/SyntaxTree.h"

namespace server {
using namespace slang;
using namespace slang::ast;
namespace {
struct AdmissionTraversal : ASTVisitor<AdmissionTraversal, VisitFlags::Symbols> {
    std::function<bool(const InstanceBodySymbol&)> consider;
    explicit AdmissionTraversal(std::function<bool(const InstanceBodySymbol&)> callback) :
        consider(std::move(callback)) {}
    void handle(const InstanceSymbol& instance) {
        if (consider(instance.body))
            visitDefault(instance);
    }
};

struct Bodies : ASTVisitor<Bodies, VisitFlags::Symbols> {
    std::vector<const InstanceBodySymbol*> values;
    void handle(const InstanceSymbol& instance) {
        values.push_back(&instance.body);
        visitDefault(instance);
    }
};
// Closed bodies need their path and resolved arguments, not mutable ancestor
// declarations. Reconstruct only active generate branches and one concrete loop
// iteration. Instance arrays and CLI overrides keep the full-context route.
// Native path, flags, parameter signature and independence are checked again.
struct BodyContext {
    std::string source;
    ClosedTypeContext types;
    std::string instancePath;
};
std::optional<BodyContext> compactContext(const Compilation& current,
                                          const InstanceBodySymbol& body,
                                          const TrustedUvmLibrary& library) {
    if (!current.getOptions().paramOverrides.empty())
        return std::nullopt;
    if (!current.isInstanceBodyBindContextCompatible())
        return std::nullopt;
    if (auto disable = current.getDefaultDisable(body)) {
        const auto& entries = current.getDefaultDisableMap();
        auto own = entries.find(&body);
        if (own == entries.end() || own->second != disable)
            return std::nullopt;
    }
    if (auto clock = current.getDefaultClocking(body)) {
        auto scope = clock->getParentScope();
        auto instance = scope ? scope->getContainingInstance() : nullptr;
        const auto root = body.getHierarchicalPath();
        if (!instance || (instance->getHierarchicalPath() != root &&
                          !instance->getHierarchicalPath().starts_with(root + ".")))
            return std::nullopt;
    }
    std::vector<const Symbol*> chain;
    const Scope* cursor = &body;
    while (cursor && cursor->asSymbol().kind != SymbolKind::Root) {
        const auto& symbol = cursor->asSymbol();
        chain.push_back(&symbol);
        if (symbol.kind == SymbolKind::InstanceBody) {
            auto instance = symbol.as<InstanceBodySymbol>().parentInstance;
            if (!instance)
                return std::nullopt;
            cursor = instance->getParentScope();
        }
        else if (symbol.kind == SymbolKind::GenerateBlock) {
            const auto& block = symbol.as<GenerateBlockSymbol>();
            if (block.isUninstantiated)
                return std::nullopt;
            cursor = block.getParentScope();
            if (block.getArrayIndex()) {
                if (!cursor || cursor->asSymbol().kind != SymbolKind::GenerateBlockArray)
                    return std::nullopt;
                cursor = cursor->asSymbol().getParentScope();
            }
        }
        else {
            return std::nullopt;
        }
    }
    std::ranges::reverse(chain);
    if (chain.size() < 2 || chain.front()->kind != SymbolKind::InstanceBody ||
        current.getOptions().topModules.size() != 1)
        return std::nullopt;
    const auto top = chain.front()->as<InstanceBodySymbol>().parentInstance;
    if (!top || !current.getOptions().topModules.contains(top->name))
        return std::nullopt;
    auto identifier = [](std::string_view name) -> std::optional<std::string> {
        if (name.empty() || name.find_first_of(" \t\r\n") != std::string_view::npos)
            return std::nullopt;
        return fmt::format("\\{} ", name);
    };
    auto wrapper = identifier(top->name);
    if (!wrapper)
        return std::nullopt;
    size_t counter = 0;
    auto nextWrapper = [&] {
        for (;;) {
            auto name = fmt::format("__slang_scope_cache_wrapper_{}", counter++);
            if (std::ranges::find(library.definitions, name) == library.definitions.end() &&
                name != top->name)
                return name;
        }
    };
    ClosedTypeContext types;
    types.fixedOwner = library.compilation.get();
    std::string arguments;
    for (auto parameter : body.getParameters()) {
        if (parameter->symbol.kind == SymbolKind::TypeParameter) {
            const auto& value = parameter->symbol.as<TypeParameterSymbol>();
            if (!value.isOverridden())
                continue;
            const auto& type = value.targetType.getType();
            if (parameter->isLocalParam())
                return std::nullopt;
            auto name = identifier(value.name);
            if (!name)
                return std::nullopt;
            // Preserve addressable package typedefs, otherwise spell out the
            // resolved builtin type (including ranges, signedness and state).
            auto scope = type.getParentScope();
            std::optional<std::string> spelling;
            if (current.isFixedInstanceBodyTypeArgument(type, body) &&
                ((type.kind == SymbolKind::TypeAlias && scope &&
                  scope->asSymbol().kind == SymbolKind::Package) ||
                 type.getCanonicalType().isSimpleBitVector()))
                spelling = type.kind == SymbolKind::TypeAlias && scope &&
                                   scope->asSymbol().kind == SymbolKind::Package
                               ? type.toString()
                               : type.getCanonicalType().toString();
            else
                spelling = types.add(type);
            if (!spelling)
                return std::nullopt;
            if (!arguments.empty())
                arguments += ",";
            arguments += fmt::format(".{}({})", *name, *spelling);
            continue;
        }
        if (parameter->symbol.kind != SymbolKind::Parameter)
            return std::nullopt;
        const auto& value = parameter->symbol.as<ParameterSymbol>();
        if (!value.isOverridden())
            continue;
        if (parameter->isLocalParam() || !value.getType().isIntegral() ||
            !value.getValue().isInteger())
            return std::nullopt;
        auto name = identifier(value.name);
        if (!name)
            return std::nullopt;
        if (!arguments.empty())
            arguments += ",";
        auto literal = value.getValue().integer().toString(LiteralBase::Binary, true);
        // A resolved integer literal loses an enum's nominal assignment type.
        // Fixed package typedefs are still available in the independent owner.
        if (value.getType().isEnum()) {
            const auto& type = value.getType();
            const auto parent = type.getParentScope();
            if (type.kind != SymbolKind::TypeAlias || !parent ||
                parent->asSymbol().kind != SymbolKind::Package)
                return std::nullopt;
            literal = fmt::format("{}'({})", type.toString(), literal);
        }
        arguments += fmt::format(".{}({})", *name, literal);
    }
    auto definition = identifier(body.getDefinition().name);
    if (!definition)
        return std::nullopt;
    std::string text = fmt::format("module {}; ", *wrapper);
    size_t openBlocks = 0;
    for (size_t i = 1; i < chain.size(); ++i) {
        const auto& symbol = *chain[i];
        if (symbol.kind == SymbolKind::GenerateBlock) {
            const auto& block = symbol.as<GenerateBlockSymbol>();
            auto parent = block.getParentScope();
            auto index = block.getArrayIndex();
            auto name = identifier(
                index ? parent->asSymbol().as<GenerateBlockArraySymbol>().getExternalName()
                      : block.getExternalName());
            if (!name)
                return std::nullopt;
            if (index) {
                auto value = index->toString(LiteralBase::Binary, true);
                auto genvar = fmt::format("__slang_scope_cache_index_{}", counter++);
                text += fmt::format("for (genvar {} = {}; {} == {}; {}++) begin: {} ", genvar,
                                    value, genvar, value, genvar, *name);
            }
            else {
                text += fmt::format("if (1) begin: {} ", *name);
            }
            ++openBlocks;
            continue;
        }
        const auto& instance = *symbol.as<InstanceBodySymbol>().parentInstance;
        auto name = identifier(instance.name);
        if (!name)
            return std::nullopt;
        const bool last = i + 1 == chain.size();
        auto next = last ? *definition : nextWrapper();
        if (last)
            text += types.declarations;
        text += fmt::format("{} {}{} (); ", next,
                            last && !arguments.empty() ? "#(" + arguments + ") " : "", *name);
        while (openBlocks) {
            text += "end ";
            --openBlocks;
        }
        text += "endmodule\n";
        if (!last)
            text += fmt::format("module {}; ", next);
    }
    return BodyContext{std::move(text), std::move(types), body.getHierarchicalPath()};
}
size_t charge(const Compilation& compilation) {
    size_t bytes = compilation.getTotalBytesAllocated() +
                   compilation.getInstanceBodyReuseMemoryUsage();
    for (const auto& tree : compilation.getSyntaxTrees())
        bytes += tree->getRetainedSyntaxBytes() + tree->getRetainedSourceBytes();
    if (auto sources = compilation.getSourceManager())
        bytes += sources->getSourceLocationTableBytes() + sources->getMemoryUsage();
    return bytes;
}
} // namespace

ScopeBodyCache::ScopeBodyCache(std::shared_ptr<TrustedUvmLibrary> library, size_t budget,
                               std::vector<std::string> modules,
                               std::vector<std::string> instances) :
    library(std::move(library)), modules(std::move(modules)), instances(std::move(instances)) {
    counters.budgetBytes = budget;
    counters.baseBytes = this->library->semanticBytes;
}

void ScopeBodyCache::install(Compilation& compilation) {
    if (compilation.hasFlag(CompilationFlags::PackageAnalysis))
        return;
    auto weak = weak_from_this();
    compilation.setInstanceBodyReuseHooks(
        [weak](Compilation& current, InstanceBodySymbol& body) {
            if (auto cache = weak.lock())
                cache->lookup(current, body);
        },
        [weak](std::shared_ptr<Compilation> current) {
            if (auto cache = weak.lock())
                cache->publish(std::move(current));
        });
}

bool ScopeBodyCache::selected(const InstanceBodySymbol& body) const {
    auto definition = library->compilation->tryGetDefinition(body.getDefinition().name,
                                                             library->compilation->getRoot());
    if (definition.definition != &body.getDefinition())
        return false;
    if (std::ranges::find(modules, body.getDefinition().name) != modules.end())
        return true;
    const auto path = body.getHierarchicalPath();
    return std::ranges::any_of(instances, [&](const auto& root) {
        return path == root || path.starts_with(root + ".");
    });
}

bool ScopeBodyCache::contains(const Compilation& current, const InstanceBodySymbol& body) const {
    const auto path = body.getHierarchicalPath();
    for (const auto& group : groups) {
        if (group.owner->getOptions() != current.getOptions())
            continue;
        auto retained = group.bodies.find(path);
        if (retained != group.bodies.end() &&
            &retained->second->getDefinition() == &body.getDefinition() &&
            current.hasMatchingInstanceBodyParameters(*retained->second, body))
            return true;
    }
    return false;
}

void ScopeBodyCache::lookup(Compilation& current, InstanceBodySymbol& body) {
    if (!selected(body))
        return;
    const auto path = body.getHierarchicalPath();
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        if (it->owner->getOptions() != current.getOptions())
            continue;
        auto retained = it->bodies.find(path);
        if (retained != it->bodies.end() &&
            &retained->second->getDefinition() == &body.getDefinition() &&
            current.tryImportInstanceBody(it->owner, *retained->second, body)) {
            ++counters.hits;
            if (auto trace = std::getenv("SLANG_SCOPE_BODY_TRACE"); trace && *trace)
                fmt::print(stderr, "scope-body reuse\t{}\n", path);
            groups.splice(groups.begin(), groups, it);
            return;
        }
    }
    ++counters.misses;
}

ScopeBodyCache::Stats ScopeBodyCache::status() const {
    auto result = counters;
    result.owners = groups.size();
    for (const auto& group : groups) {
        result.ownerBytes += group.bytes;
        for (const auto& [path, _] : group.bodies)
            result.retainedRoots.push_back(path);
    }
    std::ranges::sort(result.retainedRoots);
    result.retainedRoots.erase(std::unique(result.retainedRoots.begin(),
                                           result.retainedRoots.end()),
                               result.retainedRoots.end());
    for (const auto& pin : pins)
        if (!pin.owner.expired())
            result.pinnedBytes += pin.bytes;
    return result;
}

bool ScopeBodyCache::rejected(std::string_view context) {
    for (auto it = rejectedContexts.begin(); it != rejectedContexts.end(); ++it) {
        if (*it == context) {
            rejectedContexts.splice(rejectedContexts.begin(), rejectedContexts, it);
            ++counters.admissionHits;
            return true;
        }
    }
    return false;
}

void ScopeBodyCache::trimRejections(size_t reservedBytes) {
    // Do not discard useful rejections for a graph that cannot fit even without
    // this metadata (for example the oversized full-context retry).
    if (reservedBytes > counters.budgetBytes ||
        counters.baseBytes > counters.budgetBytes - reservedBytes)
        return;
    while (!rejectedContexts.empty() &&
           counters.admissionBytes > counters.budgetBytes - reservedBytes - counters.baseBytes) {
        const auto& key = rejectedContexts.back();
        counters.admissionBytes -= sizeof(std::string) + 4 * sizeof(void*) + key.capacity() + 1;
        rejectedContexts.pop_back();
    }
    counters.admissionRecords = rejectedContexts.size();
}

void ScopeBodyCache::rememberRejection(std::string_view context, size_t stagedBytes) {
    // Bounded exact text keys; a new library / budget creates a new registry.
    // These records can only select native analysis, never authorize sharing.
    if (context.size() > 16 * 1024)
        return;
    std::string key(context);
    const size_t bytes = sizeof(std::string) + 4 * sizeof(void*) + key.capacity() + 1;
    while (!rejectedContexts.empty() &&
           (rejectedContexts.size() >= 32 || counters.admissionBytes + bytes > 64 * 1024)) {
        const auto& old = rejectedContexts.back();
        counters.admissionBytes -= sizeof(std::string) + 4 * sizeof(void*) + old.capacity() + 1;
        rejectedContexts.pop_back();
    }
    counters.admissionRecords = rejectedContexts.size();
    const auto usage = status();
    const auto reserved = usage.ownerBytes + usage.pinnedBytes + stagedBytes;
    if (reserved > counters.budgetBytes || counters.baseBytes > counters.budgetBytes - reserved ||
        counters.admissionBytes + bytes > counters.budgetBytes - reserved - counters.baseBytes)
        return; // Never evict a working owner just to remember a failed admission.
    counters.admissionBytes += bytes;
    rejectedContexts.push_front(std::move(key));
    counters.admissionRecords = rejectedContexts.size();
}

bool ScopeBodyCache::capacityAdvice(const Compilation& current, const InstanceBodySymbol& body,
                                    const std::vector<Group>& staged, size_t stagedBytes) const {
    const auto usage = status();
    size_t unavailable = counters.baseBytes + counters.admissionBytes + usage.pinnedBytes +
                         stagedBytes;
    for (const auto& group : groups)
        if (group.owner.use_count() > 1)
            unavailable += group.bytes;
    const auto available = unavailable < counters.budgetBytes ? counters.budgetBytes - unavailable
                                                              : 0;
    const auto path = body.getHierarchicalPath();
    auto advice = [&](const Group& group) {
        if (group.bytes <= available)
            return false;
        for (const auto& [knownPath, known] : group.bodies) {
            if (&known->getDefinition() == &body.getDefinition() &&
                std::ranges::count(knownPath, '.') == std::ranges::count(path, '.') &&
                current.hasMatchingInstanceBodyParameters(*known, body))
                return true;
        }
        return false;
    };
    // Measured admission cost is conservative advice, not a lower-bound proof.
    // Skipping can only leave this candidate native; smaller / distinct variants
    // are still considered and every admitted owner gets the full certificate.
    return std::ranges::any_of(groups, advice) || std::ranges::any_of(staged, advice);
}

void ScopeBodyCache::publish(std::shared_ptr<Compilation> current) {
    if (current->hasFatalErrors())
        return;
    auto& sources = *const_cast<SourceManager*>(current->getSourceManager());
    auto bind = [&](const std::optional<BodyContext>& compact, bool inheritedBindTarget = false) {
        ++counters.ownerBuilds;
        auto result = std::make_shared<Compilation>(current->getOptions());
        result->enableInstanceBodyReferenceTracking(/* complete */ true);
        if (compact)
            result->setInstanceBodyCacheContext(inheritedBindTarget);
        if (auto lease = current->getSourceOwnerLease())
            result->retainSourceOwner(lease);
        for (const auto& name : library->packages)
            result->importPackage(library->compilation, name, sources);
        for (const auto& name : library->definitions)
            result->importDefinition(library->compilation, name, sources);
        if (library->hasUnitDeclarations)
            result->importCompilationUnitDeclarations(library->compilation, sources);
        if (compact) {
            auto parseOptions = current->getSyntaxTrees().back()->options();
            result->addSyntaxTree(syntax::SyntaxTree::fromText(compact->source, sources,
                                                               "<scope-cache-context>", "",
                                                               parseOptions),
                                  true);
        }
        else {
            for (const auto& tree : current->getSyntaxTrees())
                result->addSyntaxTree(tree, true);
        }
        const auto& ownerDiagnostics = result->getSemanticDiagnostics();
        if (compact) {
            if (auto path = std::getenv("SLANG_SCOPE_CONTEXT_TRACE")) {
                const auto stem = std::filesystem::path(path) /
                                  fmt::format("context-{}", counters.ownerBuilds);
                std::ofstream(stem.string() + ".sv") << compact->source;
                std::ofstream(stem.string() + ".diagnostics")
                    << DiagnosticEngine::reportAll(sources, ownerDiagnostics);
            }
        }
        if (compact && !compact->types.dependencies.empty()) {
            Bodies bodies;
            result->getRoot().visit(bodies);
            const Scope* scope = nullptr;
            for (auto body : bodies.values)
                if (body->getHierarchicalPath() == compact->instancePath && body->parentInstance)
                    scope = body->parentInstance->getParentScope();
            if (!scope)
                return result;
            for (const auto& dependency : compact->types.dependencies) {
                auto symbol = scope->find(dependency.name);
                if (!symbol || !symbol->isType() ||
                    !result->recordInstanceBodyTypeDependency(symbol->as<Type>(),
                                                              *dependency.original)) {
                    if (std::getenv("SLANG_SCOPE_BODY_TRACE"))
                        fmt::print(stderr, "scope-body closed-type invalid\t{}\n", dependency.name);
                    return result;
                }
            }
        }
        return result;
    };
    std::vector<Group> staged;
    size_t stagedBytes = 0;
    bool fullAttempted = false;
    auto tryFull = [&] {
        if (fullAttempted)
            return false;
        fullAttempted = true;
        size_t syntaxBytes = 0;
        for (const auto& tree : current->getSyntaxTrees())
            syntaxBytes += tree->getRetainedSyntaxBytes() + tree->getRetainedSourceBytes();
        if (syntaxBytes > counters.budgetBytes ||
            counters.baseBytes > counters.budgetBytes - syntaxBytes) {
            ++counters.declines;
            return false;
        }
        auto group = prepareGroup(bind(std::nullopt));
        return group && retain(std::move(*group), false);
    };
    bool stopped = false, fullPublished = false;
    AdmissionTraversal candidates([&](const InstanceBodySymbol& candidate) {
        if (stopped)
            return false;
        const auto body = &candidate;
        if (!selected(*body))
            return true;
        const auto path = body->getHierarchicalPath();
        if (current->isImportedInstanceBody(*body))
            return false;
        if (contains(*current, *body))
            return true;
        bool covered = std::ranges::any_of(staged, [&](const Group& group) {
            return std::ranges::any_of(group.bodies, [&](const auto& entry) {
                return path == entry.first || path.starts_with(entry.first + ".");
            });
        });
        if (covered)
            return false;
        auto context = compactContext(*current, *body, *library);
        if (context && rejected(context->source))
            return true; // Still consider descendants of a rejected parent.
        // Every independent owner is charged this same source namespace. If
        // pinned owners and this batch leave less than that unavoidable charge,
        // constructing any further graph cannot succeed. Keep existing hits.
        size_t unavailable = counters.baseBytes + counters.admissionBytes + stagedBytes +
                             status().pinnedBytes;
        for (const auto& group : groups)
            if (group.owner.use_count() > 1)
                unavailable += group.bytes;
        const auto sourceFloor = sources.getSourceLocationTableBytes() + sources.getMemoryUsage();
        trimRejections(unavailable - counters.baseBytes - counters.admissionBytes + sourceFloor);
        // Metadata may yield space to a real owner.
        unavailable = counters.baseBytes + counters.admissionBytes + stagedBytes +
                      status().pinnedBytes;
        for (const auto& group : groups)
            if (group.owner.use_count() > 1)
                unavailable += group.bytes;
        if (unavailable >= counters.budgetBytes ||
            sourceFloor > counters.budgetBytes - unavailable) {
            ++counters.capacitySkips;
            ++counters.declines;
            stopped = true;
            return false;
        }
        if (capacityAdvice(*current, *body, staged, stagedBytes)) {
            ++counters.capacitySkips;
            return true;
        }
        const bool certified = current->prepareInstanceBodyReuse(*body);
        const bool promotion = !certified && context &&
                               (current->instanceBodyReuseRequiresFreshOwner(*body) ||
                                !context->types.dependencies.empty());
        if (!certified && !promotion)
            return true;
        if (!context) {
            if (tryFull()) {
                fullPublished = stopped = true;
                return false;
            }
            return true;
        }
        auto owner = bind(context, body->flags.has(InstanceFlags::TargetedByBind));
        Bodies compactBodies;
        owner->getRoot().visit(compactBodies);
        const bool valid = !owner->hasIssuedErrors() &&
                           std::ranges::any_of(compactBodies.values, [&](auto candidate) {
                               return candidate->getHierarchicalPath() == path &&
                                      &candidate->getDefinition() == &body->getDefinition() &&
                                      candidate->flags == body->flags &&
                                      current->hasMatchingInstanceBodyParameters(*candidate,
                                                                                 *body) &&
                                      owner->prepareInstanceBodyReuse(*candidate);
                           });
        if (!valid) {
            if (auto trace = std::getenv("SLANG_SCOPE_BODY_TRACE"); trace && *trace) {
                fmt::print(stderr, "scope-body compact-context invalid\t{}\terrors={}\n", path,
                           owner->hasIssuedErrors());
            }
            owner.reset();
            if (promotion) {
                // Failed fresh certification is admission advice only. Bound the
                // cost across edits without retaining the mixed consumer graph.
                rememberRejection(context->source, stagedBytes);
                return true;
            }
            if (tryFull()) {
                fullPublished = stopped = true;
                return false;
            }
            return true;
        }
        auto group = prepareGroup(std::move(owner));
        if (!group)
            return true;
        const bool oversized = group->bytes > counters.budgetBytes ||
                               counters.baseBytes > counters.budgetBytes - group->bytes;
        trimRejections(stagedBytes + group->bytes);
        const auto base = counters.baseBytes + counters.admissionBytes;
        if (oversized || stagedBytes + group->bytes > counters.budgetBytes ||
            base > counters.budgetBytes - stagedBytes - group->bytes) {
            group.reset(); // Release this graph before attempting the full route.
            if (tryFull()) {
                fullPublished = stopped = true;
                return false;
            }
            if (oversized)
                rememberRejection(context->source, stagedBytes);
            return true;
        }
        stagedBytes += group->bytes;
        staged.push_back(std::move(*group));
        return false;
    });
    current->getRoot().visit(candidates);
    if (fullPublished)
        return;
    size_t protectedGroups = 0;
    for (auto& group : staged)
        if (retain(std::move(group), true, protectedGroups))
            ++protectedGroups;
}

std::optional<ScopeBodyCache::Group> ScopeBodyCache::prepareGroup(
    std::shared_ptr<Compilation> owner) {
    Bodies prepared;
    owner->getRoot().visit(prepared);
    std::unordered_map<std::string, const InstanceBodySymbol*> bodies;
    for (auto body : prepared.values) {
        auto instance = body->parentInstance;
        auto scope = instance ? instance->getParentScope() : nullptr;
        bool covered = false;
        while (scope) {
            auto parent = scope->getContainingInstance();
            if (!parent)
                break;
            if (bodies.contains(parent->getHierarchicalPath())) {
                covered = true;
                break;
            }
            scope = parent->parentInstance ? parent->parentInstance->getParentScope() : nullptr;
        }
        if (!covered && selected(*body) && !contains(*owner, *body) &&
            owner->prepareInstanceBodyReuse(*body))
            bodies.emplace(body->getHierarchicalPath(), body);
    }
    if (bodies.empty()) {
        ++counters.declines;
        return std::nullopt;
    }
    // Prepare every certificate against the original native graph first, so
    // detaching one override cannot hide an incoming reference to another body.
    for (const auto& [_, body] : bodies)
        owner->detachRetainedParameterInitializers(const_cast<InstanceBodySymbol&>(*body));
    if (auto trace = std::getenv("SLANG_SCOPE_BODY_TRACE"); trace && *trace) {
        size_t syntaxBytes = 0, sourceBytes = 0;
        for (const auto& tree : owner->getSyntaxTrees()) {
            syntaxBytes += tree->getRetainedSyntaxBytes();
            sourceBytes += tree->getRetainedSourceBytes();
        }
        const auto sources = owner->getSourceManager();
        fmt::print(
            stderr,
            "scope-body memory\tast={} reuse={} syntax={} source={} locations={} manager={}\n",
            owner->getTotalBytesAllocated(), owner->getInstanceBodyReuseMemoryUsage(), syntaxBytes,
            sourceBytes, sources ? sources->getSourceLocationTableBytes() : 0,
            sources ? sources->getMemoryUsage() : 0);
    }
    size_t bytes = charge(*owner) + sizeof(Group) + 2 * sizeof(void*) +
                   bodies.bucket_count() * sizeof(void*);
    for (const auto& [path, _] : bodies)
        bytes += sizeof(decltype(bodies)::value_type) + path.capacity() + 1 + sizeof(void*);
    return Group{std::move(owner), std::move(bodies), bytes};
}

bool ScopeBodyCache::retain(Group group, bool compact, size_t protectedGroups) {
    const auto bytes = group.bytes;
    trimRejections(bytes);
    const auto base = counters.baseBytes + counters.admissionBytes;
    std::erase_if(pins, [](const auto& pin) { return pin.owner.expired(); });
    if (bytes > counters.budgetBytes || base > counters.budgetBytes - bytes) {
        ++counters.declines;
        return false;
    }
    while (groups.size() > protectedGroups) {
        const auto usage = status();
        if (usage.ownerBytes + usage.pinnedBytes <= counters.budgetBytes - base - bytes)
            break;
        auto& oldest = groups.back();
        pins.push_back({oldest.owner, oldest.bytes});
        groups.pop_back();
        ++counters.evictions;
    }
    const auto usage = status();
    if (usage.ownerBytes + usage.pinnedBytes > counters.budgetBytes - base - bytes) {
        ++counters.declines;
        return false;
    }
    if (auto trace = std::getenv("SLANG_SCOPE_BODY_TRACE"); trace && *trace)
        for (const auto& [path, _] : group.bodies)
            fmt::print(stderr, "scope-body retain\t{}\n", path);
    groups.push_front(std::move(group));
    ++counters.publications;
    if (compact)
        ++counters.compactPublications;
    return true;
}
} // namespace server
