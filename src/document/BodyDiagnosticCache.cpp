// SPDX-License-Identifier: MIT
#include "document/BodyDiagnosticCache.h"

#include "ast/ServerCompilation.h"
#include "ast/TrustedUvmLibrary.h"
#include "document/BodyLineProof.h"
#include "document/MacroLineProof.h"
#include "document/NativeDiagnosticSnapshot.h"
#include "util/Converters.h"
#include "util/ModelOwnerTrace.h"
#include "util/UvmPhaseTrace.h"
#include <algorithm>
#include <deque>
#include <numeric>
#include <rfl/json/write.hpp>
#include <set>

#include "slang/analysis/ValueDriver.h"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/InstanceCacheKey.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/symbols/ClassSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/diagnostics/AnalysisDiags.h"
#include "slang/diagnostics/GeneralDiags.h"
#include "slang/diagnostics/LexerDiags.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/util/ScopeGuard.h"

namespace server {
using namespace slang;
namespace {
size_t rawDiagnosticHash(const Diagnostic& value) {
    size_t hash = std::hash<const ast::Symbol*>{}(value.symbol);
    auto add = [&](size_t part) { hash ^= part + size_t(0x9e3779b9) + (hash << 6) + (hash >> 2); };
    add(size_t(value.code.getSubsystem()));
    add(value.code.getCode());
    add(value.location.buffer().getId());
    add(value.location.offset());
    return hash;
}
// Custom arguments have no generic equality, and ConstantValue equality does not
// prove identical formatting (for example signed zero). Reformat these fresh
// diagnostics; hashes only locate candidates and never prove equality.
bool scalarDiagnosticEqual(const Diagnostic& a, const Diagnostic& b) {
    if (a.code != b.code || a.location != b.location || a.symbol != b.symbol ||
        a.coalesceCount != b.coalesceCount || a.ranges != b.ranges ||
        a.args.size() != b.args.size() || a.notes.size() != b.notes.size())
        return false;
    for (size_t i = 0; i < a.args.size(); ++i) {
        if (a.args[i].index() != b.args[i].index())
            return false;
        bool same = std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, Diagnostic::CustomArgType> ||
                              std::is_same_v<T, ConstantValue>)
                    return false;
                else
                    return value == std::get<T>(b.args[i]);
            },
            a.args[i]);
        if (!same)
            return false;
    }
    for (size_t i = 0; i < a.notes.size(); ++i)
        if (!scalarDiagnosticEqual(a.notes[i], b.notes[i]))
            return false;
    return true;
}

void require(bool condition, const char* reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
// Warm complete lazy binding operations before the final semantic traversal.
// Checkpoints are outside binders: cancellation keeps completed caches valid and
// never interrupts a shared mutable AST in the middle of a binding operation.
// This is only preparation; the ordinary complete diagnostic pass still runs.
struct SharedSemanticWarmup : ast::ASTVisitor<SharedSemanticWarmup, ast::VisitFlags::Canonical> {
    ast::Compilation& owner;
    const lsp::RequestContext& ctx;
    std::unordered_set<const ast::Symbol*> visited;
    std::unordered_set<ast::InstanceCacheKey, slang::hash<ast::InstanceCacheKey>> instances;

    SharedSemanticWarmup(ast::Compilation& owner, const lsp::RequestContext& ctx) :
        owner(owner), ctx(ctx) {}

    template<typename T>
    bool bind(const T& symbol) {
        const ast::Scope* scope = symbol.getParentScope();
        if constexpr (std::is_base_of_v<ast::Scope, T>)
            scope = &symbol;
        if ((scope && &scope->getCompilation() != &owner) || !visited.insert(&symbol).second)
            return false;
        ctx.checkpoint("before shared semantic binding warmup");
        if constexpr (std::is_same_v<T, ast::InstanceBodySymbol>) {
            if (owner.isImportedInstanceBody(symbol))
                return false;
        }
        if constexpr (std::is_same_v<T, ast::InstanceSymbol>) {
            // Optional warmup must not replace Native hierarchy depth checks.
            if (symbol.instanceDepth > std::min<uint32_t>(64, owner.getOptions().maxInstanceDepth))
                return false;
            if (!owner.hasFlag(ast::CompilationFlags::DisableInstanceCaching) &&
                ast::InstanceCacheKey::isEligibleForCaching(symbol)) {
                bool valid = true;
                SmallSet<const ast::InstanceSymbol*, 2> seen;
                ast::InstanceCacheKey key(symbol, valid, seen);
                // This only chooses warmup work. Native still performs every
                // context / side-effect check and decides actual body reuse.
                if (valid && !instances.insert(std::move(key)).second)
                    return false;
            }
        }
        if constexpr (std::is_same_v<T, ast::CheckerInstanceBodySymbol>) {
            if (symbol.instanceDepth > owner.getOptions().maxCheckerInstanceDepth)
                return false;
        }
        if constexpr (std::is_same_v<T, ast::GenerateBlockSymbol>) {
            if (symbol.isUninstantiated)
                return false;
        }
        if (auto type = symbol.getDeclaredType()) {
            (void)type->getType();
            (void)type->getInitializer();
        }
        if constexpr (requires { symbol.getBody().bad(); })
            (void)symbol.getBody();
        if constexpr (std::is_same_v<T, ast::SubroutineSymbol>)
            ctx.checkpoint("after shared method body warmup");
        ctx.checkpoint("after shared semantic binding warmup");
        return true;
    }

    template<typename T>
    void handle(const T& symbol) {
        if constexpr (std::is_base_of_v<ast::Symbol, T>) {
            if (bind(symbol))
                visitDefault(symbol);
        }
    }

    void handle(const ast::GenericClassDefSymbol& symbol) {
        if (!bind(symbol))
            return;
        // Binding can instantiate another specialization of this same class.
        // Snapshot pointers instead of keeping an iterator into its mutable map.
        std::vector<const ast::Type*> specializations;
        for (const auto& type : symbol.specializations())
            specializations.push_back(&type);
        for (auto type : specializations)
            type->visit(*this);
    }
};

bool contained(const ast::Symbol* symbol, const ast::SubroutineSymbol* fn) {
    for (; symbol;) {
        if (symbol == fn)
            return true;
        auto parent = symbol->getParentScope();
        symbol = parent ? &parent->asSymbol() : nullptr;
    }
    return false;
}
// Native does not inline nonstatic class functions into their callers. For
// these functions, unchanged direct global driver inputs need no global replay.
// Compare a multiset, keeping bounds, flags, kind and source; indirect paths and
// static locals are deliberately left to the existing complete replay path.
bool sameDirectExternalDrivers(const ast::SubroutineSymbol& original,
                               const analysis::AnalyzedProcedure& before,
                               const ast::SubroutineSymbol& replacement,
                               const analysis::AnalyzedProcedure& after,
                               const lsp::RequestContext& ctx) {
    auto collect = [&](const ast::SubroutineSymbol& fn,
                       const analysis::AnalyzedProcedure& procedure,
                       std::vector<const analysis::ValueDriver*>& values) {
        for (const auto* driver : procedure.getDrivers()) {
            ctx.checkpoint("direct class driver effect proof");
            const auto& symbol = driver->getSymbol();
            if (contained(&symbol, &fn)) {
                if (ast::VariableSymbol::isKind(symbol.kind) &&
                    symbol.as<ast::VariableSymbol>().lifetime == ast::VariableLifetime::Static)
                    return false;
                continue;
            }
            if (!ast::VariableSymbol::isKind(symbol.kind) || symbol.isConnectedToRefPort() ||
                driver->kind != analysis::DriverKind::Procedural ||
                driver->source != analysis::DriverSource::Subroutine ||
                driver->flags.has(
                    analysis::DriverFlags::InputPort | analysis::DriverFlags::OutputPort |
                    analysis::DriverFlags::ClockVar | analysis::DriverFlags::Initializer |
                    analysis::DriverFlags::FromSideEffect | analysis::DriverFlags::ViaIndirectPort))
                return false;
            for (const auto& component : driver->path) {
                if (component.kind == ast::ExpressionKind::HierarchicalValue)
                    return false;
                if (const auto* member = component.as_if<ast::MemberAccessExpression>();
                    member && member->value().type->isVirtualInterface())
                    return false;
            }
            values.push_back(driver);
        }
        return true;
    };
    std::vector<const analysis::ValueDriver*> oldDrivers, newDrivers;
    if (!collect(original, before, oldDrivers) || !collect(replacement, after, newDrivers) ||
        oldDrivers.size() != newDrivers.size())
        return false;
    std::vector<bool> matched(oldDrivers.size(), false);
    for (const auto* driver : newDrivers) {
        ctx.checkpoint("direct class driver effect proof");
        bool found = false;
        for (size_t i = 0; i < oldDrivers.size(); ++i) {
            const auto* previous = oldDrivers[i];
            if (!matched[i] && &previous->getSymbol() == &driver->getSymbol() &&
                previous->getBounds() == driver->getBounds() && previous->flags == driver->flags &&
                previous->kind == driver->kind && previous->source == driver->source) {
                matched[i] = true;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}
bool outerMethodCheck(const Diagnostic& d, const ast::SubroutineSymbol* fn) {
    if (d.symbol != fn)
        return false;
    return d.code == diag::UnusedClassMethod || d.code == diag::UnusedLocalClassMethod ||
           d.code == diag::UnusedConstructor || d.code == diag::UnusedSubroutine ||
           d.code == diag::UnusedPackageSubroutine || d.code == diag::ShadowHierarchy ||
           d.code == diag::ShadowValue;
}
void bindUris(ServerDiagClient& output, ServerDriver& driver) {
    // This output is private to one serialized publication. Snapshot only open
    // aliases once, instead of scanning every project document per diagnostic.
    std::unordered_map<URI, URI> aliases;
    for (const auto& [key, doc] : driver.docs)
        if (driver.isDocumentOpen(key))
            aliases.emplace(key, key);
    for (const auto& [key, doc] : driver.docs)
        if (driver.isDocumentOpen(key))
            aliases.try_emplace(URI::fromFile(driver.sm.getFullPath(doc->getBuffer())), key);
    output.setUriResolver([aliases = std::move(aliases)](const URI& physical) {
        auto found = aliases.find(physical);
        return found == aliases.end() ? physical : found->second;
    });
}
// Capture the actual publication URI selected by the diagnostic engine, including
// macro provenance and editor aliases. The scratch client never publishes.
struct DiagnosticBuckets : ServerDiagClient {
    size_t phase = 0;
    std::array<std::unordered_map<URI, Diagnostics>, 3> buckets;
    DiagnosticBuckets(ServerDriver& d) : ServerDiagClient(d.sm, d.client) {
        disableDestructorPublication();
        bindUris(*this, d);
    }
    void report(const ReportedDiagnostic& diagnostic) override {
        clear();
        ServerDiagClient::report(diagnostic);
        for (const auto& [key, values] : renderedDiagnostics())
            if (!values.empty())
                buckets[phase][key].push_back(diagnostic.originalDiagnostic);
    }
};
struct LocalBoundaries : ast::ASTVisitor<LocalBoundaries, ast::VisitFlags::AllGood> {
    void handle(const ast::HierarchicalValueExpression&) {
        require(false, "hierarchical method references require full diagnostics");
    }
};
std::unordered_set<const syntax::SyntaxNode*> independentNativeReceiverReads(
    const analysis::AnalysisManager& manager, const ast::Compilation& owner,
    std::span<const ast::SubroutineSymbol* const> edited, SourceManager& sources,
    const lsp::RequestContext& ctx, std::unordered_set<const syntax::SyntaxNode*>& calleeReads) {
    UvmPhaseTrace phases;
    std::unordered_set<const syntax::SyntaxNode*> result;
    auto editedSource = [&](const syntax::SyntaxNode& candidate) {
        for (auto original : edited) {
            const auto* syntax = original->getSyntax();
            if (&candidate == syntax)
                return true;
            // Multiple include occurrences can have different CST pointers for
            // the same edited physical method. Neither is an independent peer.
            const auto first = candidate.getFirstToken().location();
            const auto last = candidate.getLastToken().range().end();
            const auto oldFirst = syntax->getFirstToken().location();
            const auto oldLast = syntax->getLastToken().range().end();
            if (sources.isFileLoc(first) && sources.isFileLoc(last) &&
                sources.isFileLoc(oldFirst) && sources.isFileLoc(oldLast) &&
                first.offset() == oldFirst.offset() && last.offset() == oldLast.offset() &&
                sources.getFullPath(first.buffer()) == sources.getFullPath(oldFirst.buffer()) &&
                sources.getFullPath(last.buffer()) == sources.getFullPath(oldLast.buffer()))
                return true;
        }
        return false;
    };
    for (auto fn : manager.getAnalyzedSubroutines()) {
        ctx.checkpoint("independent native receiver reads");
        const auto* syntax = fn->getSyntax();
        const auto* parent = fn->getParentScope();
        const auto* cls = parent ? parent->asSymbol().as_if<ast::ClassType>() : nullptr;
        if (&fn->getCompilation() != &owner || !syntax || (cls && cls->genericClass) ||
            fn->flags.has(ast::MethodFlags::Static | ast::MethodFlags::Constructor) ||
            !sources.isFileLoc(syntax->getFirstToken().location()) ||
            !sources.isFileLoc(syntax->getLastToken().range().end()) || editedSource(*syntax))
            continue;
        const auto* procedure = manager.getAnalyzedSubroutine(*fn);
        if (!procedure)
            continue;
        for (auto call : procedure->getCallExpressions()) {
            ctx.checkpoint("independent native receiver call");
            if (call->isSystemCall())
                continue;
            const auto* target = std::get<const ast::SubroutineSymbol*>(call->subroutine);
            if (target->flags.has(ast::MethodFlags::Static | ast::MethodFlags::Constructor) ||
                target->subroutineKind != ast::SubroutineKind::Function)
                continue;
            // A completed, unrelated method already references this callee.
            // Preserve both implementation and extern prototype identities. This
            // proves only aggregate read usage; the effect guards remain separate.
            const auto* targetScope = target->getParentScope();
            const auto* targetClass = targetScope ? targetScope->asSymbol().as_if<ast::ClassType>()
                                                  : nullptr;
            const auto* calleeSyntax = target->getSyntax();
            if (targetScope && (!targetClass || !targetClass->genericClass) &&
                &targetScope->getCompilation() == &owner && calleeSyntax &&
                sources.isFileLoc(calleeSyntax->getFirstToken().location()) &&
                sources.isFileLoc(calleeSyntax->getLastToken().range().end())) {
                calleeReads.insert(calleeSyntax);
                if (const auto* prototype = target->getPrototype())
                    if (const auto* node = prototype->getSyntax())
                        calleeReads.insert(node);
            }
            if (!call->thisClass())
                continue;
            const auto* value = call->thisClass()->getSymbolReference(false);
            const auto* property = value ? value->as_if<ast::ClassPropertySymbol>() : nullptr;
            if (!property || property->lifetime != ast::VariableLifetime::Automatic)
                continue;
            const auto* declaration = property->getParentScope();
            const auto* declarationClass = declaration
                                               ? declaration->asSymbol().as_if<ast::ClassType>()
                                               : nullptr;
            const auto* receiverClass =
                property->getType().getCanonicalType().as_if<ast::ClassType>();
            if (!declarationClass || declarationClass->genericClass || !receiverClass ||
                receiverClass->genericClass || &declarationClass->getCompilation() != &owner)
                continue;
            if (const auto* node = property->getSyntax())
                result.insert(node);
        }
    }
    phases.mark("body-independent-receiver-reads");
    return result;
}
std::vector<std::string> rendered(const Diagnostics& diagnostics, SourceManager& sm,
                                  SlangLspClient& client,
                                  const DiagnosticEngine::SettingsSnapshot& settings) {
    auto output = std::make_shared<ServerDiagClient>(sm, client);
    output->disableDestructorPublication();
    DiagnosticEngine engine(sm);
    engine.applySettings(settings);
    engine.setMappingsFromPragmas();
    engine.addClient(output);
    for (const auto& d : diagnostics)
        engine.issue(d);
    std::vector<std::string> result;
    for (const auto& [uri, values] : output->renderedDiagnostics())
        for (const auto& value : values)
            result.push_back(uri.str() + rfl::json::write(value));
    std::sort(result.begin(), result.end());
    return result;
}
} // namespace

std::shared_ptr<const BodyDiagnosticBaseline> BodyDiagnosticCache::capture(
    ServerDriver& d, const URI& uri, size_t start, size_t stop, const lsp::RequestContext& ctx,
    bool deferPreparation) {
    ctx.checkpoint("before method diagnostic capture");
    auto baseline = std::make_shared<BodyDiagnosticBaseline>(uri);
    baseline->driver = &d;
    baseline->context = d.m_snapshotOwner;
    baseline->begin = start;
    baseline->end = stop;
    baseline->bufferLimit = uint32_t(d.sm.getSourceLocationEntryCount());
    baseline->settings = d.diagEngine.captureSettings();
    baseline->analysisOptions = d.options.getOrDefault<analysis::AnalysisOptions>();
    // A foreground query can replace the root after the last publication.
    // Its current source buffers and the driver's publication generation do
    // not prove that this new root's analysis is ready. Capture only borrows
    // prepared semantics/dataflow. An explicitly deferred capture retains the exact
    // pre-edit owner and witnesses, without preparing analysis on the caller.
    require(d.comp && (deferPreparation || !d.comp->needsDiagnosticPreparation()) &&
                d.m_fullDiagnosticGeneration &&
                d.m_fullDiagnosticGeneration == d.m_diagnosticGeneration &&
                d.comp->containsCurrentSource(d.sm.getFullPath(d.getDocument(uri)->getBuffer())),
            "local diagnostics require a completed current full-design baseline");
    for (auto& key : d.m_openDocs)
        require(!d.hasExplicitContextError(key) && d.comp->containsCurrentSource(d.sm.getFullPath(
                                                       d.getDocument(key)->getBuffer())),
                "open document outside full-design diagnostic coverage");
    require(baseline->settings.errorLimit == 0, "bounded diagnostic streams require full analysis");
    auto design = d.comp->createActiveDesignContext({}, ctx);
    auto owner = design.retainCompilation();
    baseline->owner = owner;
    baseline->pendingDesign = std::make_shared<ActiveDesignContext>(std::move(design));
    baseline->text = std::string(d.getDocument(uri)->getText());
    baseline->config = rfl::json::write(d.m_config);
    baseline->targetPath = d.sm.getFullPath(d.getDocument(uri)->getBuffer());
    auto publication = d.m_nativeDiagnosticSnapshot;
    if (publication && publication->generation == d.m_fullDiagnosticGeneration &&
        publication->context.lock() == d.m_snapshotOwner && publication->owner.lock() == owner &&
        publication->config == baseline->config) {
        baseline->publication = std::move(publication);
        ctx.checkpoint("native diagnostic routes reused");
    }
    if (deferPreparation && d.comp->needsDiagnosticPreparation())
        return baseline;
    return completeCapture(d, std::move(baseline), ctx);
}

std::shared_ptr<const BodyDiagnosticBaseline> BodyDiagnosticCache::completeCapture(
    ServerDriver& d, std::shared_ptr<const BodyDiagnosticBaseline> saved,
    const lsp::RequestContext& ctx) {
    if (!saved->pendingDesign)
        return saved;
    require(saved->driver == &d && saved->context.lock() == d.m_snapshotOwner &&
                saved->config == rfl::json::write(d.m_config),
            "deferred diagnostic baseline belongs to another context");
    // Work on a private copy: cancellation leaves the exact pre-edit witness available.
    auto baseline = std::make_shared<BodyDiagnosticBaseline>(*saved);
    auto& design = *baseline->pendingDesign;
    auto owner = baseline->owner;
    const auto start = baseline->begin;
    require(!owner->hasFatalErrors(), "fatal baseline diagnostics require native analysis");
    if (!design.getAnalysis().diagnosticsReady()) {
        UvmPhaseTrace phases;
        if (!owner->isElaborated()) {
            SharedSemanticWarmup warmup(*owner, ctx);
            owner->getRoot().visit(warmup);
        }
        phases.mark("shared-baseline-binding-warmup");
        ctx.checkpoint("before shared baseline semantic diagnostics");
        (void)owner->getSemanticDiagnostics();
        phases.mark("shared-baseline-final-semantics");
        ctx.checkpoint("after shared baseline semantic diagnostics");
        require(!owner->hasFatalErrors(), "fatal baseline diagnostics require native analysis");
    }
    auto& manager = design.getAnalysis().getAnalysisManager(ctx);
    baseline->completeAnalysis = &manager;
    baseline->procedureReuseAnalysis = &design.getAnalysis().getProcedureReuseBaseline(ctx);
    baseline->analysisOptions = manager.getOptions();
    if (modelOwnerTraceEnabled())
        traceModelOwner("baseline-pin", owner.get(), nullptr, owner->getTotalBytesAllocated(),
                        design.getAnalysis().analysisMemoryBytes(), owner.use_count());
    const auto& path = baseline->targetPath;
    for (auto fn : manager.getAnalyzedSubroutines()) {
        auto syntax = fn->getSyntax();
        if (!syntax)
            continue;
        auto first = syntax->getFirstToken().location();
        if (!d.sm.isFileLoc(first) || first.offset() != start ||
            d.sm.getFullPath(first.buffer()) != path)
            continue;
        const auto parentKind = fn->getParentScope()->asSymbol().kind;
        const bool ordinaryClass = parentKind == ast::SymbolKind::ClassType &&
                                   !fn->flags.has(ast::MethodFlags::Static |
                                                  ast::MethodFlags::Constructor);
        const auto* instance = fn->getParentScope()->asSymbol().as_if<ast::InstanceBodySymbol>();
        const bool moduleContext = instance && instance->getDefinition().definitionKind ==
                                                   ast::DefinitionKind::Module;
        const bool automaticContext =
            (parentKind == ast::SymbolKind::Package || moduleContext) &&
            fn->defaultLifetime == ast::VariableLifetime::Automatic &&
            !fn->flags.has(ast::MethodFlags::Static | ast::MethodFlags::Constructor) &&
            std::ranges::all_of(fn->getArguments(), [](const ast::FormalArgumentSymbol* arg) {
                return arg->direction == ast::ArgumentDirection::In;
            });
        require(ordinaryClass || automaticContext,
                "local diagnostics require ordinary class methods or input-only automatic "
                "package/module subroutines");
        require(fn->getCompilation().hasFlag(
                    ast::CompilationFlags::TrackSubroutineBodyDependencies) &&
                    !fn->bodyHasExternalDependents.load(std::memory_order_relaxed),
                "method has external elaboration dependencies");
        baseline->originals.push_back(fn);
    }
    require(!baseline->originals.empty(), "method absent from full-design analysis");

    if (auto outside = owner->getReferenceUsageExcludingBodies(baseline->originals, [&] {
            ctx.checkpoint("native reference provenance snapshot");
        })) {
        baseline->nativeReferences = std::make_shared<NativeReferenceUsage>(std::move(*outside));
    }

    baseline->independentReceiverReads = independentNativeReceiverReads(
        manager, *owner, baseline->originals, d.sm, ctx, baseline->independentCalleeReads);

    // Native coalesces these two diagnostics by location across instances. Keep
    // the complete timing-only set together; retaining an "unaffected" sibling
    // could duplicate or suppress a changed caller's diagnostic after coalescing.
    if (baseline->originals.front()->subroutineKind == ast::SubroutineKind::Task) {
        baseline->analysis = &manager;
        baseline->timingCallers = manager.getTaskTimingProcedures(
            [&] { ctx.checkpoint("task diagnostic caller dependencies"); });
    }

    // replaceBuffer clears the editor BufferID's FileInfo even while old FileData
    // is pinned. Keep one target-file value, not a chain of edited source owners.
    baseline->targetBuffer =
        baseline->originals.front()->getSyntax()->getFirstToken().location().buffer();
    baseline->sourcePins = d.sm.retainBuffers({&baseline->targetBuffer, 1});
    if (!baseline->publication) {
        for (const auto& tree : owner->getSyntaxTrees())
            baseline->diagnostics[0].append_range(tree->diagnostics());
        baseline->diagnostics[1].append_range(owner->getSemanticDiagnostics());
        baseline->diagnostics[2].append_range(manager.getDiagnostics());
    }
    ctx.checkpoint("after method diagnostic capture");
    // Transfer the committed publication to this certificate. Its immutable
    // physical renderings can be retained without pinning an extra semantic owner.
    if (baseline->publication && d.m_nativeDiagnosticSnapshot == baseline->publication)
        d.m_nativeDiagnosticSnapshot.reset();
    baseline->pendingDesign.reset();
    return baseline;
}

BodyDiagnosticCache::BodyDiagnosticCache(ServerDriver& d, const URI& target, size_t start,
                                         size_t stop,
                                         const std::shared_ptr<syntax::SyntaxTree>& tree,
                                         bool sensitive, const lsp::RequestContext& ctx,
                                         std::shared_ptr<const BodyDiagnosticBaseline> saved,
                                         const std::shared_ptr<syntax::SyntaxTree>& current) :
    driver(d), uri(target), begin(start), end(stop), lineSensitive(sensitive),
    baselineLine(d.sm.getRawLineNumber(SourceLocation(tree->getSourceBufferIds().front(), stop))) {
    UvmPhaseTrace phases;
    auto baseline = saved ? std::move(saved) : capture(d, target, start, stop, ctx);
    require(baseline->driver == &d && baseline->context.lock() == d.m_snapshotOwner &&
                baseline->uri == uri && baseline->begin == start && baseline->end == stop,
            "original diagnostic baseline belongs to another context");
    bufferLimit = baseline->bufferLimit;
    settings = baseline->settings;
    analysisOptions = baseline->analysisOptions;
    analysis = baseline->analysis;
    completeAnalysis = baseline->completeAnalysis;
    procedureReuseAnalysis = baseline->procedureReuseAnalysis;
    timingCallers = baseline->timingCallers;
    owner = baseline->owner;
    originals = baseline->originals;
    independentReceiverReads = baseline->independentReceiverReads;
    independentCalleeReads = baseline->independentCalleeReads;
    nativeReferences = baseline->nativeReferences;
    if (lineSensitive) {
        originalTarget = baseline->targetBuffer;
        lineText = std::make_shared<const std::string>(baseline->text);
        linePins = baseline->sourcePins;
    }
    auto path = d.sm.getFullPath(d.getDocument(uri)->getBuffer());
    // Macro locations frequently revisit the same physical buffers. Resolve each
    // buffer's path once during this construction, including synthetic buffers.
    std::unordered_map<BufferID, bool> targetBuffers;
    auto targetLocation = [&](SourceLocation loc) {
        if (!loc)
            return false;
        auto [it, inserted] = targetBuffers.try_emplace(loc.buffer(), false);
        if (inserted)
            it->second = d.sm.getFullPath(loc.buffer()) == path;
        return it->second;
    };
    auto local = [&](const Diagnostic& diagnostic) {
        if ((diagnostic.code == diag::BlockingDelayInTask ||
             diagnostic.code == diag::AlwaysWithoutTimingControl) &&
            analysis)
            return true;
        auto loc = d.sm.getFullyExpandedLoc(diagnostic.location);
        if (targetLocation(loc) && begin <= loc.offset() && loc.offset() < end &&
            !std::ranges::any_of(originals,
                                 [&](auto fn) { return outerMethodCheck(diagnostic, fn); }))
            return true;
        for (auto fn : originals)
            if (contained(diagnostic.symbol, fn) && !outerMethodCheck(diagnostic, fn))
                return true;
        return false;
    };
    Diagnostics baselineTimingDiagnostics;
    Diagnostics baselineDiagnostics;
    std::array<Diagnostics, 3> retainedDiagnostics;
    // Resolve current editor aliases once per physical URI during this
    // serialized construction. The memo is discarded before the next edit.
    std::unordered_map<URI, URI> publicationUris;
    auto publicationUri = [&](const URI& physical) -> const URI& {
        auto found = publicationUris.find(physical);
        if (found == publicationUris.end())
            found = publicationUris.emplace(physical, d.diagClient->resolvePublicationUri(physical))
                        .first;
        return found->second;
    };
    const auto& raw = baseline->publication ? baseline->publication->diagnostics
                                            : baseline->diagnostics;
    auto touchesBody = [&](auto&& self, const Diagnostic& value) -> bool {
        auto location = [&](SourceLocation loc) {
            for (auto actual : {d.sm.getFullyExpandedLoc(loc), d.sm.getFullyOriginalLoc(loc)})
                if (targetLocation(actual) && begin <= actual.offset() && actual.offset() < end)
                    return true;
            return false;
        };
        if (location(value.location))
            return true;
        for (const auto& range : value.ranges)
            if (location(range.start()) || location(range.end()))
                return true;
        for (const auto& note : value.notes)
            if (self(self, note))
                return true;
        return false;
    };
    // A global diagnostic that cites this body must be regenerated, even when
    // its driver inputs stay equal: the cited assignment may move inside it.
    for (const auto& diagnostic : raw[2]) {
        ctx.checkpoint("native driver diagnostic location proof");
        if (!std::ranges::any_of(originals,
                                 [&](auto fn) { return contained(diagnostic.symbol, fn); }) &&
            touchesBody(touchesBody, diagnostic))
            stableExternalDriverLocations = false;
    }
    for (size_t phase = 0; phase < raw.size(); ++phase)
        for (size_t index = 0; index < raw[phase].size(); ++index) {
            ctx.checkpoint("local diagnostic publication certificate");
            const auto& diagnostic = raw[phase][index];
            auto loc = d.sm.getFullyExpandedLoc(diagnostic.location);
            bool belongs = phase == 0
                               ? targetLocation(loc) && begin <= loc.offset() && loc.offset() < end
                               : local(diagnostic);
            if (belongs) {
                baselineDiagnostics.push_back(diagnostic);
                if (analysis && phase == 2 &&
                    (diagnostic.code == diag::BlockingDelayInTask ||
                     diagnostic.code == diag::AlwaysWithoutTimingControl)) {
                    if (baseline->publication) {
                        for (const auto& route : baseline->publication->routes[phase][index])
                            affectedUris.insert(publicationUri(route));
                    }
                    else
                        baselineTimingDiagnostics.push_back(diagnostic);
                }
            }
            else if (baseline->publication) {
                for (const auto& route : baseline->publication->routes[phase][index]) {
                    const auto& key = publicationUri(route);
                    retained[phase][key].push_back(diagnostic);
                    retainedNativeIndices[phase][key].push_back(index);
                }
            }
            else
                retainedDiagnostics[phase].push_back(diagnostic);
        }
    if (!baselineTimingDiagnostics.empty()) {
        auto collector = std::make_shared<DiagnosticBuckets>(d);
        DiagnosticEngine engine(d.sm);
        engine.applySettings(settings);
        engine.setMappingsFromPragmas();
        engine.addClient(collector);
        for (const auto& diagnostic : baselineTimingDiagnostics) {
            ctx.checkpoint("task diagnostic baseline routes");
            engine.issue(diagnostic);
        }
        for (const auto& [key, _] : collector->buckets[0])
            affectedUris.insert(key);
    }
    phases.mark("body-diagnostic-partition");
    auto initial = analyze(tree, ctx);
    if (initial.fullFlow) {
        baselineDiagnostics.clear();
        for (size_t phase = 0; phase < raw.size(); ++phase)
            for (const auto& diagnostic : raw[phase]) {
                auto loc = d.sm.getFullyExpandedLoc(diagnostic.location);
                if (phase == 2 ||
                    (phase == 0 ? targetLocation(loc) && begin <= loc.offset() && loc.offset() < end
                                : local(diagnostic)))
                    baselineDiagnostics.push_back(diagnostic);
            }
    }
    Diagnostics combined;
    for (auto& phase : initial.diagnostics)
        combined.append_range(phase);
    // Both streams use the same settings and source projection, with no error
    // limit. Equal scalar raw diagnostics therefore contribute identical LSP
    // values to this multiset comparison. Cancel only proven pairs, retaining
    // duplicates and rendering every unknown/custom argument normally. This
    // proof is constructed anew for this certificate, never retained across requests.
    ctx.checkpoint("initial diagnostic equality candidates");
    std::unordered_multimap<size_t, size_t> candidates;
    candidates.reserve(combined.size());
    for (size_t i = 0; i < combined.size(); ++i) {
        if ((i & 255u) == 0)
            ctx.checkpoint("initial diagnostic equality candidates");
        candidates.emplace(rawDiagnosticHash(combined[i]), i);
    }
    std::vector<bool> matched(combined.size(), false);
    Diagnostics expectedRemainder, actualRemainder;
    for (const auto& diagnostic : baselineDiagnostics) {
        ctx.checkpoint("initial diagnostic equality matching");
        auto [first, last] = candidates.equal_range(rawDiagnosticHash(diagnostic));
        bool found = false;
        for (auto it = first; it != last; ++it) {
            if (!matched[it->second] && scalarDiagnosticEqual(diagnostic, combined[it->second])) {
                matched[it->second] = true;
                found = true;
                break;
            }
        }
        if (!found)
            expectedRemainder.push_back(diagnostic);
    }
    for (size_t i = 0; i < combined.size(); ++i) {
        if ((i & 255u) == 0)
            ctx.checkpoint("initial diagnostic equality remainder");
        if (!matched[i])
            actualRemainder.push_back(combined[i]);
    }
    auto expected = rendered(expectedRemainder, d.sm, d.client, settings);
    auto actual = rendered(actualRemainder, d.sm, d.client, settings);
    require(expected == actual, "detached diagnostic baseline differs from full design");
    phases.mark("body-diagnostic-compare");
    baselineBinding = initial.compilation;
    replayEffects = initial.fullFlow;
    baselineClassLookups = initial.hasClassLookups;
    specializations = std::move(initial.specializations);
    references = std::move(initial.references);
    calls = std::move(initial.calls);
    effectSensitiveCalls = std::move(initial.effectSensitiveCalls);
    // Test current dependency / reference eligibility before formatting all retained
    // diagnostics. Unsupported first edits should not pay for a baseline we discard.
    if (current)
        validateSlice(analyze(current, ctx));
    phases.mark("body-diagnostic-eligibility");
    if (!baseline->publication) {
        auto collector = std::make_shared<DiagnosticBuckets>(d);
        DiagnosticEngine engine(d.sm);
        engine.applySettings(settings);
        engine.setMappingsFromPragmas();
        engine.addClient(collector);
        for (size_t phase = 0; phase < retainedDiagnostics.size(); ++phase) {
            collector->phase = phase;
            for (const auto& diagnostic : retainedDiagnostics[phase]) {
                ctx.checkpoint("local diagnostic bucket formatting");
                engine.issue(diagnostic);
            }
        }
        retained = std::move(collector->buckets);
    }
    else
        ctx.checkpoint("native diagnostic buckets reused");
    phases.mark(baseline->publication ? "body-diagnostic-native-routes"
                                      : "body-diagnostic-formatted-routes");
    affectedUris.insert(uri);
    const auto bodyStart = originals.front()
                               ->getSyntax()
                               ->as<syntax::FunctionDeclarationSyntax>()
                               .semi.range()
                               .end()
                               .offset();
    for (const auto& phase : retained)
        for (const auto& [key, diagnostics] : phase)
            for (const auto& diagnostic : diagnostics) {
                bool affected = false;
                auto location = [&](SourceLocation loc) {
                    for (auto actual :
                         {d.sm.getFullyExpandedLoc(loc), d.sm.getFullyOriginalLoc(loc)})
                        if (targetLocation(actual)) {
                            affected = true;
                            require(actual.offset() <= bodyStart || actual.offset() >= end,
                                    "outside diagnostic references the replaced body");
                        }
                };
                auto inspect = [&](auto&& self, const Diagnostic& value) -> void {
                    location(value.location);
                    for (auto range : value.ranges) {
                        location(range.start());
                        location(range.end());
                    }
                    for (auto& note : value.notes)
                        self(self, note);
                };
                inspect(inspect, diagnostic);
                if (affected)
                    affectedUris.insert(key);
            }
    if (baseline->publication) {
        formattedPublication = baseline->publication;
        nativeTargetBuffers.insert(baseline->targetBuffer);
        for (const auto& [buffer, isTarget] : targetBuffers)
            if (isTarget)
                nativeTargetBuffers.insert(buffer);
        // The exact prefix preceding this function never changes within this
        // certificate. Reuse only scalar diagnostics whose raw locations and
        // rendered ranges (including every note) stay strictly inside it.
        // Tail/body diagnostics still need the current source projection.
        const auto prefixEnd = toPosition(
            SourceLocation(d.getDocument(uri)->getBuffer(), begin), d.sm);
        const auto targetPath = path.generic_string();
        auto before = [&](const lsp::Position& position) {
            return std::tie(position.line, position.character) <
                   std::tie(prefixEnd.line, prefixEnd.character);
        };
        // The snapshot accepts only the builtin type formatter. For retained
        // Native diagnostics, primitive and packed bit-vector types are fixed
        // values of the completed owner. Canonical enums from the current frozen
        // provider are fixed too. Never extend this to fresh diagnostics or
        // types declared in the replaced function.
        auto fixedBitType = [&](auto&& self, const ast::Type& type, size_t depth) -> bool {
            if (depth >= 64 || std::ranges::any_of(originals, [&](auto fn) {
                    return contained(&type, fn);
                }))
                return false;
            const auto& canonical = type.getCanonicalType();
            if (&canonical != &type && std::ranges::any_of(originals, [&](auto fn) {
                    return contained(&canonical, fn);
                }))
                return false;
            switch (canonical.kind) {
                case ast::SymbolKind::ScalarType:
                case ast::SymbolKind::PredefinedIntegerType:
                    return true;
                case ast::SymbolKind::PackedArrayType:
                    return self(self, canonical.as<ast::PackedArrayType>().elementType, depth + 1);
                case ast::SymbolKind::EnumType: {
                    // Aliases have already passed the edited-method containment
                    // guards above, as for packed integer aliases. Stability
                    // comes from the provider owning the canonical enum, never
                    // from an ordinary consumer enum declaration.
                    const auto* scope = canonical.getParentScope();
                    if (!scope)
                        return false;
                    const auto* compilation = &scope->getCompilation();
                    return (d.m_trustedUvm &&
                            compilation == d.m_trustedUvm->compilation.get()) ||
                           (d.m_trustedScopes &&
                            compilation == d.m_trustedScopes->compilation.get());
                }
                default:
                    return false;
            }
        };
        auto stableArguments = [&](const Diagnostic& value) {
            for (const auto& arg : value.args) {
                if (std::holds_alternative<ConstantValue>(arg))
                    return false;
                if (auto custom = std::get_if<Diagnostic::CustomArgType>(&arg)) {
                    if (!settings.useBuiltinTypeFormatter ||
                        custom->first != SLANG_TYPEOF(const ast::Type*))
                        return false;
                    auto type = std::any_cast<const ast::Type*>(&custom->second);
                    if (!type || !*type || !fixedBitType(fixedBitType, **type, 0))
                        return false;
                }
            }
            return true;
        };
        auto stableRaw = [&](auto&& self, const Diagnostic& value) -> bool {
            auto stableLocation = [&](SourceLocation location) {
                if (!location)
                    return true;
                for (auto actual : {d.sm.getFullyExpandedLoc(location),
                                    d.sm.getFullyOriginalLoc(location)})
                    if (nativeTargetBuffers.contains(actual.buffer()) && actual.offset() >= begin)
                        return false;
                return true;
            };
            if (!stableLocation(value.location))
                return false;
            for (auto range : value.ranges)
                if (!stableLocation(range.start()) || !stableLocation(range.end()))
                    return false;
            if (!stableArguments(value))
                return false;
            return std::ranges::all_of(value.notes, [&](const Diagnostic& note) {
                return self(self, note);
            });
        };
        std::vector<CachedFormatted> prefixEntries;
        std::unordered_map<const Diagnostic*, size_t> prefixRetained;
        for (size_t phase = 0; phase < retained.size(); ++phase)
            for (const auto& [key, indices] : retainedNativeIndices[phase]) {
                if (key != uri || formattedPublication->rendered[phase].size() !=
                                      formattedPublication->diagnostics[phase].size())
                    continue;
                const auto& values = retained[phase].at(key);
                for (size_t i = 0; i < indices.size(); ++i) {
                    ctx.checkpoint("prepare prefix diagnostic rendering");
                    const auto index = indices[i];
                    const auto& raw = formattedPublication->diagnostics[phase][index];
                    const auto& rendered = formattedPublication->rendered[phase][index];
                    if (!rendered.key || rendered.values.size() != 1 || !stableRaw(stableRaw, raw))
                        continue;
                    const auto& entry = rendered.values.front();
                    if (entry.uri.getPath() != targetPath || !before(entry.value.range.end))
                        continue;
                    bool stable = true;
                    if (entry.value.relatedInformation)
                        for (const auto& note : *entry.value.relatedInformation)
                            if (note.location.uri.getPath() == targetPath &&
                                !before(note.location.range.end))
                                stable = false;
                    if (!stable)
                        continue;
                    prefixRetained.emplace(&values[i], prefixEntries.size());
                    prefixEntries.push_back({&raw, &rendered});
                    if (std::ranges::any_of(raw.args, [](const auto& arg) {
                            return std::holds_alternative<Diagnostic::CustomArgType>(arg);
                        }))
                        ctx.checkpoint("retain fixed Native type rendering");
                    if (std::ranges::any_of(raw.args, [](const auto& arg) {
                            const auto* custom = std::get_if<Diagnostic::CustomArgType>(&arg);
                            if (!custom || custom->first != SLANG_TYPEOF(const ast::Type*))
                                return false;
                            const auto* type = std::any_cast<const ast::Type*>(&custom->second);
                            return type && *type && (*type)->getCanonicalType().kind ==
                                                       ast::SymbolKind::EnumType;
                        }))
                        ctx.checkpoint("retain frozen enum rendering");
                }
            }
        ctx.checkpoint("before prefix diagnostic rendering commit");
        formattedEntries = std::move(prefixEntries);
        formattedRetained = std::move(prefixRetained);
        const size_t budget = d.m_config.experimentalMethodBodyQueries.value() &&
                                      d.m_config.experimentalMethodBodyDiagnostics.value() &&
                                      d.m_config.experimentalMethodBodyCertification.value()
                                  ? std::min<size_t>(d.m_config.methodBodyRenderingCacheKiB.value(),
                                                     16384) * 1024
                                  : 0;
        constexpr size_t memoMetadata = sizeof(TargetRenderingMemo) + 512;
        require(begin <= end && end <= baseline->text.size(), "retained diagnostic source bounds");
        const size_t outsideBytes = baseline->text.size() - (end - begin);
        if (!initial.fullFlow && budget > memoMetadata &&
            outsideBytes <= (budget - memoMetadata) / 2) {
            const auto originalEnd = toPosition(
                SourceLocation(tree->getSourceBufferIds().front(), end), d.sm);
            auto outsideRange = [&](const lsp::Range& range) {
                return before(range.end) ||
                       std::tie(range.start.line, range.start.character) >
                           std::tie(originalEnd.line, originalEnd.character);
            };
            auto outsideRaw = [&](auto&& self, const Diagnostic& value) -> bool {
                auto outsideLocation = [&](SourceLocation location) {
                    if (!location)
                        return true;
                    for (auto actual : {d.sm.getFullyExpandedLoc(location),
                                        d.sm.getFullyOriginalLoc(location)})
                        if (targetLocation(actual) && begin <= actual.offset() &&
                            actual.offset() <= end)
                            return false;
                    return true;
                };
                if (!outsideLocation(value.location))
                    return false;
                for (auto range : value.ranges) {
                    if (!outsideLocation(range.start()) || !outsideLocation(range.end()))
                        return false;
                    for (auto endpoints : {
                             std::pair{d.sm.getFullyExpandedLoc(range.start()),
                                       d.sm.getFullyExpandedLoc(range.end())},
                             std::pair{d.sm.getFullyOriginalLoc(range.start()),
                                       d.sm.getFullyOriginalLoc(range.end())}})
                        if (targetLocation(endpoints.first) && targetLocation(endpoints.second) &&
                            endpoints.first.offset() < begin && endpoints.second.offset() > end)
                            return false;
                }
                if (!stableArguments(value))
                    return false;
                return std::ranges::all_of(value.notes, [&](const Diagnostic& note) {
                    return self(self, note);
                });
            };
            size_t charge = memoMetadata + 2 * outsideBytes;
            std::unordered_set<const Diagnostic*> eligible;
            bool oversized = false;
            for (size_t phase = 0; phase < retained.size() && !oversized; ++phase)
                for (const auto& [key, indices] : retainedNativeIndices[phase]) {
                    if (key != uri || formattedPublication->rendered[phase].size() !=
                                          formattedPublication->diagnostics[phase].size())
                        continue;
                    const auto& values = retained[phase].at(key);
                    for (size_t i = 0; i < indices.size(); ++i) {
                        ctx.checkpoint("prepare target rendering eligibility");
                        if (formattedRetained.contains(&values[i]) || !outsideRaw(outsideRaw, values[i]))
                            continue;
                        const auto& rendered = formattedPublication->rendered[phase][indices[i]];
                        if (rendered.values.size() != 1)
                            continue;
                        const auto& entry = rendered.values.front();
                        if (entry.uri.getPath() != targetPath || !outsideRange(entry.value.range))
                            continue;
                        bool stable = true;
                        if (entry.value.relatedInformation)
                            for (const auto& note : *entry.value.relatedInformation)
                                if (note.location.uri.getPath() == targetPath &&
                                    !outsideRange(note.location.range))
                                    stable = false;
                        if (!stable)
                            continue;
                        constexpr size_t entryMetadata = 128;
                        if (entryMetadata > budget - charge) {
                            oversized = true;
                            break;
                        }
                        charge += entryMetadata;
                        eligible.insert(&values[i]);
                    }
                    if (oversized)
                        break;
                }
            if (!oversized && !eligible.empty()) {
                ctx.checkpoint("before target rendering eligibility commit");
                targetRenderingPrefix = baseline->text.substr(0, begin);
                targetRenderingSuffix = baseline->text.substr(end);
                targetRenderingEligible = std::move(eligible);
                targetRenderingBaseCharge = charge;
                targetRenderingBudget = budget;
            }
        }
        if (initial.fullFlow)
            prepareFormatted(ctx);
    }
    phases.mark("body-diagnostic-affected");
    ctx.checkpoint("after local diagnostic baseline");
}

BodyDiagnosticCache::Slice BodyDiagnosticCache::analyze(
    const std::shared_ptr<syntax::SyntaxTree>& tree, const lsp::RequestContext& ctx) {
    Slice result;
    result.compilation = std::make_shared<ast::Compilation>(Bag{owner->getOptions()});
    auto& compilation = *result.compilation;
    compilation.setBodyQueryContext(owner, tree);
    ast::BodyQueryReferenceGuard referenceGuard(compilation);
    const syntax::FunctionDeclarationSyntax* syntax = nullptr;
    auto find = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
        if (auto fn = node.as_if<syntax::FunctionDeclarationSyntax>()) {
            require(!syntax, "multiple detached diagnostic methods");
            syntax = fn;
            return;
        }
        for (uint32_t i = 0; i < node.getChildCount(); ++i)
            if (auto child = node.childNode(i))
                self(self, *child);
    };
    find(find, tree->root());
    require(syntax, "missing detached diagnostic method");
    ast::BodyQueryEvaluationGuard guard(originals);
    std::vector<const ast::SubroutineSymbol*> roots;
    for (auto original : originals) {
        require(!original->bodyHasExternalDependents.load(std::memory_order_relaxed),
                "method acquired external elaboration dependencies");
        auto& fn = ast::SubroutineSymbol::createBodyQuery(compilation, *syntax, *original);
        compilation.forceElaborate(fn);
        LocalBoundaries boundaries;
        fn.visit(boundaries);
        auto callEffects = ast::makeVisitor([&](auto& visitor, const ast::CallExpression& call) {
            // A safe outer call must not hide a package/static/task call in its
            // receiver or actual arguments, including system-call arguments.
            visitor.visitDefault(call);
            // Native inlines drivers / reads only from functions. Tasks have
            // their own completed timing-summary replay below; an unchanged
            // task call must not force full-design dataflow for every edit.
            // Reference bit changes still request the private usage replay.
            if (call.isSystemCall() || call.getSubroutineKind() != ast::SubroutineKind::Function)
                return;
            const auto target = std::get<const ast::SubroutineSymbol*>(call.subroutine);
            const auto parent = target->getParentScope();
            if (parent && parent->asSymbol().kind == ast::SymbolKind::ClassType &&
                !target->flags.has(ast::MethodFlags::Static | ast::MethodFlags::Constructor) &&
                target->subroutineKind == ast::SubroutineKind::Function)
                return;
            if (const auto node = target->getSyntax())
                result.effectSensitiveCalls.insert(node);
            if (const auto prototype = target->getPrototype())
                if (const auto node = prototype->getSyntax())
                    result.effectSensitiveCalls.insert(node);
        });
        fn.visit(callEffects);
        roots.push_back(&fn);
        ctx.checkpoint("after local diagnostic binding");
    }
    // Method-local nominal types belong to this disposable body. forceElaborate
    // realizes unused aliases, enum values and explicit/wildcard imports. New
    // package references resolve in the retained complete Native namespace;
    // their reference effects still participate in the private usage replay.
    std::vector<std::string> specialSyntax;
    auto inspect = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
        require(node.kind != syntax::SyntaxKind::ClassDeclaration &&
                    node.kind != syntax::SyntaxKind::CheckerDeclaration &&
                    node.kind != syntax::SyntaxKind::CheckerInstanceStatement &&
                    node.kind != syntax::SyntaxKind::CheckerInstantiation &&
                    node.kind != syntax::SyntaxKind::ConcurrentAssertionMember &&
                    node.kind != syntax::SyntaxKind::SequenceDeclaration &&
                    node.kind != syntax::SyntaxKind::PropertyDeclaration,
                "local classes or assertions require full diagnostics");
        if (node.kind == syntax::SyntaxKind::ParameterValueAssignment) {
            // A virtual interface instance is private to this body and receives
            // its own deferred validity checks. Its parameter changes do not
            // create a globally shared generic class specialization.
            if (!node.parent || node.parent->kind != syntax::SyntaxKind::VirtualInterfaceType)
                specialSyntax.push_back(node.toString());
            // Local nominal types and values are bound in this disposable
            // compilation. New class specializations stay in its private
            // cache; completed Native classes retain canonical identity. Their
            // semantic and reference closure is proved below, irrespective of
            // whether an argument's spelling names a local declaration.
        }
        for (uint32_t i = 0; i < node.getChildCount(); ++i)
            if (auto child = node.childNode(i))
                self(self, *child);
    };
    inspect(inspect, *syntax);
    result.calls = std::move(specialSyntax);
    std::sort(result.calls.begin(), result.calls.end());
    auto local = [&](const Diagnostic& diagnostic) {
        auto loc = driver.sm.getFullyExpandedLoc(diagnostic.location);
        if (loc && loc.buffer() == tree->getSourceBufferIds().front() && begin <= loc.offset())
            return true;
        // Virtual instance bodies have lexical parents at the definition, but
        // their hierarchical parent is the declaration in this edited method.
        // Accept only instances owned by this disposable compilation; never
        // reinterpret a borrowed DUT instance as a local diagnostic context.
        for (auto symbol = diagnostic.symbol; symbol;) {
            if (std::ranges::find(roots, symbol) != roots.end())
                return true;
            if (auto body = symbol->as_if<ast::InstanceBodySymbol>()) {
                if (&body->getCompilation() != &compilation)
                    return false;
                if (body->parentInstance) {
                    symbol = body->parentInstance;
                    continue;
                }
            }
            auto parent = symbol->getParentScope();
            symbol = parent ? &parent->asSymbol() : nullptr;
        }
        return false;
    };
    for (auto& diagnostic : tree->diagnostics())
        if (diagnostic.code != diag::NewlineEOF)
            result.diagnostics[0].push_back(diagnostic);
    const auto boundDiagnostics = compilation.getBodyQueryDiagnostics(
        [&ctx] { ctx.checkpoint("during local virtual interface finalization"); });
    std::vector<const ast::ClassType*> omittedClasses, additionalClasses;
    std::shared_ptr<NativeReferenceUsage> referenceView;
    const auto lookupRoots = compilation.getClassSpecializationUses();
    require(lookupRoots.has_value(), "class lookup provenance unavailable");
    result.hasClassLookups = !lookupRoots->empty();
    if (baselineClassLookups || result.hasClassLookups) {
        auto unused = owner->getUnusedClassSpecializationsExcludingBodies(
            originals, [&ctx] { ctx.checkpoint("local class dependency closure"); }, *lookupRoots);
        require(unused.has_value(), "Native class dependency provenance unavailable");
        omittedClasses = std::move(*unused);
        for (auto type : compilation.getImportedClassSpecializations()) {
            const auto* cls = type->as_if<ast::ClassType>();
            require(cls && cls->genericClass, "unsupported detached specialization");
            if (&cls->getCompilation() == &compilation)
                additionalClasses.push_back(cls);
        }
        result.classClosureProven = true;
        result.fullSemantic = !omittedClasses.empty() || !additionalClasses.empty();
        if (result.fullSemantic) {
            auto view = compilation.getBodyQuerySemanticView(
                *roots.front(), originals, omittedClasses,
                [&ctx] { ctx.checkpoint("local class semantic reference view"); });
            require(view.references.has_value(), "class reference view needs imported provenance");
            result.diagnostics[1] = std::move(view.diagnostics);
            referenceView = std::make_shared<NativeReferenceUsage>(std::move(*view.references));
            result.fullFlow = true;
        }
    }
    if (!result.fullSemantic)
        for (const auto& diagnostic : boundDiagnostics) {
            if (!local(diagnostic)) {
                DiagnosticEngine diagnosticEngine(driver.sm);
                throw std::runtime_error(
                    "detached body generates a nonlocal semantic diagnostic: " +
                    diagnosticEngine.formatMessage(diagnostic));
            }
            result.diagnostics[1].push_back(diagnostic);
        }
    for (auto type : compilation.getImportedClassSpecializations()) {
        auto cls = type->as_if<ast::ClassType>();
        require(cls && cls->genericClass, "unsupported detached specialization");
        result.specializations.push_back(cls);
    }
    require(!compilation.hasFatalErrors(), "fatal body diagnostics require full analysis");
    compilation.freeze();
    auto unfreeze = ScopeGuard([&] { compilation.unfreeze(); });
    bool stableClassEffects = true;
    analysis::AnalysisManager manager(analysisOptions);
    manager.setCheckpoint([&ctx] { ctx.checkpoint("during local diagnostic procedure"); });
    manager.addListener([&](const analysis::AnalyzedProcedure&) {
        ctx.checkpoint("after local diagnostic procedure");
    });
    if (!additionalClasses.empty())
        manager.setClassSpecializationView({}, additionalClasses);
    manager.analyzeBodies(compilation, roots);
    std::vector<analysis::AnalysisManager::TaskTimingReplacement> taskReplacements;
    for (size_t rootIndex = 0; rootIndex < roots.size(); ++rootIndex) {
        const auto* fn = roots[rootIndex];
        auto procedure = manager.getAnalyzedSubroutine(*fn);
        require(procedure, "missing local diagnostic procedure");
        const bool classFunction =
            fn->getParentScope()->asSymbol().kind == ast::SymbolKind::ClassType &&
            fn->subroutineKind == ast::SubroutineKind::Function &&
            !fn->flags.has(ast::MethodFlags::Static | ast::MethodFlags::Constructor) &&
            std::ranges::all_of(fn->getArguments(), [](const ast::FormalArgumentSymbol* arg) {
                return arg->direction == ast::ArgumentDirection::In;
            });
        const auto* originalProcedure = completeAnalysis ? completeAnalysis->getAnalyzedSubroutine(
                                                               *originals[rootIndex])
                                                         : nullptr;
        const bool sameEffects = classFunction && stableExternalDriverLocations &&
                                 originalProcedure &&
                                 sameDirectExternalDrivers(*originals[rootIndex],
                                                           *originalProcedure, *fn, *procedure,
                                                           ctx);
        stableClassEffects &= sameEffects;
        if (sameEffects)
            ctx.checkpoint("class external driver effects unchanged");
        // Native propagates package/module function drivers into each caller,
        // including static locals declared inside the function. An isolated
        // body cannot update caller conflicts from those persistent drivers.
        // Nonstatic class methods have no such Native propagation.
        if (fn->getParentScope()->asSymbol().kind != ast::SymbolKind::ClassType &&
            !procedure->getDrivers().empty())
            result.fullFlow = true;
        if (!sameEffects)
            for (auto driver : procedure->getDrivers())
                if (!contained(&driver->getSymbol(), fn))
                    result.fullFlow = true;
        if (fn->subroutineKind == ast::SubroutineKind::Task) {
            require(analysis, "task caller analysis is unavailable");
            taskReplacements.push_back({originals[rootIndex], procedure});
        }
    }
    if (!taskReplacements.empty()) {
        auto diagnostics = analysis->getTaskTimingDiagnostics(timingCallers, taskReplacements, [&] {
            ctx.checkpoint("task diagnostic caller update");
        });
        require(diagnostics.has_value(), "task timing summaries require full analysis");
        result.diagnostics[2].append_range(*diagnostics);
    }
    for (auto& diagnostic : manager.getDiagnostics())
        if (local(diagnostic))
            result.diagnostics[2].push_back(diagnostic);
    ctx.checkpoint("after local diagnostic dataflow");
    auto internalSyntax = [&](const syntax::SyntaxNode* node) {
        for (; node; node = node->parent) {
            if (node == syntax)
                return true;
            for (auto fn : originals)
                if (node == fn->getSyntax())
                    return true;
        }
        return false;
    };
    for (auto& [node, usage] : compilation.getBodyQueryReferences()) {
        const syntax::SyntaxNode* canonical = node;
        for (size_t i = 0; i < roots.size(); ++i)
            if (node == roots[i]->getSyntax())
                canonical = originals[i]->getSyntax();
        const bool declaration = std::ranges::any_of(originals, [&](auto fn) {
            return fn->getSyntax() == canonical;
        });
        if (declaration || !internalSyntax(node)) {
            auto& combined = result.references[canonical];
            combined.first |= usage.first;
            combined.second |= usage.second;
        }
    }
    if (baselineBinding && nativeReferences) {
        auto differs = [&](const syntax::SyntaxNode* node) {
            auto complete = [&](const References& body) {
                std::pair<bool, bool> usage{};
                if (auto outside = nativeReferences->find(node); outside != nativeReferences->end())
                    usage = outside->second;
                if (auto current = body.find(node); current != body.end()) {
                    usage.first |= current->second.first;
                    usage.second |= current->second.second;
                }
                return usage;
            };
            return complete(references) != complete(result.references);
        };
        result.fullFlow |=
            std::ranges::any_of(references,
                                [&](const auto& entry) { return differs(entry.first); }) ||
            std::ranges::any_of(result.references,
                                [&](const auto& entry) { return differs(entry.first); });
    }
    result.fullFlow |= replayEffects ||
                       (!stableClassEffects && !result.effectSensitiveCalls.empty()) ||
                       (baselineBinding && effectSensitiveCalls != result.effectSensitiveCalls);
    if (result.fullFlow) {
        std::vector<analysis::AnalysisManager::SubroutineReplacement> replacements;
        for (size_t i = 0; i < roots.size(); ++i)
            replacements.emplace_back(originals[i], roots[i]);
        result.replay = std::make_shared<analysis::AnalysisManager>(analysisOptions);
        result.replay->setSubroutineReplacements(replacements);
        if (result.fullSemantic)
            result.replay->setClassSpecializationView(omittedClasses, additionalClasses);
        result.replay->setCheckpoint(
            [ctx] { ctx.checkpoint("during local effect dependency index"); });
        result.replay->setProcedureReuse(
            std::shared_ptr<const analysis::AnalysisManager>(owner, procedureReuseAnalysis));
        result.replay->setCheckpoint([ctx] { ctx.checkpoint("during local effect procedure"); });
        if (referenceView) {
            result.replay->setReferenceUsageProvider(
                [profile = referenceView](
                    const syntax::SyntaxNode& node) -> std::optional<std::pair<bool, bool>> {
                    if (auto found = profile->find(&node); found != profile->end())
                        return found->second;
                    return std::pair<bool, bool>{};
                });
        }
        else if (nativeReferences) {
            result.replay->setReferenceUsageProvider(
                [outside = nativeReferences, body = result.references](
                    const syntax::SyntaxNode& node) -> std::optional<std::pair<bool, bool>> {
                    std::pair<bool, bool> usage{};
                    if (auto found = outside->find(&node); found != outside->end())
                        usage = found->second;
                    if (auto found = body.find(&node); found != body.end()) {
                        usage.first |= found->second.first;
                        usage.second |= found->second.second;
                    }
                    return usage;
                });
        }
        result.replay->addListener([&](const analysis::AnalyzedProcedure&) {
            ctx.checkpoint("local effect replay procedure");
        });
        result.replay->addListener(
            [&](const analysis::AnalyzedScope&) { ctx.checkpoint("local effect replay scope"); });
        ctx.checkpoint("before local effect replay");
        result.replay->analyze(*owner);
        result.diagnostics[2] = result.replay->getDiagnostics();
        ctx.checkpoint("after local effect replay");
    }
    return result;
}

void BodyDiagnosticCache::validateSlice(const Slice& slice) const {
    if (slice.references != references && !(slice.fullFlow && nativeReferences)) {
        // A fixed provider's exported use exists independently of this editable
        // caller. Changing a bit already set by that provider cannot change the
        // full-design read/write status. Do not use isReferenced here: later
        // navigation can add uses to a provider's separate mutable overlay.
        auto equivalent = [&](const syntax::SyntaxNode* node, std::pair<bool, bool> before,
                              std::pair<bool, bool> after) {
            if (before == after)
                return true;
            require(slice.fullFlow || (!effectSensitiveCalls.contains(node) &&
                                       !slice.effectSensitiveCalls.contains(node)),
                    "changed package/static function calls require full effect analysis");
            // Completed unrelated native methods provide positive read evidence.
            // This is independent of the replaced syntax and later navigation;
            // it proves only the aggregate read bit, never a write or call effect.
            std::pair<bool, bool> fixed{independentReceiverReads.contains(node) ||
                                            independentCalleeReads.contains(node),
                                        false};
            if (nativeReferences)
                if (auto native = nativeReferences->find(node); native != nativeReferences->end()) {
                    fixed.first |= native->second.first;
                    fixed.second |= native->second.second;
                }
            for (const auto& library : {driver.m_trustedUvm, driver.m_trustedScopes})
                if (library && library->compilation) {
                    auto usage = library->compilation->getExportedReferenceUsage(*node);
                    fixed.first |= usage.first;
                    fixed.second |= usage.second;
                }
            return (before.first == after.first || fixed.first) &&
                   (before.second == after.second || fixed.second);
        };
        for (const auto& [node, before] : references) {
            auto found = slice.references.find(node);
            require(equivalent(node, before,
                               found == slice.references.end() ? std::pair<bool, bool>{}
                                                               : found->second),
                    "method changes external reference usage");
        }
        for (const auto& [node, after] : slice.references)
            if (!references.contains(node))
                require(equivalent(node, {}, after), "method changes external reference usage");
    }
    require(slice.classClosureProven || slice.calls == calls,
            "method changes specialization or declaration dependencies");
    require(slice.classClosureProven ||
                (slice.specializations.size() == specializations.size() &&
                 std::ranges::all_of(
                     slice.specializations,
                     [&](auto current) {
                         return std::ranges::any_of(specializations, [&](auto previous) {
                             return current->genericClass == previous->genericClass &&
                                    ast::ParameterSymbolBase::allMatching(
                                        current->genericParameters, previous->genericParameters) &&
                                    std::ranges::equal(
                                        current->genericParameters, previous->genericParameters,
                                        [](auto a, auto b) {
                                            if (auto value =
                                                    a->template as_if<ast::ParameterSymbol>()) {
                                                auto& other =
                                                    b->template as<ast::ParameterSymbol>();
                                                const auto& left = value->getValue();
                                                const auto& right = other.getValue();
                                                if (left.isInteger() && right.isInteger() &&
                                                    (left.integer().getBitWidth() !=
                                                         right.integer().getBitWidth() ||
                                                     left.integer().isSigned() !=
                                                         right.integer().isSigned()))
                                                    return false;
                                                return value->getType().isMatching(other.getType());
                                            }
                                            return true;
                                        });
                         });
                     })),
            "method changes evaluated specialization parameters");
    const auto errorLimit = owner->getOptions().errorLimit;
    require(!errorLimit || owner->getIssuedErrorCount() + slice.compilation->getIssuedErrorCount() <
                               errorLimit,
            "method may exceed full-design semantic error limit");
    for (auto& phase : slice.diagnostics)
        for (auto& diagnostic : phase)
            require(driver.diagEngine.getSeverity(diagnostic.code, diagnostic.location) !=
                        DiagnosticSeverity::Fatal,
                    "fatal diagnostics require full analysis");
}

std::vector<const syntax::FunctionDeclarationSyntax*> BodyDiagnosticCache::validateGeneratedMacros(
    std::string_view current, size_t newEnd, const lsp::RequestContext& ctx) {
    require(completeAnalysis, "generated macro native analysis unavailable");
    if (!macroLineProof) {
        require(bool(lineText), "generated macro baseline text unavailable");
        auto candidate = std::make_shared<MacroLineProof>(
            driver.sm, *owner, *completeAnalysis, originalTarget, end, lineText,
            driver.sm.getFullPath(driver.getDocument(uri)->getBuffer()).string(), ctx);
        candidate->validate(current, newEnd, ctx);
        macroLineProof = std::move(candidate);
    }
    else
        macroLineProof->validate(current, newEnd, ctx);
    return macroLineProof->functionContexts();
}

void BodyDiagnosticCache::prepareFormatted(const lsp::RequestContext& ctx) {
    if (formattedPrepared || !formattedPublication)
        return;
    ctx.checkpoint("before immutable diagnostic rendering index");
    const auto& raw = formattedPublication->diagnostics;
    const auto path = driver.sm.getFullPath(driver.getDocument(uri)->getBuffer()).generic_string();
    auto targetUri = [&](const URI& value) { return value.getPath() == path; };
    auto touchesTarget = [&](auto&& self, const Diagnostic& value) -> bool {
        auto location = [&](SourceLocation loc) {
            return loc &&
                   (nativeTargetBuffers.contains(driver.sm.getFullyExpandedLoc(loc).buffer()) ||
                    nativeTargetBuffers.contains(driver.sm.getFullyOriginalLoc(loc).buffer()));
        };
        if (location(value.location))
            return true;
        for (auto range : value.ranges)
            if (location(range.start()) || location(range.end()))
                return true;
        for (const auto& note : value.notes)
            if (self(self, note))
                return true;
        return false;
    };
    // Build privately: cancellation may occur while an existing local cache is
    // being promoted to a full effect replay. No partial index can escape.
    std::vector<CachedFormatted> nextEntries = formattedEntries;
    std::unordered_multimap<size_t, size_t> nextByHash;
    std::unordered_map<const Diagnostic*, size_t> nextRetained = formattedRetained;
    size_t total = 0;
    for (const auto& values : raw)
        total += values.size();
    nextEntries.reserve(total);
    nextByHash.reserve(total);
    nextRetained.reserve(total);
    for (size_t phase = 0; phase < raw.size(); ++phase) {
        const auto& rendered = formattedPublication->rendered[phase];
        if (rendered.size() != raw[phase].size())
            continue;
        std::vector<size_t> entries(raw[phase].size(), size_t(-1));
        for (size_t i = 0; i < raw[phase].size(); ++i) {
            ctx.checkpoint("prepare immutable diagnostic rendering");
            if (!rendered[i].key || rendered[i].values.size() != 1 ||
                touchesTarget(touchesTarget, raw[phase][i]))
                continue;
            const auto& value = rendered[i].values.front();
            bool sensitive = targetUri(value.uri);
            if (value.value.relatedInformation)
                for (const auto& note : *value.value.relatedInformation)
                    sensitive |= targetUri(note.location.uri);
            if (sensitive)
                continue;
            entries[i] = nextEntries.size();
            nextByHash.emplace(rawDiagnosticHash(raw[phase][i]), entries[i]);
            nextEntries.push_back({&raw[phase][i], &rendered[i]});
        }
        for (const auto& [key, indices] : retainedNativeIndices[phase]) {
            const auto& diagnostics = retained[phase].at(key);
            for (size_t i = 0; i < indices.size(); ++i)
                if (entries[indices[i]] != size_t(-1))
                    nextRetained.emplace(&diagnostics[i], entries[indices[i]]);
        }
    }
    ctx.checkpoint("commit immutable diagnostic rendering index");
    formattedEntries = std::move(nextEntries);
    formattedByHash = std::move(nextByHash);
    formattedRetained = std::move(nextRetained);
    formattedPrepared = true;
    retainedNativeIndices = {};
    nativeTargetBuffers = {};
}

const BodyDiagnosticCache::CachedFormatted* BodyDiagnosticCache::findFormatted(
    const Diagnostic& value) const {
    if (auto found = formattedRetained.find(&value); found != formattedRetained.end())
        return &formattedEntries[found->second];
    auto [begin, end] = formattedByHash.equal_range(rawDiagnosticHash(value));
    for (auto it = begin; it != end; ++it) {
        const auto& entry = formattedEntries[it->second];
        if (scalarDiagnosticEqual(value, *entry.raw))
            return &entry;
    }
    return nullptr;
}

void BodyDiagnosticCache::publish(const std::shared_ptr<syntax::SyntaxTree>& tree, size_t newEnd,
                                  const lsp::RequestContext& ctx,
                                  const std::function<void()>& validate, bool deferTransport) {
    UvmPhaseTrace phases;
    auto document = driver.getDocument(uri);
    lsp::Position coordinate{};
    if (targetRenderingBudget) {
        require(newEnd < document->getText().size(), "retained diagnostic current source bounds");
        coordinate = toPosition(SourceLocation(document->getBuffer(), newEnd), driver.sm);
    }
    const bool sameCoordinate = targetRenderingMemo &&
        targetRenderingMemo->coordinate.line == coordinate.line &&
        targetRenderingMemo->coordinate.character == coordinate.character;
    if (targetRenderingBudget) {
        const auto current = document->getText();
        require(newEnd <= current.size() && current.substr(0, begin) == targetRenderingPrefix &&
                    current.substr(newEnd) == targetRenderingSuffix,
                "retained rendering surrounding text changed");
    }
    const auto rawEndLine = lineSensitive
        ? driver.sm.getRawLineNumber(SourceLocation(tree->getSourceBufferIds().front(), newEnd)) : 0;
    if (lineSensitive && baselineLine != rawEndLine) {
        if (macroLineProof) {
            if (validatedGeneratedEnd != newEnd || validatedGeneratedTree.lock() != tree)
                macroLineProof->validate(driver.getDocument(uri)->getText(), newEnd, ctx);
            else {
                ctx.checkpoint("reuse current macro syntax proof");
                macroLineProof->validateDependencies();
            }
        }
        else {
            if (!lineProof) {
                lineProof = std::make_shared<BodyLineProof>(
                    driver.sm, *owner, *originals.front(), originalTarget, end, *lineText,
                    driver.sm.getFullPath(driver.getDocument(uri)->getBuffer()).string(), ctx,
                    driver.m_config.experimentalMethodBodyQueries.value() &&
                            driver.m_config.experimentalMethodBodyDiagnostics.value() &&
                            driver.m_config.experimentalMethodBodyCertification.value()
                        ? std::min<size_t>(driver.m_config.methodBodyLineSyntaxCacheKiB.value(),
                                           65536) * 1024
                        : 0);
                lineText.reset();
            }
            lineProof->validate(driver.getDocument(uri)->getText(), newEnd, ctx, rawEndLine);
        }
    }
    phases.mark("local-publish-line-proof");
    auto slice = analyze(tree, ctx);
    validateSlice(slice);
    phases.mark("local-publish-analyze");
    BodySourceProjection projection(driver.sm, document->getBuffer(),
                                    tree->getSourceBufferIds().front(), end, newEnd, bufferLimit);
    auto cwd = driver.sm.beginFastCwdPhase();
    if (slice.fullFlow)
        prepareFormatted(ctx);
    auto output = std::make_shared<ServerDiagClient>(driver.sm, driver.client);
    output->disableDestructorPublication();
    bindUris(*output, driver);
    DiagnosticEngine engine(driver.sm);
    engine.applySettings(settings);
    engine.setMappingsFromPragmas();
    engine.addClient(output);
    // Discover any newly emitted macro diagnostic URI before selecting its
    // retained siblings. Only these publication buckets need to be reformatted.
    std::unordered_set<URI> cachedRoutes;
    for (const auto& phase : slice.diagnostics)
        for (const auto& diagnostic : phase) {
            ctx.checkpoint("discover local diagnostic routes");
            if (auto cached = findFormatted(diagnostic)) {
                for (const auto& value : cached->rendered->values)
                    cachedRoutes.insert(output->resolvePublicationUri(value.uri));
            }
            else
                engine.issue(diagnostic);
        }
    auto changed = affectedUris;
    changed.insert(cachedRoutes.begin(), cachedRoutes.end());
    if (slice.fullFlow)
        for (const auto& [key, _] : retained[2])
            changed.insert(key);
    if (slice.fullSemantic)
        for (const auto& [key, _] : retained[1])
            changed.insert(key);
    for (const auto& [key, _] : output->renderedDiagnostics())
        changed.insert(key);
    output->clear();
    if (!formattedPrepared &&
        std::ranges::any_of(changed, [&](const URI& key) { return key != uri; }))
        prepareFormatted(ctx);
    phases.mark("local-publish-discover-routes");
    const bool reuseTarget = targetRenderingBudget && sameCoordinate &&
                             !targetRenderingMemo->oversized &&
                             !slice.fullFlow && !slice.fullSemantic;
    std::unique_ptr<TargetRenderingMemo> targetDraft;
    if (targetRenderingBudget && !sameCoordinate && !slice.fullFlow && !slice.fullSemantic) {
        targetDraft = std::make_unique<TargetRenderingMemo>();
        targetDraft->coordinate = coordinate;
    }
    // Native renderings stay owned by formattedPublication throughout this call.
    // Fresh diagnostics and alias-adjusted copies have stable addresses in the
    // deque. Only a changed committed bucket needs to copy these payloads.
    std::deque<lsp::Diagnostic> ownedRenderings;
    std::unordered_map<URI, std::vector<const lsp::Diagnostic*>> renderedViews;
    auto appendView = [&](const URI& physical, const lsp::Diagnostic& value) {
        const auto* resolved = &value;
        if (value.relatedInformation &&
            std::ranges::any_of(*value.relatedInformation, [&](const auto& note) {
                return output->resolvePublicationUri(note.location.uri) != note.location.uri;
            })) {
            auto& copy = ownedRenderings.emplace_back(value);
            for (auto& note : *copy.relatedInformation)
                note.location.uri = output->resolvePublicationUri(note.location.uri);
            resolved = &copy;
        }
        renderedViews[output->resolvePublicationUri(physical)].push_back(resolved);
    };
    const Diagnostic* collecting = nullptr;
    auto previousPhysical = output->exchangePhysicalReportObserver(
        [&](const URI& physical, lsp::Diagnostic value) {
            if (collecting && targetDraft && !targetDraft->oversized) {
                const auto available = targetRenderingBudget - targetRenderingBaseCharge -
                                       targetDraft->charge;
                size_t lower = value.message.size();
                if (value.relatedInformation)
                    for (const auto& note : *value.relatedInformation) {
                        ctx.checkpoint("charge target rendering note");
                        lower += note.message.size() + note.location.uri.str().size();
                        if (lower > available / 2)
                            break;
                    }
                // Slang's client emits no arbitrary data. Do not serialize a
                // future extension of unknown size on this optional path.
                const auto charge = value.data || lower > available / 2
                    ? available + 1
                    : 2 * rfl::json::write(value).size() +
                          sizeof(NativeDiagnosticSnapshot::PhysicalDiagnostic) + 128;
                if (charge > available) {
                    targetDraft->entries.clear();
                    targetDraft->charge = 0;
                    targetDraft->oversized = true;
                }
                else {
                    targetDraft->charge += charge;
                    targetDraft->entries[collecting].push_back({physical, value});
                }
            }
            appendView(physical, ownedRenderings.emplace_back(std::move(value)));
        });
    auto restorePhysical = ScopeGuard(
        [&] { output->exchangePhysicalReportObserver(std::move(previousPhysical)); });
    uint64_t reused = 0, fresh = 0;
    for (size_t phase = 0; phase < retained.size(); ++phase) {
        std::vector<const Diagnostic*> diagnostics;
        for (const auto& diagnostic : slice.diagnostics[phase])
            diagnostics.push_back(&diagnostic);
        for (const auto& key : changed)
            if ((phase != 2 || !slice.fullFlow) && (phase != 1 || !slice.fullSemantic))
                if (auto found = retained[phase].find(key); found != retained[phase].end())
                    for (const auto& diagnostic : found->second)
                        diagnostics.push_back(&diagnostic);
        std::vector<const CachedFormatted*> formatted;
        formatted.reserve(diagnostics.size());
        for (auto diagnostic : diagnostics)
            formatted.push_back(findFormatted(*diagnostic));
        std::vector<const std::vector<NativeDiagnosticSnapshot::PhysicalDiagnostic>*> memoized;
        memoized.reserve(diagnostics.size());
        for (auto diagnostic : diagnostics) {
            const std::vector<NativeDiagnosticSnapshot::PhysicalDiagnostic>* entry = nullptr;
            if (reuseTarget)
                if (auto found = targetRenderingMemo->entries.find(diagnostic);
                    found != targetRenderingMemo->entries.end())
                    entry = &found->second;
            memoized.push_back(entry);
        }
        std::vector<size_t> order(diagnostics.size());
        std::iota(order.begin(), order.end(), size_t(0));
        // Resolve paths and projected positions once per diagnostic, rather
        // than allocating strings and revisiting line maps in every comparison.
        // A singleton needs no ordering or source lookup, as in the native sort.
        if (diagnostics.size() > 1) {
            using Key = std::tuple<std::string, unsigned, unsigned, DiagCode>;
            std::vector<Key> freshKeys;
            freshKeys.reserve(diagnostics.size());
            std::vector<const Key*> keys;
            keys.reserve(diagnostics.size());
            for (size_t i = 0; i < diagnostics.size(); ++i) {
                if ((i & 255u) == 0)
                    ctx.checkpoint("prepare local diagnostic ordering");
                if (formatted[i])
                    keys.push_back(&*formatted[i]->rendered->key);
                else {
                    const auto& diagnostic = *diagnostics[i];
                    auto loc = driver.sm.getFullyExpandedLoc(diagnostic.location);
                    auto pos = toPosition(loc, driver.sm);
                    freshKeys.emplace_back(driver.sm.getFullPath(loc.buffer()).string(), pos.line,
                                           pos.character, diagnostic.code);
                    keys.push_back(&freshKeys.back());
                }
            }
            size_t comparisons = 0;
            std::ranges::stable_sort(order, [&](size_t a, size_t b) {
                if ((++comparisons & 1023u) == 0)
                    ctx.checkpoint("during local diagnostic ordering");
                return *keys[a] < *keys[b];
            });
        }
        phases.mark("local-publish-order");
        for (auto index : order) {
            ctx.checkpoint("retained local diagnostic publication");
            if (formatted[index]) {
                ++reused;
                for (const auto& value : formatted[index]->rendered->values)
                    appendView(value.uri, value.value);
            }
            else if (memoized[index]) {
                ++reused;
                for (const auto& value : *memoized[index])
                    appendView(value.uri, value.value);
            }
            else {
                ++fresh;
                collecting = targetDraft && !targetDraft->oversized &&
                                     targetRenderingEligible.contains(diagnostics[index])
                                 ? diagnostics[index] : nullptr;
                if (collecting)
                    targetDraft->entries.try_emplace(collecting);
                engine.issue(*diagnostics[index]);
                collecting = nullptr;
            }
        }
        phases.mark("local-publish-render");
    }
    for (const auto& [key, _] : renderedViews)
        changed.insert(key);
    if (targetDraft)
        ctx.checkpoint("before target rendering memo publication");
    validate();
    phases.mark("local-publish-input-proof");
    ctx.checkpoint("before local diagnostic publication");
    cwd.finish();
    auto update = driver.diagClient->beginUpdate();
    for (const auto& key : changed) {
        auto found = renderedViews.find(key);
        driver.diagClient->replaceRenderedViewsIfChanged(
            key, found == renderedViews.end() ? std::span<const lsp::Diagnostic* const>{}
                                              : found->second);
    }
    for (const auto& [key, doc] : driver.docs)
        driver.diagClient->setVersion(key, doc->getVersion());
    update.commit();
    phases.mark("local-publish-replace");
    driver.publishCertifiedLocalDiagnostics(uri, deferTransport);
    phases.mark("local-publish-send");
    if (targetDraft)
        targetRenderingMemo = std::move(targetDraft);
    affectedUris = std::move(changed);
    driver.m_fullDiagnosticGeneration = driver.m_diagnosticGeneration;
    driver.m_pendingIncludedDiagnostics.clear();
    if (slice.replay) {
        auto counts = slice.replay->getProcedureReuseCounts();
        reusedProcedures = counts.first;
        recomputedProcedures = counts.second;
    }
    else {
        reusedProcedures = recomputedProcedures = 0;
    }
    reusedRenderings = reused;
    freshRenderings = fresh;
    ++updates;
}
} // namespace server
