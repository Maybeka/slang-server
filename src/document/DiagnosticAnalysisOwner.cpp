// SPDX-License-Identifier: MIT
#include "document/DiagnosticAnalysisOwner.h"

#include "ast/ActiveDesignContext.h"
#include "ast/ServerCompilationAnalysis.h"
#include <stdexcept>

#include "slang/ast/symbols/SubroutineSymbols.h"

namespace server {
DiagnosticAnalysisOwner DiagnosticAnalysisOwner::pending(const ActiveDesignContext& design) {
    DiagnosticAnalysisOwner result;
    // This is an aliasing pointer: its control block owns the analysis wrapper,
    // not merely the Compilation whose address it exposes.
    result.owner = design.retainCompilation();
    return result;
}

DiagnosticAnalysisOwner DiagnosticAnalysisOwner::prepared(const ActiveDesignContext& design,
                                                          const lsp::RequestContext& ctx) {
    auto result = pending(design);
    result.full = &design.getAnalysis().getAnalysisManager(ctx);
    result.intrinsic = &design.getAnalysis().getProcedureReuseBaseline(ctx);
    return result;
}

void DiagnosticAnalysisOwner::selectTaskTiming(const slang::ast::SubroutineSymbol& selected) {
    if (!full || selected.subroutineKind != slang::ast::SubroutineKind::Task ||
        !full->getAnalyzedSubroutine(selected))
        throw std::logic_error("task timing requires a task from the retained analysis");
    task = full;
}

slang::analysis::AnalysisManager& DiagnosticAnalysisOwner::manager() const {
    if (!full)
        throw std::logic_error("diagnostic analysis owner is not prepared");
    return *full;
}
} // namespace server
