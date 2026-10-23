// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "diagnostics/DiagnosticWorker.h"
#include "utils/ServerHarness.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "slang/util/ScopeGuard.h"

namespace {
class CancellationHarness : public ServerHarness {
public:
    using ServerHarness::ServerHarness;
    using Observer = std::function<void(std::string_view)>;

    lsp::RequestContext track(const lsp::RpcRequest& request, Observer observer = {}) {
        auto ctx = createContext(request);
        if (observer)
            ctx.setCheckpointObserverForTesting(std::move(observer));
        registerContext(request, ctx);
        return ctx;
    }

    std::string dispatch(const lsp::RpcRequest& request, lsp::RequestContext ctx) {
        std::ostringstream output;
        auto* original = std::cout.rdbuf(output.rdbuf());
        slang::ScopeGuard restore([&] { std::cout.rdbuf(original); });
        handleMessage(request, std::move(ctx));
        return output.str();
    }

    std::string dispatch(const lsp::RpcRequest& request) {
        return dispatch(request, track(request));
    }

    void cancel(const lsp::ID_t& id) {
        // Exercise the registered $/cancelRequest notification, as the reader does.
        processMessage({.jsonrpc = "2.0",
                        .method = "$/cancelRequest",
                        .params = rfl::to_generic(lsp::CancelParams{.id = id})});
    }
};

const std::string initialText =
    "module foreground;\n  int old_value;\n  initial old_value = 1;\nendmodule\n";
const std::string editedText =
    "module foreground;\n  int newest_value;\n  initial newest_value = 1;\nendmodule\n";
const lsp::Position usePosition{2, 12};

void change(CancellationHarness& server, const URI& uri, int version, std::string text) {
    // Deliberately leave eventual diagnostics pending, as the protocol handler does.
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = version, .uri = uri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = std::move(text)}}});
}

lsp::RpcRequest query(const URI& uri, std::string method, lsp::ID_t id = 17) {
    return {.jsonrpc = "2.0",
            .id = std::move(id),
            .method = std::move(method),
            .params = rfl::to_generic(lsp::TextDocumentPositionParams{.textDocument = {.uri = uri},
                                                                      .position = usePosition})};
}

size_t occurrences(std::string_view text, std::string_view needle) {
    size_t count = 0;
    for (size_t offset = 0; (offset = text.find(needle, offset)) != std::string_view::npos;
         offset += needle.size())
        ++count;
    return count;
}

void checkCancelledWire(const std::string& wire) {
    CHECK(occurrences(wire, "Content-Length:") == 1);
    CHECK(occurrences(wire, "\"code\":-32800") == 1);
    CHECK(wire.find("\"result\"") == std::string::npos);
}

void checkCurrent(CancellationHarness& server, DocumentHandle& document) {
    auto completion = server.dispatch(query(document.m_uri, "textDocument/completion", "latest"));
    CHECK(occurrences(completion, "Content-Length:") == 1);
    CHECK(completion.find("\"error\"") == std::string::npos);
    CHECK(completion.find("newest_value") != std::string::npos);
    CHECK(completion.find("old_value") == std::string::npos);
    auto definition = server.m_driver->getDefinitionInfoAt(document.m_uri, usePosition);
    REQUIRE(definition);
    auto locations = definition->getDefinitionLspLocs();
    REQUIRE(locations.size() == 1);
    CHECK(locations.front().uri == document.m_uri);
    CHECK(locations.front().range.start == lsp::Position{1, 6});
}
} // namespace

TEST_CASE("Foreground cancellation stops fresh analysis at safe phase boundaries",
          "[foreground-cancellation]") {
    const std::vector<std::string> boundaries = {
        "after owner resolution",     "after dependency resolution",
        "after owner recheck",        "before analysis construction",
        "after syntax indexing",      "after compilation construction",
        "after adding syntax tree",   "after root binding",
        "before symbol indexing",     "after symbol indexing",
        "before analysis publication"};
    for (const auto& method :
         {"textDocument/completion", "textDocument/definition", "textDocument/hover"}) {
        for (const auto& boundary : boundaries) {
            DYNAMIC_SECTION(method << " cancelled " << boundary) {
                CancellationHarness server;
                auto document = server.openFile("foreground_cancel.sv", initialText);
                change(server, document.m_uri, 2, editedText);
                REQUIRE_FALSE(document.doc->hasAnalysis());
                std::vector<std::string> phases;
                auto request = query(document.m_uri, method);
                auto ctx = server.track(request, [&](std::string_view phase) {
                    phases.emplace_back(phase);
                    if (phase == boundary)
                        server.cancel(*request.id);
                });
                REQUIRE(ctx.supportsCancellation());
                checkCancelledWire(server.dispatch(request, ctx));
                REQUIRE_FALSE(phases.empty());
                CHECK(phases.back() == boundary);
                CHECK(std::count(phases.begin(), phases.end(), boundary) == 1);
                CHECK_FALSE(document.doc->hasAnalysis());
                CHECK_FALSE(document.doc->getCachedAnalysisTree());
                const auto visited = [&](std::string_view phase) {
                    return std::count(phases.begin(), phases.end(), phase);
                };
                // These are phase-entry counters, not elapsed-time assertions.
                if (boundary == "after owner resolution")
                    CHECK(visited("before dependency resolution") == 0);
                if (boundary == "after dependency resolution")
                    CHECK(visited("before analysis construction") == 0);
                if (boundary == "after syntax indexing") {
                    // Main owns the shared compilation before per-file indexing.
                    CHECK(visited("before root binding") == 0);
                    CHECK(visited("before symbol indexing") == 0);
                }
                if (boundary == "after compilation construction")
                    CHECK(visited("before adding syntax tree") == 0);
                if (boundary == "after adding syntax tree")
                    CHECK(visited("before root binding") == 0);
                if (boundary == "after root binding")
                    CHECK(visited("before symbol indexing") == 0);
                if (boundary == "after symbol indexing")
                    CHECK(visited("before analysis publication") == 0);
                checkCurrent(server, document);
                CHECK(document.doc->getVersion() == 2);
            }
        }
    }
}

TEST_CASE("Foreground parse cancellation retains only a complete reusable syntax tree",
          "[foreground-cancellation]") {
    CancellationHarness server;
    auto document = server.openFile("foreground_parse.sv", initialText);
    change(server, document.m_uri, 2, editedText);
    lsp::RequestContext ctx("textDocument/completion", 18);
    std::vector<std::string> phases;
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        phases.emplace_back(phase);
        if (phase == "after syntax parsing")
            ctx.cancel();
    });
    REQUIRE_THROWS_AS(document.doc->getSyntaxTree(ctx), lsp::RequestCancelled);
    REQUIRE_FALSE(phases.empty());
    CHECK(phases.front() == "before syntax parsing");
    CHECK(phases.back() == "after syntax parsing");
    CHECK(std::ranges::count(phases, "after syntax parsing") == 1);
    CHECK(std::ranges::find(phases, "before compilation construction") == phases.end());
    auto parsed = document.doc->getCachedSyntaxTree();
    REQUIRE(parsed);
    CHECK_FALSE(document.doc->hasAnalysis());
    CHECK(document.doc->getSyntaxTree() == parsed);
    checkCurrent(server, document);
}

TEST_CASE("Queued explicit cancellation skips analysis and preserves uncancelled FIFO queries",
          "[foreground-cancellation]") {
    CancellationHarness server;
    auto document = server.openFile("foreground_fifo.sv", initialText);
    auto cancelled = query(document.m_uri, "textDocument/completion", 19);
    auto first = query(document.m_uri, "textDocument/hover", 20);
    auto last = query(document.m_uri, "textDocument/hover", 21);
    unsigned entries = 0;
    auto cancelledContext = server.track(cancelled, [&](std::string_view) { ++entries; });
    auto firstContext = server.track(first);
    auto lastContext = server.track(last);
    server.cancel(*cancelled.id);
    checkCancelledWire(server.dispatch(cancelled, cancelledContext));
    CHECK(entries == 0); // This queued skip already existed before this optimization.
    auto oldResponse = server.dispatch(first, firstContext);
    CHECK(oldResponse.find("old_value") != std::string::npos);
    CHECK(oldResponse.find("\"error\"") == std::string::npos);
    change(server, document.m_uri, 2, editedText);
    auto newestResponse = server.dispatch(last, lastContext);
    CHECK(newestResponse.find("newest_value") != std::string::npos);
    CHECK(newestResponse.find("old_value") == std::string::npos);
    CHECK(newestResponse.find("\"error\"") == std::string::npos);
    CHECK(occurrences(newestResponse, "Content-Length:") == 1);
    checkCurrent(server, document);
}

TEST_CASE("Cancelled foreground preserves every incremental edit and restored diagnostics",
          "[foreground-cancellation]") {
    CancellationHarness server;
    auto header = server.openFile(
        "foreground_item.svh",
        "class item; int old_value; function int get(); return old_value; endfunction endclass\n");
    auto owner = server.openFile("foreground_owner.sv",
                                 "package p;\n`include \"foreground_item.svh\"\nendpackage\n");
    const std::string broken = "class item; missing_type renamed; function int get(); return "
                               "renamed; endfunction endclass\n";
    change(server, header.m_uri, 2, broken);
    auto request = query(header.m_uri, "textDocument/completion", "obsolete-header");
    auto ctx = server.track(request, [&](std::string_view phase) {
        if (phase == "after root binding")
            server.cancel(*request.id);
    });
    checkCancelledWire(server.dispatch(request, ctx));
    REQUIRE(server.m_driver->hasPendingDiagnostics());
    // Two sequential ranges refer to the state produced by the previous delta.
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = 3, .uri = header.m_uri},
         .contentChanges = {
             lsp::TextDocumentContentChangePartial{.range = {{0, 12}, {0, 24}}, .text = "int"},
             lsp::TextDocumentContentChangePartial{.range = {{0, 0}, {0, 0}}, .text = "\n\n"}}});
    const auto expected =
        "\n\nclass item; int renamed; function int get(); return renamed; endfunction endclass\n";
    auto actual = header.doc->getText();
    REQUIRE_FALSE(actual.empty());
    REQUIRE(actual.back() == '\0'); // SourceManager keeps one terminal sentinel.
    actual.remove_suffix(1);
    CHECK(actual == expected);
    CHECK(header.doc->getVersion() == 3);
    auto definition = server.m_driver->getDefinitionInfoAt(header.m_uri, {2, 55});
    REQUIRE(definition);
    auto locations = definition->getDefinitionLspLocs();
    REQUIRE(locations.size() == 1);
    CHECK(locations.front().uri == header.m_uri);
    CHECK(locations.front().range.start == lsp::Position{2, 16});
    // Use the unchanged private snapshot worker/commit path to prove the final generation
    // still completes after foreground cancellation, including empty repair publication.
    auto snapshot = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(snapshot);
    auto output = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(snapshot->metadata, *snapshot->input));
    server.m_driver->commitDiagnosticSnapshot(*snapshot, std::move(output), {});
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
    CHECK(server.client.getDiagnostics(header.m_uri).empty());
    CHECK(server.client.diagnosticVersions.at(header.m_uri) == 3);
}

TEST_CASE("Cancelled tree addition skips the remaining dependency trees",
          "[foreground-cancellation]") {
    CancellationHarness server("included_package");
    const std::string initial = "module foreground; required_pkg::base_t value; endmodule\n";
    auto document = server.openFile("foreground_dependency.sv", initial);
    change(server, document.m_uri, 2, initial + "\n");
    auto request = query(document.m_uri, "textDocument/completion", "dependency-cancel");
    unsigned additions = 0;
    unsigned rootBindings = 0;
    auto ctx = server.track(request, [&](std::string_view phase) {
        if (phase == "before adding syntax tree")
            ++additions;
        if (phase == "before root binding")
            ++rootBindings;
        if (phase == "after adding syntax tree")
            server.cancel(*request.id);
    });
    checkCancelledWire(server.dispatch(request, ctx));
    CHECK(additions == 1);
    CHECK(rootBindings == 0);
    REQUIRE_FALSE(document.doc->hasAnalysis());
    auto analysis = document.doc->getAnalysis();
    CHECK(analysis->getCompilation()->getSyntaxTrees().size() == 2);
    CHECK(analysis->getCompilation()->getSemanticDiagnostics().empty());
}

namespace {
// As in IndexedOwnerTests, each fixture owns a small private workspace and outlives its server.
class CancellationWorkspace {
public:
    CancellationWorkspace() {
        static std::atomic_uint64_t serial{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (;;) {
            auto candidate = std::filesystem::temp_directory_path() /
                             ("slang-cancel-lifecycle-" + std::to_string(stamp) + "-" +
                              std::to_string(serial++));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root = std::filesystem::canonical(candidate);
                break;
            }
            REQUIRE_FALSE(error);
        }
    }
    ~CancellationWorkspace() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    auto path(std::string_view relative) const { return root / relative; }
    URI uri(std::string_view relative) const { return URI::fromFile(path(relative)); }
    void write(std::string_view relative, std::string_view text) const {
        std::filesystem::create_directories(path(relative).parent_path());
        std::ofstream file(path(relative), std::ios::binary | std::ios::trunc);
        REQUIRE(file.good());
        file << text;
        file.close();
        REQUIRE(file.good());
    }
    lsp::InitializeParams params() const {
        return {.workspaceFolders = {{lsp::WorkspaceFolder{.uri = URI::fromFile(root),
                                                           .name = "cancellation-lifecycle"}}}};
    }
    std::filesystem::path root;
};

lsp::Position positionOf(std::string_view text, std::string_view needle) {
    const auto offset = text.find(needle);
    REQUIRE(offset != std::string_view::npos);
    lsp::Position result{};
    for (size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') {
            ++result.line;
            result.character = 0;
        }
        else {
            ++result.character;
        }
    }
    return result;
}

void cancelAt(CancellationHarness& server, const URI& uri, std::string_view boundary) {
    auto request = query(uri, "textDocument/completion", "lifecycle-cancel");
    std::vector<std::string> phases;
    auto ctx = server.track(request, [&](std::string_view phase) {
        phases.emplace_back(phase);
        if (phase == boundary)
            server.cancel(*request.id);
    });
    checkCancelledWire(server.dispatch(request, ctx));
    REQUIRE_FALSE(phases.empty());
    CHECK(phases.back() == boundary);
    CHECK(std::count(phases.begin(), phases.end(), "before analysis construction") == 0);
}

void checkDefinition(CancellationHarness& server, const URI& source, const lsp::Position& position,
                     const URI& target, const lsp::Position& targetPosition) {
    auto definition = server.m_driver->getDefinitionInfoAt(source, position);
    REQUIRE(definition);
    auto locations = definition->getDefinitionLspLocs();
    REQUIRE(locations.size() == 1);
    CHECK(locations.front().uri == target);
    CHECK(locations.front().range.start == targetPosition);
}

void replayAndCommitCurrent(CancellationHarness& server, const URI& target, int version) {
    REQUIRE(server.m_driver->hasPendingDiagnostics());
    auto snapshot = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(snapshot);
    for (auto buffer : snapshot->buffers)
        CHECK(server.sourceManager().isLatestData(buffer));
    auto output = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(snapshot->metadata, *snapshot->input));
    server.m_driver->commitDiagnosticSnapshot(*snapshot, std::move(output), {});
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
    CHECK(server.client.getDiagnostics(target).empty());
    CHECK(server.client.diagnosticVersions.at(target) == version);
}
} // namespace

TEST_CASE("Cancelled owner discovery survives owner replacement and restoration",
          "[foreground-cancellation][lifecycle]") {
    CancellationWorkspace workspace;
    const std::string headerText = "class item; value_t value; endclass\n";
    const std::string firstOwner =
        "package first_pkg; typedef int value_t;\n`include \"item.svh\"\nendpackage\n";
    const std::string secondOwner =
        "package second_pkg; typedef byte value_t;\n`include \"item.svh\"\nendpackage\n";
    workspace.write("item.svh", headerText);
    workspace.write("first.sv", firstOwner);
    CancellationHarness server(workspace.params());
    auto header = server.openFile("item.svh", headerText);
    const std::string edited = "\n" + headerText;
    change(server, header.m_uri, 2, edited);
    cancelAt(server, header.m_uri, "after owner resolution");
    CHECK_FALSE(header.doc->hasAnalysis());
    auto firstTree = server.m_driver->getDocument(workspace.uri("first.sv"))->getCachedSyntaxTree();
    REQUIRE(firstTree); // The cancelled discovery retained its complete parsed owner.

    workspace.write("first.sv", "package first_pkg; endpackage\n");
    workspace.write("second.sv", secondOwner);
    server.onWorkspaceDidChangeWatchedFiles(
        {.changes = {{.uri = workspace.uri("first.sv"), .type = lsp::FileChangeType::Changed},
                     {.uri = workspace.uri("second.sv"), .type = lsp::FileChangeType::Created}}});
    CHECK_FALSE(server.sourceManager().isLatestData(firstTree->getSourceBufferIds().front()));
    checkDefinition(server, header.m_uri, positionOf(edited, "value_t"), workspace.uri("second.sv"),
                    positionOf(secondOwner, "value_t"));
    CHECK(server.sourceManager().getFullPath(
              header.doc->getAnalysis()->getSyntaxTree()->getSourceBufferIds().front()) ==
          workspace.path("second.sv"));
    replayAndCommitCurrent(server, header.m_uri, 2);

    change(server, header.m_uri, 3, edited + "\n");
    cancelAt(server, header.m_uri, "after owner resolution");
    workspace.write("first.sv", firstOwner);
    workspace.write("second.sv", "package second_pkg; endpackage\n");
    server.onWorkspaceDidChangeWatchedFiles(
        {.changes = {{.uri = workspace.uri("first.sv"), .type = lsp::FileChangeType::Changed},
                     {.uri = workspace.uri("second.sv"), .type = lsp::FileChangeType::Changed}}});
    checkDefinition(server, header.m_uri, positionOf(edited, "value_t"), workspace.uri("first.sv"),
                    positionOf(firstOwner, "value_t"));
    CHECK(server.sourceManager().getFullPath(
              header.doc->getAnalysis()->getSyntaxTree()->getSourceBufferIds().front()) ==
          workspace.path("first.sv"));
    replayAndCommitCurrent(server, header.m_uri, 3);
}

TEST_CASE("Cancelled dependency discovery replaces virtual interface cache entries",
          "[foreground-cancellation][lifecycle]") {
    CancellationWorkspace workspace;
    const std::string first = "interface first; int value; endinterface\n";
    const std::string second = "interface second; byte value; endinterface\n";
    workspace.write("first.sv", first);
    workspace.write("second.sv", second);
    workspace.write("owner.sv", "package p;\n`include \"item.svh\"\nendpackage\n");
    const std::string original = "class item; virtual first vif; endclass\n";
    workspace.write("item.svh", original);
    CancellationHarness server(workspace.params());
    auto header = server.openFile("item.svh", original);
    int version = 1;
    for (const auto& name : {"second", "first"}) {
        auto currentText = header.doc->getText();
        REQUIRE_FALSE(currentText.empty());
        REQUIRE(currentText.back() == '\0');
        currentText.remove_suffix(1);
        change(server, header.m_uri, ++version, std::string(currentText) + "\n");
        cancelAt(server, header.m_uri, "after dependency resolution");
        REQUIRE_FALSE(header.doc->hasAnalysis());
        const std::string current = "\nclass item; virtual " + std::string(name) +
                                    " vif; endclass\n";
        change(server, header.m_uri, ++version, current);
        const auto provider = std::string(name) + ".sv";
        const auto& providerText = std::string_view(name) == "first" ? first : second;
        checkDefinition(server, header.m_uri, positionOf(current, name), workspace.uri(provider),
                        positionOf(providerText, name));
        auto analysis = header.doc->getAnalysis();
        std::vector<std::filesystem::path> treePaths;
        for (const auto& tree : analysis->getCompilation()->getSyntaxTrees())
            treePaths.push_back(
                server.sourceManager().getFullPath(tree->getSourceBufferIds().front()));
        REQUIRE(treePaths.size() == 2);
        CHECK(treePaths.front() == workspace.path("owner.sv"));
        CHECK(treePaths.back() == workspace.path(provider));
        replayAndCommitCurrent(server, header.m_uri, version);
    }
}

TEST_CASE("Cancelled preinclude parsing preserves overlay aliases and driver recreation",
          "[foreground-cancellation][lifecycle]") {
    CancellationWorkspace workspace;
    workspace.write(".slang/server.json", R"({"explorePreincludeFiles":["common.svh"]})");
    workspace.write("common.svh", "`include \"nested.svh\"\n");
    workspace.write("owner.sv", "package p;\n`include \"item.svh\"\nendpackage\n");
    const std::string text = "class item; `FIELD_TYPE value; endclass\n";
    workspace.write("item.svh", text);
    CancellationHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    REQUIRE_FALSE(server.client.getDiagnostics(header.m_uri).empty());
    const std::string edited = "\n" + text;
    change(server, header.m_uri, 2, edited);
    cancelAt(server, header.m_uri, "after owner resolution");
    auto nested = server.openFile("nested.svh", "`define FIELD_TYPE byte\n");
    REQUIRE_FALSE(std::filesystem::exists(workspace.path("nested.svh")));
    auto hover = server.getDocHover(
        {.textDocument = {.uri = header.m_uri}, .position = positionOf(edited, "value")});
    REQUIRE(hover);
    CHECK(rfl::json::write(*hover).find("byte") != std::string::npos);
    replayAndCommitCurrent(server, header.m_uri, 2);

    const std::string newMacro = "\n`define FIELD_TYPE int\n";
    change(server, nested.m_uri, 2, newMacro);
    cancelAt(server, header.m_uri, "after owner resolution");
    hover = server.getDocHover(
        {.textDocument = {.uri = header.m_uri}, .position = positionOf(edited, "value")});
    REQUIRE(hover);
    CHECK(rfl::json::write(*hover).find("int") != std::string::npos);
    checkDefinition(server, header.m_uri, positionOf(edited, "`FIELD_TYPE"), nested.m_uri,
                    positionOf(newMacro, "FIELD_TYPE"));
    replayAndCommitCurrent(server, header.m_uri, 2);
    CHECK(server.client.getDiagnostics(nested.m_uri).empty());
    CHECK(server.client.diagnosticVersions.at(nested.m_uri) == 2);

    change(server, nested.m_uri, 3, "`define FIELD_TYPE byte\n");
    cancelAt(server, header.m_uri, "after owner resolution");
    const auto config = server.getConfig();
    server.loadConfig(config, true);
    auto currentHeader = server.getDoc(header.m_uri);
    REQUIRE(currentHeader);
    CHECK(currentHeader->getVersion() == 2);
    CHECK(server.getDoc(nested.m_uri)->getVersion() == 3);
    hover = server.getDocHover(
        {.textDocument = {.uri = header.m_uri}, .position = positionOf(edited, "value")});
    REQUIRE(hover);
    CHECK(rfl::json::write(*hover).find("byte") != std::string::npos);
    change(server, header.m_uri, 3, edited);
    replayAndCommitCurrent(server, header.m_uri, 3);
}
