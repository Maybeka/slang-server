// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "diagnostics/DiagnosticWorker.h"
#include "lsp/RequestContext.h"
#include "utils/ServerHarness.h"
#include <algorithm>
#include <atomic>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/types/AllTypes.h"

namespace {
namespace fs = std::filesystem;

// Construct before ServerHarness so every file survives driver destruction.
class ExplicitWorkspace {
public:
    ExplicitWorkspace() {
        static std::atomic_uint64_t serial{0};
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (;;) {
            auto candidate = fs::temp_directory_path() /
                             ("slang-explicit-context-" + std::to_string(stamp) + "-" +
                              std::to_string(serial++));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                root = fs::canonical(candidate);
                break;
            }
            REQUIRE_FALSE(error);
        }
    }

    ~ExplicitWorkspace() {
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

    void configure(std::string flags) const {
        write(".slang/server.json",
              "{\"indexingThreads\":1,\"flags\":" + rfl::json::write(flags) + "}\n");
    }

    lsp::InitializeParams params() const {
        return {.workspaceFolders = {{lsp::WorkspaceFolder{.uri = URI::fromFile(root),
                                                           .name = "explicit-context-test"}}}};
    }

    fs::path root;
};

const std::string nativeHeader =
    "class item;\n    owner_t value;\n    selected_t selected;\n"
    "    function owner_t get(); return value; endfunction\nendclass\n";

void writeNativeContext(const ExplicitWorkspace& workspace) {
    workspace.write("preamble.sv", "`define FIELD_TYPE byte\n");
    workspace.write("selected.sv", "package dep_pkg; typedef int selected_t; endpackage\n");
    workspace.write("a_offlist.sv", "package dep_pkg; typedef bit wrong_t; endpackage\n");
    workspace.write("owner.sv", "package p;\nimport dep_pkg::*;\n"
                                "typedef `FIELD_TYPE owner_t;\n"
                                "`include \"item.svh\"\nendpackage\n");
    workspace.write("item.svh", nativeHeader);
    workspace.write("recursive.sv", "module recursive; recursive next(); endmodule\n");
    workspace.write("lists/top.arguments", "-F nested/sources.manifest\n");
    workspace.write("lists/nested/sources.manifest",
                    "../../preamble.sv\n../../selected.sv\n../../owner.sv\n"
                    "../../recursive.sv\n");
    workspace.configure("--single-unit -F \"" + workspace.path("lists/top.arguments").string() +
                        "\"");
}

uint64_t aliasWidth(DocumentHandle& document, std::string_view packageName,
                    std::string_view aliasName) {
    const auto& compilation = document.doc->getAnalysis()->getCompilation();
    auto* package = compilation->getPackage(packageName);
    REQUIRE(package);
    auto* symbol = package->find(aliasName);
    REQUIRE(symbol);
    return symbol->as<slang::ast::TypeAliasType>().getCanonicalType().getBitWidth();
}

void checkDefinition(DocumentHandle& document, std::string_view needle, const URI& expected) {
    auto definitions = document.before(std::string(needle)).getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == expected);
}

bool isContextDiagnostic(const lsp::Diagnostic& diagnostic) {
    return diagnostic.code && rfl::holds_alternative<std::string>(*diagnostic.code) &&
           rfl::get<std::string>(*diagnostic.code) == "context-unavailable";
}

void checkContextUnavailable(ServerHarness& server, DocumentHandle& document,
                             std::string_view token = "value") {
    CHECK(server.m_driver->hasExplicitContextError(document.m_uri));
    CHECK(std::ranges::any_of(document.getDiagnostics(), isContextDiagnostic));
    auto position = document.before(std::string(token)).getPosition();
    CHECK_THROWS_AS(server.getDocHover(
                        {.textDocument = {.uri = document.m_uri}, .position = position}),
                    lsp::RequestFailed);
    CHECK_THROWS_AS(server.getDocDefinition(
                        {.textDocument = {.uri = document.m_uri}, .position = position}),
                    lsp::RequestFailed);
    CHECK_THROWS_AS(server.getDocCompletion(
                        {.textDocument = {.uri = document.m_uri}, .position = position}),
                    lsp::RequestFailed);
}

void changeWithoutDiagnostics(ServerHarness& server, DocumentHandle& document, int version,
                              std::string text) {
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = version, .uri = document.m_uri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
    document.m_text = std::move(text);
}

} // namespace

TEST_CASE("Configured native filelists retain roots providers and package-only semantics",
          "[explicit-context][native-unit]") {
    ExplicitWorkspace workspace;
    writeNativeContext(workspace);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", nativeHeader);

    REQUIRE(server.m_driver->hasConfiguredCompilationInputs());
    REQUIRE(server.m_driver->comp == nullptr);
    CHECK(server.m_driver->isPackageOnlyContext(header.m_uri));
    CHECK_FALSE(server.m_driver->hasExplicitContextError(header.m_uri));
    CHECK(header.getDiagnostics().empty());
    CHECK(aliasWidth(header, "p", "owner_t") == 8);
    CHECK(header.doc->getAnalysis()->getCompilation()->getRoot().topInstances.empty());
    CHECK_FALSE(header.doc->getCachedSyntaxTree()); // No duplicate standalone header parse.
    checkDefinition(header, "owner_t value", workspace.uri("owner.sv"));
    checkDefinition(header, "selected_t selected", workspace.uri("selected.sv"));
    CHECK(server.m_indexer.getFilesForSymbol("dep_pkg").size() == 2);
    CHECK_FALSE(server.m_driver->isDocumentOpen(workspace.uri("owner.sv")));
    CHECK_FALSE(server.m_driver->isDocumentOpen(workspace.uri("selected.sv")));
}

TEST_CASE("Configured contexts observe unsaved earlier roots owners and headers",
          "[explicit-context][native-unit]") {
    ExplicitWorkspace workspace;
    writeNativeContext(workspace);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", nativeHeader);
    auto preamble = server.openFile("preamble.sv", "`define FIELD_TYPE byte\n");
    const auto revision = server.m_driver->getExplicitContextRevision(header.m_uri);
    changeWithoutDiagnostics(server, preamble, 2, "`define FIELD_TYPE shortint\n");
    CHECK(server.m_driver->getExplicitContextRevision(header.m_uri) != revision);
    CHECK(aliasWidth(header, "p", "owner_t") == 16);

    auto owner = server.openFile("owner.sv", "package p;\nimport dep_pkg::*;\n"
                                             "typedef `FIELD_TYPE owner_t;\n"
                                             "`include \"item.svh\"\nendpackage\n");
    changeWithoutDiagnostics(server, owner, 2,
                             "// unsaved owner\npackage p;\nimport dep_pkg::*;\n"
                             "typedef `FIELD_TYPE replacement_t;\n"
                             "`include \"item.svh\"\nendpackage\n");
    changeWithoutDiagnostics(server, header, 2,
                             "class item; replacement_t value; selected_t selected; endclass\n");
    CHECK(aliasWidth(header, "p", "replacement_t") == 16);
    checkDefinition(header, "replacement_t value", workspace.uri("owner.sv"));
    server.m_driver->runPendingDiagnostics();
    CHECK(header.getDiagnostics().empty());
    CHECK(header.doc->getAnalysis()->getCompilation()->getRoot().topInstances.empty());
}

TEST_CASE("Configured package snapshots freeze full native input graphs and commit repairs",
          "[explicit-context][diagnostic-snapshot]") {
    ExplicitWorkspace workspace;
    writeNativeContext(workspace);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", nativeHeader);
    changeWithoutDiagnostics(server, header, 2,
                             "class item; snapshot_missing_type value; endclass\n");
    auto snapshot = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(snapshot);
    auto bytes = server::encodeDiagnosticJob(snapshot->metadata, *snapshot->input);
    // No watcher event is sent: the worker must use its frozen bytes regardless
    // of what these already-captured paths contain when the job actually runs.
    workspace.write("preamble.sv", "`define FIELD_TYPE poisoned_disk_type\n");
    workspace.write("owner.sv", "unparseable disk replacement\n");
    auto output = server::executeDiagnosticJob(bytes);
    auto result = std::find_if(output.documents.begin(), output.documents.end(),
                               [&](const auto& item) { return item.uri == header.m_uri; });
    REQUIRE(result != output.documents.end());
    CHECK(std::ranges::any_of(result->diagnostics, [](const auto& diagnostic) {
        return diagnostic.message.find("snapshot_missing_type") != std::string::npos;
    }));
    CHECK(std::ranges::none_of(result->diagnostics, [](const auto& diagnostic) {
        return diagnostic.message.find("poisoned_disk_type") != std::string::npos;
    }));
    server.m_driver->commitDiagnosticSnapshot(*snapshot, std::move(output), {});
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
    CHECK(server.client.diagnosticVersions.at(header.m_uri) == 2);

    changeWithoutDiagnostics(server, header, 3, nativeHeader);
    auto repaired = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(repaired);
    auto clean = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(repaired->metadata, *repaired->input));
    server.m_driver->commitDiagnosticSnapshot(*repaired, std::move(clean), {});
    CHECK(header.getDiagnostics().empty());
    CHECK(server.client.diagnosticVersions.at(header.m_uri) == 3);
}

TEST_CASE("Configured inputs reject unrelated documents instead of adopting indexed owners",
          "[explicit-context]") {
    ExplicitWorkspace workspace;
    workspace.write("selected.sv", "package selected; int field; endpackage\n");
    workspace.write("inputs.arguments", "selected.sv\n");
    workspace.write("other.sv", "package other; typedef int value_t;\n"
                                "`include \"item.svh\"\nendpackage\n");
    const std::string text = "class item; value_t value; endclass\n";
    workspace.write("item.svh", text);
    workspace.configure("-F \"" + workspace.path("inputs.arguments").string() + "\"");
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    checkContextUnavailable(server, header);
}

TEST_CASE("Configured source hints cannot name an off-filelist same-name provider",
          "[explicit-context][source-context-hints]") {
    ExplicitWorkspace workspace;
    writeNativeContext(workspace);
    workspace.write("a_offlist.sv", "package dep_pkg; typedef int selected_t; endpackage\n");
    workspace.write("owner.sv", "package p;\nimport dep_pkg::*; "
                                "// slang-server: source \"a_offlist.sv\"\n"
                                "typedef `FIELD_TYPE owner_t;\n"
                                "`include \"item.svh\"\nendpackage\n");
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", nativeHeader);
    checkContextUnavailable(server, header);
}

TEST_CASE("Configured owner hints select only compatible real compilation contexts",
          "[explicit-context][source-context-hints]") {
    ExplicitWorkspace workspace;
    writeNativeContext(workspace);
    workspace.write("off_owner.sv",
                    "package offlist; typedef int owner_t; typedef int selected_t;\n"
                    "`include \"item.svh\"\nendpackage\n");
    std::string hint;
    bool compatible = false;
    SECTION("a configured owner remains usable") {
        hint = "owner.sv";
        compatible = true;
    }
    SECTION("an unlisted owner cannot expand the configured declaration universe") {
        hint = "off_owner.sv";
    }
    auto text = "// slang-server: owner \"" + hint + "\"\n" + nativeHeader;
    workspace.write("item.svh", text);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    if (compatible) {
        CHECK(header.getDiagnostics().empty());
        checkDefinition(header, "owner_t value", workspace.uri("owner.sv"));
    }
    else {
        checkContextUnavailable(server, header);
    }
}

TEST_CASE("Configured source hints must name the physical package provider in a combined unit",
          "[explicit-context][source-context-hints]") {
    ExplicitWorkspace workspace;
    writeNativeContext(workspace);
    workspace.write("owner.sv", "package p;\nimport dep_pkg::*; "
                                "// slang-server: source \"preamble.sv\"\n"
                                "typedef `FIELD_TYPE owner_t;\n"
                                "`include \"item.svh\"\nendpackage\n");
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", nativeHeader);
    checkContextUnavailable(server, header);
}

TEST_CASE("Owner hint chains require actual nested inclusion under a package",
          "[explicit-context][source-context-hints]") {
    ExplicitWorkspace workspace;
    const std::string text = "// slang-server: owner \"wrapper.svh\"\n"
                             "class item; value_t value; endclass\n";
    workspace.write("headers/item.svh", text);
    workspace.write("headers/wrapper.svh", "// slang-server: owner \"../pkg/owner.sv\"\n"
                                           "`include \"item.svh\"\n");
    SECTION("each hinted ancestor occurs on the real include chain") {
        workspace.write("pkg/owner.sv", "package p; typedef int value_t;\n"
                                        "`include \"../headers/wrapper.svh\"\nendpackage\n");
        ServerHarness server(workspace.params());
        auto header = server.openFile("headers/item.svh", text);
        CHECK(header.getDiagnostics().empty());
        CHECK(server.m_driver->isPackageOnlyContext(header.m_uri));
        checkDefinition(header, "value_t value", workspace.uri("pkg/owner.sv"));
    }
    SECTION("a real inclusion cannot validate an invented intermediate ancestor") {
        workspace.write("pkg/owner.sv", "package p; typedef int value_t;\n"
                                        "`include \"../headers/item.svh\"\nendpackage\n");
        ServerHarness server(workspace.params());
        auto header = server.openFile("headers/item.svh", text);
        checkContextUnavailable(server, header);
    }
    SECTION("a global include beside a package does not establish package ownership") {
        workspace.write("pkg/owner.sv", "package p; typedef int value_t; endpackage\n"
                                        "`include \"../headers/wrapper.svh\"\n");
        ServerHarness server(workspace.params());
        auto header = server.openFile("headers/item.svh", text);
        checkContextUnavailable(server, header);
    }
}

TEST_CASE("Owner hints reject missing sources cycles and conflicting duplicates",
          "[explicit-context][source-context-hints]") {
    ExplicitWorkspace workspace;
    std::string text;
    SECTION("missing path") {
        text = "// slang-server: owner \"missing.sv\"\n";
    }
    SECTION("self cycle") {
        text = "// slang-server: owner \"item.svh\"\n";
    }
    SECTION("two-file cycle") {
        text = "// slang-server: owner \"wrapper.svh\"\n";
        workspace.write("wrapper.svh", "// slang-server: owner \"item.svh\"\n");
    }
    SECTION("conflicting duplicates") {
        text = "// slang-server: owner \"first.sv\"\n"
               "// slang-server: owner \"second.sv\"\n";
        workspace.write("first.sv", "package a;\n`include \"item.svh\"\nendpackage\n");
        workspace.write("second.sv", "package b;\n`include \"item.svh\"\nendpackage\n");
    }
    text += "class item; int value; endclass\n";
    workspace.write("item.svh", text);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    checkContextUnavailable(server, header);
}

TEST_CASE("Unsaved owner-hint replacement removal and inclusion restoration refresh context",
          "[explicit-context][source-context-hints]") {
    ExplicitWorkspace workspace;
    const std::string body = "class item; value_t value; endclass\n";
    const std::string original = "// slang-server: owner \"owner.sv\"\n" + body;
    const std::string ownerText = "package p; typedef int value_t;\n"
                                  "`include \"item.svh\"\nendpackage\n";
    workspace.write("item.svh", original);
    workspace.write("owner.sv", ownerText);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", original);
    checkDefinition(header, "value_t value", workspace.uri("owner.sv"));
    auto revision = server.m_driver->getExplicitContextRevision(header.m_uri);
    header.replaceAll("// slang-server: owner \"missing.sv\"\n" + body);
    header.publishChanges();
    CHECK(server.m_driver->getExplicitContextRevision(header.m_uri) != revision);
    checkContextUnavailable(server, header);

    header.replaceAll(body);
    header.publishChanges();
    CHECK_FALSE(server.m_driver->hasExplicitContextError(header.m_uri));
    checkDefinition(header, "value_t value", workspace.uri("owner.sv"));
    header.replaceAll(original);
    header.publishChanges();
    auto owner = server.openFile("owner.sv", ownerText);
    owner.replaceAll("package p; typedef int value_t; endpackage\n");
    owner.publishChanges();
    checkContextUnavailable(server, header);
    owner.replaceAll(ownerText);
    owner.publishChanges();
    CHECK_FALSE(server.m_driver->hasExplicitContextError(header.m_uri));
    CHECK(header.getDiagnostics().empty());
    checkDefinition(header, "value_t value", workspace.uri("owner.sv"));
}

TEST_CASE("Active source hints in included headers use their physical source directory",
          "[explicit-context][source-context-hints]") {
    ExplicitWorkspace workspace;
    const std::string text =
        "// slang-server: owner \"../owner.sv\"\n"
        "import required_pkg::*; // slang-server: source \"../deps/right.sv\"\n"
        "class item; required_t value; endclass\n";
    workspace.write("headers/item.svh", text);
    workspace.write("owner.sv", "package p;\n`include \"headers/item.svh\"\nendpackage\n");
    workspace.write("deps/right.sv",
                    "package required_pkg; typedef shortint required_t; endpackage\n");
    workspace.write("a_wrong.sv", "package required_pkg; typedef bit offlist_t; endpackage\n");
    ServerHarness server(workspace.params());
    auto header = server.openFile("headers/item.svh", text);
    CHECK(header.getDiagnostics().empty());
    CHECK_FALSE(server.m_driver->hasExplicitContextError(header.m_uri));
    checkDefinition(header, "required_t value", workspace.uri("deps/right.sv"));
}

TEST_CASE("Explicit package sources reject absent or incorrectly named declarations",
          "[explicit-context][source-context-hints]") {
    ExplicitWorkspace workspace;
    SECTION("missing file") {
    }
    SECTION("different package name") {
        workspace.write("named.sv", "package unrelated_pkg; typedef int required_t; endpackage\n");
    }
    SECTION("same-name module is not a package") {
        workspace.write("named.sv", "module required_pkg; typedef int required_t; endmodule\n");
    }
    workspace.write("a_fallback.sv", "package required_pkg; typedef int required_t; endpackage\n");
    const std::string text = "package p;\nimport required_pkg::*; "
                             "// slang-server: source \"named.sv\"\n"
                             "required_t value; endpackage\n";
    workspace.write("consumer.sv", text);
    ServerHarness server(workspace.params());
    auto consumer = server.openFile("consumer.sv", text);
    checkContextUnavailable(server, consumer);
}

TEST_CASE("Conflicting source hints cannot silently select either package provider",
          "[explicit-context][source-context-hints]") {
    ExplicitWorkspace workspace;
    workspace.write("first.sv", "package required_pkg; typedef int required_t; endpackage\n");
    workspace.write("second.sv", "package required_pkg; typedef byte required_t; endpackage\n");
    const std::string text = "package p;\n"
                             "import required_pkg::*; // slang-server: source \"first.sv\"\n"
                             "import required_pkg::*; // slang-server: source \"second.sv\"\n"
                             "required_t value; endpackage\n";
    workspace.write("consumer.sv", text);
    ServerHarness server(workspace.params());
    auto consumer = server.openFile("consumer.sv", text);
    checkContextUnavailable(server, consumer);
}

TEST_CASE("Explicit context failures retain parse diagnostics and reject semantic queries",
          "[explicit-context][source-context-hints]") {
    ExplicitWorkspace workspace;
    const std::string text = "// slang-server: owner \"missing.sv\"\n"
                             "class item; int value endclass\n";
    workspace.write("item.svh", text);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    checkContextUnavailable(server, header);
    CHECK_FALSE(header.doc->hasAnalysis()); // Invalid context must not bind unrelated RTL.
    CHECK(std::ranges::any_of(header.getDiagnostics(), [](const auto& diagnostic) {
        return !isContextDiagnostic(diagnostic) &&
               diagnostic.severity == lsp::DiagnosticSeverity::Error;
    }));
    changeWithoutDiagnostics(server, header, 2, text + "// still unresolved\n");
    CHECK_THROWS_AS(server.m_driver->captureDiagnosticSnapshot({}), std::runtime_error);
    CHECK(server.m_driver->hasPendingDiagnostics());
}

TEST_CASE("Native library contexts reject private capture and preserve sequential semantics",
          "[explicit-context][native-unit][diagnostic-snapshot]") {
    ExplicitWorkspace workspace;
    workspace.write("owner.sv", "package p; typedef `FIELD_TYPE owner_t;\n"
                                "`include \"item.svh\"\nendpackage\n");
    const std::string text = "class item; owner_t value; endclass\n";
    workspace.write("item.svh", text);
    workspace.write("library.arguments", "+define+FIELD_TYPE=longint\n"
                                         "--library explicit_test_library\nowner.sv\n");
    workspace.configure("-C \"" + workspace.path("library.arguments").string() + "\"");
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    REQUIRE(server.m_driver->hasConfiguredCompilationInputs());
    CHECK(aliasWidth(header, "p", "owner_t") == 64);
    changeWithoutDiagnostics(server, header, 2, text + "// unsaved library include\n");
    CHECK_THROWS_AS(server.m_driver->captureDiagnosticSnapshot({}), std::runtime_error);
    REQUIRE(server.m_driver->hasPendingDiagnostics());
    server.m_driver->runPendingDiagnostics();
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
    CHECK_FALSE(server.m_driver->hasExplicitContextError(header.m_uri));
    CHECK(header.getDiagnostics().empty());
    CHECK(aliasWidth(header, "p", "owner_t") == 64);
    checkDefinition(header, "owner_t value", workspace.uri("owner.sv"));
}

TEST_CASE("Native inherited contexts reject private capture and replay unsaved producers",
          "[explicit-context][native-unit][diagnostic-snapshot]") {
    ExplicitWorkspace workspace;
    workspace.write("preamble.sv", "`define FIELD_TYPE shortint\n");
    workspace.write("owner.sv", "package p; typedef `FIELD_TYPE owner_t;\n"
                                "`include \"item.svh\"\nendpackage\n");
    const std::string text = "class item; owner_t value; endclass\n";
    workspace.write("item.svh", text);
    workspace.write("consumer.arguments", "owner.sv\n");
    workspace.write("native.arguments", "preamble.sv\n-C consumer.arguments\n");
    workspace.configure("--single-unit -F \"" + workspace.path("native.arguments").string() + "\"");
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    CHECK(aliasWidth(header, "p", "owner_t") == 16);
    auto preamble = server.openFile("preamble.sv", "`define FIELD_TYPE shortint\n");
    changeWithoutDiagnostics(server, preamble, 2, "`define FIELD_TYPE byte\n");
    changeWithoutDiagnostics(server, header, 2, text + "// unsaved inherited include\n");
    CHECK_THROWS_AS(server.m_driver->captureDiagnosticSnapshot({}), std::runtime_error);
    REQUIRE(server.m_driver->hasPendingDiagnostics());
    server.m_driver->runPendingDiagnostics();
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
    CHECK_FALSE(server.m_driver->hasExplicitContextError(header.m_uri));
    CHECK(header.getDiagnostics().empty());
    CHECK(aliasWidth(header, "p", "owner_t") == 8);
    checkDefinition(header, "owner_t value", workspace.uri("owner.sv"));
}

TEST_CASE("Hinted package cancellation is not cached as a context error", "[owner-cancellation]") {
    const auto boundary = GENERATE("during source hint lexing", "during source import binding",
                                   "before import hint cache commit",
                                   "during disposable syntax parsing");
    CAPTURE(boundary);
    ExplicitWorkspace workspace;
    workspace.write(".slang/server.json", "{\"experimentalCooperativeAnalysis\":true}\n");
    workspace.write("p.sv", "package p; typedef bit [7:0] data_t; endpackage\n");
    std::string provider = "package p; typedef logic [63:0] data_t;\n";
    for (int i = 0; i < 5000; ++i)
        provider += "int padding_" + std::to_string(i) + ";\n";
    provider += "endpackage\n";
    workspace.write("wanted/p.sv", provider);
    workspace.write("consumer.sv",
                    "package consumer;\nimport p::*; // slang-server: source \"wanted/p.sv\"\n"
                    "typedef data_t selected_t; endpackage\n");
    ServerHarness server(workspace.params());
    auto doc = server.m_driver->getDocument(workspace.uri("consumer.sv"));
    (void)doc->getSyntaxTree();
    bool hinted = false, interrupted = false;
    lsp::RequestContext ctx("hinted package", lsp::ID_t{902});
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "before hinted package syntax")
            hinted = true;
        if (where == boundary && (where != "during disposable syntax parsing" || hinted)) {
            interrupted = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(doc->getAnalysis(ctx), lsp::RequestCancelled);
    REQUIRE(interrupted);
    CHECK_FALSE(server.m_driver->hasExplicitContextError(workspace.uri("consumer.sv")));
    auto analysis = doc->getAnalysis();
    auto compilation = analysis->getCompilation();
    auto package = compilation->getPackage("consumer");
    REQUIRE(package);
    auto selected = package->find("selected_t");
    REQUIRE(selected);
    CHECK(selected->as<slang::ast::TypeAliasType>().getCanonicalType().getBitWidth() == 64);
    (void)compilation->getSemanticDiagnostics();
    CHECK_FALSE(compilation->hasIssuedErrors());
}

TEST_CASE("Cancelled virtual interface collection retries the complete dependency set",
          "[owner-cancellation]") {
    ExplicitWorkspace workspace;
    workspace.write(".slang/server.json", "{\"experimentalCooperativeAnalysis\":true,"
                                          "\"explorePreincludeFiles\":[\"pre.svh\"]}\n");
    workspace.write("pre.svh", "`define ENVIRONMENT 1\n");
    workspace.write("a.sv", "interface if_a; logic a; endinterface\n");
    workspace.write("b.sv", "interface if_b; logic b; endinterface\n");
    workspace.write("consumer.sv", "package consumer; class agent; virtual if_a a; virtual if_b b; "
                                   "endclass endpackage\n");
    ServerHarness server(workspace.params());
    const auto uri = workspace.uri("consumer.sv");
    auto doc = server.m_driver->getDocument(uri);
    auto tree = doc->getSyntaxTree();
    int seen = 0;
    lsp::RequestContext ctx("interfaces", lsp::ID_t{903});
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "during virtual interface collection" && ++seen == 2)
            ctx.cancel();
    });
    CHECK_THROWS_AS(server.m_driver->getDependentTrees(tree, &uri, ctx), lsp::RequestCancelled);
    REQUIRE(seen == 2);
    const auto dependencies = server.m_driver->getDependentTrees(tree, &uri);
    CHECK(dependencies.size() == 2);
    for (auto name : {"a.sv", "b.sv"})
        CHECK(std::ranges::any_of(dependencies, [&](const auto& dependency) {
            return server.m_driver->sm.getFullPath(dependency->getSourceBufferIds().front()) ==
                   workspace.path(name);
        }));
    auto compilation = doc->getAnalysis()->getCompilation();
    (void)compilation->getSemanticDiagnostics();
    CHECK_FALSE(compilation->hasIssuedErrors());
}

TEST_CASE("Inferred include reparsing keeps the disposable parser checkpoint",
          "[owner-cancellation]") {
    ExplicitWorkspace workspace;
    workspace.write(".slang/server.json", "{\"experimentalCooperativeAnalysis\":true}\n");
    workspace.write("inc/defs.svh", "typedef logic [31:0] word_t;\n");
    workspace.write("top.sv", "`include \"defs.svh\"\nmodule top; word_t value; endmodule\n");
    ServerHarness server(workspace.params());
    auto doc = server.m_driver->getDocument(workspace.uri("top.sv"));
    bool inference = false, interrupted = false;
    lsp::RequestContext ctx("inferred include", lsp::ID_t{904});
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "during inferred include search")
            inference = true;
        if (inference && where == "during disposable syntax parsing") {
            interrupted = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(doc->getSyntaxTree(ctx), lsp::RequestCancelled);
    REQUIRE(interrupted);
    auto tree = doc->getSyntaxTree();
    CHECK(tree->diagnostics().empty());
    REQUIRE(tree->getIncludeDirectives().size() == 1);
    CHECK(tree->getIncludeDirectives().front().buffer.id.valid());
    auto compilation = doc->getAnalysis()->getCompilation();
    (void)compilation->getSemanticDiagnostics();
    CHECK_FALSE(compilation->hasIssuedErrors());
}

TEST_CASE("Standalone owner probing cancels without committing discovery", "[owner-cancellation]") {
    ExplicitWorkspace workspace;
    workspace.write(".slang/server.json", "{\"experimentalCooperativeAnalysis\":true}\n");
    std::string text = "module top;\n";
    for (int i = 0; i < 5000; ++i)
        text += "int padding_" + std::to_string(i) + ";\n";
    text += "endmodule\n";
    workspace.write("top.sv", text);
    ServerHarness server(workspace.params());
    const auto uri = workspace.uri("top.sv");
    auto doc = server.m_driver->getDocument(uri);
    bool probing = false, interrupted = false;
    lsp::RequestContext ctx("standalone owner", lsp::ID_t{905});
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "before standalone owner probe")
            probing = true;
        if (probing && where == "during disposable syntax parsing") {
            interrupted = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(server.m_driver->getIncludeContext(uri, nullptr, ctx), lsp::RequestCancelled);
    REQUIRE(interrupted);
    CHECK_FALSE(doc->getCachedSyntaxTree());
    CHECK(server.m_driver->getOwnerDiscoveryAttemptCount() == 0);
    CHECK_FALSE(server.m_driver->getIncludeContext(uri));
    CHECK(server.m_driver->getOwnerDiscoveryAttemptCount() == 0);
    CHECK(doc->getSyntaxTree()->diagnostics().empty());
}

TEST_CASE("Cooperative package queries avoid design rebuild and retain declared interface types",
          "[package-query-context][semantic-cancellation]") {
    for (bool enabled : {false, true}) {
        ExplicitWorkspace workspace;
        workspace.write(".slang/server.json",
                        "{\"flags\":\"--single-unit --top top\",\"indexingThreads\":1,"
                        "\"experimentalCooperativeAnalysis\":" +
                            std::string(enabled ? "true" : "false") + "}\n");
        workspace.write("bus.sv",
                        "interface bus_if #(parameter N=7); logic [N-1:0] data; endinterface\n");
        const std::string original =
            "class C;\nvirtual bus_if #(9) vif;\nint scalar;\n"
            "function new(); scalar=0; endfunction\n"
            "extern function logic[8:0] read();\nendclass\n"
            "function logic[8:0] C::read();\nreturn vif.data;\nendfunction\n";
        workspace.write("item.svh", original);
        workspace.write("p_pkg.sv", "package p;\n`include \"item.svh\"\nendpackage\n");
        workspace.write("top.sv", "module top; bus_if #(16) chosen(); endmodule\n");
        workspace.write("design.f", "bus.sv\np_pkg.sv\ntop.sv\n");
        ServerHarness server(workspace.params());
        server.setBuildFile(workspace.path("design.f").string());
        REQUIRE(server.m_driver->comp);
        auto native = server.m_driver->comp->createActiveDesignContext({}).retainCompilation();
        auto package = native->getPackage("p");
        REQUIRE(package);
        auto cls = package->find("C");
        REQUIRE(cls);
        auto member = cls->as<slang::ast::ClassType>().find("vif");
        REQUIRE(member);
        auto& vif = member->as<slang::ast::ValueSymbol>()
                        .getType()
                        .getCanonicalType()
                        .as<slang::ast::VirtualInterfaceType>();
        auto data = vif.iface.body.find("data");
        REQUIRE(data);
        CHECK(data->as<slang::ast::ValueSymbol>().getType().getBitWidth() == 9);
        auto header = server.openFile("item.svh");
        auto latest = original;
        latest.replace(latest.find("scalar"), std::string("scalar").size(), "latest_scalar");
        server.SlangServer::onDocDidChange(
            {.textDocument = {.version = 2, .uri = header.m_uri},
             .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = latest}}});
        lsp::RequestContext ctx("textDocument/hover", 112);
        size_t rebuilds = 0;
        ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
            if (phase == "before full-model root binding")
                ++rebuilds;
        });
        auto current = server.m_driver->getDocument(header.m_uri);
        auto analysis = current->getAnalysis(ctx);
        CHECK(server.m_driver->isPackageQueryContext(header.m_uri) == enabled);
        CHECK(rebuilds == (enabled ? 0 : 1));
        auto position = header.before("data").getPosition();
        auto hover = rfl::json::write(
            server.getDocHover({.textDocument = {.uri = header.m_uri}, .position = position}, ctx));
        CAPTURE(enabled, hover);
        CHECK(hover.find("data") != std::string::npos);
        if (enabled)
            CHECK(hover.find("[8:0]") != std::string::npos);
        CHECK(rebuilds == (enabled ? 0 : 1));
        server.m_driver->runPendingDiagnostics();
        // A real RTL source still uses its selected instantiated parameters.
        REQUIRE(server.m_driver->comp->setActiveInstance("top.chosen"));
        auto bus = server.openFile("bus.sv");
        auto rtl = rfl::json::write(server.getDocHover(
            {.textDocument = {.uri = bus.m_uri}, .position = bus.before("data").getPosition()}));
        CHECK_FALSE(server.m_driver->isPackageQueryContext(bus.m_uri));
        CHECK(rtl.find("[15:0]") != std::string::npos);
    }
}

TEST_CASE("Package query eligibility follows the latest configured include owner",
          "[package-query-context][semantic-cancellation]") {
    ExplicitWorkspace workspace;
    workspace.write(".slang/server.json",
                    "{\"flags\":\"--single-unit --top top\",\"indexingThreads\":1,"
                    "\"experimentalCooperativeAnalysis\":true}\n");
    workspace.write("item.svh", "class C; int scalar; endclass\n");
    workspace.write("owner.sv", "package p;\n`include \"item.svh\"\nendpackage\n");
    workspace.write("top.sv", "module top; endmodule\n");
    workspace.write("design.f", "owner.sv\ntop.sv\n");
    ServerHarness server(workspace.params());
    server.setBuildFile(workspace.path("design.f").string());
    auto header = server.openFile("item.svh");
    auto current = server.m_driver->getDocument(header.m_uri);
    auto [beforeInputs, before] = current->getAnalysisAndInputs();
    CHECK(beforeInputs.tree == before->getSyntaxTree());
    CHECK(beforeInputs.buffer == before->getBuffer());
    CHECK(server.m_driver->isPackageQueryContext(header.m_uri));
    auto owner = server.openFile("owner.sv");
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = 2, .uri = owner.m_uri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
             .text = "module bridge;\n`include \"item.svh\"\nendmodule\n"}}});
    auto [afterInputs, after] = current->getAnalysisAndInputs();
    CHECK(after != before);
    CHECK(afterInputs.tree == after->getSyntaxTree());
    CHECK(afterInputs.buffer == after->getBuffer());
    CHECK_FALSE(server.m_driver->isPackageQueryContext(header.m_uri));
}

TEST_CASE("Resolved source graphs reuse their analysis once and cancel before publication",
          "[package-query-context][resolved-analysis-inputs]") {
    const auto cancelAt = GENERATE("before analysis publication", "after resolved analysis reuse");
    ExplicitWorkspace workspace;
    workspace.write(".slang/server.json",
                    "{\"flags\":\"--single-unit --top top\",\"indexingThreads\":1,"
                    "\"experimentalCooperativeAnalysis\":true}\n");
    workspace.write("item.svh", "class C; int scalar; endclass\n");
    workspace.write("owner.sv", "package p;\n`include \"item.svh\"\nendpackage\n");
    workspace.write("top.sv", "module top; endmodule\n");
    workspace.write("design.f", "owner.sv\ntop.sv\n");
    ServerHarness server(workspace.params());
    server.setBuildFile(workspace.path("design.f").string());
    auto header = server.openFile("item.svh");
    auto current = server.m_driver->getDocument(header.m_uri);
    auto previous = current->getAnalysis();
    const auto query = lsp::HoverParams{.textDocument = {.uri = header.m_uri},
                                        .position = header.before("scalar").getPosition()};
    const auto expected = rfl::json::write(server.getDocHover(query));
    size_t resolutions = 0, repeated = 0;
    lsp::RequestContext count("resolved graph reuse", 270);
    count.setCheckpointObserverForTesting([&](std::string_view phase) {
        resolutions += phase == "before owner resolution";
        repeated += phase == "before cached owner resolution";
    });
    auto [inputs, reused] = current->getAnalysisAndInputs(count);
    CHECK(reused == previous);
    CHECK(inputs.buffer == reused->getBuffer());
    CHECK(inputs.tree == reused->getSyntaxTree());
    CHECK(resolutions == 1);
    CHECK(repeated == 0);
    current->invalidateAnalysis();
    bool reached = false;
    lsp::RequestContext cancel("cancel resolved graph", 271);
    cancel.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == cancelAt) {
            reached = true;
            cancel.cancel();
        }
    });
    CHECK_THROWS_AS(current->getAnalysisAndInputs(cancel), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK_FALSE(current->hasAnalysis());
    CHECK(previous->hasValidBuffers());
    auto [latestInputs, latest] = current->getAnalysisAndInputs();
    CHECK(latest != previous);
    CHECK(latestInputs.buffer == latest->getBuffer());
    CHECK(latestInputs.tree == latest->getSyntaxTree());
    CHECK(expected == rfl::json::write(server.getDocHover(query)));
}

TEST_CASE("Package queries retain full context for generic and out-of-block methods",
          "[package-query-context][semantic-cancellation]") {
    struct Scenario {
        std::string header, owner, instance;
    };
    const Scenario cases[] = {
        {"class C #(type T=byte); T field; function T read(); return field; endfunction endclass\n",
         "package p;\n`include \"item.svh\"\nendpackage\n", "p::C#(logic[15:0]) item;"},
        {"class Inner; T field; function T read(); return field; endfunction endclass\n",
         "package p; class Outer #(type T=byte);\n`include \"item.svh\"\nendclass endpackage\n",
         "p::Outer#(logic[15:0]) item;"},
        {"function int C::read(); return field; endfunction\n",
         "package p; class C #(type T=byte); T field; extern function int read(); endclass\n"
         "`include \"item.svh\"\nendpackage\n",
         "p::C#(logic[15:0]) item;"}};
    for (const auto& scenario : cases) {
        std::optional<std::string> reference;
        for (bool enabled : {false, true}) {
            ExplicitWorkspace workspace;
            workspace.write(".slang/server.json",
                            "{\"flags\":\"--single-unit --top top\",\"indexingThreads\":1,"
                            "\"experimentalCooperativeAnalysis\":" +
                                std::string(enabled ? "true" : "false") + "}\n");
            workspace.write("item.svh", scenario.header);
            workspace.write("owner.sv", scenario.owner);
            workspace.write("top.sv", "module top; " + scenario.instance + " endmodule\n");
            workspace.write("design.f", "owner.sv\ntop.sv\n");
            ServerHarness server(workspace.params());
            server.setBuildFile(workspace.path("design.f").string());
            auto header = server.openFile("item.svh");
            auto latest = scenario.header;
            latest.insert(latest.find("return"), "/* changed */ ");
            server.SlangServer::onDocDidChange(
                {.textDocument = {.version = 2, .uri = header.m_uri},
                 .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = latest}}});
            auto doc = server.m_driver->getDocument(header.m_uri);
            lsp::RequestContext ctx("textDocument/hover", 114);
            size_t rebuilds = 0;
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == "before full-model root binding")
                    ++rebuilds;
            });
            auto analysis = doc->getAnalysis(ctx);
            CHECK(rebuilds == 1);
            CHECK_FALSE(
                analysis->getCompilation()->hasFlag(slang::ast::CompilationFlags::PackageAnalysis));
            CHECK_FALSE(server.m_driver->isPackageQueryContext(header.m_uri));
            auto hover = rfl::json::write(
                server.getDocHover({.textDocument = {.uri = header.m_uri},
                                    .position = header.before("field").getPosition()}));
            CAPTURE(enabled, scenario.header, hover);
            // Match the existing full-context result, including unresolved generic positions.
            if (reference)
                CHECK(hover == *reference);
            else
                reference = hover;
            server.m_driver->runPendingDiagnostics();
        }
    }
}

TEST_CASE("Package queries retain explicit root hierarchy context through macros and helpers",
          "[package-query-context][semantic-cancellation]") {
    const auto kind = GENERATE("direct", "macro", "helper");
    std::optional<std::string> reference;
    for (bool enabled : {false, true}) {
        ExplicitWorkspace workspace;
        workspace.write(".slang/server.json",
                        "{\"flags\":\"--single-unit --top top\",\"indexingThreads\":1,"
                        "\"experimentalCooperativeAnalysis\":" +
                            std::string(enabled ? "true" : "false") + "}\n");
        const bool helper = std::string_view(kind) == "helper";
        const std::string expression = std::string_view(kind) == "macro" ? "`DUT.value"
                                                                         : "$root.top.value";
        const std::string header = "class C; int field; function int read(); return " +
                                   (helper ? std::string("Helper::read()") : expression) +
                                   "; endfunction endclass\n";
        workspace.write("item.svh", header);
        workspace.write("owner.sv",
                        "`define DUT $root.top\npackage p;\n" +
                            std::string(helper ? "class Helper; static function int read(); return "
                                                 "$root.top.value; endfunction endclass\n"
                                               : "") +
                            "`include \"item.svh\"\nendpackage\n");
        workspace.write("top.sv", "module top; int value; endmodule\n");
        workspace.write("design.f", "owner.sv\ntop.sv\n");
        ServerHarness server(workspace.params());
        server.setBuildFile(workspace.path("design.f").string());
        auto doc = server.openFile("item.svh");
        auto analysis = server.m_driver->getDocument(doc.m_uri)->getAnalysis();
        CHECK_FALSE(server.m_driver->isPackageQueryContext(doc.m_uri));
        CHECK_FALSE(
            analysis->getCompilation()->hasFlag(slang::ast::CompilationFlags::PackageAnalysis));
        const auto hover = rfl::json::write(server.getDocHover(
            {.textDocument = {.uri = doc.m_uri}, .position = doc.before("read").getPosition()}));
        if (reference)
            CHECK(hover == *reference);
        else
            reference = hover;
        server.m_driver->runPendingDiagnostics();
    }
}
