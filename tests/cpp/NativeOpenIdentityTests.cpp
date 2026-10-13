// SPDX-License-Identifier: MIT
#include "utils/ServerHarness.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <rfl/json.hpp>

#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/types/Type.h"

namespace {
namespace fs = std::filesystem;

class NativeOpenWorkspace {
public:
    NativeOpenWorkspace() {
        static std::atomic_uint64_t serial{0};
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (;;) {
            auto candidate = fs::temp_directory_path() /
                             ("slang-native-open-" + std::to_string(stamp) + "-" +
                              std::to_string(serial++));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                root = fs::canonical(candidate);
                break;
            }
            REQUIRE_FALSE(error);
        }
    }
    ~NativeOpenWorkspace() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path path(std::string_view name) const { return root / name; }
    URI uri(std::string_view name) const { return URI::fromFile(path(name)); }
    void write(std::string_view name, std::string_view text) const {
        fs::create_directories(path(name).parent_path());
        std::ofstream output(path(name), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << text;
        output.close();
        REQUIRE(output.good());
    }
    lsp::InitializeParams params() const {
        return {.workspaceFolders = {
                    {lsp::WorkspaceFolder{.uri = URI::fromFile(root), .name = "native-open"}}}};
    }
    fs::path root;
};
} // namespace

TEST_CASE("Native replay during didOpen preserves the editor document and version",
          "[explicit-context][native-unit][document-version]") {
    NativeOpenWorkspace workspace;
    workspace.write("producer.sv", "`define FIELD_TYPE shortint\n");
    workspace.write("consumer.sv", "package p; typedef `FIELD_TYPE field_t; endpackage\n");
    workspace.write("consumer.arguments", "consumer.sv\n");
    workspace.write("native.arguments", "producer.sv\n-C consumer.arguments\n");
    workspace.write(".slang/server.json", rfl::json::write(std::map<std::string, std::string>{
        {"flags", "--single-unit -F \"" + workspace.path("native.arguments").string() + "\""}}));
    ServerHarness server(workspace.params());

    std::string filename;
    std::string editorText;
    SECTION("primary macro producer") {
        filename = "producer.sv";
        editorText = "// unsaved editor text\n`define FIELD_TYPE byte\n";
    }
    SECTION("primary inherited consumer") {
        filename = "consumer.sv";
        editorText = "// unsaved editor text\npackage p; typedef `FIELD_TYPE edited_t; endpackage\n";
    }
    auto uri = workspace.uri(filename);
    auto diskDocument = server.m_driver->getDocument(uri);
    REQUIRE(diskDocument->isNativeCompilationUnit());
    REQUIRE(diskDocument->requiresNativeReplay());
    server.onDocDidOpen({.textDocument = {.uri = uri,
                                         .languageId = "systemverilog",
                                         .version = 7,
                                         .text = editorText}});

    auto opened = server.m_driver->getDocument(uri);
    REQUIRE(server.m_driver->isDocumentOpen(uri));
    REQUIRE(opened->getVersion() == 7);
    CHECK(opened != diskDocument);
    CHECK(opened->textMatches(editorText));
    CHECK(server.client.diagnosticVersions.at(uri) == 7);
    auto analysis = opened->getAnalysis();
    CHECK(analysis->getCompilation()->getSyntaxTrees().size() == 2);
    CHECK(analysis->getCompilation()->getRoot().topInstances.empty());

    workspace.write(filename, editorText);
    server.onDocDidSave({.textDocument = {.uri = uri}, .text = editorText});
    CHECK(server.m_driver->getDocument(uri) == opened);
    CHECK(opened->getVersion() == 7);
    CHECK(opened->textMatches(editorText));
    CHECK(server.client.diagnosticVersions.at(uri) == 7);
}

TEST_CASE("Unsaved native macro edits refresh diagnostics on an open package root",
          "[explicit-context][native-unit][diagnostic-refresh]") {
    NativeOpenWorkspace workspace;
    const std::string producerText = "`define FIELD_TYPE shortint\n";
    const std::string consumerText = "package p; typedef `FIELD_TYPE field_t; endpackage\n";
    workspace.write("producer.sv", producerText);
    workspace.write("consumer.sv", consumerText);
    workspace.write("consumer.arguments", "consumer.sv\n");
    workspace.write("native.arguments", "producer.sv\n-C consumer.arguments\n");
    workspace.write(".slang/server.json", rfl::json::write(std::map<std::string, std::string>{
        {"flags", "--single-unit -F \"" + workspace.path("native.arguments").string() + "\""}}));
    ServerHarness server(workspace.params());
    const auto consumerUri = workspace.uri("consumer.sv");
    const auto producerUri = workspace.uri("producer.sv");
    server.onDocDidOpen({.textDocument = {.uri = consumerUri,
                                         .languageId = "systemverilog",
                                         .version = 1,
                                         .text = consumerText}});
    server.onDocDidOpen({.textDocument = {.uri = producerUri,
                                         .languageId = "systemverilog",
                                         .version = 1,
                                         .text = producerText}});
    REQUIRE(server.client.getDiagnostics(consumerUri).empty());

    // Deliberately do not query the consumer after this edit. Only the pending
    // diagnostic queue may refresh its already-published diagnostics.
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = 2, .uri = producerUri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
             .text = "`define FIELD_TYPE missing_live_type\n"}}});
    server.m_driver->runPendingDiagnostics();
    const auto broken = server.client.getDiagnostics(consumerUri);
    REQUIRE(std::any_of(broken.begin(), broken.end(), [](const auto& diagnostic) {
        return diagnostic.message.find("missing_live_type") != std::string::npos;
    }));
    CHECK(server.client.diagnosticVersions.at(consumerUri) == 1);
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());

    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = 3, .uri = producerUri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
             .text = "`define FIELD_TYPE byte\n"}}});
    server.m_driver->runPendingDiagnostics();
    CHECK(server.client.getDiagnostics(consumerUri).empty());
    CHECK(server.client.diagnosticVersions.at(consumerUri) == 1);
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
}

TEST_CASE("Full-design save preserves native single-unit roots and macro-derived types",
          "[explicit-context][native-unit][full-save]") {
    NativeOpenWorkspace workspace;
    const std::string initialProducer = "`define PAYLOAD_WIDTH 8\n";
    const std::string editedProducer = "`define PAYLOAD_WIDTH 16\n";
    workspace.write("producer.sv", initialProducer);
    workspace.write("types.sv",
                    "package payload_types; typedef logic [`PAYLOAD_WIDTH-1:0] payload_t; endpackage\n");
    workspace.write("top.sv", "module top; payload_types::payload_t payload; endmodule\n");
    workspace.write("native.arguments",
                    "--single-unit\n\"" + workspace.path("producer.sv").string() + "\"\n\"" +
                        workspace.path("types.sv").string() + "\"\n\"" +
                        workspace.path("top.sv").string() + "\"\n--top top\n");
    workspace.write(".slang/server.json", rfl::json::write(std::map<std::string, std::string>{
        {"build", workspace.path("native.arguments").string()}}));
    ServerHarness server(workspace.params());
    server.client.capabilities.inactiveRegionsSupported = false;
    REQUIRE(server.m_driver->comp);

    auto checkFullCompilation = [&](uint64_t width) {
        const auto* top = server.m_driver->comp->getActiveInstanceSymbol("top");
        REQUIRE(top);
        auto& compilation = top->body.getCompilation();
        const auto trees = compilation.getSyntaxTrees();
        REQUIRE(trees.size() == 1);
        REQUIRE(trees.front()->diagnostics().empty());
        std::vector<fs::path> roots;
        auto& sm = server.m_driver->sm;
        for (auto buffer : trees.front()->getSourceBufferIds()) {
            CHECK(sm.isLatestData(buffer));
            if (!sm.getIncludedFrom(buffer).valid())
                roots.push_back(sm.getFullPath(buffer));
        }
        std::reverse(roots.begin(), roots.end());
        CHECK(roots == std::vector<fs::path>{workspace.path("producer.sv"),
                                           workspace.path("types.sv"),
                                           workspace.path("top.sv")});
        const auto* payload = top->body.find("payload");
        REQUIRE(payload);
        CHECK(payload->as<slang::ast::ValueSymbol>().getType().getBitWidth() == width);
        for (const auto& root : roots)
            CHECK(server.m_driver->comp->containsCurrentSource(root));
    };
    checkFullCompilation(8);

    const auto producerUri = workspace.uri("producer.sv");
    server.onDocDidOpen({.textDocument = {.uri = producerUri,
                                         .languageId = "systemverilog",
                                         .version = 1,
                                         .text = initialProducer}});
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = 2, .uri = producerUri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = editedProducer}}});
    workspace.write("producer.sv", editedProducer);
    server.onDocDidSave({.textDocument = {.uri = producerUri}, .text = editedProducer});

    checkFullCompilation(16);
    CHECK(server.m_driver->getDocument(producerUri)->getVersion() == 2);
}
