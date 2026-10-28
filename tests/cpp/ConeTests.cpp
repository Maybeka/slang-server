#include <catch2/generators/catch_generators.hpp>
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "ast/ServerCompilation.h"
#include "ast/ServerCompilationAnalysis.h"
#include "lsp/LspTypes.h"
#include "utils/ServerHarness.h"
#include <array>

TEST_CASE("Replacement cache release preserves borrowed models and reconstructs native results",
          "[model-cache-release]") {
    ServerHarness server("");
    Config config{.build = "cone_test.f"};
    config.experimentalCooperativeAnalysis = true;
    server.loadConfig(config);
    auto& driver = *server.m_driver;
    REQUIRE(driver.comp);
    auto& model = *driver.comp;
    REQUIRE(driver.setActiveInstance("test.the_sub_2"));
    const auto drivers = model.getDriverCone("test.the_sub_2.x").getPaths();
    const auto loads = model.getLoadCone("test.a").getPaths();
    REQUIRE_FALSE(drivers.empty());
    REQUIRE_FALSE(loads.empty());
    auto collectDiagnostics = [&] {
        std::vector<std::pair<slang::SourceLocation, std::string>> result;
        model.issueDiagnosticsTo(driver.diagEngine, {}, [&](size_t, const auto& diagnostic) {
            result.emplace_back(diagnostic.location, driver.diagEngine.formatMessage(diagnostic));
        });
        return result;
    };
    const auto diagnostics = collectDiagnostics();

    const std::array<std::string_view, 1> names{"sub"};
    auto borrowed = std::make_optional(model.createActiveDesignContext(names));
    auto* root = &borrowed->getAnalysis().compilation;
    auto* manager = &borrowed->getAnalysis().getAnalysisManager();
    driver.prepareForContextRetirement(true);
    REQUIRE_FALSE(model.releaseUnsharedAnalysisCaches());
    CHECK(&borrowed->getAnalysis().getAnalysisManager() == manager);
    CHECK(model.getDriverCone("test.the_sub_2.x").getPaths() == drivers);
    borrowed.reset();

    REQUIRE(model.releaseUnsharedAnalysisCaches());
    CHECK_FALSE(model.releaseUnsharedAnalysisCaches());
    {
        auto context = model.createActiveDesignContext(names);
        CHECK(&context.getAnalysis().compilation == root);
        CHECK_FALSE(context.getAnalysis().diagnosticsReady());
    }
    // Keeping the old model after a replacement failure must still support
    // diagnostics, cone indexes and the user's selected instance.
    CHECK(model.getDriverCone("test.the_sub_2.x").getPaths() == drivers);
    CHECK(model.getLoadCone("test.a").getPaths() == loads);
    CHECK(collectDiagnostics() == diagnostics);
    auto active = model.getActiveInstanceSymbol("sub");
    REQUIRE(active);
    auto context = model.createActiveDesignContext(names);
    CHECK(context.getBoundInstance("sub") == active);
    CHECK(context.getAnalysis().diagnosticsReady());
}

TEST_CASE("Replacement analysis cache release remains disabled by default",
          "[model-cache-release]") {
    ServerHarness server("");
    server.loadConfig(Config{.build = "cone_test.f"});
    auto& driver = *server.m_driver;
    REQUIRE(driver.comp);
    const std::array<std::string_view, 1> names{"sub"};
    {
        auto context = driver.comp->createActiveDesignContext(names);
        (void)context.getAnalysis().getAnalysisManager();
    }
    driver.prepareForContextRetirement(true);
    auto context = driver.comp->createActiveDesignContext(names);
    CHECK(context.getAnalysis().diagnosticsReady());
}

TEST_CASE("Released complete models recover after cancelled reconstruction",
          "[model-cache-release][model-root-release]") {
    const auto cancellationPhase = GENERATE("during full-model module indexing",
                                           "before full-model publication");
    ServerHarness server("");
    Config config{.build = "cone_test.f"};
    config.experimentalCooperativeAnalysis = true;
    server.loadConfig(config);
    auto& driver = *server.m_driver;
    REQUIRE(driver.comp);
    auto& model = *driver.comp;
    REQUIRE(driver.setActiveInstance("test.the_sub_2"));
    auto sortedPaths = [](auto paths) {
        std::ranges::sort(paths);
        return paths;
    };
    const auto drivers = sortedPaths(model.getDriverCone("test.the_sub_2.x").getPaths());
    const auto loads = sortedPaths(model.getLoadCone("test.a").getPaths());
    const auto preferred = model.getPreferredSymbolPath("sub");
    REQUIRE(preferred);
    const std::array<std::string_view, 1> names{"sub"};
    auto borrowed = std::make_optional(model.createActiveDesignContext(names));
    auto old = borrowed->getAnalysis().weak_from_this();
    driver.prepareForContextRetirement(true);
    CHECK_FALSE(old.expired());
    borrowed.reset();
    driver.prepareForContextRetirement(true);
    CHECK(old.expired());

    lsp::RequestContext ctx("released-root-rebuild", 1);
    bool checkedHint = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == cancellationPhase) {
            checkedHint = true;
            CHECK(model.getPreferredSymbolPath("sub") == preferred);
            CHECK_FALSE(model.getPreferredSymbolPath("missing_definition"));
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(model.getScopesByModule(ctx), lsp::RequestCancelled);
    CHECK(checkedHint);
    CHECK(old.expired());
    CHECK(sortedPaths(model.getDriverCone("test.the_sub_2.x").getPaths()) == drivers);
    CHECK(sortedPaths(model.getLoadCone("test.a").getPaths()) == loads);
    CHECK(model.getPreferredSymbolPath("sub") == preferred);
    auto context = model.createActiveDesignContext(names);
    REQUIRE(context.getBoundInstance("sub"));
    CHECK(context.getBoundInstance("sub")->getHierarchicalPath() == "test.the_sub_2");
}

TEST_CASE("Cone Tracing") {
    ServerHarness server("");

    server.loadConfig(Config{.build = "cone_test.f"});

    // This will actually load the compilation
    server.onInitialized(lsp::InitializedParams{});

    auto doc = server.openFile("cone_test.sv");

    // TODO -- prepare for interfaces -- list signals?

    SECTION("Prepare Multiple") {
        auto cursor = doc.before("x <= a + b;");
        server.checkPrepareCallHierarchy(cursor, {"test.the_sub_1.x", "test.the_sub_2.x"});
    }

    SECTION("Prepare Module Outside Design") {
        auto unusedDoc = server.openFile("cone_unused.sv");
        auto cursor = unusedDoc.before("unused_signal");
        server.checkPrepareCallHierarchy(cursor, {});
        server.client.expectWarning("The selected signal is not part of the current design");
    }

    SECTION("Prepare Unsupported") {
        auto cursor = doc.before("the_sub_1(");
        server.checkPrepareCallHierarchy(cursor, {});
        server.client.expectWarning("Only simple logic vectors are currently supported");
    }

    SECTION("Prepare Single") {
        auto cursor = doc.before("a,");
        server.checkPrepareCallHierarchy(cursor, {"test.a"});
    }

    SECTION("Drivers Multiple") {
        auto cursor_a = doc.after("module sub(").after("input logic [31:0] ");
        auto cursor_b = cursor_a.after("input logic [31:0] ");
        server.checkConeCommand("slang.getDriversWithLocation", "test.the_sub_2.x",
                                {{"test.the_sub_2.a", &cursor_a}, {"test.the_sub_2.b", &cursor_b}});
    }

    SECTION("Drivers Single") {
        auto cursor = doc.before("x1;");
        server.checkConeCommand("slang.getDriversWithLocation", "test.the_sub_2.b",
                                {{"test.x1", &cursor}});
    }

    SECTION("Drivers Single2") {
        // This points at the port declartion.  It would be more consistent to point at the
        // port map instead, but that location information doesn't appear to be attached to
        // PortSymbol
        auto cursor = doc.after("module sub").after("output logic [31:0] ");
        server.checkConeCommand("slang.getDriversWithLocation", "test.x1",
                                {{"test.the_sub_1.x", &cursor}});
    }

    SECTION("Drivers Constant") {
        auto cursor_foo = doc.after("module sub_sub(").after("input logic ");
        auto cursor_bar = cursor_foo.after("input logic ");
        server.checkConeCommand("slang.getDriversWithLocation", "test.the_sub_2.the_sub_sub.result",
                                {{"test.the_sub_2.the_sub_sub.foo", &cursor_foo},
                                 {"test.the_sub_2.the_sub_sub.bar", &cursor_bar}});
    }

    SECTION("Drivers Switched") {
        auto cursor_foo = doc.after("module sub_sub(").after("input logic ");
        auto cursor_bar = cursor_foo.after("input logic ");
        server.checkConeCommand("slang.getDriversWithLocation",
                                "test.the_sub_2.the_sub_sub.switched_result",
                                {{"test.the_sub_2.the_sub_sub.bar", &cursor_bar},
                                 {"test.the_sub_2.the_sub_sub.foo", &cursor_foo}});
    }

    SECTION("Drivers Interface") {
        auto cursor_qux = doc.before("qux;");
        auto cursor_b =
            doc.after("module sub(").after("input logic [31:0] ").after("input logic [31:0] ");
        server.checkConeCommand("slang.getDriversWithLocation", "test.the_intfs[2].qux",
                                {{"test.the_intfs[1].qux", &cursor_qux},
                                 {"test.the_sub_2.b", &cursor_b}});
    }

    SECTION("Drivers Interface Reference") {
        auto cursor_qux = doc.before("qux;");
        auto cursor_b =
            doc.after("module sub(").after("input logic [31:0] ").after("input logic [31:0] ");
        server.checkConeCommand("slang.getDriversWithLocation", "test.the_sub_1.qux_out.qux",
                                {{"test.the_intfs[0].qux", &cursor_qux},
                                 {"test.the_sub_1.b", &cursor_b}});
    }

    SECTION("Parameters Are Not Drivers") {
        server.checkConeCommand("slang.getDriversWithLocation", "test.parameter_driven", {});
    }

    SECTION("Loads Multiple") {
        auto cursor = doc.after("module sub(").after("input logic [31:0] ");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.a",
                                {{"test.the_sub_2.a", &cursor}, {"test.the_sub_1.a", &cursor}});
    }

    SECTION("Loads Up Down") {
        auto cursor_x = doc.after("module sub").after("output logic [31:0] ");
        auto cursor_foo = doc.after("module sub_sub(").after("input logic ");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_sub_2.a",
                                {{"test.the_sub_2.x", &cursor_x},
                                 {"test.the_sub_2.the_sub_sub.foo", &cursor_foo}});
    }

    SECTION("Loads Single") {
        auto cursor = doc.after("module test(").after("output logic [31:0] ");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_sub_2.x",
                                {{"test.x", &cursor}});
    }

    SECTION("Loads Conditional") {
        auto cursor_result = doc.before("result;");
        auto cursor_switched = doc.before("switched_result;");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_sub_1.the_sub_sub.foo",
                                {{"test.the_sub_1.the_sub_sub.result", &cursor_result},
                                 {"test.the_sub_1.the_sub_sub.switched_result", &cursor_switched}});
    }

    SECTION("Loads Switched") {
        auto cursor_result = doc.before("result;");
        auto cursor_switched = doc.before("switched_result;");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_sub_2.the_sub_sub.bar",
                                {{"test.the_sub_2.the_sub_sub.result", &cursor_result},
                                 {"test.the_sub_2.the_sub_sub.switched_result", &cursor_switched}});
    }

    SECTION("Loads Interface") {
        auto cursor = doc.before("quz;");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_intfs[1].quz",
                                {{"test.the_intfs[0].quz", &cursor}});
    }

    SECTION("Loads Interface Reference") {
        auto cursor = doc.before("qux;");
        server.checkConeCommand("slang.getLoadsWithLocation", "test.the_sub_1.qux_out.qux",
                                {{"test.the_intfs[2].qux", &cursor}});
    }
}

TEST_CASE("Cone locations deduplicate generated multi-bit drivers") {
    ServerHarness server("");
    server.loadConfig(Config{.build = "cone_test.f"});
    server.onInitialized(lsp::InitializedParams{});
    auto doc = server.openFile("cone_test.sv");
    auto declaration = doc.before("bit_driver;");

    server.checkConeCommand("slang.getDriversWithLocation", "test.bit_driven",
                            {{"test.bit_driver", &declaration}});
}

TEST_CASE("Undriven signals have empty driver cones") {
    ServerHarness server;
    auto doc = server.openFile("undriven_cone.sv", R"(
module top;
    logic undriven;
    logic unused;
    logic result;
    assign result = undriven;
endmodule
)");
    server.setTopLevel(std::string(doc.m_uri.getPath()));

    server.checkConeCommand("slang.getDriversWithLocation", "top.undriven", {});
    server.checkConeCommand("slang.getDriversWithLocation", "top.unused", {});
}

TEST_CASE("Cone cancellation discards partial reference indices and retries",
          "[hierarchy-cancellation][cone-cancellation]") {
    const auto boundary = GENERATE("during cone reference indexing",
                                   "before cone reference index commit",
                                   "during cone AST traversal", "during cone result conversion");
    ServerHarness server("");
    server.loadConfig(Config{.build = "cone_test.f"});
    server.onInitialized(lsp::InitializedParams{});
    lsp::RequestContext ctx("workspace/executeCommand", lsp::ID_t{713});
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view at) {
        if (at == boundary) {
            reached = true;
            ctx.cancel();
        }
    });
    lsp::ExecuteCommandParams params{.command = "slang.getLoadsWithLocation",
                                     .arguments = std::vector<lsp::LSPAny>{
                                         rfl::to_generic(std::string("test.a"))}};
    CHECK_THROWS_AS(server.executeCommand(params, ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    auto retried = server.executeCommand(params);
    ServerHarness native("");
    native.loadConfig(Config{.build = "cone_test.f"});
    native.onInitialized(lsp::InitializedParams{});
    // Endpoints are a set; independent compilations allocate their symbols in
    // different orders. Compare every endpoint field without imposing that order.
    auto normalized = [](const std::optional<lsp::LSPAny>& values) {
        REQUIRE(values);
        auto entries = rfl::from_generic<std::vector<server::ConeEntry>>(*values);
        REQUIRE(entries);
        std::vector<std::string> result;
        for (const auto& entry : entries.value())
            result.push_back(rfl::json::write(entry));
        std::ranges::sort(result);
        return result;
    };
    CHECK(normalized(retried) == normalized(native.executeCommand(params)));
}
