#include "slang/analysis/AnalysisManager.h"
// SPDX-License-Identifier: MIT
#include "ast/ScopeBodyCache.h"

#include "ast/ClosedTypeContext.h"
#include "ast/TrustedUvmLibrary.h"
#include "util/SourceStorageRegistry.h"
#include "util/UvmPhaseTrace.h"
#include <algorithm>
#include <cstdlib>
#include <fmt/format.h>
#include <fstream>
#include <map>
#include <limits>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/util/String.h"
#include "slang/util/ScopeGuard.h"

namespace server {
using namespace slang;
using namespace slang::ast;
namespace {
struct BodySourceOwner {
    std::shared_ptr<SourceManager> sources;
    std::shared_ptr<const void> libraries;
};
struct AdmissionTraversal : ASTVisitor<AdmissionTraversal, VisitFlags::Symbols> {
    std::function<bool(const InstanceBodySymbol&)> consider;
    explicit AdmissionTraversal(std::function<bool(const InstanceBodySymbol&)> callback) :
        consider(std::move(callback)) {}
    void handle(const InstanceSymbol& instance) {
        if (consider(instance.body))
            visitDefault(instance);
    }
    void handle(const GenerateBlockSymbol& block) {
        if (!block.isUninstantiated)
            visitDefault(block);
    }
};

struct Bodies : ASTVisitor<Bodies, VisitFlags::Symbols> {
    std::vector<const InstanceBodySymbol*> values;
    void handle(const InstanceSymbol& instance) {
        values.push_back(&instance.body);
        visitDefault(instance);
    }
    void handle(const GenerateBlockSymbol& block) {
        if (!block.isUninstantiated)
            visitDefault(block);
    }
};
struct SubtreeBodies : ASTVisitor<SubtreeBodies, VisitFlags::Symbols> {
    std::map<std::string, const InstanceBodySymbol*> values;
    void handle(const InstanceSymbol& instance) {
        values.emplace(instance.body.getHierarchicalPath(), &instance.body);
        visitDefault(instance);
    }
    void handle(const GenerateBlockSymbol& block) {
        if (!block.isUninstantiated)
            visitDefault(block);
    }
    void handle(const ClassType&) {}
    void handle(const GenericClassDefSymbol&) {}
};
bool matchesNativeSubtree(const InstanceBodySymbol& native, const InstanceBodySymbol& retained) {
    auto collect = [](const InstanceBodySymbol& root) {
        SubtreeBodies visitor;
        visitor.values.emplace(root.getHierarchicalPath(), &root);
        root.visit(visitor);
        return std::move(visitor.values);
    };
    const auto a = collect(native), b = collect(retained);
    if (a.size() != b.size())
        return false;
    for (const auto& [path, body] : a) {
        const auto found = b.find(path);
        if (found == b.end() || &body->getDefinition() != &found->second->getDefinition() ||
            body->flags != found->second->flags)
            return false;
    }
    // Match the public boundary signature in its actual context. The complete
    // root certificate separately audits descendant arguments and dependencies.
    // A descendant-only signature check would incorrectly treat a nominal type
    // declared in the retained root as an external, mutable type dependency.
    return native.getCompilation().hasMatchingInstanceBodyParameters(retained, native);
}
// A parameter-only ancestor scaffold cannot remove these body-local obstacles.
// Type provenance and other contextual failures can instead benefit from the
// compact owner's reconstructed dependency closure, so must keep that route.
bool scaffoldIndependentRefusal(std::string_view reason) {
    return reason == "assertion-formal-ports" || reason == "upward-side-effect" ||
           reason == "child-upward-side-effect" || reason == "unreviewed-ast-kind" ||
           reason == "symbol-kind" || reason == "outside-hierarchy";
}
// Closed bodies need their path and resolved arguments, not mutable ancestor
// declarations. Reconstruct only active generate branches and one concrete loop
// iteration. Instance arrays and CLI overrides keep the full-context route.
// Native path, flags, parameter signature and independence are checked again.
struct BodyContext {
    std::string source;
    ClosedTypeContext types;
    std::string instancePath;
    // The independent proof replaces this ancestor with a minimal scaffold.
    // It must not simultaneously import the original fixed definition.
    std::string scaffoldTop;
};
std::optional<BodyContext> compactContext(const Compilation& current,
                                          const InstanceBodySymbol& body,
                                          const TrustedUvmLibrary& library) {
    if (!current.getOptions().paramOverrides.empty())
        return std::nullopt;
    if (!current.isInstanceBodyBindContextCompatible(&body))
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
    if (chain.size() < 2 || chain.front()->kind != SymbolKind::InstanceBody)
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
                name != top->name && !current.getOptions().topModules.contains(name))
                return name;
        }
    };
    ClosedTypeContext types;
    types.fixedOwner = library.compilation.get();
    std::string arguments;
    auto valueLiteral = [&](auto&& self, const Type& type,
                             const ConstantValue& value) -> std::optional<std::string> {
        const auto& canonical = type.getCanonicalType();
        if (canonical.isString()) {
            if (!value.isString() || value.str().find('\0') != std::string::npos ||
                value.str().size() > size_t(SVInt::MAX_BITS) / 8)
                return std::nullopt;
            return quoteSVString(value.str());
        }
        if (canonical.kind == SymbolKind::FixedSizeUnpackedArrayType) {
            if (!value.isUnpacked())
                return std::nullopt;
            const auto& array = canonical.as<FixedSizeUnpackedArrayType>();
            const auto& elements = std::get<ConstantValue::Elements>(value.getVariant());
            if (elements.size() != array.range.width())
                return std::nullopt;
            std::string text = "'{";
            for (const auto& element : elements) {
                auto literal = self(self, array.elementType, element);
                if (!literal)
                    return std::nullopt;
                if (text.size() > 2)
                    text += ',';
                text += *literal;
            }
            return text + '}';
        }
        if (!type.isIntegral() || !value.isInteger())
            return std::nullopt;
        auto literal = value.integer().toString(LiteralBase::Binary, true);
        // Assignment patterns preserve unpacked shape. Enum elements also need
        // their fixed nominal assignment type, just like scalar overrides.
        if (type.isEnum()) {
            const auto parent = type.getParentScope();
            if (type.kind != SymbolKind::TypeAlias || !parent ||
                parent->asSymbol().kind != SymbolKind::Package)
                return std::nullopt;
            literal = fmt::format("{}'({})", type.toString(), literal);
        }
        return literal;
    };
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
                 type.getCanonicalType().isSimpleBitVector() || type.isString()))
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
        if (parameter->isLocalParam() ||
            !current.isFixedInstanceBodyTypeArgument(value.getType(), body))
            return std::nullopt;
        auto name = identifier(value.name);
        if (!name)
            return std::nullopt;
        if (!arguments.empty())
            arguments += ",";
        auto literal = valueLiteral(valueLiteral, value.getType(), value.getValue());
        if (!literal)
            return std::nullopt;
        arguments += fmt::format(".{}({})", *name, *literal);
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
    // Preserve the original compilation options so import compatibility remains
    // exact. Unrelated mutable tops need names, not bodies, in a closed proof.
    // Fixed tops are left as their original imported definitions: replacing one
    // could change a definition used inside the candidate's subtree.
    for (const auto& name : current.getOptions().topModules) {
        if (name == top->name || std::ranges::find(library.definitions, name) !=
                                     library.definitions.end())
            continue;
        auto spelling = identifier(name);
        if (!spelling)
            return std::nullopt;
        // Do not manufacture a top which was absent from the native model.
        if (std::ranges::none_of(current.getRootNoFinalize().topInstances,
                                [&](auto instance) { return instance->name == name; }))
            return std::nullopt;
        text += fmt::format("module {}; endmodule\n", *spelling);
    }
    return BodyContext{std::move(text), std::move(types), body.getHierarchicalPath(),
                       std::string(top->name)};
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
                               std::vector<std::string> modules, std::vector<std::string> instances,
                               std::shared_ptr<SourceStorageRegistry> sourceRegistry,
                               bool contextualInitial) :
    library(std::move(library)), sourceRegistry(std::move(sourceRegistry)),
    modules(std::move(modules)), instances(std::move(instances)), contextualInitial(contextualInitial) {
    counters.budgetUnlimited = budget == std::numeric_limits<size_t>::max();
    counters.budgetBytes = budget;
    counters.refusalReportBytes = sizeof(refusalReport);
    counters.baseBytes = this->library->semanticBytes + counters.refusalReportBytes;
}

void ScopeBodyCache::install(Compilation& compilation,
                             std::shared_ptr<const void> sourceLibraries, bool independentInitial) {
    if (compilation.hasFlag(CompilationFlags::PackageAnalysis))
        return;
    auto weak = weak_from_this();
    compilation.setInstanceBodyReuseHooks(
        [weak](Compilation& current, InstanceBodySymbol& body) {
            if (auto cache = weak.lock())
                cache->lookup(current, body);
        },
        [weak, sourceLibraries = std::move(sourceLibraries), independentInitial](std::shared_ptr<Compilation> current) {
            if (auto cache = weak.lock())
                cache->publish(std::move(current), sourceLibraries, independentInitial);
        });
}

bool ScopeBodyCache::wantsInitialModel() const {
    if (!groups.empty() || contextualInitial)
        return false;
    if (!instances.empty())
        return true;
    // An interface-only template selection can be used solely by virtual
    // interfaces in editable classes, with no RTL instance forest to retain.
    // Keep that shared class warmup on its ordinary source namespace.
    return std::ranges::any_of(modules, [&](const std::string& name) {
        const auto definition = library->compilation->tryGetDefinition(
            name, library->compilation->getRoot());
        return definition.definition &&
               definition.definition->as<DefinitionSymbol>().definitionKind == DefinitionKind::Module;
    });
}

void ScopeBodyCache::trackInitialSources(const std::shared_ptr<SourceManager>& sources) {
    if (sourceRegistry)
        sourceRegistry->track(sources);
}

bool ScopeBodyCache::selected(const InstanceBodySymbol& body) const {
    auto definition = library->compilation->tryGetDefinition(body.getDefinition().name,
                                                             library->compilation->getRoot());
    if (definition.definition != &body.getDefinition())
        return false;
    if (std::ranges::find(modules, body.getDefinition().name) != modules.end())
        return true;
    // Selecting a module selects its instantiated descendants as well. The
    // descendant still needs its own immutable definition above; an unsafe
    // parent must not disable otherwise closed children.
    const Scope* ancestor = body.parentInstance ? body.parentInstance->getParentScope() : nullptr;
    while (!modules.empty() && ancestor) {
        const auto& symbol = ancestor->asSymbol();
        if (auto parent = symbol.as_if<InstanceBodySymbol>()) {
            const auto& type = parent->getDefinition();
            const auto declarationScope = type.getParentScope();
            if (type.sourceLibrary.isDefault && declarationScope &&
                declarationScope->asSymbol().kind == SymbolKind::CompilationUnit &&
                std::ranges::find(modules, type.name) != modules.end())
                return true;
            ancestor = parent->parentInstance ? parent->parentInstance->getParentScope() : nullptr;
        }
        else
            ancestor = symbol.getParentScope();
    }
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

void ScopeBodyCache::recordRefusal(std::string_view category, std::string_view path) {
    // Categories are static compiler/cache literals. Paths are copied into a
    // fixed report buffer; no certificate, syntax or editing owner escapes.
    ++counters.refusalAttempts;
    if (category.empty())
        category = "uncertified-body";
    size_t index = 0;
    while (index < refusalReport.countSize && refusalReport.counts[index].category != category)
        ++index;
    if (index == refusalReport.countSize) {
        if (index == refusalReport.counts.size()) {
            counters.refusalSamplesTruncated = true;
            ++counters.uncategorizedRefusals;
            return;
        }
        refusalReport.counts[index].category = category;
        ++refusalReport.countSize;
    }
    ++refusalReport.counts[index].attempts;
    counters.refusalSamplesTruncated |= path.size() > 255;
    size_t end = std::min(path.size(), size_t(255));
    while (end && end < path.size() && (static_cast<unsigned char>(path[end]) & 0xc0) == 0x80)
        --end; // Keep a complete UTF-8 code point at the sample boundary.
    path = path.substr(0, end);
    for (size_t i = 0; i < refusalReport.sampleSize; ++i)
        if (refusalReport.samples[i].category == index &&
            std::string_view(refusalReport.samples[i].path.data()) == path)
            return;
    if (refusalReport.sampleSize == refusalReport.samples.size()) {
        counters.refusalSamplesTruncated = true;
        return;
    }
    auto& sample = refusalReport.samples[refusalReport.sampleSize++];
    sample.category = index;
    std::copy(path.begin(), path.end(), sample.path.begin());
}

bool ScopeBodyCache::certify(Compilation& compilation, const InstanceBodySymbol& body,
                             std::string_view path) {
    std::string_view reason;
    if (compilation.prepareInstanceBodyReuse(body, &reason))
        return true;
    if (path.empty() && refusalReport.sampleSize < refusalReport.samples.size())
        recordRefusal(reason, body.getHierarchicalPath());
    else
        recordRefusal(reason, path);
    return false;
}

std::shared_ptr<const slang::analysis::AnalysisManager> ScopeBodyCache::analysisFor(
    const Compilation& current) {
    for (const auto& group : groups) {
        if (!group.analysis)
            continue;
        for (const auto& [_, body] : group.bodies)
            if (current.isImportedInstanceBody(*body) ||
                &current.getCurrentInstanceBody(*body) != body) {
                ++counters.analysisHits;
                return group.analysis;
            }
    }
    return {};
}

void ScopeBodyCache::retainAnalysis(
    std::shared_ptr<Compilation> current,
    std::shared_ptr<slang::analysis::AnalysisManager> analysis) {
    if (!analysis || !analysis->hasFlag(slang::analysis::AnalysisFlags::RetainProcedureSummaries))
        return;
    for (auto& group : groups) {
        // No edited consumer, reconstruction or analysis chain is retained.
        // The exact standalone semantic owner already belongs to this group.
        if (group.owner != current || group.analysis)
            continue;
        const auto bytes = analysis->getStats().memoryUsage;
        const auto usage = accounting();
        if (bytes > counters.budgetBytes || counters.baseBytes > counters.budgetBytes - bytes ||
            usage.ownerBytes > counters.budgetBytes - bytes - counters.baseBytes ||
            usage.pinnedBytes > counters.budgetBytes - bytes - counters.baseBytes - usage.ownerBytes)
            return;
        struct Snapshot {
            std::shared_ptr<Compilation> owner;
            std::shared_ptr<slang::analysis::AnalysisManager> analysis;
        };
        // Server callbacks capture a request cell that is cleared on completion,
        // not the editing analysis wrapper. Keep the manager unchanged.
        auto snapshot = std::make_shared<Snapshot>(current, analysis);
        group.analysis = {snapshot, analysis.get()};
        group.analysisBytes = bytes;
        group.bytes += bytes;
        return;
    }
}

bool ScopeBodyCache::releaseAnalysisSnapshots() {
    bool released = false;
    std::erase_if(pins, [](const auto& pin) { return pin.owner.expired(); });
    for (auto& group : groups) {
        if (!group.analysis)
            continue;
        // Active replay managers can still use this immutable snapshot. Charge
        // that lease until the last reader releases it, without keeping it alive.
        pins.push_back({group.analysis, group.analysisBytes});
        group.bytes -= group.analysisBytes;
        group.analysisBytes = 0;
        group.analysis.reset();
        released = true;
    }
    return released;
}

void ScopeBodyCache::noteProcedureReuse(size_t reused, size_t recomputed) {
    counters.proceduresReused += reused;
    counters.proceduresRecomputed += recomputed;
}

ScopeBodyCache::Stats ScopeBodyCache::accounting() const {
    // Admission needs byte totals only. Public reports allocate value copies
    // when queried, never for each candidate in a large design traversal.
    auto result = counters;
    result.owners = groups.size();
    for (const auto& group : groups) {
        result.ownerBytes += group.bytes;
        result.analysisBytes += group.analysisBytes;
    }
    for (const auto& pin : pins)
        if (!pin.owner.expired())
            result.pinnedBytes += pin.bytes;
    return result;
}

ScopeBodyCache::Stats ScopeBodyCache::status() const {
    auto result = accounting();
    if (result.budgetUnlimited)
        result.budgetBytes = 0; // No finite limit; do not serialize SIZE_MAX as a fake quota.
    for (size_t i = 0; i < refusalReport.countSize; ++i) {
        Refusal refusal{std::string(refusalReport.counts[i].category),
                        refusalReport.counts[i].attempts, {}};
        for (size_t n = 0; n < refusalReport.sampleSize; ++n)
            if (refusalReport.samples[n].category == i)
                refusal.sampleRoots.emplace_back(refusalReport.samples[n].path.data());
        result.refusals.push_back(std::move(refusal));
    }
    for (const auto& group : groups) {
        for (const auto& [path, _] : group.bodies)
            result.retainedRoots.push_back(path);
    }
    std::ranges::sort(result.retainedRoots);
    result.retainedRoots.erase(std::unique(result.retainedRoots.begin(),
                                           result.retainedRoots.end()),
                               result.retainedRoots.end());
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
    const auto usage = accounting();
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
    const auto usage = accounting();
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

void ScopeBodyCache::publish(std::shared_ptr<Compilation> current,
                             std::shared_ptr<const void> sourceLibraries, bool independentInitial) {
    if (current->hasFatalErrors())
        return;
    UvmPhaseTrace publicationTrace;
    auto publicationDone = ScopeGuard([&] { publicationTrace.mark("scope-owner-publication"); });
    auto& sources = *const_cast<SourceManager*>(current->getSourceManager());
    if (independentInitial && groups.empty() && !current->hasImportedInstanceBodies()) {
        // Record original nominal identities before asking for any certificate.
        // Import still checks provenance and the complete concrete layout in
        // each consumer; an unsupported argument keeps the existing fallback.
        AdmissionTraversal types([&](const InstanceBodySymbol& body) {
            if (selected(body))
                for (auto parameter : body.getParameters())
                    if (auto type = parameter->symbol.as_if<TypeParameterSymbol>()) {
                        const auto& argument = type->targetType.getType();
                        if (!current->isFixedInstanceBodyTypeArgument(argument, body) &&
                            !current->retainInstanceBodyTypeDependency(argument) &&
                            std::getenv("SLANG_SCOPE_BODY_TRACE")) {
                            auto scope = argument.getParentScope();
                            fmt::print(stderr, "scope-body native-type-refusal\t{}\t{}\t{}\towned={}\n",
                                       body.getHierarchicalPath(), type->name, argument.toString(),
                                       scope && &scope->getCompilation() == current.get());
                        }
                    }
            return true;
        });
        current->getRoot().visit(types);
    }
    // A batch can publish multiple independent bodies. Share one namespace for
    // their generated wrappers, but never append later editor generations to it.
    std::shared_ptr<SourceManager> compactSources;
    std::shared_ptr<SourceManager> fullSources;
    auto bind = [&](const std::optional<BodyContext>& compact, bool inheritedBindTarget = false) {
        UvmPhaseTrace bindTrace;
        auto bindDone = ScopeGuard([&] { bindTrace.mark("scope-owner-bind"); });
        ++counters.ownerBuilds;
        auto result = std::make_shared<Compilation>(current->getOptions());
        result->enableInstanceBodyReferenceTracking(/* complete */ true);
        if (compact)
            result->setInstanceBodyCacheContext(inheritedBindTarget);
        auto* ownerSources = &sources;
        if (compact) {
            if (!compactSources) {
                compactSources = std::make_shared<SourceManager>();
                compactSources->inheritIncludeConfiguration(sources.getIncludeConfiguration());
                compactSources->inheritImmutablePrefix(library->sources);
                compactSources->retainSourceNames();
                if (sourceRegistry)
                    sourceRegistry->track(compactSources);
            }
            ownerSources = compactSources.get();
        }
        else {
            if (!fullSources) {
                std::vector<BufferID> required;
                for (const auto& tree : current->getSyntaxTrees())
                    for (auto id : tree->getSourceBufferIds())
                        required.push_back(id);
                fullSources = sources.forkRetainedNamespace(required);
                if (sourceRegistry)
                    sourceRegistry->track(fullSources);
            }
            ownerSources = fullSources.get();
        }
        // Keep library identities independently of the mutable driver's manager.
        // A reconstructed compact model imports only the fixed library. The
        // isolated initial namespace is no longer its source or identity owner;
        // keeping that lease would pin an otherwise retired source manager.
        auto identities = compact && independentInitial
                              ? std::static_pointer_cast<const void>(library) : sourceLibraries;
        auto lease = std::make_shared<BodySourceOwner>(compact ? compactSources : fullSources,
                                                       std::move(identities));
        result->retainSourceOwner(lease);
        for (const auto& name : library->packages)
            result->importPackage(library->compilation, name, *ownerSources);
        for (const auto& name : library->definitions)
            if (!compact || name != compact->scaffoldTop)
                result->importDefinition(library->compilation, name, *ownerSources);
        if (library->hasUnitDeclarations)
            result->importCompilationUnitDeclarations(library->compilation, *ownerSources);
        if (compact) {
            auto parseOptions = current->getSyntaxTrees().back()->options();
            if (auto import = parseOptions.get<TrustedUvmImport>()) {
                auto fixed = *import;
                fixed.sourceOwner = lease;
                parseOptions.set(std::move(fixed));
            }
            if (auto prefix = parseOptions.get<syntax::PrecompiledUvmSyntax>();
                prefix && prefix->tree) {
                auto view = *prefix;
                view.tree = syntax::SyntaxTree::sharedPrefixView(prefix->tree, *ownerSources);
                parseOptions.set(std::move(view));
            }
            result->addSyntaxTree(syntax::SyntaxTree::fromText(compact->source, *ownerSources,
                                                               "<scope-cache-context>", "",
                                                               parseOptions),
                                  true);
        }
        else {
            for (const auto& tree : current->getSyntaxTrees()) {
                auto parseOptions = tree->options();
                if (auto import = parseOptions.get<TrustedUvmImport>()) {
                    auto fixed = *import;
                    fixed.sourceOwner = lease;
                    parseOptions.set(std::move(fixed));
                }
                result->addSyntaxTree(
                    syntax::SyntaxTree::sharedPrefixView(tree, *ownerSources, &parseOptions), true);
            }
        }
        const auto& ownerDiagnostics = result->getSemanticDiagnostics();
        if (compact) {
            if (auto path = std::getenv("SLANG_SCOPE_CONTEXT_TRACE")) {
                const auto stem = std::filesystem::path(path) /
                                  fmt::format("context-{}", counters.ownerBuilds);
                std::ofstream(stem.string() + ".sv") << compact->source;
                std::ofstream(stem.string() + ".diagnostics")
                    << DiagnosticEngine::reportAll(*ownerSources, ownerDiagnostics);
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
        const bool direct = independentInitial && groups.empty() &&
                            !current->hasImportedInstanceBodies();
        auto group = prepareGroup(direct ? current : bind(std::nullopt));
        // Exact object identity already proves the native boundary. Comparing
        // every descendant against itself would repeat a full hierarchy walk.
        if (group && !direct) {
            Bodies originals;
            current->getRoot().visit(originals);
            for (const auto& [path, body] : group->bodies) {
                const auto original = std::ranges::find_if(originals.values, [&](auto candidate) {
                    return candidate->getHierarchicalPath() == path;
                });
                if (original == originals.values.end() || !matchesNativeSubtree(**original, *body)) {
                    ++counters.declines;
                    return false;
                }
            }
        }
        if (!group)
            return false;
        // Preserve failed exact-context admission knowledge before borrowed
        // children can mask the original refusal in the next editing graph.
        // This is negative advice only; it never authorizes body sharing.
        std::vector<std::string> refusedContexts;
        AdmissionTraversal refusals([&](const InstanceBodySymbol& body) {
            if (!selected(body))
                return true;
            if (group->bodies.contains(body.getHierarchicalPath()))
                return false;
            std::string_view reason;
            if (!group->owner->prepareInstanceBodyReuse(body, &reason)) {
                // These refusals survive removal of the enclosing scaffold.
                // Other name, type and diagnostic failures are retried. An
                // outside hierarchical runtime reference cannot become local
                // by replacing ancestors with a parameter-only scaffold.
                if (scaffoldIndependentRefusal(reason)) {
                    if (auto context = compactContext(*group->owner, body, *library))
                        refusedContexts.push_back(std::move(context->source));
                }
            }
            return true;
        });
        group->owner->getRoot().visit(refusals);
        if (!retain(std::move(*group), false))
            return false;
        for (const auto& context : refusedContexts)
            rememberRejection(context, 0);
        return true;
    };
    // The isolated initial model already owns its semantics. Admission can
    // retain it directly, without a second planning walk to decide whether
    // constructing another full owner would be worthwhile.
    const bool wholeTop = modules.empty() && instances.size() == 1 &&
                          instances.front().find('.') == std::string::npos;
    if (independentInitial && wholeTop && groups.empty() &&
        !current->hasImportedInstanceBodies() && tryFull())
        return;
    // A fragmented selection is cheaper to retain as one independent forest
    // than as many wrappers that a later full-context fallback discards. Plan
    // from native certificates before constructing any candidate owner. Small
    // selections keep the compact route; failed full admission still falls back.
    if (groups.empty() && !current->hasImportedInstanceBodies()) {
        size_t frontierRoots = 0;
        bool fragmented = false;
        bool fullPlanSafe = true;
        AdmissionTraversal plan([&](const InstanceBodySymbol& body) {
            if (!fullPlanSafe || frontierRoots >= 8)
                return false;
            if (!selected(body))
                return true;
            std::string_view reason;
            if (current->prepareInstanceBodyReuse(body, &reason)) {
                ++frontierRoots;
                return false;
            }
            if (!scaffoldIndependentRefusal(reason)) {
                fullPlanSafe = false;
                return false;
            }
            fragmented = true;
            return true;
        });
        current->getRoot().visit(plan);
        if (fullPlanSafe && ((independentInitial && frontierRoots > 0) || (fragmented && frontierRoots >= 8)) && tryFull())
            return;
    }
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
        if (context && rejected(context->source)) {
            recordRefusal("remembered-admission", path);
            return true; // Still consider descendants of a rejected parent.
        }
        // Every independent owner is charged this same source namespace. If
        // pinned owners and this batch leave less than that unavoidable charge,
        // constructing any further graph cannot succeed. Keep existing hits.
        size_t unavailable = counters.baseBytes + counters.admissionBytes + stagedBytes +
                             accounting().pinnedBytes;
        for (const auto& group : groups)
            if (group.owner.use_count() > 1)
                unavailable += group.bytes;
        const auto sourceFloor = sources.getSourceLocationTableBytes() + sources.getMemoryUsage();
        trimRejections(unavailable - counters.baseBytes - counters.admissionBytes + sourceFloor);
        // Metadata may yield space to a real owner.
        unavailable = counters.baseBytes + counters.admissionBytes + stagedBytes +
                      accounting().pinnedBytes;
        for (const auto& group : groups)
            if (group.owner.use_count() > 1)
                unavailable += group.bytes;
        if (unavailable >= counters.budgetBytes ||
            sourceFloor > counters.budgetBytes - unavailable) {
            ++counters.capacitySkips;
            ++counters.declines;
            recordRefusal("budget-source-floor", path);
            stopped = true;
            return false;
        }
        if (capacityAdvice(*current, *body, staged, stagedBytes)) {
            recordRefusal("budget-advice", path);
            ++counters.capacitySkips;
            return true;
        }
        const bool certified = certify(*current, *body, path);
        const bool promotion = !certified && context &&
                               (current->instanceBodyReuseRequiresFreshOwner(*body) ||
                                !context->types.dependencies.empty());
        if (!certified && !promotion)
            return true;
        if (!context) {
            recordRefusal("compact-context-unavailable", path);
            if (tryFull()) {
                fullPublished = stopped = true;
                return false;
            }
            return true;
        }
        auto owner = bind(context, body->flags.has(InstanceFlags::TargetedByBind));
        Bodies compactBodies;
        owner->getRoot().visit(compactBodies);
        const bool valid = std::ranges::any_of(compactBodies.values, [&](auto candidate) {
                               return candidate->getHierarchicalPath() == path &&
                                      &candidate->getDefinition() == &body->getDefinition() &&
                                      candidate->flags == body->flags &&
                                      current->hasMatchingInstanceBodyParameters(*candidate,
                                                                                 *body) &&
                                      matchesNativeSubtree(*body, *candidate) &&
                                      certify(*owner, *candidate, path);
                           }) && !owner->hasUncertifiedInstanceBodyErrors();
        if (!valid) {
            recordRefusal("compact-context-mismatch-or-errors", path);
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
        auto group = prepareGroup(std::move(owner), path);
        if (!group)
            return true;
        const bool oversized = group->bytes > counters.budgetBytes ||
                               counters.baseBytes > counters.budgetBytes - group->bytes;
        trimRejections(stagedBytes + group->bytes);
        const auto base = counters.baseBytes + counters.admissionBytes;
        if (oversized || stagedBytes + group->bytes > counters.budgetBytes ||
            base > counters.budgetBytes - stagedBytes - group->bytes) {
            recordRefusal("budget-owner", path);
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
    std::shared_ptr<Compilation> owner, std::string_view admittedRoot) {
    UvmPhaseTrace certificateTrace;
    auto certificateDone = ScopeGuard([&] { certificateTrace.mark("scope-owner-certificates"); });
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
        // Compact ancestors are scaffolds, so their declaration names cannot
        // re-prove a module selector. Use the exact root already validated
        // against the selected native graph, never an unchecked new path.
        const bool admitted = admittedRoot.empty() ? selected(*body)
                                                   : body->getHierarchicalPath() == admittedRoot;
        if (!covered && admitted && !contains(*owner, *body) &&
            certify(*owner, *body))
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
    owner->releaseInstanceBodyAdmissionIndexes();
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
        const auto usage = accounting();
        if (usage.ownerBytes + usage.pinnedBytes <= counters.budgetBytes - base - bytes)
            break;
        auto& oldest = groups.back();
        pins.push_back({oldest.owner, oldest.bytes});
        groups.pop_back();
        ++counters.evictions;
    }
    const auto usage = accounting();
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
