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

/// @brief Contains the analysis state surrounding a compilation, recreated on every refresh (file
/// save when compilation is set). This includes the compilation itself, references to used buffers,
/// instance indexer, etc.
class ServerCompilationAnalysis : public std::enable_shared_from_this<ServerCompilationAnalysis> {
public:
    ServerCompilationAnalysis(
        const std::unordered_map<std::string, std::shared_ptr<SlangDoc>>& documents, Bag& options,
        SourceManager& sourceManager);

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
        const slang::ast::ValueSymbol& symbol);

    /// Get driver cone leaves for a given RTL path.
    std::set<ConeLeaf> getDriverCone(const std::string& path);

    /// Get load cone leaves for a given RTL path.
    std::set<ConeLeaf> getLoadCone(const std::string& path);

    /// Share completed current driver analysis with readonly fixed-source views.
    slang::analysis::AnalysisManager& getAnalysisManager(const lsp::RequestContext& ctx = {});

private:
    /// Resolve an RTL path for cone tracing, throwing if it is not in the compiled design.
    const slang::ast::Symbol& lookupConeSymbol(const std::string& path);

    /// Retained buffer data to prevent deallocation while this compilation exists
    std::vector<std::shared_ptr<void>> m_retainedBuffers;

    /// Analysis options from the bag, used for driver analysis
    slang::analysis::AnalysisOptions m_analysisOptions;

    /// Lazily created analysis manager used for diagnostics and driver queries.
    std::unique_ptr<slang::analysis::AnalysisManager> m_driverAnalysis;

    /// Instance-aware driver queries that can elaborate the unfrozen compilation.
    std::unique_ptr<slang::analysis::AnalysisQueries> m_analysisQueries;

    /// Index of value symbol -> uses (e.g. processes or continuous assignments)
    std::optional<ReferenceIndexer> m_references = std::nullopt;
};

} // namespace server
