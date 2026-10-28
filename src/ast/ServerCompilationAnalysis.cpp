#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
//------------------------------------------------------------------------------
// ServerCompilationAnalysis.cpp
// Implementation of server compilation analysis class
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "ast/ServerCompilationAnalysis.h"

#include "ast/TrustedUvmLibrary.h"
#include "util/Logging.h"
#include "util/ModelOwnerTrace.h"
#include "util/UvmPhaseTrace.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include "slang/analysis/AnalysisManager.h"
#include "slang/analysis/ValueDriver.h"
#include "slang/ast/Compilation.h"
#include "slang/text/SourceManager.h"
#include "slang/util/ScopeGuard.h"

namespace server {

namespace {
// Mechanism-only sampling. One row per root; no per-checkpoint file I/O and no
// clock reads unless this separate probe is explicitly enabled.
class RootBindingTrace {
    using Clock = std::chrono::steady_clock;
    std::string path;
    Clock::time_point last;
    Clock::duration maximum{};
    size_t checks = 0;
    bool finished = false;

public:
    explicit RootBindingTrace(bool active) {
        if (active)
            if (auto value = std::getenv("SLANG_ROOT_QUANTUM_TRACE")) {
                path = value;
                last = Clock::now();
            }
    }
    void checkpoint() {
        if (path.empty())
            return;
        auto now = Clock::now();
        maximum = std::max(maximum, now - last);
        last = now;
        ++checks;
    }
    void finish(bool completed) noexcept {
        if (path.empty() || finished)
            return;
        finished = true;
        maximum = std::max(maximum, Clock::now() - last);
        if (auto file = std::fopen(path.c_str(), "a")) {
            auto micros = std::chrono::duration_cast<std::chrono::microseconds>(maximum).count();
            std::fprintf(file, "%ld\t%lld\t%zu\t%d\n", long(slang::OS::getpid()),
                         static_cast<long long>(micros), checks, int(completed));
            std::fclose(file);
        }
    }
    ~RootBindingTrace() { finish(false); }
};
} // namespace

ServerCompilationAnalysis::ServerCompilationAnalysis(
    const std::unordered_map<std::string, std::shared_ptr<SlangDoc>>& documents, Bag& options,
    SourceManager& sourceManager, const lsp::RequestContext& ctx, bool allowDisposableRoot,
    std::shared_ptr<const void> sourceOwner) :
    compilation(options),
    m_analysisOptions(options.getOrDefault<slang::analysis::AnalysisOptions>()),
    m_parallelOptions(options.getOrDefault<ParallelDiagnosticOptions>()) {
    if (compilation.hasFlag(ast::CompilationFlags::TrackSubroutineBodyDependencies) ||
        (m_parallelOptions.enabled && m_parallelOptions.workers > 1))
        m_analysisOptions.flags |= slang::analysis::AnalysisFlags::RetainProcedureSummaries;
    traceModelOwner("full-start", &compilation);
    bool completed = false;
    auto traceFailure = slang::ScopeGuard([&] {
        if (!completed && modelOwnerTraceEnabled())
            traceModelOwner("full-abort", &compilation, nullptr,
                            compilation.getTotalBytesAllocated());
    });
    UvmPhaseTrace phases;
    std::vector<BufferID> bufferIds;
    std::vector<std::shared_ptr<syntax::SyntaxTree>> trees;
    trees.reserve(documents.size());
    for (auto& [_, doc] : documents) {
        ctx.checkpoint("during full-model syntax discovery");
        trees.push_back(doc->getSyntaxTree(ctx));
    }
    phases.mark("model-syntax-discovery");
    if (modelOwnerTraceEnabled())
        traceModelOwner("full-syntax-discovered", &compilation, nullptr,
                        compilation.getTotalBytesAllocated());
    ctx.checkpoint("before full-model library import");
    const bool trusted = importTrustedUvm(compilation, sourceManager, options, trees);
    // Native models need the same namespace lifetime guarantee as imported views.
    // Keep only the SourceLifetime (sources + library identities), never the driver.
    if (sourceOwner && !compilation.getSourceOwnerLease())
        compilation.retainSourceOwner(std::move(sourceOwner));
    ctx.checkpoint("after full-model library import");
    phases.mark("model-library-import");
    if (modelOwnerTraceEnabled())
        traceModelOwner("full-library-imported", &compilation, nullptr,
                        compilation.getTotalBytesAllocated());
    std::unordered_set<BufferID> seenBuffers;

    for (auto& tree : trees) {
        ctx.checkpoint("before full-model syntax installation");
        compilation.addSyntaxTree(tree, trusted);
        ctx.checkpoint("after full-model syntax installation");

        for (auto bufferId : tree->getSourceBufferIds()) {
            if (seenBuffers.insert(bufferId).second)
                bufferIds.push_back(bufferId);
        }
    }

    phases.mark("model-syntax-installation");
    if (modelOwnerTraceEnabled())
        traceModelOwner("full-syntax-installed", &compilation, nullptr,
                        compilation.getTotalBytesAllocated());
    // Retain buffer data to prevent deallocation while this compilation exists
    m_retainedBuffers = sourceManager.retainBuffers(bufferIds);

    // reset and rebuild indexed info
    ctx.checkpoint("before full-model root binding");
    const bool disposableRoot = ctx && allowDisposableRoot;
    RootBindingTrace rootTrace(disposableRoot);
    auto checkpointRoot = [&] {
        rootTrace.checkpoint();
        ctx.checkpoint("during private root binding");
    };
    auto& root = disposableRoot ? compilation.getDisposableRoot(checkpointRoot)
                                : compilation.getRoot();
    rootTrace.finish(true);
    ctx.checkpoint("after full-model root binding");
    phases.mark("model-root-binding");
    if (modelOwnerTraceEnabled())
        traceModelOwner("full-root-bound", &compilation, nullptr,
                        compilation.getTotalBytesAllocated());
    instances.reset(&root, [&] { ctx.checkpoint("during full-model instance indexing"); });
    ctx.checkpoint("after full-model instance indexing");
    phases.mark("model-instance-indexing");

    // symbol references are not indexed until cone tracing is requested
    m_references.reset();
    completed = true;
    if (modelOwnerTraceEnabled())
        traceModelOwner("full-ready", &compilation, nullptr, compilation.getTotalBytesAllocated());
}

ServerCompilationAnalysis::~ServerCompilationAnalysis() {
    if (modelOwnerTraceEnabled())
        traceModelOwner("full-release", &compilation, nullptr, compilation.getTotalBytesAllocated(),
                        analysisMemoryBytes());
}

bool ServerCompilationAnalysis::releaseRecomputableCaches() {
    const bool released = m_driverAnalysis || m_intrinsicAnalysis || m_analysisQueries || m_references.has_value();
    // Queries and reference indexes contain pointers into the manager.
    m_references.reset();
    m_analysisQueries.reset();
    m_driverAnalysis.reset();
    m_intrinsicAnalysis.reset();
    return released;
}

void ServerCompilationAnalysis::prepareDisposableDiagnostics(const lsp::RequestContext& ctx) {
    UvmPhaseTrace phases;
    ctx.checkpoint("before private full-design semantics");
    (void)compilation.getDisposableSemanticDiagnostics(
        ctx ? std::function<void()>([&] { ctx.checkpoint("during private full-design semantics"); })
            : std::function<void()>{});
    ctx.checkpoint("after private full-design semantics");
    phases.mark("private-full-semantics");
    (void)getAnalysisManager(ctx);
    ctx.checkpoint("after private full-design diagnostics");
    phases.mark("private-full-dataflow");
}

size_t ServerCompilationAnalysis::analysisMemoryBytes() const {
    // Replay stats exclude borrowed summaries. Account for both retained owners.
    return (m_driverAnalysis ? m_driverAnalysis->getStats().memoryUsage : 0) +
           (m_intrinsicAnalysis ? m_intrinsicAnalysis->getStats().memoryUsage : 0);
}

slang::analysis::AnalysisManager& ServerCompilationAnalysis::getProcedureReuseBaseline(
    const lsp::RequestContext& ctx) {
    auto& serial = getAnalysisManager(ctx);
    return m_intrinsicAnalysis ? *m_intrinsicAnalysis : serial;
}

slang::analysis::AnalysisManager& ServerCompilationAnalysis::getAnalysisManager(
    const lsp::RequestContext& ctx) {
    if (!m_driverAnalysis) {
        // A query can refresh the design before the save-triggered diagnostic
        // pass. Complete elaboration before freezing it for driver analysis.
        ctx.throwIfCancelled("before full-design semantics");
        (void)compilation.getSemanticDiagnostics();
        ctx.throwIfCancelled("after full-design semantics");
        if (auto self = weak_from_this().lock())
            compilation.offerInstanceBodies(std::shared_ptr<ast::Compilation>(self, &compilation));
        ctx.throwIfCancelled("before full-design dataflow");
        // Analyze a private candidate. Cancellation must neither cache a partial
        // manager nor leave the shared compilation frozen for foreground queries.
        m_parallelStats = {};
        std::shared_ptr<slang::ThreadPool> pool;
#if defined(SLANG_USE_THREADS)
        m_parallelStats.observerSerial = ctx.hasCheckpointObserver();
        if (m_parallelOptions.enabled && m_parallelOptions.workers > 1 &&
            !compilation.hasFatalErrors() && !ctx.hasCheckpointObserver() && !BS::this_thread::get_index()) {
            const auto count = std::min(m_parallelOptions.workers, uint32_t(8));
            pool = std::make_shared<slang::ThreadPool>(count);
            m_parallelStats.workers = count;
            m_parallelStats.attempted = true;
        }
#endif
        const char* trace = std::getenv("SLANG_PARALLEL_DIAGNOSTIC_TRACE");
        auto event = [trace](const char* name, size_t workers) {
            if (trace)
                if (auto* file = std::fopen(trace, "a")) {
                    std::fprintf(file, "%s\t%zu\n", name, workers);
                    std::fclose(file);
                }
        };
        auto active = std::make_shared<std::optional<lsp::RequestContext>>();
        auto releaseRequest = slang::ScopeGuard([&] { active->reset(); });
        if (ctx) active->emplace(ctx);
        const auto caller = std::this_thread::get_id();
        auto workerNoted = std::make_shared<std::atomic_bool>(false);
        auto makeManager = [&](std::shared_ptr<slang::ThreadPool> workers) {
            auto result = std::make_unique<slang::analysis::AnalysisManager>(m_analysisOptions,
                                                                           workers);
            if (ctx) {
                if (workers || compilation.hasFlag(ast::CompilationFlags::TrackSubroutineBodyDependencies))
                    result->setCheckpoint([active, caller, workerNoted, event, workers] {
                        if (workers && std::this_thread::get_id() != caller &&
                            !workerNoted->exchange(true, std::memory_order_relaxed))
                            event("worker-active", uint32_t(workers->get_thread_count()));
                        if (*active) (*active)->checkpoint("during diagnostic procedure");
                    });
                result->addListener([active](const slang::analysis::AnalyzedProcedure&) {
                    if (*active) (*active)->checkpoint("after diagnostic procedure");
                });
                result->addListener([active](const slang::analysis::AnalyzedScope&) {
                    if (*active) (*active)->checkpoint("after diagnostic scope");
                });
                result->addListener([active](const slang::analysis::AnalyzedAssertion&) {
                    if (*active) (*active)->checkpoint("after diagnostic assertion");
                });
            }
            return result;
        };
        auto analysis = makeManager(pool);
        std::shared_ptr<slang::analysis::AnalysisManager> intrinsic;
        compilation.freeze();
        {
            auto unfreeze = slang::ScopeGuard([&] {
                compilation.unfreeze();
                event("unfrozen", m_parallelStats.workers);
            });
            {
                // analyze() can throw while still launching tasks, before its
                // ordinary wait(). Never thaw or clear request state first.
                auto drain = slang::ScopeGuard([&] {
                    if (pool) pool->wait();
                    event("drained", m_parallelStats.workers);
                });
                event("start", m_parallelStats.workers);
                analysis->analyze(compilation);
            }
            ctx.throwIfCancelled("after full-design dataflow");
            if (pool) {
                // Parallel registration is allowed only in the private summary
                // pass. Replay all Native scopes / ports / drivers serially using
                // unchanged intrinsic summaries; contextual checks remain fresh.
                intrinsic = std::move(analysis);
                analysis = makeManager(nullptr);
                ctx.throwIfCancelled("before serial driver diagnostic replay");
                analysis->setProcedureReuse(intrinsic);
                m_parallelStats.serialReplay = true;
                event("serial-replay", m_parallelStats.workers);
                analysis->analyze(compilation);
                const auto [reused, recomputed] = analysis->getProcedureReuseCounts();
                m_parallelStats.proceduresReused = reused;
                m_parallelStats.proceduresRecomputed = recomputed;
                event("procedures-reused", reused);
                event("procedures-recomputed", recomputed);
                m_parallelStats.kept = true;
                event("parallel-summary-kept", m_parallelStats.workers);
            }
        }
        ctx.throwIfCancelled("after full-design dataflow");
        m_intrinsicAnalysis = std::move(intrinsic);
        m_driverAnalysis = std::move(analysis);
        if (modelOwnerTraceEnabled())
            traceModelOwner("full-dataflow", &compilation, nullptr,
                            compilation.getTotalBytesAllocated(),
                            analysisMemoryBytes(), weak_from_this().use_count());
    }
    return *m_driverAnalysis;
}

void ServerCompilationAnalysis::issueDiagnosticsTo(
    slang::DiagnosticEngine& diagEngine, const lsp::RequestContext& ctx,
    const std::function<void(size_t, const slang::Diagnostic&)>& issue) {
    UvmPhaseTrace phases;
    ctx.throwIfCancelled("before full-design semantics");
    // Semantic diagnostics from compilation
    for (auto& diag : compilation.getSemanticDiagnostics()) {
        if (issue)
            issue(1, diag);
        else
            diagEngine.issue(diag);
    }

    phases.mark("diagnostic-full-semantics");
    ctx.throwIfCancelled("after full-design semantics");
    // Driver analysis diagnostics (multi-driven, unused, etc)
    auto& driverAnalysis = getAnalysisManager(ctx);
    phases.mark("diagnostic-full-dataflow");
    const auto diagnostics = driverAnalysis.getDiagnostics();
    INFO("Driver analysis found {} diagnostics", diagnostics.size());
    for (const auto& diag : diagnostics) {
        if (issue)
            issue(2, diag);
        else
            diagEngine.issue(diag);
    }
}

std::vector<const slang::analysis::ValueDriver*> ServerCompilationAnalysis::getDrivers(
    const slang::ast::ValueSymbol& symbol, const lsp::RequestContext& ctx) {
    if (!m_analysisQueries) {
        auto& manager = getAnalysisManager(ctx);
        m_analysisQueries = std::make_unique<slang::analysis::AnalysisQueries>(compilation,
                                                                               manager);
    }
    ctx.checkpoint("before cone driver lookup");
    auto result = m_analysisQueries->getDrivers(symbol);
    ctx.checkpoint("after cone driver lookup");
    return result;
}

const slang::ast::Symbol& ServerCompilationAnalysis::lookupConeSymbol(
    const std::string& path, const lsp::RequestContext& ctx) {
    ctx.checkpoint("before cone path lookup");
    slang::ast::LookupResult result;
    slang::ast::ASTContext context(compilation.getRoot(), slang::ast::LookupLocation::max);
    slang::ast::Lookup::name(compilation.parseName(path), context, slang::ast::LookupFlags::None,
                             result);
    if (!result.found) {
        throw std::runtime_error(fmt::format("Could not find path in compiled design: {}", path));
    }

    ctx.checkpoint("after cone path lookup");
    return *result.found;
}

std::set<ConeLeaf> ServerCompilationAnalysis::getDriverCone(const std::string& path,
                                                            const lsp::RequestContext& ctx) {
    const auto& symbol = lookupConeSymbol(path, ctx);
    const slang::ast::ValueSymbol* lookupSymbol = symbol.as_if<slang::ast::ValueSymbol>();
    const slang::ast::ValueSymbol* resultSymbol =
        ConeLeaf::concreteSymbol(&symbol)->as_if<slang::ast::ValueSymbol>();
    if (!lookupSymbol || !resultSymbol) {
        throw std::runtime_error(fmt::format("Path is not a value symbol: {}", path));
    }

    auto drivers = getDrivers(*resultSymbol, ctx);
    if (lookupSymbol != resultSymbol) {
        for (auto* driver : getDrivers(*lookupSymbol, ctx)) {
            if (std::ranges::find(drivers, driver) == drivers.end()) {
                drivers.push_back(driver);
            }
        }
    }
    DriversTracer tracer(resultSymbol, ctx);
    for (auto* driver : drivers) {
        ctx.checkpoint("during driver cone traversal");
        // slang sets the containingSymbol on an input port driver to the
        // instance body. Visit the parent InstanceSymbol instead so we
        // include the port's connection expression in the driver.
        if (driver->isInputPort()) {
            auto* body = driver->containingSymbol->as_if<ast::InstanceBodySymbol>();
            if (body && body->parentInstance) {
                body->parentInstance->visit(tracer);
                continue;
            }
        }

        // Side-effect drivers use the instance symbol as the containing
        // symbol, while the actual use of the symbol was in the instance
        // body. The drivers tracer does not descend from an instance
        // symbol into the instance body, to prevent a symbol that is
        // connected to an instance's output port from inheriting that
        // output's drivers within the instance body.
        if (driver->flags.has(analysis::DriverFlags::FromSideEffect)) {
            if (auto* instance = driver->containingSymbol->as_if<ast::InstanceSymbol>()) {
                instance->body.visit(tracer);
                continue;
            }
        }
        driver->containingSymbol->visit(tracer);
    }

    return tracer.getLeaves();
}

std::set<ConeLeaf> ServerCompilationAnalysis::getLoadCone(const std::string& path,
                                                          const lsp::RequestContext& ctx) {
    const auto& symbol = lookupConeSymbol(path, ctx);
    if (!m_references) {
        ReferenceIndexer prepared;
        prepared.reset(&compilation.getRoot(), ctx);
        ctx.checkpoint("before cone reference index commit");
        m_references.emplace(std::move(prepared));
    }

    auto it = m_references->symbolToUses.find(
        ConeLeaf::concreteSymbol(&symbol)->as_if<slang::ast::ValueSymbol>());
    if (it == m_references->symbolToUses.end()) {
        throw std::runtime_error(fmt::format("Could not find reference to: {}", path));
    }

    std::set<ConeLeaf> leaves;
    for (const auto* use : it->second) {
        ctx.checkpoint("during load cone traversal");
        // Event / control state belongs to this use only. Symbol allocation
        // order must not turn an unrelated port connection into a self-load.
        LoadsTracer coneTracer(&symbol, ctx);
        use->visit(coneTracer);
        const auto current = coneTracer.getLeaves();
        leaves.insert(current.begin(), current.end());
    }

    return leaves;
}

} // namespace server
