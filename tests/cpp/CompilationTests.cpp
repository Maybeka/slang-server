// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "lsp/LspTypes.h"
#include "utils/ServerHarness.h"
#include <catch2/generators/catch_generators.hpp>
#include <cstdlib>

TEST_CASE("SetBuildFile") {
    ServerHarness server("comp_repo");

    server.setBuildFile("cpu_design.f");

    server.openFile("cpu.sv");

    auto scope = server.getScope("");

    CHECK(scope.size() > 0);
}

TEST_CASE("SetTopLevel") {
    ServerHarness server("comp_repo");

    auto hdl = server.openFile("cpu.sv");

    server.setTopLevel(std::string{hdl.m_uri.getPath()});

    auto scope = server.getScope("");

    CHECK(scope.size() > 0);
}

TEST_CASE("ServerStateSwitching") {
    ServerHarness server("comp_repo");

    auto hdl = server.openFile("cpu.sv");

    server.setTopLevel(std::string{hdl.m_uri.getPath()});
    CHECK(server.getScope("").size() > 0);

    server.setBuildFile("cpu_design.f");
    CHECK(server.getScope("").size() > 0);

    server.setBuildFile("");
    CHECK(server.getScope("").size() == 0);
}

TEST_CASE("ModifyOutOfCompilation") {
    ServerHarness server("comp_repo");
    server.setBuildFile("cpu_design.f");

    auto scope = server.getScope("");

    CHECK(scope.size() > 0);

    auto hdl = server.openFile("alu.sv");

    scope = server.getScope("");

    CHECK(scope.size() > 0);

    hdl.append("   ");

    auto tree = hdl.getSymbolTree();

    CHECK(tree.size() > 0);
}

TEST_CASE("SpamModifyCompilation") {
    ServerHarness server("comp_repo");
    server.loadConfig(Config{.indexGlobs = {std::vector<std::string>{"*.sv"}}});
    server.setBuildFile("cpu_design.f");

    auto scope = server.getScope("");
    CHECK(scope.size() > 0);

    // If file doesn't end in newline, buffer validation will fail
    auto hdl = server.openFile("memory_controller.sv");

    for (int i = 0; i < 10; i++) {
        hdl.insert(10, "   ");
        hdl.save();
    }
}

TEST_CASE("CompilationGotos") {
    ServerHarness server("comp_repo");

    // Open the CPU module and test goto definition for ALU instantiation
    auto hdl = server.openFile("cpu.sv");

    // Test goto definition in explore mode - look for ALU module reference
    auto cursor = hdl.after("alu_inst");
    lsp::DefinitionParams params{.textDocument = {.uri = hdl.m_uri},
                                 .position = cursor.getPosition()};

    // Should find the definition in explore mode
    CHECK(server.hasDefinition(params));

    // Set buildfile to get full compilation context with all modules
    server.setBuildFile("cpu_design.f");

    // Should still find the definition with buildfile (Open docs should be copied over, reparsed
    // with new options)
    CHECK(server.hasDefinition(params));

    // Unset buildfile (go back to explore mode)
    server.setBuildFile("");

    // Should still work in explore mode
    CHECK(server.hasDefinition(params));
}

TEST_CASE("CompilationDiags") {
    ServerHarness server("");

    // Create a test file with some intentional syntax/semantic errors
    auto testContent = R"(
module test_diag;
    logic clk;
    undeclared_type signal; // This should cause a diagnostic

    // Missing semicolon should cause syntax error
    logic reset

    // Using undeclared signal
    assign signal = unknown_signal;
endmodule
)";

    auto hdl = server.openFile("test_diag.sv", testContent);
    hdl.save();

    // Test diagnostics in explore mode
    auto exploreDiags = hdl.getDiagnostics();
    CHECK(exploreDiags.size() > 0); // Should have some diagnostics

    // Set buildfile to get full compilation context
    server.setBuildFile("test1.f");

    // Buildfile mode might clear syntax errors (this is expected behavior)
    // The important thing is that the diagnostic client transitions modes properly

    // Unset buildfile (go back to explore mode)
    server.setBuildFile("");

    // Test diagnostics after unsetting buildfile - should restore explore mode diagnostics
    auto postBuildDiags = hdl.getDiagnostics();
    CHECK(postBuildDiags.size() > 0); // Should have diagnostics restored in explore mode
}

namespace {
const std::string modelOriginal =
    "module leaf #(parameter W=8); logic [W-1:0] old_value; endmodule\n"
    "module model_top; leaf #(.W(8)) left(); leaf #(.W(16)) right(); endmodule\n";
const std::string modelEdited =
    "module leaf #(parameter W=8); logic [W-1:0] newest_value; endmodule\n"
    "module model_top; leaf #(.W(8)) left(); leaf #(.W(16)) right(); endmodule\n";
std::string nativeModelDiagnostics(const std::string& file, const std::string& text) {
    ServerHarness native;
    auto doc = native.openFile(file, text);
    native.setTopLevel(std::string(doc.m_uri.getPath()));
    return rfl::json::write(native.client.getDiagnostics(doc.m_uri));
}
void changeModel(ServerHarness& server, const URI& uri, int version, const std::string& text) {
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = version, .uri = uri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
}
} // namespace

TEST_CASE("Full model cancellation publishes no partial indexes or frozen selections",
          "[model-cancellation]") {
    const auto expected = nativeModelDiagnostics("model_cancel.sv", modelEdited);
    const std::vector<std::string> boundaries = {
        "during full-model module indexing",     "during full-model syntax discovery",
        "before full-model library import",      "after full-model library import",
        "before full-model syntax installation", "after full-model syntax installation",
        "before full-model root binding",        "after full-model root binding",
        "during full-model instance indexing",   "after full-model instance indexing",
        "during full-model active selection",    "during full-model frozen selection",
        "after full-model frozen selection",     "before full-model publication"};
    for (const auto& boundary : boundaries) {
        DYNAMIC_SECTION(boundary) {
            ServerHarness server;
            auto doc = server.openFile("model_cancel.sv", modelOriginal);
            server.setTopLevel(std::string(doc.m_uri.getPath()));
            auto& comp = *server.m_driver->comp;
            REQUIRE(comp.setActiveInstance("model_top.right"));
            size_t callbacks = 0;
            comp.configureFrozenInstances({"model_top.right"}, 1024 * 1024, {},
                                          [&](const auto&) { ++callbacks; });
            auto before = comp.createActiveDesignContext({});
            auto original = before.retainCompilation();
            auto selections = rfl::json::write(comp.frozenInstances()->selections());
            const auto publications = server.client.diagnosticPublications.size();
            changeModel(server, doc.m_uri, 2, modelEdited);
            lsp::RequestContext ctx("textDocument/hover", 91);
            size_t visited = 0;
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == boundary &&
                    ++visited == (boundary == "during full-model instance indexing" ? 2 : 1))
                    ctx.cancel();
            });
            CHECK_THROWS_AS(comp.createActiveDesignContext({}, ctx), lsp::RequestCancelled);
            CHECK(ctx.isCancelled());
            CHECK(callbacks == 1);
            CHECK(server.client.diagnosticPublications.size() == publications);
            CHECK(rfl::json::write(comp.frozenInstances()->selections()) == selections);
            // Held public contexts still own a coherent previous compilation.
            REQUIRE(before.getAnalysis().instances.moduleToInstances.at("leaf").size() == 2);
            CHECK(before.retainCompilation() == original);
            auto current = comp.createActiveDesignContext({});
            CHECK(current.retainCompilation() != original);
            CHECK(callbacks == 2);
            auto scope = rfl::json::write(comp.getScope("model_top.right"));
            CHECK(scope.find("newest_value") != std::string::npos);
            CHECK(scope.find("old_value") == std::string::npos);
            auto selected = comp.getActiveInstance("leaf");
            REQUIRE(selected);
            CHECK(selected->instPath == "model_top.right");
            server.m_driver->runPendingDiagnostics();
            CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == expected);
        }
    }
}

TEST_CASE("Full model refresh forwards cancellation into syntax and retries latest text",
          "[model-cancellation]") {
    const auto expected = nativeModelDiagnostics("model_syntax_cancel.sv", modelEdited);
    ServerHarness server;
    server.loadConfig(Config{.experimentalCooperativeAnalysis = true});
    auto doc = server.openFile("model_syntax_cancel.sv", modelOriginal);
    server.setTopLevel(std::string(doc.m_uri.getPath()));
    auto& comp = *server.m_driver->comp;
    auto original = comp.createActiveDesignContext({}).retainCompilation();
    auto edited = modelEdited;
    edited.insert(edited.find("logic"), std::string(20000, ' ') + "\n");
    changeModel(server, doc.m_uri, 2, edited);
    lsp::RequestContext ctx("textDocument/hover", 92);
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "during disposable syntax parsing")
            ctx.cancel();
    });
    CHECK_THROWS_AS(comp.createActiveDesignContext({}, ctx), lsp::RequestCancelled);
    CHECK(ctx.isCancelled());
    changeModel(server, doc.m_uri, 3, modelEdited);
    auto current = comp.createActiveDesignContext({}).retainCompilation();
    CHECK(current != original);
    CHECK(rfl::json::write(comp.getScope("model_top.left")).find("newest_value") !=
          std::string::npos);
    server.m_driver->runPendingDiagnostics();
    CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == expected);
}

TEST_CASE("Canceled frozen instance configuration leaves the previous selection intact",
          "[model-cancellation]") {
    ServerHarness server;
    auto doc = server.openFile("model_config_cancel.sv", modelOriginal);
    server.setTopLevel(std::string(doc.m_uri.getPath()));
    auto& comp = *server.m_driver->comp;
    size_t callbacks = 0;
    comp.configureFrozenInstances({"model_top.left"}, 1024 * 1024, {},
                                  [&](const auto&) { ++callbacks; });
    const auto before = rfl::json::write(comp.frozenInstances()->selections());
    lsp::RequestContext ctx("workspace/executeCommand", 93);
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "before frozen instance configuration publication")
            ctx.cancel();
    });
    CHECK_THROWS_AS(comp.configureFrozenInstances(
                        {"model_top.right"}, 1024 * 1024, {}, [&](const auto&) { ++callbacks; }, {},
                        ctx),
                    lsp::RequestCancelled);
    CHECK(callbacks == 1);
    CHECK(rfl::json::write(comp.frozenInstances()->selections()) == before);
    CHECK(comp.frozenInstances()->selected("model_top.left"));
    CHECK_FALSE(comp.frozenInstances()->selected("model_top.right"));
}

TEST_CASE("Canceled full model diagnostics retain publication then diagnose and repair latest text",
          "[model-cancellation]") {
    auto broken = modelEdited;
    broken.replace(broken.find("logic [W-1:0]"), std::string("logic [W-1:0]").size(),
                   "missing_type");
    const auto expectedBroken = nativeModelDiagnostics("model_diagnostics.sv", broken);
    const auto expectedFixed = nativeModelDiagnostics("model_diagnostics.sv", modelEdited);
    CHECK(expectedBroken != expectedFixed);
    ServerHarness server;
    auto doc = server.openFile("model_diagnostics.sv", modelOriginal);
    server.setTopLevel(std::string(doc.m_uri.getPath()));
    const auto old = rfl::json::write(server.client.getDiagnostics(doc.m_uri));
    const auto publications = server.client.diagnosticPublications.size();
    changeModel(server, doc.m_uri, 2, broken);
    lsp::RequestContext ctx("server/diagnostics", 94);
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "before full-model publication")
            ctx.cancel();
    });
    CHECK_THROWS_AS(server.m_driver->runPendingDiagnostics(ctx), lsp::RequestCancelled);
    CHECK(server.client.diagnosticPublications.size() == publications);
    CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == old);
    server.m_driver->runPendingDiagnostics();
    CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == expectedBroken);
    changeModel(server, doc.m_uri, 3, modelEdited);
    server.m_driver->runPendingDiagnostics();
    CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == expectedFixed);
}

namespace {
std::string manySemanticMethods() {
    std::string text = "package many; class C;\n";
    for (int i = 0; i < 1600; ++i)
        text += "function int f" + std::to_string(i) + "(); return " + std::to_string(i) +
                "; endfunction\n";
    text += "endclass endpackage\nmodule top; many::C obj; initial begin obj=new; "
            "$display(obj.f1599()); end endmodule\n";
    return text;
}
} // namespace

TEST_CASE("Private complete semantic cancellation retains model and native diagnostic baseline",
          "[semantic-cancellation]") {
    auto originalText = manySemanticMethods();
    auto broken = originalText;
    broken.replace(broken.find("return 1599;"), std::string("return 1599;").size(),
                   "return missing_user_value;");
    const auto expectedBroken = nativeModelDiagnostics("semantic_cancel.sv", broken);
    const auto expectedFixed = nativeModelDiagnostics("semantic_cancel.sv", originalText);
    CHECK(expectedBroken != expectedFixed);
    for (const auto& boundary :
         {"during private full-design semantics", "after private full-design semantics",
          "after private full-design diagnostics"}) {
        DYNAMIC_SECTION(boundary) {
            ServerHarness server;
            server.loadConfig(Config{.experimentalCooperativeAnalysis = true});
            auto doc = server.openFile("semantic_cancel.sv", originalText);
            server.setTopLevel(std::string(doc.m_uri.getPath()));
            auto& comp = *server.m_driver->comp;
            auto original = comp.createActiveDesignContext({}).retainCompilation();
            const auto oldDiagnostics = rfl::json::write(server.client.getDiagnostics(doc.m_uri));
            const auto publications = server.client.diagnosticPublications.size();
            changeModel(server, doc.m_uri, 2, broken);
            lsp::RequestContext ctx("server/diagnostics", 95);
            size_t visits = 0;
            std::shared_ptr<slang::ast::Compilation> observed;
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase != boundary)
                    return;
                ++visits;
                if (std::string_view(boundary) == "during private full-design semantics" &&
                    visits < 3)
                    return;
                // During candidate work, reentrant discovery sees the old coherent model.
                observed = comp.createActiveDesignContext({}).retainCompilation();
                ctx.cancel();
            });
            CHECK_THROWS_AS(server.m_driver->runPendingDiagnostics(ctx), lsp::RequestCancelled);
            REQUIRE(observed);
            CHECK(observed == original);
            CHECK(server.client.diagnosticPublications.size() == publications);
            CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == oldDiagnostics);
            server.m_driver->runPendingDiagnostics();
            CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == expectedBroken);
            changeModel(server, doc.m_uri, 3, originalText);
            server.m_driver->runPendingDiagnostics();
            CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == expectedFixed);
        }
    }
}

TEST_CASE("Private semantic preparation reuses a completed current model",
          "[semantic-cancellation]") {
    ServerHarness server;
    auto doc = server.openFile("semantic_ready.sv", modelOriginal);
    server.setTopLevel(std::string(doc.m_uri.getPath()));
    auto& comp = *server.m_driver->comp;
    auto before = comp.createActiveDesignContext({}).retainCompilation();
    lsp::RequestContext ctx("server/diagnostics", 96);
    size_t constructions = 0;
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "during full-model module indexing")
            ++constructions;
    });
    comp.prepareDiagnostics(ctx);
    CHECK(comp.createActiveDesignContext({}).retainCompilation() == before);
    CHECK(constructions == 0);
}

TEST_CASE("Disposable symbol indexing can cancel midway and resolves latest fields on retry",
          "[semantic-cancellation]") {
    ServerHarness server;
    std::string text = "class C;\n";
    for (int i = 0; i < 2200; ++i)
        text += "int field_" + std::to_string(i) + ";\n";
    text += "endclass\nmodule top; C obj; function int read(); return obj.field_2199; "
            "endfunction endmodule\n";
    auto doc = server.openFile("symbol_index_cancel.sv", text);
    for (size_t at = 0; (at = text.find("field_2199", at)) != std::string::npos;) {
        text.replace(at, std::string("field_2199").size(), "newest_field");
        at += std::string("newest_field").size();
    }
    changeModel(server, doc.m_uri, 2, text);
    auto current = server.m_driver->getDocument(doc.m_uri);
    lsp::RequestContext ctx("textDocument/definition", 97);
    size_t visits = 0;
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "during symbol indexing" && ++visits == 2)
            ctx.cancel();
    });
    CHECK_THROWS_AS(current->getAnalysis(ctx), lsp::RequestCancelled);
    CHECK(visits == 2);
    CHECK_FALSE(current->hasAnalysis());
    const auto offset = text.rfind("newest_field") + 3;
    lsp::Position position{};
    for (size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') {
            ++position.line;
            position.character = 0;
        }
        else
            ++position.character;
    }
    auto definition = server.m_driver->getDefinitionInfoAt(doc.m_uri, position);
    REQUIRE(definition);
    auto locations = definition->getDefinitionLspLocs();
    REQUIRE(locations.size() == 1);
    CHECK(locations.front().uri == doc.m_uri);
    CHECK(locations.front().range.start == lsp::Position{2200, 4});
}

TEST_CASE("Private full semantic preparation remains behind the default-off cooperative setting",
          "[semantic-cancellation]") {
    const auto expected = nativeModelDiagnostics("semantic_flag.sv", modelEdited);
    for (bool enabled : {false, true}) {
        ServerHarness server;
        auto config = server.getConfig();
        if (!enabled)
            CHECK_FALSE(config.experimentalCooperativeAnalysis.value());
        config.experimentalCooperativeAnalysis = enabled;
        server.loadConfig(config);
        auto doc = server.openFile("semantic_flag.sv", modelOriginal);
        server.setTopLevel(std::string(doc.m_uri.getPath()));
        changeModel(server, doc.m_uri, 2, modelEdited);
        lsp::RequestContext ctx("server/diagnostics", 98);
        size_t privatePasses = 0;
        ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
            if (phase == "before private full-design semantics")
                ++privatePasses;
        });
        server.m_driver->runPendingDiagnostics(ctx);
        CHECK((privatePasses > 0) == enabled);
        CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == expected);
    }
}

TEST_CASE("First diagnostics reuse only an unshared current full model",
          "[model-cancellation][unshared-model-diagnostics]") {
    const bool retained = GENERATE(false, true);
    ServerHarness server;
    auto doc = server.openFile("unshared_model.sv", modelOriginal);
    server.setTopLevel(std::string(doc.m_uri.getPath()));
    auto& comp = *server.m_driver->comp;
    comp.refresh();
    std::weak_ptr<slang::ast::Compilation> original;
    std::optional<::server::ActiveDesignContext> held;
    {
        auto view = comp.createActiveDesignContext({});
        CHECK_FALSE(view.getAnalysis().diagnosticsReady());
        original = view.retainCompilation();
        if (retained)
            held = std::move(view);
    }
    size_t roots = 0, reused = 0;
    lsp::RequestContext ctx("first current diagnostics", 290);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "before full-model root binding")
            ++roots;
        if (where == "before unshared full-model diagnostics")
            ++reused;
    });
    comp.prepareDiagnostics(ctx);
    CHECK(roots == (retained ? 1 : 0));
    CHECK(reused == (retained ? 0 : 1));
    auto current = comp.createActiveDesignContext({});
    CHECK(current.getAnalysis().diagnosticsReady());
    if (held) {
        CHECK(current.retainCompilation() != original.lock());
        CHECK_FALSE(held->getAnalysis().diagnosticsReady());
        CHECK(held->getAnalysis().instances.moduleToInstances.at("leaf").size() == 2);
        // A retained old view can still prepare and use its own analysis.
        (void)held->getAnalysis().getAnalysisManager();
        CHECK(held->getAnalysis().diagnosticsReady());
    }
    else
        CHECK(current.retainCompilation() == original.lock());
    server.m_driver->runPendingDiagnostics();
    CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) ==
          nativeModelDiagnostics("unshared_model.sv", modelOriginal));
}

TEST_CASE("Cancelled unshared semantic preparation rebuilds latest input without publishing",
          "[model-cancellation][unshared-model-diagnostics]") {
    const auto boundary = GENERATE("during private full-design semantics",
                                   "after private full-design semantics",
                                   "after private full-design diagnostics",
                                   "before reused full-model publication");
    const auto original = manySemanticMethods();
    auto broken = original;
    broken.replace(broken.find("return 1599;"), std::string("return 1599;").size(),
                   "return missing_user_value;");
    const auto expectedBroken = nativeModelDiagnostics("unshared_cancel.sv", broken);
    const auto expectedFixed = nativeModelDiagnostics("unshared_cancel.sv", original);
    ServerHarness server;
    auto doc = server.openFile("unshared_cancel.sv", original);
    server.setTopLevel(std::string(doc.m_uri.getPath()));
    auto& comp = *server.m_driver->comp;
    comp.refresh();
    std::weak_ptr<slang::ast::Compilation> previous;
    {
        auto view = comp.createActiveDesignContext({});
        previous = view.retainCompilation();
        CHECK_FALSE(view.getAnalysis().diagnosticsReady());
    }
    const auto diagnostics = rfl::json::write(server.client.getDiagnostics(doc.m_uri));
    const auto publications = server.client.diagnosticPublications.size();
    size_t visited = 0;
    lsp::RequestContext ctx("cancel unshared diagnostics", 291);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == boundary &&
            ++visited ==
                (std::string_view(boundary) == "during private full-design semantics" ? 3u : 1u))
            ctx.cancel();
    });
    CHECK_THROWS_AS(comp.prepareDiagnostics(ctx), lsp::RequestCancelled);
    CHECK(ctx.isCancelled());
    CHECK_FALSE(comp.containsCurrentSource(std::filesystem::path(doc.m_uri.getPath())));
    CHECK(server.client.diagnosticPublications.size() == publications);
    CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == diagnostics);
    changeModel(server, doc.m_uri, 2, broken);
    comp.prepareDiagnostics({});
    CHECK(previous.expired());
    CHECK(comp.containsCurrentSource(std::filesystem::path(doc.m_uri.getPath())));
    server.m_driver->runPendingDiagnostics();
    CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == expectedBroken);
    changeModel(server, doc.m_uri, 3, original);
    server.m_driver->runPendingDiagnostics();
    CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == expectedFixed);
}
