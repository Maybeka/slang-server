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
    m_analysisOptions(options.getOrDefault<slang::analysis::AnalysisOptions>()) {
    UvmPhaseTrace phases;
    std::vector<BufferID> bufferIds;
    std::vector<std::shared_ptr<syntax::SyntaxTree>> trees;
    trees.reserve(documents.size());
    for (auto& [_, doc] : documents) {
        ctx.checkpoint("during full-model syntax discovery");
        trees.push_back(doc->getSyntaxTree(ctx));
    }
    phases.mark("model-syntax-discovery");
    ctx.checkpoint("before full-model library import");
    const bool trusted = importTrustedUvm(compilation, sourceManager, options, trees);
    // Native models need the same namespace lifetime guarantee as imported views.
    // Keep only the SourceLifetime (sources + library identities), never the driver.
    if (sourceOwner && !compilation.getSourceOwnerLease())
        compilation.retainSourceOwner(std::move(sourceOwner));
    ctx.checkpoint("after full-model library import");
    phases.mark("model-library-import");
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
    instances.reset(&root, [&] { ctx.checkpoint("during full-model instance indexing"); });
    ctx.checkpoint("after full-model instance indexing");
    phases.mark("model-instance-indexing");

    // symbol references are not indexed until cone tracing is requested
    m_references.reset();
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
        auto analysis = std::make_unique<slang::analysis::AnalysisManager>(m_analysisOptions);
        auto active = std::make_shared<std::optional<lsp::RequestContext>>();
        auto releaseRequest = slang::ScopeGuard([&] { active->reset(); });
        if (ctx) {
            active->emplace(ctx);
            analysis->addListener([active](const slang::analysis::AnalyzedProcedure&) {
                if (*active)
                    (*active)->checkpoint("after diagnostic procedure");
            });
            analysis->addListener([active](const slang::analysis::AnalyzedScope&) {
                if (*active)
                    (*active)->checkpoint("after diagnostic scope");
            });
            analysis->addListener([active](const slang::analysis::AnalyzedAssertion&) {
                if (*active)
                    (*active)->checkpoint("after diagnostic assertion");
            });
        }
        compilation.freeze();
        {
            auto unfreeze = slang::ScopeGuard([&] { compilation.unfreeze(); });
            analysis->analyze(compilation);
        }
        ctx.throwIfCancelled("after full-design dataflow");
        m_driverAnalysis = std::move(analysis);
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
