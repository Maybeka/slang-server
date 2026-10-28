// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "diagnostics/DiagnosticWorker.h"
#include "lsp/RequestContext.h"
#include "utils/ServerHarness.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>

namespace {
namespace fs = std::filesystem;

// Keep the workspace alive until its ServerHarness and document handles expire.
class ContextRecoveryWorkspace {
public:
    ContextRecoveryWorkspace() {
        static std::atomic_uint64_t serial{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (;;) {
            auto candidate = fs::temp_directory_path() /
                             ("slang-context-recovery-" + std::to_string(stamp) + "-" +
                              std::to_string(serial++));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                root = fs::canonical(candidate);
                break;
            }
            REQUIRE_FALSE(error);
        }
    }

    ~ContextRecoveryWorkspace() {
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

    void configure(std::optional<std::string> filelist = std::nullopt,
                   std::string flags = {}) const {
        auto text = "{\"indexingThreads\":1,\"flags\":" + rfl::json::write(flags);
        if (filelist)
            text += ",\"compilationFilelist\":" + rfl::json::write(*filelist);
        write(".slang/server.json", text + "}\n");
    }

    lsp::InitializeParams params() const {
        return {.workspaceFolders = {{lsp::WorkspaceFolder{.uri = URI::fromFile(root),
                                                           .name = "context-recovery"}}}};
    }

    fs::path root;
};

bool isContextUnavailable(const lsp::Diagnostic& diagnostic) {
    return diagnostic.code && rfl::holds_alternative<std::string>(*diagnostic.code) &&
           rfl::get<std::string>(*diagnostic.code) == "context-unavailable";
}

void checkUnavailable(ServerHarness& server, DocumentHandle& document) {
    CHECK(server.m_driver->hasExplicitContextError(document.m_uri));
    CHECK(std::ranges::any_of(document.getDiagnostics(), isContextUnavailable));
    const auto position = document.before("value").getPosition();
    CHECK_THROWS_AS(server.getDocHover(
                        {.textDocument = {.uri = document.m_uri}, .position = position}),
                    lsp::RequestFailed);
}

bool watches(const server::ServerDriver& driver, const fs::path& path) {
    const auto paths = driver.getContextWatchPaths();
    return std::ranges::find(paths, path) != paths.end();
}

void checkDefinition(DocumentHandle& document, std::string_view token, const URI& expected) {
    const auto definitions = document.before(std::string(token)).getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == expected);
}

void notifyCreatedOrChanged(ServerHarness& server, const URI& uri, lsp::FileChangeType type,
                            DocumentHandle& document) {
    server.onWorkspaceDidChangeWatchedFiles(
        {.changes = {{lsp::FileEvent{.uri = uri, .type = type}}}});
    // Retrying a negative SourceManager lookup can recreate the whole driver.
    document.doc = server.getDoc(document.m_uri);
    REQUIRE(document.doc);
}

void changeWithoutDiagnostics(ServerHarness& server, DocumentHandle& document, int version,
                              std::string text) {
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = version, .uri = document.m_uri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
    document.m_text = std::move(text);
}

} // namespace

TEST_CASE("Explicit compilationFilelist without roots remains authoritative while flags-only files "
          "allow discovery",
          "[explicit-context][context-recovery]") {
    ContextRecoveryWorkspace workspace;
    const std::string text = "class item; value_t value; endclass\n";
    workspace.write("item.svh", text);
    workspace.write("owner.sv", "package p; typedef `FIELD_TYPE value_t;\n"
                                "`include \"item.svh\"\nendpackage\n");
    bool authoritative = true;
    SECTION("an explicitly empty compilation filelist") {
        workspace.write("inputs.commands", "// deliberately no source roots\n");
        workspace.configure("inputs.commands", "-DFIELD_TYPE=shortint");
    }
    SECTION("an explicit filelist that only defines flags") {
        workspace.write("inputs.commands", "+define+FIELD_TYPE=shortint\n");
        workspace.configure("inputs.commands");
    }
    SECTION("an ordinary flags-only command file") {
        authoritative = false;
        workspace.write("inputs.commands", "+define+FIELD_TYPE=shortint\n");
        workspace.configure(std::nullopt,
                            "-F \"" + workspace.path("inputs.commands").string() + "\"");
    }

    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    CHECK(server.m_driver->hasConfiguredCompilationInputs() == authoritative);
    CHECK_FALSE(server.m_driver->configuredInputLoadFailed());
    if (authoritative) {
        checkUnavailable(server, header);
    }
    else {
        CHECK_FALSE(server.m_driver->hasExplicitContextError(header.m_uri));
        CHECK(header.getDiagnostics().empty());
        checkDefinition(header, "value_t value", workspace.uri("owner.sv"));
    }
}

TEST_CASE("Invalid native options remain context failures after source edits",
          "[explicit-context][context-recovery]") {
    ContextRecoveryWorkspace workspace;
    const std::string text = "class item; value_t value; endclass\n";
    workspace.write("item.svh", text);
    workspace.write("owner.sv", "package p; typedef int value_t;\n"
                                "`include \"item.svh\"\nendpackage\n");
    int nativeOptionErrors = 1;
    SECTION("invalid configuration flags") {
        workspace.write("inputs.commands", "owner.sv\n");
        workspace.configure("inputs.commands", "--slang-server-unknown-recovery-option");
        // The Driver reports its retained command-line error again when it
        // subsequently parses the compilation filelist.
        nativeOptionErrors = 2;
    }
    SECTION("invalid option inside the native manifest") {
        workspace.write("inputs.commands", "--slang-server-unknown-recovery-option\nowner.sv\n");
        workspace.configure("inputs.commands");
    }

    ServerHarness server(workspace.params());
    for (int index = 0; index < nativeOptionErrors; ++index)
        server.expectError(
            "unknown command line argument '--slang-server-unknown-recovery-option'");
    server.expectError("Failed to parse configuration or compilation filelist");
    auto header = server.openFile("item.svh", text);
    REQUIRE(server.m_driver->hasConfiguredCompilationInputs());
    CHECK(server.m_driver->configuredInputLoadFailed());
    checkUnavailable(server, header);

    header.replaceAll(text + "// unsaved edit must not clear option failure\n");
    header.publishChanges();
    CHECK(server.m_driver->configuredInputLoadFailed());
    checkUnavailable(server, header);
}

TEST_CASE("A missing nested native manifest stays watched and recovers on creation",
          "[explicit-context][context-recovery]") {
    ContextRecoveryWorkspace workspace;
    const std::string text = "class item; value_t value; endclass\n";
    workspace.write("item.svh", text);
    workspace.write("owner.sv", "package p; typedef int value_t;\n"
                                "`include \"item.svh\"\nendpackage\n");
    workspace.write("inputs.commands", "owner.sv\n-F generated/late.manifest\n");
    workspace.configure("inputs.commands");
    auto event = lsp::FileChangeType::Created;
    SECTION("Created event") {
    }
    SECTION("coalesced Changed event") {
        event = lsp::FileChangeType::Changed;
    }

    ServerHarness server(workspace.params());
    server.expectError("command file 'generated/late.manifest': " +
                       std::make_error_code(std::errc::no_such_file_or_directory).message());
    server.expectError("Failed to parse configuration or compilation filelist");
    auto header = server.openFile("item.svh", text);
    CHECK(server.m_driver->configuredInputLoadFailed());
    checkUnavailable(server, header);
    CHECK(watches(*server.m_driver, workspace.path("generated/late.manifest")));

    header.replaceAll(text + "// retained unsaved header\n");
    header.publishChanges();
    CHECK(server.m_driver->configuredInputLoadFailed());
    checkUnavailable(server, header);
    workspace.write("generated/late.manifest", "// repaired nested manifest\n");
    notifyCreatedOrChanged(server, workspace.uri("generated/late.manifest"), event, header);

    CHECK_FALSE(server.m_driver->configuredInputLoadFailed());
    CHECK_FALSE(server.m_driver->hasExplicitContextError(header.m_uri));
    CHECK(header.doc->getText().find("retained unsaved header") != std::string_view::npos);
    CHECK(header.getDiagnostics().empty());
    checkDefinition(header, "value_t value", workspace.uri("owner.sv"));
}

TEST_CASE("Missing owner targets are watched and creation preserves the editor overlay",
          "[explicit-context][context-recovery][source-context-hints]") {
    ContextRecoveryWorkspace workspace;
    const std::string text = "// slang-server: owner \"../generated/owner.sv\"\n"
                             "class item; value_t value; endclass\n";
    workspace.write("headers/item.svh", text);
    workspace.configure();
    auto event = lsp::FileChangeType::Created;
    SECTION("Created event") {
    }
    SECTION("coalesced Changed event") {
        event = lsp::FileChangeType::Changed;
    }

    ServerHarness server(workspace.params());
    auto header = server.openFile("headers/item.svh", text);
    checkUnavailable(server, header);
    CHECK(watches(*server.m_driver, workspace.path("generated/owner.sv")));
    header.replaceAll(text + "// unsaved owner consumer\n");
    header.publishChanges();
    CHECK(watches(*server.m_driver, workspace.path("generated/owner.sv")));

    workspace.write("generated/owner.sv", "package p; typedef shortint value_t;\n"
                                          "`include \"../headers/item.svh\"\nendpackage\n");
    notifyCreatedOrChanged(server, workspace.uri("generated/owner.sv"), event, header);
    CHECK_FALSE(server.m_driver->hasExplicitContextError(header.m_uri));
    CHECK(header.doc->getText().find("unsaved owner consumer") != std::string_view::npos);
    CHECK(header.getDiagnostics().empty());
    checkDefinition(header, "value_t value", workspace.uri("generated/owner.sv"));
}

TEST_CASE("Missing import sources remain watched despite an indexed same-name fallback",
          "[explicit-context][context-recovery][source-context-hints]") {
    ContextRecoveryWorkspace workspace;
    const std::string text = "package consumer;\nimport required_pkg::*; "
                             "// slang-server: source \"generated/provider.sv\"\n"
                             "required_t value; endpackage\n";
    workspace.write("consumer.sv", text);
    workspace.write("a_fallback.sv", "package required_pkg; typedef byte required_t; endpackage\n");
    workspace.configure();
    auto event = lsp::FileChangeType::Created;
    SECTION("Created event") {
    }
    SECTION("coalesced Changed event") {
        event = lsp::FileChangeType::Changed;
    }

    ServerHarness server(workspace.params());
    auto consumer = server.openFile("consumer.sv", text);
    checkUnavailable(server, consumer);
    CHECK(watches(*server.m_driver, workspace.path("generated/provider.sv")));
    consumer.replaceAll(text + "// retained unsaved import consumer\n");
    consumer.publishChanges();
    CHECK(watches(*server.m_driver, workspace.path("generated/provider.sv")));

    workspace.write("generated/provider.sv",
                    "package required_pkg; typedef shortint required_t; endpackage\n");
    notifyCreatedOrChanged(server, workspace.uri("generated/provider.sv"), event, consumer);
    CHECK_FALSE(server.m_driver->hasExplicitContextError(consumer.m_uri));
    CHECK(consumer.doc->getText().find("retained unsaved import consumer") !=
          std::string_view::npos);
    CHECK(consumer.getDiagnostics().empty());
    checkDefinition(consumer, "required_t value", workspace.uri("generated/provider.sv"));
}

TEST_CASE("Removing a failed source hint restores discovery and drops its missing watch target",
          "[explicit-context][context-recovery][source-context-hints]") {
    ContextRecoveryWorkspace workspace;
    std::string name;
    std::string hinted;
    std::string ordinary;
    std::string token;
    std::string provider;
    SECTION("owner hint") {
        name = "item.svh";
        ordinary = "class item; value_t value; endclass\n";
        hinted = "// slang-server: owner \"missing/unused.sv\"\n" + ordinary;
        token = "value_t value";
        provider = "owner.sv";
        workspace.write(provider, "package p; typedef int value_t;\n"
                                  "`include \"item.svh\"\nendpackage\n");
    }
    SECTION("import source hint") {
        name = "consumer.sv";
        ordinary = "package consumer;\nimport required_pkg::*;\n"
                   "required_t value; endpackage\n";
        hinted = "package consumer;\nimport required_pkg::*; "
                 "// slang-server: source \"missing/unused.sv\"\n"
                 "required_t value; endpackage\n";
        token = "required_t value";
        provider = "provider.sv";
        workspace.write(provider, "package required_pkg; typedef int required_t; endpackage\n");
    }
    workspace.write(name, hinted);
    workspace.configure();
    ServerHarness server(workspace.params());
    auto document = server.openFile(name, hinted);
    checkUnavailable(server, document);
    CHECK(watches(*server.m_driver, workspace.path("missing/unused.sv")));

    document.replaceAll(ordinary);
    document.publishChanges();
    CHECK_FALSE(server.m_driver->hasExplicitContextError(document.m_uri));
    CHECK(document.getDiagnostics().empty());
    CHECK_FALSE(watches(*server.m_driver, workspace.path("missing/unused.sv")));
    checkDefinition(document, token, workspace.uri(provider));
}

TEST_CASE("Supported compilationFilelist units remain eligible for private package diagnostic jobs",
          "[explicit-context][context-recovery][diagnostic-snapshot]") {
    ContextRecoveryWorkspace workspace;
    const std::string text = "class item; value_t value; endclass\n";
    workspace.write("preamble.sv", "`define FIELD_TYPE shortint\n");
    workspace.write("owner.sv", "package p; typedef `FIELD_TYPE value_t;\n"
                                "`include \"item.svh\"\nendpackage\n");
    workspace.write("item.svh", text);
    workspace.write("recursive.sv", "module recursive; recursive next(); endmodule\n");
    workspace.write("lists/native.manifest", "../preamble.sv\n../owner.sv\n../recursive.sv\n");
    workspace.configure("lists/native.manifest", "--single-unit");
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    REQUIRE(server.m_driver->hasConfiguredCompilationInputs());
    REQUIRE(server.m_driver->comp == nullptr);
    CHECK(server.m_driver->isPackageOnlyContext(header.m_uri));
    CHECK(header.getDiagnostics().empty());
    CHECK(header.doc->getAnalysis()->getCompilation()->getRoot().topInstances.empty());

    changeWithoutDiagnostics(server, header, 2, "class item; recovery_missing_t value; endclass\n");
    auto snapshot = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(snapshot);
    REQUIRE(snapshot->metadata.targets.size() == 1);
    CHECK(snapshot->metadata.targets.front().packageOnly);
    std::set<fs::path> captured;
    for (auto buffer : snapshot->buffers)
        captured.insert(server.m_driver->sm.getFullPath(buffer));
    CHECK(captured.contains(workspace.path("preamble.sv")));
    CHECK(captured.contains(workspace.path("owner.sv")));
    CHECK(captured.contains(workspace.path("recursive.sv")));
    CHECK(captured.contains(workspace.path("item.svh")));

    auto output = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(snapshot->metadata, *snapshot->input));
    auto document = std::find_if(output.documents.begin(), output.documents.end(),
                                 [&](const auto& value) { return value.uri == header.m_uri; });
    REQUIRE(document != output.documents.end());
    CHECK(std::ranges::any_of(document->diagnostics, [](const auto& diagnostic) {
        return diagnostic.message.find("recovery_missing_t") != std::string::npos;
    }));
    CHECK(std::ranges::none_of(document->diagnostics, isContextUnavailable));
    server.m_driver->commitDiagnosticSnapshot(*snapshot, std::move(output), {});
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());

    changeWithoutDiagnostics(server, header, 3, text);
    auto repair = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(repair);
    auto clean = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(repair->metadata, *repair->input));
    server.m_driver->commitDiagnosticSnapshot(*repair, std::move(clean), {});
    CHECK(header.getDiagnostics().empty());
    CHECK(server.client.diagnosticVersions.at(header.m_uri) == 3);
}

TEST_CASE("Ordinary flag errors do not opt an unconfigured document into explicit context",
          "[explicit-context][context-recovery]") {
    ContextRecoveryWorkspace workspace;
    const std::string text = "class item; int value; endclass\n";
    workspace.write("item.svh", text);
    workspace.configure(std::nullopt, "--slang-server-unknown-recovery-option");
    ServerHarness server(workspace.params());
    server.expectError("unknown command line argument '--slang-server-unknown-recovery-option'");
    server.expectError("Failed to parse config flags");
    auto header = server.openFile("item.svh", text);
    CHECK_FALSE(server.m_driver->hasConfiguredCompilationInputs());
    CHECK_FALSE(server.m_driver->hasExplicitContextError(header.m_uri));
    CHECK(header.doc->getAnalysis());
}

TEST_CASE("Watch paths union shared included views and discover later includes",
          "[explicit-context][context-recovery][context-watch-paths]") {
    ContextRecoveryWorkspace workspace;
    const std::string first = "class First; int value; endclass\n";
    const std::string second = "class Second; int value; endclass\n";
    workspace.write("first.svh", first);
    workspace.write("second.svh", second);
    workspace.write("later.svh", "class Later; int value; endclass\n");
    workspace.write("owner.sv", "package p;\n`include \"first.svh\"\n"
                                "`include \"second.svh\"\nendpackage\n");
    workspace.write("inputs.commands", "+incdir+.\nowner.sv\n");
    workspace.configure("inputs.commands", "--single-unit --threads 1");
    ServerHarness server(workspace.params());
    auto a = server.openFile("first.svh", first);
    auto b = server.openFile("second.svh", second);
    a.doc->getAnalysis();
    b.doc->getAnalysis();
    auto before = server.m_driver->getContextWatchPaths();
    for (const auto* name : {"inputs.commands", "owner.sv", "first.svh", "second.svh"})
        CHECK(std::ranges::find(before, workspace.path(name)) != before.end());
    CHECK(std::ranges::find(before, workspace.path("later.svh")) == before.end());
    CHECK(std::ranges::is_sorted(before));
    CHECK(std::adjacent_find(before.begin(), before.end()) == before.end());
    changeWithoutDiagnostics(server, a, 2, first + "// ordinary body-independent text\n");
    a.doc->getAnalysis();
    CHECK(server.m_driver->getContextWatchPaths() == before);
    changeWithoutDiagnostics(server, a, 3, first + "`include \"later.svh\"\n");
    a.doc->getAnalysis();
    const auto after = server.m_driver->getContextWatchPaths();
    CHECK(std::ranges::find(after, workspace.path("later.svh")) != after.end());
    for (const auto& path : before)
        CHECK(std::ranges::find(after, path) != after.end());
    CHECK(std::ranges::is_sorted(after));
    CHECK(std::adjacent_find(after.begin(), after.end()) == after.end());
}
