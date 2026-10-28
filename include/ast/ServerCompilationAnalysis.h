//------------------------------------------------------------------------------
// ServerCompilationAnalysis.h
// Contains the analysis state from a refreshed compilation
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "ConeTracer.h"
#include "InstanceIndexer.h"
#include "ReferenceIndexer.h"
#include "document/SlangDoc.h"
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/analysis/AnalysisOptions.h"
#include "slang/analysis/AnalysisQueries.h"
#include "slang/util/Bag.h"

namespace server {
using namespace slang;

// Diagnostic execution policy only; never a semantic input or a cached proof.
struct ParallelDiagnosticOptions {
    bool enabled = false;
    uint32_t workers = 2;
};
struct ParallelDiagnosticStats {
    uint32_t workers = 0;
    bool attempted = false, kept = false, serialReplay = false, observerSerial = false;
    size_t proceduresReused = 0, proceduresRecomputed = 0;
};


/// @brief Contains the analysis state surrounding a compilation, recreated on every refresh (file
/// save when compilation is set). This includes the compilation itself, references to used buffers,
/// instance indexer, etc.
class ServerCompilationAnalysis : public std::enable_shared_from_this<ServerCompilationAnalysis> {
public:
    ServerCompilationAnalysis(
        const std::unordered_map<std::string, std::shared_ptr<SlangDoc>>& documents, Bag& options,
        SourceManager& sourceManager, const lsp::RequestContext& ctx = {},
        bool disposableRoot = false, std::shared_ptr<const void> sourceOwner = {});
    ~ServerCompilationAnalysis();

    /// Only called before this analysis escapes from a private model / driver.
    void prepareDisposableDiagnostics(const lsp::RequestContext& ctx);
    bool diagnosticsReady() const { return bool(m_driverAnalysis); }
    ParallelDiagnosticStats parallelDiagnosticStats() const { return m_parallelStats; }

    slang::ast::Compilation compilation;

    /// Index of buffer -> definitions and definition -> instances given a compilation. Used for
    /// navigating a compilation via the sidebar
    InstanceIndexer instances;

    /// Issue all semantic diagnostics from the compilation to the diagnostic engine
    void issueDiagnosticsTo(
        slang::DiagnosticEngine& diagEngine, const lsp::RequestContext& ctx = {},
        const std::function<void(size_t, const slang::Diagnostic&)>& issue = {});

    /// Get the full-design drivers for a value symbol.
    std::vector<const slang::analysis::ValueDriver*> getDrivers(
        const slang::ast::ValueSymbol& symbol, const lsp::RequestContext& ctx = {});

    /// Get driver cone leaves for a given RTL path.
    std::set<ConeLeaf> getDriverCone(const std::string& path, const lsp::RequestContext& ctx = {});

    /// Get load cone leaves for a given RTL path.
    std::set<ConeLeaf> getLoadCone(const std::string& path, const lsp::RequestContext& ctx = {});

    /// Share completed current driver analysis with readonly fixed-source views.
    slang::analysis::AnalysisManager& getAnalysisManager(const lsp::RequestContext& ctx = {});
    /// Original unchanged intrinsic summaries; public diagnostics and driver queries
    /// continue to use getAnalysisManager(). Both are pinned by this model.
    slang::analysis::AnalysisManager& getProcedureReuseBaseline(const lsp::RequestContext& ctx = {});
    size_t analysisMemoryBytes() const;

private:
    friend class ServerCompilation;
    // Caller must hold the sole owning reference and discard diagnostic routes first.
    bool releaseRecomputableCaches();

    /// Resolve an RTL path for cone tracing, throwing if it is not in the compiled design.
    const slang::ast::Symbol& lookupConeSymbol(const std::string& path,
                                               const lsp::RequestContext& ctx = {});

    /// Retained buffer data to prevent deallocation while this compilation exists
    std::vector<std::shared_ptr<void>> m_retainedBuffers;

    /// Analysis options from the bag, used for driver analysis
    slang::analysis::AnalysisOptions m_analysisOptions;
    ParallelDiagnosticOptions m_parallelOptions;
    ParallelDiagnosticStats m_parallelStats;

    /// Lazily created analysis manager used for diagnostics and driver queries.
    // Declare the intrinsic owner first so serial views are destroyed before it.
    std::shared_ptr<slang::analysis::AnalysisManager> m_intrinsicAnalysis;
    std::unique_ptr<slang::analysis::AnalysisManager> m_driverAnalysis;

    /// Instance-aware driver queries that can elaborate the unfrozen compilation.
    std::unique_ptr<slang::analysis::AnalysisQueries> m_analysisQueries;

    /// Index of value symbol -> uses (e.g. processes or continuous assignments)
    std::optional<ReferenceIndexer> m_references = std::nullopt;
};

} // namespace server
