// SPDX-License-Identifier: MIT
#include "ast/ActiveDesignContext.h"
#include "ast/ServerCompilationAnalysis.h"
#include "document/DiagnosticAnalysisOwner.h"
#include "utils/ServerHarness.h"
#include <catch2/generators/catch_generators.hpp>

#include "slang/ast/symbols/SubroutineSymbols.h"

using server::DiagnosticAnalysisOwner;

namespace {
constexpr auto designText = R"(
module diagnostic_owner;
  int value;
  function automatic int step(input int x); return x + 1; endfunction
  task automatic update(); value++; endtask
  initial begin value = step(1); update(); end
endmodule
)";
}

TEST_CASE("Diagnostic analysis views retain the original model across replacement and release",
          "[diagnostic-analysis-owner]") {
    const bool timing = GENERATE(false, true);
    ServerHarness server;
    auto doc = server.openFile("diagnostic_owner.sv", designText);
    server.setTopLevel(std::string(doc.m_uri.getPath()));
    auto& compilation = *server.m_driver->comp;
    DiagnosticAnalysisOwner retained;
    std::weak_ptr<slang::ast::Compilation> original;
    const slang::ast::SubroutineSymbol* function = nullptr;
    {
        auto design = compilation.createActiveDesignContext({});
        auto pending = DiagnosticAnalysisOwner::pending(design);
        REQUIRE_FALSE(pending.isPrepared());
        CHECK_FALSE(pending.complete());
        retained = DiagnosticAnalysisOwner::prepared(design, {});
        REQUIRE(retained.isPrepared());
        REQUIRE(retained.procedures());
        original = retained.compilation();
        auto functions = retained.complete()->getAnalyzedSubroutines();
        REQUIRE_FALSE(functions.empty());
        function = functions.front();
        for (const auto fn : functions) {
            if (fn->subroutineKind == slang::ast::SubroutineKind::Function)
                CHECK_THROWS_AS(retained.selectTaskTiming(*fn), std::logic_error);
            else if (timing)
                retained.selectTaskTiming(*fn);
        }
        CHECK(retained.timing() == (timing ? retained.complete() : nullptr));
    }
    compilation.refresh();
    auto replacement = compilation.createActiveDesignContext({});
    CHECK(replacement.retainCompilation() != original.lock());
    REQUIRE_FALSE(original.expired());
    CHECK(retained.complete()->getAnalyzedSubroutine(*function));
    auto secondConsumer = retained;
    retained = {};
    CHECK_FALSE(original.expired());
    auto finalConsumer = std::move(secondConsumer);
    CHECK_FALSE(secondConsumer.compilation());
    CHECK_FALSE(secondConsumer.isPrepared());
    CHECK_FALSE(secondConsumer.procedures());
    CHECK_FALSE(secondConsumer.timing());
    secondConsumer = std::move(finalConsumer);
    CHECK_FALSE(finalConsumer.compilation());
    CHECK_FALSE(finalConsumer.isPrepared());
    CHECK_FALSE(finalConsumer.procedures());
    CHECK_FALSE(finalConsumer.timing());
    CHECK_FALSE(original.expired());
    CHECK(secondConsumer.complete()->getAnalyzedSubroutine(*function));
    secondConsumer = {};
    CHECK(original.expired());
}

TEST_CASE("Cancelled analysis preparation leaves a retained pending owner retryable",
          "[diagnostic-analysis-owner]") {
    ServerHarness server;
    auto doc = server.openFile("diagnostic_owner.sv", designText);
    server.setTopLevel(std::string(doc.m_uri.getPath()));
    server.m_driver->comp->refresh();
    auto design = server.m_driver->comp->createActiveDesignContext({});
    REQUIRE_FALSE(design.getAnalysis().diagnosticsReady());
    auto pending = DiagnosticAnalysisOwner::pending(design);
    lsp::RequestContext cancelled("diagnostic owner preparation", 901);
    cancelled.cancel();
    CHECK_THROWS_AS(DiagnosticAnalysisOwner::prepared(design, cancelled), lsp::RequestCancelled);
    CHECK_FALSE(pending.isPrepared());
    CHECK(pending.compilation() == design.retainCompilation());
    auto prepared = DiagnosticAnalysisOwner::prepared(design, {});
    CHECK(prepared.isPrepared());
    CHECK(prepared.compilation() == pending.compilation());
}
