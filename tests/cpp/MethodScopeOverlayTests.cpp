// SPDX-License-Identifier: MIT
#include "completions/MemberCompletions.h"
#include "document/BodyDiagnosticCache.h"
#include "document/BodyLineProof.h"

#include "document/MethodScopeOverlay.h"
#include "util/Converters.h"
#include "utils/ServerHarness.h"
#include <atomic>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <rfl/json/write.hpp>
#include <thread>
#include <unordered_set>

#if defined(__APPLE__)
#    include <sys/resource.h>
#    include <fcntl.h>
#    include <unistd.h>
#endif

#include "slang/ast/symbols/ClassSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/util/ScopeGuard.h"
#include "util/DirectoryProofPlan.h"

namespace {
const std::string header =
    "class Accumulator;\n"
    "  int marker;\n"
    "  function int earlier(input int value); return value; endfunction\n"
    "  function int combine(input int alpha, input int bravo, input int charl);\n"
    "    int local_0 = alpha + bravo;\n"
    "    int local_1 = local_0 * charl;\n"
    "    return (local_1 + charl);\n"
    "  endfunction\n"
    "  int tail_field;\n"
    "  function int later(input int value); return value; endfunction\n"
    "endclass\n";

struct Workspace {
    std::filesystem::path root, previous = std::filesystem::current_path();
    Workspace() {
        static std::atomic_uint64_t serial{0};
        const auto temporary = std::filesystem::canonical(std::filesystem::temp_directory_path());
        // Clock resolution can give separate CTest processes the same name.
        // Claim the root atomically before writing or owning its cleanup.
        do {
            root = temporary /
                   ("slang-method-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                    "-" + std::to_string(serial++));
        } while (!std::filesystem::create_directory(root));
        std::filesystem::create_directory(root / ".slang");
        write(".slang/server.json",
              "{\"indexingThreads\":1,\"flags\":\"--single-unit --threads 1\","
              "\"compilationFilelist\":\"design.f\",\"experimentalMethodScopeOverlay\":true}");
        write("design.f", "+incdir+.\nprelude.sv\nowner.sv\ntail.sv\n");
        write("prelude.sv", "package prelude; int value; endpackage\n");
        write("owner.sv", "package owner;\n`include \"item.svh\"\nendpackage\n");
        write("tail.sv", "module tail; int value; endmodule\n");
        write("item.svh", header);
    }
    ~Workspace() {
        std::filesystem::current_path(previous);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    void write(const std::string& name, const std::string& bytes) {
        std::ofstream out(root / name, std::ios::binary);
        out << bytes;
        REQUIRE(out.good());
    }
    lsp::InitializeParams params() const {
        return {.workspaceFolders = {
                    {lsp::WorkspaceFolder{.uri = URI::fromFile(root), .name = "method"}}}};
    }
};

struct State {
    bool active;
    uint64_t accepted;
    int version;
    std::string decline;
};
State state(ServerHarness& server) {
    auto result = server.executeCommand(
        {.command = "slang.methodScopeStatus",
         .arguments = std::vector<lsp::LSPAny>{rfl::to_generic(std::string("state"))}});
    REQUIRE(result);
    auto value = rfl::from_generic<State>(*result).value();
    if (!value.decline.empty())
        std::cerr << "Overlay decline: " << value.decline << "\n";
    return value;
}
void change(ServerHarness& server, const URI& uri, int version, const std::string& text,
            lsp::RequestContext ctx = {}) {
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = version, .uri = uri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}},
        ctx);
}
std::string edited() {
    auto text = header;
    text.replace(text.find("local_1 +"), 7, "local_0");
    return text;
}
} // namespace

TEST_CASE("Method overlay cancellation preserves text before and after publication",
          "[method-overlay]") {
    for (const auto* boundary :
         {"method certificate begin", "method certificate recipe ready",
          "method certificate graph ready", "method certificate semantics ready",
          "method certificate method ready", "method certificate completions ready",
          "method certificate snapshot ready", "method certificate sources ready",
          "method certificate completion scope", "after method certificate",
          "before method mini parse", "before method publication",
          "after method text synchronization"}) {
        DYNAMIC_SECTION(boundary) {
            Workspace workspace;
            ServerHarness server(workspace.params());
            auto document = server.openFile("item.svh", header);
            document.doc->getAnalysis();
            lsp::RequestContext ctx("textDocument/didChange", 41);
            bool reached = false;
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == boundary) {
                    reached = true;
                    ctx.cancel();
                }
            });
            REQUIRE_THROWS_AS(change(server, document.m_uri, 2, edited(), ctx),
                              lsp::RequestCancelled);
            REQUIRE(reached);
            CHECK(document.doc->getVersion() == 2);
            CHECK(server.m_driver->getDocument(document.m_uri)->getText() == edited() + '\0');
            CHECK_FALSE(state(server).active);
            // Fresh native analysis must remain usable after the rejected overlay.
            CHECK(document.doc->getAnalysis()->getCompilation()->getAllDiagnostics().empty());
        }
    }
}

TEST_CASE("Method overlay never replays an incremental edit after sync throws",
          "[method-overlay]") {
    Workspace workspace;
    ServerHarness server(workspace.params());
    auto document = server.openFile("item.svh", header);
    document.doc->getAnalysis();
    auto offset = header.find("alpha + bravo") + 6;
    lsp::DidChangeTextDocumentParams params{
        .textDocument = {.version = 2, .uri = document.m_uri},
        .contentChanges = {lsp::TextDocumentContentChangePartial{
            .range = {.start = document.getPosition(offset),
                      .end = document.getPosition(offset + 1)},
            .text = "-"}}};
    lsp::RequestContext ctx("textDocument/didChange", 42);
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "after method text synchronization") {
            reached = true;
            throw std::runtime_error("test failure after sync");
        }
    });
    REQUIRE_THROWS(server.SlangServer::onDocDidChange(params, ctx));
    REQUIRE(reached);
    auto expected = header;
    expected[offset] = '-';
    CHECK(server.m_driver->getDocument(document.m_uri)->getText() == expected + '\0');
    CHECK(document.doc->getVersion() == 2);
    CHECK_FALSE(state(server).active);
}

TEST_CASE("Method overlay declines unsupported body and interface changes", "[method-overlay]") {
    for (const auto& replacement :
         {std::string("marker "), std::string("earlier"), std::string("local_9")}) {
        DYNAMIC_SECTION(replacement) {
            Workspace workspace;
            ServerHarness server(workspace.params());
            auto document = server.openFile("item.svh", header);
            document.doc->getAnalysis();
            auto text = header;
            text.replace(text.find("local_1 +"), 7, replacement);
            change(server, document.m_uri, 2, text);
            CHECK_FALSE(state(server).active);
            CHECK(server.m_driver->getDocument(document.m_uri)->getText() == text + '\0');
            CHECK(document.doc->getVersion() == 2);
        }
    }
}

TEST_CASE("Method overlay lifecycle invalidation keeps current editor text", "[method-overlay]") {
    for (const auto* action : {"close", "save", "open", "watch", "config"}) {
        DYNAMIC_SECTION(action) {
            Workspace workspace;
            ServerHarness server(workspace.params());
            auto document = server.openFile("item.svh", header);
            document.doc->getAnalysis();
            const auto text = edited();
            change(server, document.m_uri, 2, text);
            REQUIRE(state(server).active);
            if (std::string_view(action) == "close") {
                server.onDocDidClose({.textDocument = {.uri = document.m_uri}});
                server.onDocDidOpen({.textDocument = {.uri = document.m_uri,
                                                      .languageId = "systemverilog",
                                                      .version = 3,
                                                      .text = text}});
            }
            else if (std::string_view(action) == "save")
                server.onDocDidSave({.textDocument = {.uri = document.m_uri}, .text = text});
            else if (std::string_view(action) == "open")
                server.openFile("tail.sv", "module tail; int value; endmodule\n");
            else {
                auto path = workspace.root / (std::string_view(action) == "config"
                                                  ? ".slang/server.json"
                                                  : "owner.sv");
                if (std::string_view(action) == "config")
                    workspace.write(
                        ".slang/server.json",
                        "{\"indexingThreads\":1,\"flags\":\"--single-unit --threads 1\","
                        "\"compilationFilelist\":\"design.f\",\"experimentalMethodScopeOverlay\":"
                        "false}");
                server.onWorkspaceDidChangeWatchedFiles(
                    {.changes = {
                         {.uri = URI::fromFile(path), .type = lsp::FileChangeType::Changed}}});
            }
            CHECK_FALSE(state(server).active);
            CHECK(server.getDoc(document.m_uri)->getVersion() ==
                  (std::string_view(action) == "close" ? 3 : 2));
            auto bytes = server.getDoc(document.m_uri)->getText();
            CHECK(bytes == text + '\0');
        }
    }
}

TEST_CASE("Method overlay rejects changed lengths line layout and signatures", "[method-overlay]") {
    for (const auto* action : {"length", "newline", "signature", "syntax", "local-count"}) {
        DYNAMIC_SECTION(action) {
            Workspace workspace;
            ServerHarness server(workspace.params());
            auto document = server.openFile("item.svh", header);
            document.doc->getAnalysis();
            auto text = header;
            if (std::string_view(action) == "length")
                text.insert(text.find("local_1 +"), " ");
            else if (std::string_view(action) == "newline")
                text[text.find("return (") + 6] = '\n';
            else if (std::string_view(action) == "signature")
                text.replace(text.find("input int alpha"), 5, "inout");
            else if (std::string_view(action) == "syntax")
                text[text.find("local_1 +") + 3] = '?';
            else
                text.replace(text.find("int local_1 = local_0 * charl;"), 29,
                             "                            ;");
            change(server, document.m_uri, 2, text);
            CHECK_FALSE(state(server).active);
            CHECK(server.m_driver->getDocument(document.m_uri)->getText() == text + '\0');
            CHECK(document.doc->getVersion() == 2);
        }
    }
}

TEST_CASE("Method overlay refuses shadowing macro and repeated include environments",
          "[method-overlay]") {
    for (const auto* action : {"shadow", "macro", "repeated", "import", "inheritance"}) {
        DYNAMIC_SECTION(action) {
            Workspace workspace;
            auto text = header;
            if (std::string_view(action) == "shadow") {
                text.replace(text.find("int marker;"), 11, "int alpha; ");
                workspace.write("item.svh", text);
            }
            else if (std::string_view(action) == "macro")
                workspace.write(
                    "owner.sv",
                    "`define UNUSED 1\npackage owner;\n`include \"item.svh\"\nendpackage\n");
            else if (std::string_view(action) == "repeated")
                workspace.write("owner.sv",
                                "package owner;\n`include \"item.svh\"\nendpackage\n"
                                "package another;\n`include \"item.svh\"\nendpackage\n");
            else if (std::string_view(action) == "import")
                workspace.write(
                    "owner.sv",
                    "package owner; import prelude::*;\n`include \"item.svh\"\nendpackage\n");
            else {
                text.replace(text.find("class Accumulator;"), 18,
                             "class Accumulator extends Missing;");
                workspace.write("item.svh", text);
            }
            ServerHarness server(workspace.params());
            auto document = server.openFile("item.svh", text);
            text.replace(text.find("local_1 +"), 7, "local_0");
            change(server, document.m_uri, 2, text);
            CHECK_FALSE(state(server).active);
            CHECK(server.m_driver->getDocument(document.m_uri)->getText() == text + '\0');
        }
    }
}

TEST_CASE("Method overlay certifies sibling nested and repeated unrelated includes",
          "[method-overlay]") {
    Workspace workspace;
    workspace.write("sibling.svh", "class Sibling; int field; endclass\n");
    workspace.write("nested.svh", "`include \"sibling.svh\"\n");
    workspace.write("owner.sv", "package owner;\n`include \"nested.svh\"\n"
                                "`include \"item.svh\"\nendpackage\n"
                                "package another;\n`include \"sibling.svh\"\nendpackage\n");
    ServerHarness server(workspace.params());
    auto document = server.openFile("item.svh", header);
    document.doc->getAnalysis();
    change(server, document.m_uri, 2, edited());
    REQUIRE(state(server).active);
    // Every dependency byte is a witness, even if no watcher event arrives.
    workspace.write("sibling.svh", "class Sibling; int other; endclass\n");
    change(server, document.m_uri, 3, header);
    CHECK_FALSE(state(server).active);
    CHECK(document.doc->getVersion() == 3);
    CHECK(server.m_driver->getDocument(document.m_uri)->getText() == header + '\0');
}

TEST_CASE("Method overlay certifies same length local declaration renames", "[method-overlay]") {
    Workspace workspace;
    ServerHarness server(workspace.params());
    auto document = server.openFile("item.svh", header);
    document.doc->getAnalysis();
    auto text = header;
    for (size_t at = 0; (at = text.find("local_0", at)) != std::string::npos; at += 7)
        text.replace(at, 7, "value_0");
    change(server, document.m_uri, 2, text);
    REQUIRE(state(server).active);
    CHECK(state(server).accepted == 1);
    CHECK(server.m_driver->getDocument(document.m_uri)->getText() == text + '\0');
}

TEST_CASE("Method overlay refuses dependency changes before certification", "[method-overlay]") {
    Workspace workspace;
    workspace.write("sibling.svh", "class Sibling; int field; endclass\n");
    workspace.write("owner.sv", "package owner;\n`include \"sibling.svh\"\n"
                                "`include \"item.svh\"\nendpackage\n");
    ServerHarness server(workspace.params());
    auto document = server.openFile("item.svh", header);
    document.doc->getAnalysis();
    workspace.write("sibling.svh", "class Sibling; int other; endclass\n");
    change(server, document.m_uri, 2, edited());
    auto status = state(server);
    CHECK_FALSE(status.active);
    CHECK(status.decline == "source changed before certification");
    CHECK(server.m_driver->getDocument(document.m_uri)->getText() == edited() + '\0');
}

TEST_CASE("Method certificate refuses a dependency change between source and lookup witnesses",
          "[method-overlay]") {
    Workspace workspace;
    workspace.write("sibling.svh", "class Sibling; int field; endclass\n");
    workspace.write("owner.sv", "package owner;\n`include \"sibling.svh\"\n"
                                "`include \"item.svh\"\nendpackage\n");
    ServerHarness server(workspace.params());
    auto document = server.openFile("item.svh", header);
    document.doc->getAnalysis();
    lsp::RequestContext ctx("textDocument/didChange", 44);
    bool changed = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "method certificate sources ready") {
            REQUIRE_FALSE(changed);
            changed = true;
            workspace.write("sibling.svh", "class Sibling; int other; endclass\n");
        }
    });
    change(server, document.m_uri, 2, edited(), ctx);
    REQUIRE(changed);
    CHECK_FALSE(state(server).active);
    CHECK(state(server).decline == "input changed during certification");
    CHECK(document.doc->getVersion() == 2);
    CHECK(server.m_driver->getDocument(document.m_uri)->getText() == edited() + '\0');
}

// Mechanism attribution only. Run explicitly and retain stdout; use the ordinary
// LSP binary and benchmark.py for end-to-end latency without an observer.
TEST_CASE("Profile first method certificate by phase", "[.method-profile]") {
    struct Phase {
        std::string boundary;
        double elapsedMs;
    };
    struct Sample {
        size_t classes;
        size_t repetition;
        std::vector<Phase> phases;
    };
    const std::vector<std::string_view> boundaries = {
        "method certificate begin",          "method certificate recipe ready",
        "method certificate graph ready",    "method certificate semantics ready",
        "method certificate method ready",   "method certificate completions ready",
        "method certificate snapshot ready", "after method certificate"};
    for (size_t classes : {500, 5000, 10000}) {
        for (size_t repetition = 0; repetition < 3; ++repetition) {
            Workspace workspace;
            std::string owner = "package owner;\n";
            for (size_t chunk = 0; chunk < 5; ++chunk) {
                std::string noise;
                for (size_t i = chunk; i < classes; i += 5)
                    noise += "class Noise_" + std::to_string(i) +
                             "; int field; function int value(input int arg); "
                             "return arg; endfunction endclass\n";
                auto name = "classes_" + std::to_string(chunk) + ".svh";
                workspace.write(name, noise);
                if (chunk == 2) {
                    workspace.write("nested.svh", "`include \"" + name + "\"\n");
                    owner += "`include \"nested.svh\"\n";
                }
                else
                    owner += "`include \"" + name + "\"\n";
            }
            owner += "`include \"item.svh\"\nendpackage\n";
            workspace.write("owner.sv", owner);
            ServerHarness server(workspace.params());
            auto document = server.openFile("item.svh", header);
            document.doc->getAnalysis();
            Sample sample{classes, repetition, {}};
            lsp::RequestContext ctx("textDocument/didChange", 43);
            auto previous = std::chrono::steady_clock::now();
            ctx.setCheckpointObserverForTesting([&](std::string_view boundary) {
                if (std::find(boundaries.begin(), boundaries.end(), boundary) == boundaries.end())
                    return;
                auto now = std::chrono::steady_clock::now();
                sample.phases.push_back(
                    {std::string(boundary),
                     std::chrono::duration<double, std::milli>(now - previous).count()});
                previous = now;
            });
            change(server, document.m_uri, 2, edited(), ctx);
            REQUIRE(state(server).active);
            REQUIRE(sample.phases.size() == boundaries.size());
            for (size_t i = 0; i < boundaries.size(); ++i)
                CHECK(sample.phases[i].boundary == boundaries[i]);
            std::cout << "METHOD_CERTIFICATE_PROFILE " << rfl::json::write(sample) << '\n';
        }
    }
}

namespace {
void bodyConfig(Workspace& workspace, bool enabled = true, bool uvm = false, bool certify = false) {
    workspace.write(".slang/server.json",
                    std::string("{\"indexingThreads\":1,\"flags\":\"--single-unit --threads 1\",") +
                        "\"compilationFilelist\":\"design.f\",\"experimentalMethodBodyQueries\":" +
                        (enabled ? "true" : "false") +
                        (certify ? ",\"experimentalMethodBodyCertification\":true" : "") +
                        (uvm ? ",\"uvm\":{\"useBundled\":true},"
                               "\"experimentalUvmSemanticCache\":true,"
                               "\"experimentalScopeSemanticCache\":true,"
                               "\"frozenSources\":{\"packages\":[\"uvm_pkg\"]}"
                             : "") +
                        "}");
}
std::vector<lsp::CompletionItem> bodyCompletions(ServerHarness& server, DocumentHandle& doc,
                                                 size_t offset) {
    auto response = server.getDocCompletion(
        {.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(offset)});
    auto* items = rfl::get_if<std::vector<lsp::CompletionItem>>(&response);
    REQUIRE(items);
    auto result = *items;
    for (auto& item : result) {
        if (result.size() < 100 || item.label == "local_0" || item.label == "local_1" ||
            item.label == "this" || item.label == "tail_field" || item.label == "get_name")
            item = server.getCompletionItemResolve(item);
        item.data.reset(); // Source handles are owner-specific, all public values must match.
    }
    return result;
}
} // namespace

TEST_CASE("Body queries preserve native values through variable length multiline edits",
          "[method-body]") {
    const bool certify = GENERATE(false, true);
    Workspace workspace;
    bodyConfig(workspace, true, false, certify);
    const auto baseline = header;
    std::vector<std::string> sequence;
    auto text = baseline;
    text.replace(text.find("local_1 + charl"), 15, "local_0 +\n      (charl * 123)");
    sequence.push_back(text);
    text.replace(text.find("alpha + bravo"), 13, "(alpha - bravo) * 12");
    sequence.push_back(text);
    sequence.push_back(baseline);
    std::vector<std::vector<lsp::CompletionItem>> actual;
    std::vector<std::string> definitions;

    {
        ServerHarness server(workspace.params());
        auto doc = server.openFile("item.svh", baseline);
        doc.doc->getAnalysis();
        int version = 1;
        for (const auto& current : sequence) {
            change(server, doc.m_uri, ++version, current);
            doc.m_text = current;
            REQUIRE(state(server).active);
            const auto offset = current.find("return (local_") + 10;
            definitions.push_back(rfl::json::write(server.getDocDefinition(
                {.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(offset)})));
            REQUIRE(state(server).active);
            // Exercise native public completion and deferred resolution, including
            // class members declared after the moved method and method-owned locals.
            actual.push_back(bodyCompletions(server, doc, offset));
            REQUIRE(state(server).active);
        }
        CHECK(state(server).accepted == sequence.size());
    }
    bodyConfig(workspace, false);
    ServerHarness native(workspace.params());
    auto doc = native.openFile("item.svh", baseline);
    int version = 1;
    for (size_t i = 0; i < sequence.size(); ++i) {
        const auto& current = sequence[i];
        change(native, doc.m_uri, ++version, current);
        doc.m_text = current;
        const auto offset = current.find("return (local_") + 10;
        CHECK(definitions[i] ==
              rfl::json::write(native.getDocDefinition(
                  {.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(offset)})));
        CHECK(rfl::json::write(actual[i]) ==
              rfl::json::write(bodyCompletions(native, doc, offset)));
    }
}

struct BodyTestServer : ServerHarness {
    using server::SlangServer::runBackgroundWork;
    using ServerHarness::ServerHarness;
};

TEST_CASE("Body queries support calls declaration edits and incomplete expressions",
          "[method-body]") {
    const bool certify = GENERATE(false, true);
    for (const auto& replacement : {std::string("local_1 + (charl"), std::string("marker + charl"),
                                    std::string("earlier(charl)"), std::string("`VALUE + charl")}) {
        DYNAMIC_SECTION(replacement) {
            Workspace workspace;
            bodyConfig(workspace, true, false, certify);
            BodyTestServer server(workspace.params());
            auto doc = server.openFile("item.svh", header);
            doc.doc->getAnalysis();
            auto text = header;
            text.replace(text.find("local_1 + charl"), 15, replacement);
            change(server, doc.m_uri, 2, text);
            doc.m_text = text;
            CHECK(state(server).active);
            CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
            const auto at = text.rfind("charl") + 2;
            auto actual = bodyCompletions(server, doc, at);
            server.runBackgroundWork({});
            auto diagnostics = doc.getDiagnostics();
            bodyConfig(workspace, false);
            ServerHarness native(workspace.params());
            auto oracle = native.openFile("item.svh", text);
            CHECK(rfl::json::write(actual) ==
                  rfl::json::write(bodyCompletions(native, oracle, at)));
            CHECK(rfl::json::write(diagnostics) == rfl::json::write(oracle.getDiagnostics()));
        }
    }
    Workspace workspace;
    bodyConfig(workspace, true, false, certify);
    BodyTestServer server(workspace.params());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = header;
    for (size_t at = 0; (at = text.find("local_0", at)) != std::string::npos; at += 7)
        text.replace(at, 7, "value_0");
    change(server, doc.m_uri, 2, text);
    CHECK(state(server).active);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
}

TEST_CASE("Body queries accept a closed helper inside an inherited UVM class", "[method-body]") {
    Workspace workspace;
    bodyConfig(workspace, true, true);
    workspace.write("owner.sv", "package owner; import uvm_pkg::*;\n"
                                "`include \"uvm_macros.svh\"\n"
                                "`include \"item.svh\"\nendpackage\n");
    auto text = header;
    text.replace(text.find("class Accumulator;"), 18,
                 "class Accumulator extends uvm_object;\n  `uvm_object_utils(Accumulator)\n"
                 "  function new(string name=\"Accumulator\"); super.new(name); endfunction");
    workspace.write("item.svh", text);
    ServerHarness server(workspace.params());
    auto doc = server.openFile("item.svh", text);
    doc.doc->getAnalysis();
    text.replace(text.find("local_1 + charl"), 15, "local_0 +\n      (charl * 123)");
    change(server, doc.m_uri, 2, text);
    doc.m_text = text;
    REQUIRE(state(server).active);
    auto actual = bodyCompletions(server, doc, text.find("return (local_") + 10);
    REQUIRE(actual.size() > 10);
    REQUIRE(state(server).active);
    bodyConfig(workspace, false, true);
    ServerHarness native(workspace.params());
    auto current = native.openFile("item.svh", text);
    auto expected = bodyCompletions(native, current, text.find("return (local_") + 10);
    CHECK(rfl::json::write(actual) == rfl::json::write(expected));
}

TEST_CASE("Body queries invalidate on dependencies and method boundary changes", "[method-body]") {
    for (const auto* mode : {"dependency", "signature", "local", "directive"}) {
        DYNAMIC_SECTION(mode) {
            Workspace workspace;
            bodyConfig(workspace);
            auto original = header;
            if (std::string_view(mode) == "directive") {
                workspace.write("owner.sv", "`define VALUE alpha\npackage owner;\n"
                                            "`include \"item.svh\"\nendpackage\n");
                original.replace(original.find("alpha + bravo"), 13, "`VALUE + bravo");
                workspace.write("item.svh", original);
            }
            ServerHarness server(workspace.params());
            auto doc = server.openFile("item.svh", original);
            doc.doc->getAnalysis();
            auto text = original;
            text.replace(text.find("local_1 + charl"), 15, "local_0 + (charl * 123)");
            if (std::string_view(mode) == "dependency")
                workspace.write("prelude.sv", "package prelude; int changed; endpackage\n");
            else if (std::string_view(mode) == "signature")
                text.replace(text.find("input int alpha"), 15, "input int changed");
            else if (std::string_view(mode) == "local")
                text.insert(text.find("    return"), "    int added = 0;\n");
            change(server, doc.m_uri, 2, text);
            CHECK(state(server).active ==
                  (std::string_view(mode) == "local" || std::string_view(mode) == "directive"));
            CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
        }
    }
}

TEST_CASE("Body queries retain text certificates on cancellation without replaying text",
          "[method-body]") {
    for (const auto* boundary : {"before method mini parse", "before method publication",
                                 "after method text synchronization"}) {
        DYNAMIC_SECTION(boundary) {
            Workspace workspace;
            bodyConfig(workspace);
            ServerHarness server(workspace.params());
            auto doc = server.openFile("item.svh", header);
            doc.doc->getAnalysis();
            auto text = header;
            text.replace(text.find("local_1 + charl"), 15, "local_0 +\n      (charl * 123)");
            lsp::RequestContext ctx("textDocument/didChange", 123);
            bool reached = false;
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == boundary) {
                    reached = true;
                    ctx.cancel();
                }
            });
            REQUIRE_THROWS_AS(change(server, doc.m_uri, 2, text, ctx), lsp::RequestCancelled);
            REQUIRE(reached);
            CHECK(state(server).active ==
                  (std::string_view(boundary) != "after method text synchronization"));
            CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
            CHECK(doc.doc->getVersion() == 2);
            doc.m_text = text;
            const auto at = text.rfind("charl") + 2;
            auto actual = bodyCompletions(server, doc, at);
            bodyConfig(workspace, false);
            ServerHarness native(workspace.params());
            auto oracle = native.openFile("item.svh", text);
            CHECK(rfl::json::write(actual) ==
                  rfl::json::write(bodyCompletions(native, oracle, at)));
        }
    }
}

TEST_CASE("Body queries parse only the mini source after a warm native query", "[method-body]") {
    Workspace workspace;
    bodyConfig(workspace);
    ServerHarness server(workspace.params());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = header;
    text.replace(text.find("local_1 + charl"), 15, "local_0 +\n      (charl * 123)");
    size_t miniParses = 0, fullParses = 0;
    lsp::RequestContext ctx("textDocument/didChange", 124);
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        miniParses += phase == "before method mini parse";
        fullParses += phase == "before syntax parsing";
    });
    change(server, doc.m_uri, 2, text, ctx);
    doc.m_text = text;
    REQUIRE(state(server).active);
    CHECK(miniParses == 1);
    CHECK(fullParses == 0);
    const auto offset = text.find("return (local_") + 10;
    server.getDocCompletion(
        {.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(offset)}, ctx);
    server.getDocDefinition(
        {.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(offset)}, ctx);
    CHECK(fullParses == 0);
    CHECK(state(server).active);
}

TEST_CASE("Body query handles expire across edits and certificate replacement", "[method-body]") {
    Workspace workspace;
    bodyConfig(workspace);
    ServerHarness server(workspace.params());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = header;
    text.replace(text.find("local_1 + charl"), 15, "local_0 + (charl * 123)");
    change(server, doc.m_uri, 2, text);
    doc.m_text = text;
    REQUIRE(state(server).active);
    auto response = server.getDocCompletion(
        {.textDocument = {.uri = doc.m_uri},
         .position = doc.getPosition(text.find("return (local_") + 10)});
    const auto& items = rfl::get<std::vector<lsp::CompletionItem>>(response);
    auto found = std::find_if(items.begin(), items.end(),
                              [](const auto& item) { return item.label == "local_0"; });
    REQUIRE(found != items.end());
    auto old = *found;
    REQUIRE(old.data);
    REQUIRE_FALSE(old.documentation);
    change(server, doc.m_uri, 3, header);
    REQUIRE(state(server).active);
    auto expired = server.getCompletionItemResolve(old);
    CHECK_FALSE(expired.data);
    CHECK_FALSE(expired.documentation);
    // Configuration reload replaces the driver and can reuse numeric buffer IDs.
    auto rebuilt = server.executeCommand(
        {.command = "slang.rebuildScopeLibrary", .arguments = std::vector<lsp::LSPAny>{}});
    REQUIRE(rebuilt);
    auto reopened = server.openFile("item.svh", header);
    reopened.doc->getAnalysis();
    change(server, reopened.m_uri, 2, text);
    REQUIRE(state(server).active);
    expired = server.getCompletionItemResolve(old);
    CHECK_FALSE(expired.data);
    CHECK_FALSE(expired.documentation);
}

TEST_CASE("Body query certificates have bounded edit lifetime and can prepare again",
          "[method-body]") {
    Workspace workspace;
    bodyConfig(workspace);
    ServerHarness server(workspace.params());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto edited = header;
    edited.replace(edited.find("local_1 + charl"), 15, "local_0 + (charl * 123)");
    for (int version = 2; version <= 65; ++version) {
        change(server, doc.m_uri, version, version % 2 ? header : edited);
        REQUIRE(state(server).active);
        CHECK(state(server).accepted == uint64_t(version - 1));
    }
    change(server, doc.m_uri, 66, edited);
    CHECK_FALSE(state(server).active);
    doc.doc->getAnalysis();
    change(server, doc.m_uri, 67, header);
    CHECK(state(server).active);
    CHECK(state(server).accepted == 1);
}

TEST_CASE("General body queries rebind arbitrary class function and task bodies",
          "[method-body][general-body]") {
    const std::string original = R"(class Base;
  int inherited;
endclass
class Accumulator extends Base;
  typedef struct { int count; string label; } payload_t;
  payload_t payload;
  int tail_field;
  function string format(input int count = this.tail_field);
    string result;
    result = $sformatf("%0d", count);
    return result;
  endfunction
  task run(input int count);
    int old_local;
    old_local = count;
    payload.count = old_local;
  endtask
endclass
)";
    for (bool task : {false, true}) {
        DYNAMIC_SECTION((task ? "task" : "function")) {
            Workspace workspace;
            bodyConfig(workspace);
            workspace.write("item.svh", original);
            ServerHarness server(workspace.params());
            auto doc = server.openFile("item.svh", original);
            doc.doc->getAnalysis();
            auto next = original;
            std::string replacement;
            size_t begin, end;
            if (task) {
                begin = next.find("    int old_local;");
                end = next.find("  endtask", begin);
                replacement = "    payload_t local_payload;\n    int fresh = count + inherited;\n"
                              "    for (int i = 0; i < fresh; i++) begin\n"
                              "      local_payload.count = i;\n    end\n"
                              "    payload = local_payload;\n    #1;\n";
            }
            else {
                begin = next.find("    string result;");
                end = next.find("  endfunction", begin);
                replacement = "    payload_t local_payload;\n    string fresh;\n"
                              "    local_payload.count = count + inherited;\n"
                              "    if (count > 1) fresh = local_payload.label;\n"
                              "    else fresh = $sformatf(\"%0d\", payload.count);\n"
                              "    return fresh;\n";
            }
            next.replace(begin, end - begin, replacement);
            change(server, doc.m_uri, 2, next);
            doc.m_text = next;
            REQUIRE(state(server).active);
            const std::vector<size_t> positions = {
                next.find("local_payload.count", begin) + 16,
                next.find(task ? "payload = local_payload" : "return fresh", begin) +
                    (task ? 16 : 10),
                next.find("inherited", begin) + 5};
            std::vector<std::vector<lsp::CompletionItem>> actual;
            std::vector<std::string> definitions;
            for (auto at : positions) {
                actual.push_back(bodyCompletions(server, doc, at));
                definitions.push_back(rfl::json::write(server.getDocDefinition(
                    {.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(at)})));
            }
            REQUIRE(state(server).active);
            bodyConfig(workspace, false);
            ServerHarness native(workspace.params());
            auto oracle = native.openFile("item.svh", next);
            for (size_t i = 0; i < positions.size(); ++i) {
                auto expected = bodyCompletions(native, oracle, positions[i]);
                CHECK(rfl::json::write(actual[i]) == rfl::json::write(expected));
                CHECK(definitions[i] == rfl::json::write(native.getDocDefinition(
                                            {.textDocument = {.uri = oracle.m_uri},
                                             .position = oracle.getPosition(positions[i])})));
            }
        }
    }
}

TEST_CASE("General body queries retain package module and out-of-block contexts",
          "[method-body][general-body]") {
    for (const auto& scope :
         {std::string("package"), std::string("module"), std::string("out-of-block")}) {
        DYNAMIC_SECTION(scope) {
            Workspace workspace;
            bodyConfig(workspace);
            std::string original;
            if (scope == "out-of-block")
                original = "class C; int field; extern task run(input int arg); endclass\n"
                           "task C::run(input int arg);\n int before_local; before_local = "
                           "arg;\nendtask\n";
            else if (scope == "module") {
                workspace.write("owner.sv", "`include \"item.svh\"\n");
                original = "module M #(parameter int WIDTH=32);\n logic [WIDTH-1:0] field;\n"
                           "task run(input int arg);\n int before_local; before_local = "
                           "arg;\nendtask\nendmodule\n";
            }
            else
                original = "int field;\ntask run(input int arg);\n int before_local; before_local "
                           "= arg;\nendtask\n";
            workspace.write("item.svh", original);
            ServerHarness server(workspace.params());
            auto doc = server.openFile("item.svh", original);
            doc.doc->getAnalysis();
            auto text = original;
            auto begin = text.find(" int before_local;");
            text.replace(begin, text.find("endtask", begin) - begin,
                         " int after_local[];\n after_local = new[2];\n foreach (after_local[i]) "
                         "after_local[i] = arg + field;\n");
            change(server, doc.m_uri, 2, text);
            doc.m_text = text;
            REQUIRE(state(server).active);
            const auto at = text.find("arg + field") + 8;
            auto actual = bodyCompletions(server, doc, at);
            auto definition = rfl::json::write(server.getDocDefinition(
                {.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(at)}));
            REQUIRE(state(server).active);
            bodyConfig(workspace, false);
            ServerHarness native(workspace.params());
            auto oracle = native.openFile("item.svh", text);
            CHECK(rfl::json::write(actual) ==
                  rfl::json::write(bodyCompletions(native, oracle, at)));
            CHECK(definition ==
                  rfl::json::write(native.getDocDefinition({.textDocument = {.uri = oracle.m_uri},
                                                            .position = oracle.getPosition(at)})));
        }
    }
}

TEST_CASE("General body queries invalidate constant-function dependencies",
          "[method-body][general-body]") {
    for (const auto& initial : {std::string("4"), std::string("runtime_input")}) {
        DYNAMIC_SECTION(initial) {
            Workspace workspace;
            bodyConfig(workspace);
            const std::string original =
                "int runtime_input;\nfunction automatic int width(); return " + initial +
                "; endfunction\nlocalparam int W = width();\nlogic [W-1:0] data;\n";
            workspace.write("item.svh", original);
            ServerHarness server(workspace.params());
            auto doc = server.openFile("item.svh", original);
            (void)doc.doc->getAnalysis()->getCompilation()->getAllDiagnostics();
            auto text = original;
            text.replace(text.find("return " + initial), 7 + initial.size(), "return 8");
            change(server, doc.m_uri, 2, text);
            doc.m_text = text;
            CHECK_FALSE(state(server).active);
            CHECK(state(server).decline == "method has external constant dependencies");
            CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
            auto actual = server.getDocHover({.textDocument = {.uri = doc.m_uri},
                                              .position = doc.getPosition(text.find("data;") + 2)});
            bodyConfig(workspace, false);
            ServerHarness native(workspace.params());
            auto oracle = native.openFile("item.svh", text);
            CHECK(rfl::json::write(actual) ==
                  rfl::json::write(native.getDocHover(
                      {.textDocument = {.uri = oracle.m_uri},
                       .position = oracle.getPosition(text.find("data;") + 2)})));
        }
    }
}

TEST_CASE("General body queries accept batched edits and inherited macro state",
          "[method-body][general-body]") {
    Workspace workspace;
    bodyConfig(workspace);
    workspace.write(
        "owner.sv",
        "`define ENTRY_VALUE 4\npackage owner;\n`include \"item.svh\"\n"
        "`undef ENTRY_VALUE\n`define ENTRY_VALUE missing_later_definition\nendpackage\n");
    const std::string original = "class C;\ntask run();\n  // 中文\n  int value = `ENTRY_VALUE;\n  "
                                 "value += 1;\nendtask\nendclass\n";
    workspace.write("item.svh", original);
    ServerHarness server(workspace.params());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    lsp::DidChangeTextDocumentParams params{
        .textDocument = {.version = 2, .uri = doc.m_uri},
        .contentChanges = {
            lsp::TextDocumentContentChangePartial{.range = {.start = {.line = 4, .character = 11},
                                                            .end = {.line = 4, .character = 12}},
                                                  .text = "123"},
            lsp::TextDocumentContentChangePartial{.range = {.start = {.line = 4, .character = 2},
                                                            .end = {.line = 4, .character = 2}},
                                                  .text = "if (value > 0) "}}};
    server.SlangServer::onDocDidChange(params, {});
    const auto expected = std::string(
        "class C;\ntask run();\n  // 中文\n  int value = `ENTRY_VALUE;\n  if (value > 0) value += "
        "123;\nendtask\nendclass\n");
    REQUIRE(server.m_driver->getDocument(doc.m_uri)->getText() == expected + '\0');
    doc.m_text = expected;
    REQUIRE(state(server).active);
    auto at = expected.find("value +=") + 3;
    auto actual = bodyCompletions(server, doc, at);
    bodyConfig(workspace, false);
    ServerHarness native(workspace.params());
    auto oracle = native.openFile("item.svh", expected);
    CHECK(rfl::json::write(actual) == rfl::json::write(bodyCompletions(native, oracle, at)));
}

TEST_CASE("General body queries keep balanced conditional compilation local",
          "[method-body][general-body]") {
    Workspace workspace;
    bodyConfig(workspace);
    workspace.write("owner.sv",
                    "`define LOCAL_ENABLED\npackage owner;\n`include \"item.svh\"\nendpackage\n");
    const std::string original =
        "class C;\nfunction int f(input int arg);\n`ifdef LOCAL_ENABLED\n  return arg;\n`else\n  "
        "return 0;\n`endif\nendfunction\nendclass\n";
    workspace.write("item.svh", original);
    ServerHarness server(workspace.params());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("return arg;"), 11, "int local_value = arg + 1;\n  return local_value;");
    change(server, doc.m_uri, 2, text);
    doc.m_text = text;
    REQUIRE(state(server).active);
    const auto at = text.find("return local_value") + 11;
    auto actual = bodyCompletions(server, doc, at);
    bodyConfig(workspace, false);
    ServerHarness native(workspace.params());
    auto oracle = native.openFile("item.svh", text);
    CHECK(rfl::json::write(actual) == rfl::json::write(bodyCompletions(native, oracle, at)));
}

TEST_CASE("Detached body construction owns attributes and preserves its lexical owner",
          "[method-body][general-body]") {
    using namespace slang;
    SourceManager sm;
    auto source = sm.assignText(
        "class Box #(type T=int); T item; endclass\n"
        "(* keep=1 *) function automatic int f(input int arg); return arg; endfunction");
    auto tree = syntax::SyntaxTree::fromBuffer(source, sm);
    ast::CompilationOptions options;
    options.flags |= ast::CompilationFlags::TrackSubroutineBodyDependencies;
    auto owner = std::make_shared<ast::Compilation>(options);
    owner->addSyntaxTree(tree);
    owner->getRoot();
    auto original = owner->getCompilationUnits().front()->find("f");
    REQUIRE(original);
    auto& originalFn = original->as<ast::SubroutineSymbol>();
    REQUIRE(owner->getAttributes(originalFn).size() == 1);
    auto& generic =
        owner->getCompilationUnits().front()->find("Box")->as<ast::GenericClassDefSymbol>();
    size_t originalSpecializations = 0;
    for (const auto& ignored : generic.specializations()) {
        (void)ignored;
        ++originalSpecializations;
    }
    auto changed = sm.assignText(
        "(* keep=1 *) function automatic int f(input int arg); "
        "typedef struct packed {logic [7:0] data;} local_t; Box#(local_t) value; "
        "value = new; value.item.data = arg; return value.item.data; endfunction");
    auto mini = syntax::SyntaxTree::fromBuffer(changed, sm);
    auto& functionSyntax = mini->root()
                               .as<syntax::CompilationUnitSyntax>()
                               .members.front()
                               ->as<syntax::FunctionDeclarationSyntax>();
    auto query = std::make_shared<ast::Compilation>(options);
    query->addSyntaxTree(mini);
    query->setBodyQueryContext(owner);
    CHECK(&query->getStdPackage() == &owner->getStdPackage());
    CHECK(query->getCompilationUnits().front() == owner->getCompilationUnits().front());
    auto& fn = ast::SubroutineSymbol::createBodyQuery(*query, functionSyntax, originalFn);
    CHECK(owner->getAttributes(fn).empty());
    CHECK(query->getAttributes(fn).size() == 1);
    CHECK(owner->getCompilationUnits().front()->find("f") == &originalFn);
    CHECK(fn.getParentScope() == originalFn.getParentScope());
    CHECK(fn.getSyntax() == &functionSyntax);
    (void)fn.getBody();
    size_t retainedSpecializations = 0;
    for (const auto& ignored : generic.specializations()) {
        (void)ignored;
        ++retainedSpecializations;
    }
    CHECK(retainedSpecializations == originalSpecializations);
}

TEST_CASE("Body line changes invalidate macro-dependent declarations and lazy sibling values",
          "[method-body][general-body]") {
    for (bool sibling : {false, true}) {
        DYNAMIC_SECTION(sibling) {
            Workspace workspace;
            bodyConfig(workspace);
            const std::string original =
                sibling ? "function automatic int f(input int arg); return arg; endfunction\n"
                          "function automatic int line_value(); return `__LINE__; endfunction\n"
                        : "class C;\nfunction int f(input int arg); return arg; endfunction\n"
                          "localparam int WIDTH=`__LINE__;\nlogic [WIDTH-1:0] data;\nendclass\n";
            workspace.write("item.svh", original);
            ServerHarness server(workspace.params());
            auto doc = server.openFile("item.svh", original);
            doc.doc->getAnalysis();
            auto text = original;
            text.replace(text.find("return arg;"), 11,
                         sibling ? "localparam int value = line_value();\nreturn value;"
                                 : "int value = arg;\nreturn value;");
            change(server, doc.m_uri, 2, text);
            doc.m_text = text;
            CHECK_FALSE(state(server).active);
            CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
            if (!sibling)
                CHECK(state(server).decline == "body shifts line-dependent enclosing declarations");
            auto at = text.find("return value") + 10;
            auto actual = bodyCompletions(server, doc, at);
            bodyConfig(workspace, false);
            ServerHarness native(workspace.params());
            auto oracle = native.openFile("item.svh", text);
            CHECK(rfl::json::write(actual) ==
                  rfl::json::write(bodyCompletions(native, oracle, at)));
        }
    }
}

TEST_CASE("Body queries resolve containing class documentation from current source",
          "[method-body][general-body]") {
    const bool certify = GENERATE(false, true);
    struct Ticket {
        uint64_t contextEpoch;
        rfl::Generic payload;
        std::optional<uint64_t> methodBodyCertificate;
        std::optional<int> methodBodyVersion;
    };
    struct Descriptor {
        std::string documentUri, symbolPath, symbolName;
        uint32_t bufferId;
        uint64_t offset;
        slang::ast::SymbolKind symbolKind;
        bool labelOnly = false;
    };
    Workspace workspace;
    bodyConfig(workspace, true, false, certify);
    const std::string original =
        "class C;\nfunction int f(input int arg);\n  return arg;\nendfunction\nendclass\n";
    workspace.write("item.svh", original);
    ServerHarness server(workspace.params());
    auto doc = server.openFile("item.svh", original);
    auto baseline = doc.doc->getAnalysis();
    auto cls = baseline->getCompilation()->getPackage("owner")->find("C");
    REQUIRE(cls);
    auto text = original;
    text.replace(text.find("return arg;"), 11, "return arg + 123;");
    change(server, doc.m_uri, 2, text);
    doc.m_text = text;
    REQUIRE(state(server).active);
    auto response = server.getDocCompletion(
        {.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(text.find("arg +") + 1)});
    const auto& items = rfl::get<std::vector<lsp::CompletionItem>>(response);
    auto found = std::ranges::find_if(items, [](const auto& i) { return i.label == "arg"; });
    REQUIRE(found != items.end());
    REQUIRE(found->data);
    auto ticket = rfl::from_generic<Ticket>(*found->data).value();
    ticket.payload = rfl::to_generic<rfl::UnderlyingEnums>(
        Descriptor{doc.m_uri.str(), cls->getHierarchicalPath(), std::string(cls->name),
                   cls->location.buffer().getId(), cls->location.offset(), cls->kind});
    auto actual = server.getCompletionItemResolve(lsp::CompletionItem{
        .label = "C", .kind = lsp::CompletionItemKind::Class, .data = rfl::to_generic(ticket)});
    REQUIRE(actual.documentation);
    CHECK(rfl::json::write(actual.documentation).find("arg + 123") != std::string::npos);
    bodyConfig(workspace, false);
    ServerHarness native(workspace.params());
    auto oracle = native.openFile("item.svh", text);
    auto currentClass = oracle.doc->getAnalysis()->getCompilation()->getPackage("owner")->find("C");
    REQUIRE(currentClass);
    lsp::CompletionItem expected{.label = "C"};
    server::completions::MemberCompletionQuery::resolve(*currentClass, expected);
    CHECK(rfl::json::write(actual.documentation) == rfl::json::write(expected.documentation));
}

TEST_CASE("Body source projection leaves freshly parsed native locations unchanged",
          "[method-body][general-body]") {
    using namespace slang;
    SourceManager sm;
    const std::string oldText = "a\nb\nc\n", currentText = "a\nb\ninserted\nc\n";
    auto original = sm.assignText("projection.sv", oldText);
    const auto limit = uint32_t(sm.getSourceLocationEntryCount());
    auto current = sm.assignQueryText("projection.sv", currentText);
    auto mini = sm.assignQueryText("projection.sv", currentText);
    auto native = sm.assignQueryText("projection.sv", currentText);
    server::BodySourceProjection projection(sm, current.id, mini.id, oldText.find('c'),
                                            currentText.find('c'), limit);
    auto expected = server::toPosition(SourceLocation(current.id, currentText.find('c')), sm);
    CHECK(server::toPosition(SourceLocation(original.id, oldText.find('c')), sm) == expected);
    CHECK(server::toPosition(SourceLocation(native.id, currentText.find('c')), sm) == expected);
    CHECK(sm.isLatestData(original.id)); // Private query text never replaces the editor lookup.
}

TEST_CASE("Superseded body edits keep one lexical owner and bind only the latest query",
          "[method-body][query-scheduling]") {
    for (bool unsafe : {false, true}) {
        Workspace workspace;
        bodyConfig(workspace);
        BodyTestServer server(workspace.params());
        auto doc = server.openFile("item.svh", header);
        doc.doc->getAnalysis();
        change(server, doc.m_uri, 2, edited());
        REQUIRE(state(server).active);
        doc.m_text = edited();
        std::string latest;
        size_t parses = 0;
        for (int version = 3; version <= 10; ++version) {
            latest = header;
            latest.replace(latest.find("local_1 + charl"), 15,
                           version % 2 ? "local_0 + (charl" : "marker + earlier(charl)");
            if (unsafe && version == 10)
                latest.insert(latest.find("    int local_0"), "    `define BODY_GLOBAL 1\n");
            lsp::RequestContext ctx("textDocument/didChange", version);
            ctx.setCheckpointObserverForTesting(
                [&](std::string_view phase) { parses += phase == "before method mini parse"; });
            ctx.cancel();
            if (version % 2) {
                CHECK_THROWS_AS(change(server, doc.m_uri, version, latest, ctx),
                                lsp::RequestCancelled);
            }
            else {
                const auto previous = doc.m_text;
                size_t begin = 0, suffix = 0;
                while (begin < previous.size() && begin < latest.size() &&
                       previous[begin] == latest[begin])
                    ++begin;
                while (suffix < previous.size() - begin && suffix < latest.size() - begin &&
                       previous[previous.size() - suffix - 1] == latest[latest.size() - suffix - 1])
                    ++suffix;
                lsp::DidChangeTextDocumentParams params{
                    .textDocument = {.version = version, .uri = doc.m_uri},
                    .contentChanges = {lsp::TextDocumentContentChangePartial{
                        .range = {doc.getPosition(begin),
                                  doc.getPosition(previous.size() - suffix)},
                        .text = latest.substr(begin, latest.size() - begin - suffix)}}};
                CHECK_THROWS_AS(server.SlangServer::onDocDidChange(params, ctx),
                                lsp::RequestCancelled);
            }
            doc.m_text = latest;
            CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == latest + '\0');
            CHECK(doc.doc->getVersion() == version);
            REQUIRE(state(server).active);
        }
        CHECK(parses == 0);
        CHECK(state(server).accepted == 9);
        doc.m_text = latest;
        const auto at = latest.rfind("charl") + 2;
        auto actual = bodyCompletions(server, doc, at);
        CHECK(state(server).active == !unsafe);
        server.runBackgroundWork({});
        const auto diagnostics = rfl::json::write(doc.getDiagnostics());
        bodyConfig(workspace, false);
        ServerHarness native(workspace.params());
        auto oracle = native.openFile("item.svh", latest);
        CHECK(rfl::json::write(actual) == rfl::json::write(bodyCompletions(native, oracle, at)));
        CHECK(diagnostics == rfl::json::write(oracle.getDiagnostics()));
    }
}

TEST_CASE("Local body diagnostics match full design through errors repair and line shifts",
          "[method-body-diagnostics]") {
    Workspace workspace;
    bodyConfig(workspace);
    auto config = std::string("{\"indexingThreads\":1,\"flags\":\"--single-unit --threads 1\",") +
                  "\"compilationFilelist\":\"design.f\",\"experimentalMethodBodyQueries\":true,"
                  "\"experimentalMethodBodyDiagnostics\":true}";
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    struct DiagnosticState {
        uint64_t localDiagnosticUpdates;
        std::string localDiagnosticDecline;
    };
    std::vector<std::string> sequence;
    for (auto expr : {"local_0 + charl", "local_0 + absent_local", "local_0 + charl",
                      "local_0 +\n        charl", "local_0 + (charl"}) {
        auto text = header;
        text.replace(text.find("local_1 + charl"), 15, expr);
        sequence.push_back(text);
    }
    auto initializerError = header;
    initializerError.replace(initializerError.find("alpha + bravo"), 13, "missing_initializer");
    sequence.push_back(initializerError);
    auto noReturn = header;
    noReturn.replace(noReturn.find("return (local_1 + charl);"), 25, "alpha = bravo;");
    sequence.push_back(noReturn);
    sequence.push_back(header);
    int version = 1;
    for (const auto& text : sequence) {
        change(server, doc.m_uri, ++version, text);
        doc.m_text = text;
        REQUIRE(state(server).active);
        server.runBackgroundWork({});
        auto status =
            rfl::from_generic<DiagnosticState>(
                *server.executeCommand({.command = "slang.methodScopeStatus",
                                        .arguments = std::vector<lsp::LSPAny>{rfl::to_generic(
                                            std::string("compact"))}}))
                .value();
        INFO(status.localDiagnosticDecline);
        CHECK(status.localDiagnosticUpdates == uint64_t(version - 1));
        auto actual = rfl::json::write(doc.getDiagnostics());
        bodyConfig(workspace, false);
        {
            BodyTestServer native(workspace.params());
            native.setBuildFile((workspace.root / "design.f").string());
            auto oracle = native.openFile("item.svh", text);
            CHECK(actual == rfl::json::write(oracle.getDiagnostics()));
        }
        workspace.write(".slang/server.json", config);
    }
}

namespace {
const std::string localDiagnosticConfig =
    "{\"indexingThreads\":1,\"flags\":\"--single-unit --threads 1\","
    "\"compilationFilelist\":\"design.f\",\"experimentalMethodBodyQueries\":true,"
    "\"experimentalMethodBodyDiagnostics\":true}";
struct LocalDiagnosticState {
    uint64_t localDiagnosticUpdates;
    std::string localDiagnosticDecline;
};
LocalDiagnosticState diagnosticState(ServerHarness& server) {
    return rfl::from_generic<LocalDiagnosticState>(
               *server.executeCommand({.command = "slang.methodScopeStatus",
                                       .arguments = std::vector<lsp::LSPAny>{rfl::to_generic(
                                           std::string("compact"))}}))
        .value();
}
auto diagnosticMap(ServerHarness& server) {
    std::map<std::string, std::vector<std::string>> result;
    for (const auto& [key, values] : server.m_driver->diagClient->renderedDiagnostics()) {
        auto& out = result[key.str()];
        for (const auto& value : values)
            out.push_back(rfl::json::write(value));
        std::sort(out.begin(), out.end());
    }
    return result;
}
void checkNativeDiagnostics(Workspace& workspace, ServerHarness& server, const std::string& text,
                            bool comparePublications = false,
                            std::optional<lsp::Position> hoverAt = {},
                            std::string_view instance = {},
                            std::string_view sourceName = "item.svh") {
    std::optional<std::string> actualHover;
    if (hoverAt) {
        std::vector<std::string> phases;
        lsp::RequestContext ctx("local-type-hover", 205);
        ctx.setCheckpointObserverForTesting(
            [&](std::string_view phase) { phases.emplace_back(phase); });
        actualHover = rfl::json::write(server.getDocHover(
            {.textDocument = {.uri = URI::fromFile(workspace.root / std::string(sourceName))},
             .position = *hoverAt},
            ctx));
        INFO(rfl::json::write(phases));
        CHECK(state(server).active);
    }
    auto actual = diagnosticMap(server);
    std::ifstream input(workspace.root / ".slang/server.json");
    const std::string saved{std::istreambuf_iterator<char>(input), {}};
    bodyConfig(workspace, false);
    auto restore = slang::ScopeGuard([&] { workspace.write(".slang/server.json", saved); });
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    if (!instance.empty())
        REQUIRE(native.setActiveInstance(std::string(instance)));
    auto oracle = native.openFile(std::string(sourceName), text);
    if (hoverAt)
        CHECK(*actualHover == rfl::json::write(native.getDocHover(
                                  {.textDocument = {.uri = oracle.m_uri}, .position = *hoverAt})));
    auto expected = diagnosticMap(native);
    if (comparePublications) {
        // A local repair deliberately publishes [] to clear an old error; a
        // fresh Native server may never create that empty internal bucket.
        // First compare every actual client payload, including old URI routes,
        // so normalizing empty buckets cannot hide a stale diagnostic.
        std::unordered_set<URI> keys(server.client.diagnosticPublications.begin(),
                                     server.client.diagnosticPublications.end());
        keys.insert(native.client.diagnosticPublications.begin(),
                    native.client.diagnosticPublications.end());
        auto publicValues = [](auto& client, const URI& uri) {
            std::vector<std::string> result;
            for (const auto& diagnostic : client.getDiagnostics(uri))
                result.push_back(rfl::json::write(diagnostic));
            std::sort(result.begin(), result.end());
            return result;
        };
        for (const auto& uri : keys)
            CHECK(publicValues(server.client, uri) == publicValues(native.client, uri));
        REQUIRE(server.client.diagnosticVersions.contains(oracle.m_uri));
        REQUIRE(server.getDoc(oracle.m_uri));
        CHECK(server.client.diagnosticVersions.at(oracle.m_uri) ==
              server.getDoc(oracle.m_uri)->getVersion());
        std::erase_if(actual, [](const auto& item) { return item.second.empty(); });
        std::erase_if(expected, [](const auto& item) { return item.second.empty(); });
    }
    if (actual != expected) {
        INFO("Actual diagnostic map: " << rfl::json::write(actual));
        INFO("Native diagnostic map: " << rfl::json::write(expected));
        CHECK(actual == expected);
        return;
    }
    CHECK(actual == expected);
}
} // namespace

TEST_CASE("Local diagnostics update outside usage without rebuilding the native root",
          "[method-body-diagnostics]") {
    Workspace workspace;
    workspace.write(".slang/server.json", localDiagnosticConfig);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = header;
    text.replace(text.find("local_1 + charl"), 15, "marker + charl");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    REQUIRE(state(server).active);
    auto first = diagnosticState(server);
    CHECK(first.localDiagnosticUpdates == 1);
    CHECK(first.localDiagnosticDecline.empty());
    checkNativeDiagnostics(workspace, server, text, true);
    text.replace(text.find("marker + charl"), 14, "marker + charl + 10");
    change(server, doc.m_uri, 3, text);
    server.runBackgroundWork({});
    auto second = diagnosticState(server);
    INFO(second.localDiagnosticDecline);
    CHECK(second.localDiagnosticUpdates == 2);
    CHECK_FALSE(server.m_driver->comp->containsCurrentSource(workspace.root / "item.svh"));
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 3);
    checkNativeDiagnostics(workspace, server, text, true);
}

TEST_CASE("Local diagnostic cancellation publishes no partial state and retries safely",
          "[method-body-diagnostics][diagnostic-cancellation]") {
    for (auto boundary :
         {"after local diagnostic binding", "after local diagnostic procedure",
          "retained local diagnostic publication", "before local diagnostic publication"}) {
        DYNAMIC_SECTION(boundary) {
            Workspace workspace;
            workspace.write(".slang/server.json", localDiagnosticConfig);
            BodyTestServer server(workspace.params());
            server.setBuildFile((workspace.root / "design.f").string());
            auto doc = server.openFile("item.svh", header);
            doc.doc->getAnalysis();
            auto before = diagnosticMap(server);
            auto text = header;
            text.replace(text.find("alpha + bravo"), 13, "missing_initializer");
            change(server, doc.m_uri, 2, text);
            lsp::RequestContext ctx("background", 91);
            bool reached = false;
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == boundary) {
                    reached = true;
                    ctx.cancel();
                }
            });
            CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
            REQUIRE(reached);
            CHECK(before == diagnosticMap(server));
            CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
            CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
            server.runBackgroundWork({});
            auto result = diagnosticState(server);
            INFO(result.localDiagnosticDecline);
            CHECK(result.localDiagnosticUpdates == 1);
            CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
            checkNativeDiagnostics(workspace, server, text);
        }
    }
}

TEST_CASE("Local task diagnostics preserve timing caller invalidation",
          "[method-body-diagnostics]") {
    Workspace workspace;
    const std::string original = "class C; task update(input int value); int local_value = value; "
                                 "local_value += 1; endtask endclass\n";
    workspace.write("item.svh", original);
    workspace.write(".slang/server.json", localDiagnosticConfig);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("+= 1"), 4, "+= 2");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    auto first = diagnosticState(server);
    INFO(first.localDiagnosticDecline);
    CHECK(first.localDiagnosticUpdates == 1);
    checkNativeDiagnostics(workspace, server, text);
    text.insert(text.find("local_value +="), "#1; ");
    change(server, doc.m_uri, 3, text);
    server.runBackgroundWork({});
    auto second = diagnosticState(server);
    CHECK(second.localDiagnosticUpdates == 2);
    CHECK(second.localDiagnosticDecline.empty());
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Local diagnostics inherit warning pragmas and current line macro values",
          "[method-body-diagnostics]") {
    const bool certify = GENERATE(false, true);
    for (bool pragma : {false, true}) {
        DYNAMIC_SECTION(pragma) {
            Workspace workspace;
            auto original = header;
            if (pragma)
                original = "`pragma diagnostic ignore=\"unused-variable\"\n" + original;
            else {
                original = "`define BODY_LINE `__LINE__\n" + original;
                original.replace(original.rfind("return value;"), 13, "return `BODY_LINE;");
            }
            workspace.write("item.svh", original);
            auto cfg = localDiagnosticConfig;
            if (certify)
                cfg.insert(cfg.size() - 1, ",\"experimentalMethodBodyCertification\":true");
            workspace.write(".slang/server.json", cfg);
            BodyTestServer server(workspace.params());
            server.setBuildFile((workspace.root / "design.f").string());
            auto doc = server.openFile("item.svh", original);
            doc.doc->getAnalysis();
            auto text = original;
            text.replace(text.find("local_1 + charl"), 15,
                         pragma ? "local_0 + charl" : "local_0 +\n charl");
            change(server, doc.m_uri, 2, text);
            server.runBackgroundWork({});
            REQUIRE(state(server).active);
            auto result = diagnosticState(server);
            INFO(result.localDiagnosticDecline);
            CHECK(result.localDiagnosticUpdates == (pragma ? 1 : 0));
            if (!pragma)
                CHECK(result.localDiagnosticDecline ==
                      "later macro line value affects semantic structure");
            checkNativeDiagnostics(workspace, server, text);
        }
    }
}

TEST_CASE("Local diagnostics realize attributes local conflicts and vector types",
          "[method-body-diagnostics]") {
    for (const auto& declaration :
         {"(* keep = missing_attribute *) int extra = alpha;\n", "logic [7:0] local_0 = alpha;\n",
          "logic [127:0] extra = 128'h1;\n"}) {
        DYNAMIC_SECTION(declaration) {
            Workspace workspace;
            workspace.write(".slang/server.json", localDiagnosticConfig);
            BodyTestServer server(workspace.params());
            server.setBuildFile((workspace.root / "design.f").string());
            auto doc = server.openFile("item.svh", header);
            doc.doc->getAnalysis();
            auto text = header;
            text.insert(text.find("    int local_0"), declaration);
            change(server, doc.m_uri, 2, text);
            server.runBackgroundWork({});
            REQUIRE(state(server).active);
            auto result = diagnosticState(server);
            INFO(result.localDiagnosticDecline);
            CHECK(result.localDiagnosticUpdates == 1);
            checkNativeDiagnostics(workspace, server, text);
        }
    }
}

TEST_CASE("Local diagnostics track imported uses and evaluated generic parameters",
          "[method-body-diagnostics]") {
    for (bool generic : {false, true}) {
        DYNAMIC_SECTION(generic) {
            Workspace workspace;
            const std::string original =
                generic ? "class Box #(int N=1); int data; endclass\n"
                          "class C; function int work(input int arg); localparam int N=2; "
                          "Box#(N) box; return arg + N; endfunction endclass\n"
                        : "import prelude::*; class C; function int work(input int arg); "
                          "return arg + value; endfunction endclass\n";
            workspace.write("item.svh", original);
            workspace.write(".slang/server.json", localDiagnosticConfig);
            BodyTestServer server(workspace.params());
            server.setBuildFile((workspace.root / "design.f").string());
            auto doc = server.openFile("item.svh", original);
            doc.doc->getAnalysis();
            auto text = original;
            if (generic)
                text.replace(text.find("N=2"), 3, "N=3");
            else
                text.replace(text.find("arg + value"), 11, "arg + 1");
            change(server, doc.m_uri, 2, text);
            server.runBackgroundWork({});
            REQUIRE(state(server).active);
            auto result = diagnosticState(server);
            CHECK(result.localDiagnosticUpdates == 1);
            CHECK(result.localDiagnosticDecline.empty());
            checkNativeDiagnostics(workspace, server, text, true);
        }
    }
}

#if defined(__linux__) || defined(__APPLE__)
TEST_CASE("Local diagnostic owners retire with the context and preserve unsaved errors",
          "[method-body-diagnostics][frozen-retirement]") {
    Workspace workspace;
    auto config = localDiagnosticConfig;
    config.insert(1, "\"experimentalUvmPrefixCache\":true,");
    workspace.write(".slang/server.json", config);
    std::optional<std::string> previous;
    if (auto value = std::getenv("SLANG_UVM_GENERATION_EDIT_LIMIT"))
        previous = value;
    setenv("SLANG_UVM_GENERATION_EDIT_LIMIT", "1", 1);
    auto restore = slang::ScopeGuard([&] {
        if (previous)
            setenv("SLANG_UVM_GENERATION_EDIT_LIMIT", previous->c_str(), 1);
        else
            unsetenv("SLANG_UVM_GENERATION_EDIT_LIMIT");
    });
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto epoch = server.m_driver->completions.getCompletionEpoch();
    change(server, doc.m_uri, 2, edited());
    server.runBackgroundWork({});
    REQUIRE(diagnosticState(server).localDiagnosticUpdates == 1);
    auto text = header;
    text.replace(text.find("alpha + bravo"), 13, "missing_after_retirement");
    change(server, doc.m_uri, 3, text);
    server.runBackgroundWork({});
    CHECK(server.m_driver->completions.getCompletionEpoch() != epoch);
    CHECK_FALSE(state(server).active);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getVersion() == 3);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 3);
    auto actual = diagnosticMap(server);
    unsetenv("SLANG_UVM_GENERATION_EDIT_LIMIT");
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    auto oracle = native.openFile("item.svh", text);
    CHECK(actual == diagnosticMap(native));
}
#endif

TEST_CASE("Body macro proof preserves existing effects and admits unused new names",
          "[method-body][method-body-diagnostics][method-macro-proof]") {
    const bool existing = GENERATE(false, true);
    const bool certify = GENERATE(false, true);
    const bool returnsLine = GENERATE(false, true);
    Workspace workspace;
    const std::string prefix = "class C; function int work(input int arg);\n";
    const std::string original = prefix +
        (existing ? "`define SAVED_BODY_FLAG 1\n" : "") +
        "return arg; endfunction\n" +
        (returnsLine ? "function int later(); return `__LINE__; endfunction endclass\n" :
         "function void report(input int line_number); endfunction\n"
         "function void later(); report(`__LINE__); endfunction endclass\n");
    workspace.write("item.svh", original);
    auto config = localDiagnosticConfig;
    config.insert(1, std::string("\"experimentalMethodBodyMacroProof\":true,") +
                     (certify ? "\"experimentalMethodBodyCertification\":true," : ""));
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto& expression : {std::string("arg + 1"), std::string("arg + 22"),
                                    std::string("arg + 3")}) {
        auto current = original;
        current.replace(current.find("return arg;"), 11,
            (existing ? std::string{} : "`define NEW_BODY_VALUE 7\n") +
                "return " + expression + ";");
        change(server, doc.m_uri, ++version, current);
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(state(server).active);
        // Returning a changed line literal still requires the existing full
        // diagnostic path once; subsequent same-line edits are local again.
        CHECK(diagnosticState(server).localDiagnosticUpdates ==
              uint64_t(version - 1 - (!existing && returnsLine ? 1 : 0)));
        checkNativeDiagnostics(workspace, server, current);
    }
}

TEST_CASE("Public macro queries observe newly admitted definitions",
          "[method-body][method-macro-proof][method-macro-public]") {
    const auto operation = GENERATE("hover", "source", "expanded", "both", "define", "complete", "expand");
    Workspace workspace;
    const std::string original = "class C; function int work(input int arg);\n"
                                 "return arg; endfunction endclass\n";
    workspace.write("item.svh", original);
    auto config = localDiagnosticConfig;
    config.insert(1, "\"experimentalMethodBodyMacroProof\":true,");
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto current = original;
    current.replace(current.find("return arg;"), 11,
        "`define NEW_BODY_VALUE 7\nreturn arg + `NEW_BODY_VALUE;");
    change(server, doc.m_uri, 2, current);
    server.runBackgroundWork({});
    INFO(state(server).decline);
    REQUIRE(state(server).active);
    doc.m_text = current;
    auto query = [&](BodyTestServer& instance, DocumentHandle& handle, const char* output) {
        const auto at = current.rfind("NEW_BODY_VALUE") + 5;
        if (std::string_view(operation) == "source" || std::string_view(operation) == "expanded" ||
            std::string_view(operation) == "both")
            return rfl::json::write(instance.previewHover(
                {.textDocument={.uri=handle.m_uri}, .position=handle.getPosition(at), .macroView=operation}));
        if (std::string_view(operation) == "hover")
            return rfl::json::write(instance.getDocHover(
                {.textDocument={.uri=handle.m_uri}, .position=handle.getPosition(at)}));
        if (std::string_view(operation) == "define")
            return rfl::json::write(instance.getDocDefinition(
                {.textDocument={.uri=handle.m_uri}, .position=handle.getPosition(at)}));
        if (std::string_view(operation) == "complete") {
            auto response = instance.getDocCompletion(
                {.textDocument={.uri=handle.m_uri}, .position=handle.getPosition(at)});
            auto list = rfl::get_if<lsp::CompletionList>(&response);
            REQUIRE(list);
            CHECK(list->isIncomplete);
            CHECK(std::ranges::any_of(list->items, [](const auto& item) {
                return item.label == "`NEW_BODY_VALUE";
            }));
            auto items = list->items;
            for (auto& item : items) {
                item = instance.getCompletionItemResolve(item);
                item.data.reset();
            }
            return rfl::json::write(items);
        }
        auto destination = workspace.root / output;
        REQUIRE(instance.expandMacros({.src=(workspace.root/"item.svh").string(),
                                       .dst=destination.string()}));
        std::ifstream stream(destination);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    auto actual = query(server, doc, "actual.sv");
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    auto reference = native.openFile("item.svh", current);
    CHECK(actual == query(native, reference, "expected.sv"));
}

TEST_CASE("Body macro proof rejects changes observed by later conditional code",
          "[method-body][method-body-diagnostics][method-macro-proof]") {
    Workspace workspace;
    const std::string original =
        "class C; function int work(input int arg);\nreturn arg; endfunction\n"
        "function int later();\n`ifdef BODY_FLAG\nreturn 1;\n`else\nreturn 2;\n`endif\n"
        "endfunction endclass\n";
    workspace.write("item.svh", original);
    auto config = localDiagnosticConfig;
    config.insert(1, "\"experimentalMethodBodyMacroProof\":true,");
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto current = original;
    current.replace(current.find("return arg;"), 11, "`define BODY_FLAG 1\nreturn arg;");
    change(server, doc.m_uri, 2, current);
    server.runBackgroundWork({});
    CHECK_FALSE(state(server).active);
    checkNativeDiagnostics(workspace, server, current);
}

TEST_CASE("Fresh macro effects survive ranged incomplete and repaired body edits",
          "[method-macro-proof][macro-boundaries]") {
    const bool ranged = GENERATE(false, true);
    Workspace workspace;
    const std::string head = "class C; function int work(input int arg);\n";
    const std::string tail = "endfunction endclass\n";
    const std::string original = head + "return arg;\n" + tail;
    workspace.write("item.svh", original);
    auto config = localDiagnosticConfig;
    config.insert(1, "\"experimentalMethodBodyMacroProof\":true,\"experimentalMethodBodyCertification\":true,");
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const std::vector<std::string> bodies = {
        "`define LOCAL_VALUE 7\nreturn arg + `LOCAL_VALUE;\n",
        "`define LOCAL_VALUE 9\nreturn arg + `LOCAL_VALUE;\n",
        "`define LOCAL_VALUE 9\n`undef LOCAL_VALUE\nreturn arg;\n",
        "`undef LOCAL_VALUE\nreturn arg + 1;\n",
        "return arg +;\n",
        "`define LOCAL_VALUE 3\nreturn missing_symbol + `LOCAL_VALUE;\n",
        "return arg + 2;\n"};
    int version = 1;
    for (const auto& body : bodies) {
        const auto current = head + body + tail;
        if (ranged)
            server.SlangServer::onDocDidChange(
                {.textDocument={.version=++version,.uri=doc.m_uri},
                 .contentChanges={lsp::TextDocumentContentChangePartial{
                     .range={.start=doc.getPosition(head.size()),
                             .end=doc.getPosition(doc.m_text.find("endfunction"))},
                     .text=body}}});
        else change(server, doc.m_uri, ++version, current);
        doc.m_text = current;
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(state(server).active);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        CHECK(server.m_driver->getDocument(doc.m_uri)->getVersion() == version);
        CHECK(server.client.diagnosticVersions.at(doc.m_uri) == version);
        checkNativeDiagnostics(workspace, server, current, true);
    }
}

TEST_CASE("Unchanged undef effects permit later edits on the same source line",
          "[method-macro-proof][macro-boundaries]") {
    Workspace workspace;
    const std::string original = "`define SAVED_FLAG 1\nclass C; function int work(input int arg);\n"
                                 "`undef SAVED_FLAG return arg;\nendfunction endclass\n";
    workspace.write("item.svh", original);
    auto config = localDiagnosticConfig;
    config.insert(1, "\"experimentalMethodBodyMacroProof\":true,");
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto current = original;
    current.replace(current.find("return arg;"), 11, "return arg + 1;");
    change(server, doc.m_uri, 2, current);
    server.runBackgroundWork({});
    INFO(state(server).decline);
    INFO(diagnosticState(server).localDiagnosticDecline);
    CHECK(state(server).active);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    checkNativeDiagnostics(workspace, server, current, true);
}

TEST_CASE("Inactive original macro directives do not obstruct ordinary body edits",
          "[method-macro-proof][macro-boundaries]") {
    Workspace workspace;
    const std::string original = "class C; function int work(input int arg);\n"
        "`ifdef ABSENT_FLAG\n`define DORMANT 7\n`endif\nreturn arg;\nendfunction endclass\n";
    workspace.write("item.svh", original);
    auto config = localDiagnosticConfig;
    config.insert(1, "\"experimentalMethodBodyMacroProof\":true,");
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    std::vector<std::string> sequence;
    auto ordinary=original;
    ordinary.replace(ordinary.find("return arg;"),11,"return arg + 1;");
    sequence.push_back(ordinary);
    auto changed=original;
    changed.replace(changed.find("DORMANT 7"),9,"DORMANT 9");
    sequence.push_back(changed);
    auto activated=changed;
    activated.replace(activated.find("`ifdef"),6,"`ifndef");
    sequence.push_back(activated);
    sequence.push_back("class C; function int work(input int arg);\nreturn arg + 2;\nendfunction endclass\n");
    int version=1;
    for(const auto& current : sequence) {
        change(server,doc.m_uri,++version,current);
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(state(server).active);
        CHECK(diagnosticState(server).localDiagnosticUpdates==uint64_t(version-1));
        checkNativeDiagnostics(workspace,server,current,true);
    }
}

TEST_CASE("Included negative macro observations forbid new body definitions",
          "[method-macro-proof][macro-boundaries]") {
    Workspace workspace;
    workspace.write("observer.svh", "`ifdef BODY_FLAG\nint observed_field;\n`endif\n");
    const std::string original = "class C; function int work(input int arg);\n"
        "return arg; endfunction\n`include \"observer.svh\"\nendclass\n";
    workspace.write("item.svh", original);
    auto config = localDiagnosticConfig;
    config.insert(1, "\"experimentalMethodBodyMacroProof\":true,");
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto current = original;
    current.replace(current.find("return arg;"), 11, "`define BODY_FLAG 1\nreturn arg;");
    change(server, doc.m_uri, 2, current);
    server.runBackgroundWork({});
    CHECK_FALSE(state(server).active);
    checkNativeDiagnostics(workspace, server, current, true);
}

TEST_CASE("Task macro effects retain local timing diagnostics through add and remove",
          "[method-macro-proof][macro-boundaries]") {
    Workspace workspace;
    const std::string head="class C; task work(input int arg);\n";
    const std::string tail="endtask endclass\n";
    const auto original=head+"arg = arg + 1;\n"+tail;
    workspace.write("item.svh",original);
    auto config=localDiagnosticConfig;
    config.insert(1,"\"experimentalMethodBodyMacroProof\":true,");
    workspace.write(".slang/server.json",config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root/"design.f").string());
    auto doc=server.openFile("item.svh",original);
    doc.doc->getAnalysis();
    int version=1;
    for(const auto& body : {"`define TASK_VALUE 2\n#1 arg = arg + `TASK_VALUE;\n",
                           "`define TASK_VALUE 3\n#2 arg = arg + `TASK_VALUE;\n",
                           "arg = arg + 4;\n"}) {
        const auto current=head+body+tail;
        change(server,doc.m_uri,++version,current);
        server.runBackgroundWork({});
        INFO(state(server).decline);
        CHECK(state(server).active);
        CHECK(diagnosticState(server).localDiagnosticUpdates==uint64_t(version-1));
        checkNativeDiagnostics(workspace,server,current,true);
    }
}

TEST_CASE("Observed macro values and definition locations require native reconstruction",
          "[method-macro-proof][macro-boundaries]") {
    const bool move=GENERATE(false,true);
    Workspace workspace;
    const std::string original="class C; function int work(input int arg);\n"
        "`define OBSERVED_VALUE 1\nreturn arg; endfunction\n"
        "function int later(); return `OBSERVED_VALUE; endfunction endclass\n";
    workspace.write("item.svh",original);
    auto config=localDiagnosticConfig;
    config.insert(1,"\"experimentalMethodBodyMacroProof\":true,");
    workspace.write(".slang/server.json",config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root/"design.f").string());
    auto doc=server.openFile("item.svh",original);
    doc.doc->getAnalysis();
    auto current=original;
    if(move) current.insert(current.find("`define"),"\n");
    else current.replace(current.find("VALUE 1"),7,"VALUE 2");
    change(server,doc.m_uri,2,current);
    server.runBackgroundWork({});
    CHECK_FALSE(state(server).active);
    checkNativeDiagnostics(workspace,server,current,true);
    doc.m_text=current;
    const auto at=current.rfind("OBSERVED_VALUE")+4;
    auto actual=rfl::json::write(server.getDocHover(
        {.textDocument={.uri=doc.m_uri},.position=doc.getPosition(at)}));
    bodyConfig(workspace,false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root/"design.f").string());
    auto reference=native.openFile("item.svh",current);
    CHECK(actual==rfl::json::write(native.getDocHover(
        {.textDocument={.uri=reference.m_uri},.position=reference.getPosition(at)})));
}

TEST_CASE("Fresh body macros cannot bypass outside token-pasted lookup proof",
          "[method-macro-proof][macro-boundaries]") {
    const bool lookup=GENERATE(false,true);
    Workspace workspace;
    const std::string original=std::string(lookup ?
        "`define F1 report(1);\n`define LATER(N) `F``N\n" :
        "`define LATER(N) report(arg``N);\n") +
        "class C; int arg1; function int work(input int arg);\nreturn arg; endfunction\n"
        "function void report(input int n); endfunction\n"
        "function void later(); `LATER(1) endfunction endclass\n";
    workspace.write("item.svh",original);
    auto config=localDiagnosticConfig;
    config.insert(1,"\"experimentalMethodBodyMacroProof\":true,");
    workspace.write(".slang/server.json",config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root/"design.f").string());
    auto doc=server.openFile("item.svh",original);
    doc.doc->getAnalysis();
    auto current=original;
    current.insert(current.find("return arg;"),"`define NEW_BODY_VALUE 7\n");
    change(server,doc.m_uri,2,current);
    server.runBackgroundWork({});
    CHECK_FALSE(state(server).active);
    checkNativeDiagnostics(workspace,server,current,true);
}

TEST_CASE("Macro proof remains opt in and zero budget disables new eligibility",
          "[method-macro-proof][macro-boundaries]") {
    const int mode=GENERATE(0,1,2);
    Workspace workspace;
    const std::string original="class C; function int work(input int arg);\n"
                                 "return arg; endfunction endclass\n";
    workspace.write("item.svh",original);
    auto config=localDiagnosticConfig;
    if(mode) config.insert(1,"\"experimentalMethodBodyMacroProof\":true,");
    if(mode==1) config.insert(1,"\"methodBodyMacroProofMaxMiB\":0,");
    workspace.write(".slang/server.json",config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root/"design.f").string());
    auto doc=server.openFile("item.svh",original);
    doc.doc->getAnalysis();
    auto current=original;
    current.insert(current.find("return arg;"),"`define NEW_BODY_VALUE 7\n");
    change(server,doc.m_uri,2,current);
    server.runBackgroundWork({});
    CHECK(state(server).active==(mode==2));
    CHECK(diagnosticState(server).localDiagnosticUpdates==(mode==2?1:0));
    checkNativeDiagnostics(workspace,server,current,true);
}

TEST_CASE("Removing existing preprocessing state cannot retain stale method suffixes",
          "[method-body][method-body-diagnostics]") {
    Workspace workspace;
    const std::string original =
        "class C; function int work(input int arg);\n"
        "`define SAVED_BODY_FLAG 1\n"
        "return arg; endfunction\n"
        "function int later(); return `SAVED_BODY_FLAG; endfunction endclass\n";
    workspace.write("item.svh", original);
    workspace.write(".slang/server.json", localDiagnosticConfig);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("`define SAVED_BODY_FLAG 1"), 25, "// removed definition");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    CHECK_FALSE(state(server).active);
    CHECK(state(server).decline == "body changes preprocessing state");
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Local diagnostics preserve nonlocal persistent driver analysis",
          "[method-body-diagnostics]") {
    for (bool initializer : {false, true}) {
        DYNAMIC_SECTION(initializer) {
            Workspace workspace;
            workspace.write("prelude.sv",
                            "package prelude; int value; function automatic int bump(int arg); "
                            "value=arg; return arg; endfunction endpackage\n");
            const std::string original =
                initializer
                    ? "class C; function int work(input int arg); int "
                      "local_value=prelude::bump(arg); return local_value; endfunction endclass\n"
                    : "class C; function int work(input int arg); prelude::value=arg; return arg; "
                      "endfunction endclass\n";
            workspace.write("item.svh", original);
            workspace.write(".slang/server.json", localDiagnosticConfig);
            BodyTestServer server(workspace.params());
            server.setBuildFile((workspace.root / "design.f").string());
            auto doc = server.openFile("item.svh", original);
            doc.doc->getAnalysis();
            auto text = original;
            text.insert(text.rfind("return ") + 7, "1 + ");
            change(server, doc.m_uri, 2, text);
            server.runBackgroundWork({});
            auto result = diagnosticState(server);
            INFO(result.localDiagnosticDecline);
            CHECK(result.localDiagnosticUpdates == 1);
            CHECK(result.localDiagnosticDecline.empty());
            checkNativeDiagnostics(workspace, server, text);
        }
    }
}

TEST_CASE("Local diagnostics distinguish untyped generic value widths",
          "[method-body-diagnostics]") {
    Workspace workspace;
    const std::string original =
        "class Box #(parameter N=1); int values[($bits(N)==32)?2:0]; endclass\n"
        "class C; function int work(input int arg); localparam N=32'd2; "
        "Box#(N) box; return arg; endfunction endclass\n";
    workspace.write("item.svh", original);
    workspace.write(".slang/server.json", localDiagnosticConfig);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("32'd2"), 5, "64'd2");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    auto result = diagnosticState(server);
    INFO(result.localDiagnosticDecline);
    CHECK(result.localDiagnosticUpdates == 1);
    CHECK(result.localDiagnosticDecline.empty());
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Local hover matches native values and macro views without rebuilding the document",
          "[method-body][general-body][body-hover]") {
    Workspace workspace;
    bodyConfig(workspace);
    workspace.write(
        "owner.sv",
        "`define VALUE 7\n`define SUM(x) ((x) + `VALUE)\npackage owner;\n`include \"item.svh\"\n"
        "`undef VALUE\n`define VALUE missing_suffix_value\nendpackage\n");
    const std::string original =
        "class Base; int field; function int read(); return field; endfunction endclass\n"
        "class C extends Base;\nfunction int run(input int arg);\n"
        " int local_value = `SUM(1);\n return local_value + read() + arg;\n"
        "endfunction\nendclass\n";
    workspace.write("item.svh", original);
    struct Probe {
        size_t offset;
        std::string view;
    };
    std::vector<std::string> actual;
    std::vector<Probe> probes;
    auto text = original;
    text.replace(text.find("`SUM(1)"), 7, "`SUM(3)");
    text.replace(text.find(" int local_value"), 16, " longint local_value");
    text.insert(text.find(" return local_value"), " // added line\n");
    probes = {{text.find("return local_value") + 9, ""},
              {text.find("read() + arg") + 1, ""},
              {text.find("read() + arg") + 10, ""},
              {text.find("// added line") + 4, ""}};
    for (auto view : {"source", "expanded", "both"})
        probes.push_back({text.find("`SUM") + 2, view});
    auto hover = [&](ServerHarness& s, DocumentHandle& doc, const Probe& probe) {
        if (probe.view.empty())
            return rfl::json::write(s.getDocHover(
                {.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(probe.offset)}));
        return rfl::json::write(s.previewHover({.textDocument = {.uri = doc.m_uri},
                                                .position = doc.getPosition(probe.offset),
                                                .macroView = probe.view}));
    };
    {
        ServerHarness server(workspace.params());
        auto doc = server.openFile("item.svh", original);
        doc.doc->getAnalysis();
        lsp::RequestContext ctx("textDocument/didChange", 17);
        bool journal = false;
        ctx.setCheckpointObserverForTesting([&](std::string_view at) {
            if (at == "method macro environment journal")
                journal = true;
            CHECK(at != "method macro environment replay");
        });
        change(server, doc.m_uri, 2, text, ctx);
        doc.m_text = text;
        REQUIRE(journal);
        REQUIRE(state(server).active);
        CHECK_FALSE(doc.doc->hasAnalysis());
        for (const auto& probe : probes) {
            actual.push_back(hover(server, doc, probe));
            CHECK(state(server).active);
            CHECK_FALSE(doc.doc->hasAnalysis());
        }
        CHECK(actual[0].find("longint") != std::string::npos);
        CHECK(actual[3] == "null");
        CHECK(actual[4].find("SUM(3)") != std::string::npos);
        lsp::RequestContext canceled("textDocument/hover", 18);
        canceled.setCheckpointObserverForTesting([&](std::string_view at) {
            if (at == "before method hover response")
                canceled.cancel();
        });
        CHECK_THROWS_AS(server.getDocHover({.textDocument = {.uri = doc.m_uri},
                                            .position = doc.getPosition(probes[0].offset)},
                                           canceled),
                        lsp::RequestCancelled);
        CHECK(state(server).active);
        CHECK(hover(server, doc, probes[0]) == actual[0]);
    }
    bodyConfig(workspace, false);
    ServerHarness native(workspace.params());
    auto oracle = native.openFile("item.svh", text);
    for (size_t i = 0; i < probes.size(); ++i)
        CHECK(hover(native, oracle, probes[i]) == actual[i]);
}

TEST_CASE("Unchanged method signature hover reuses certified current binding",
          "[method-body][general-body][body-hover][signature-hover]") {
    const std::string context = GENERATE("class", "package", "module-task", "non-ansi");
    const std::string commentFormat = GENERATE("markdown", "plaintext", "raw");
    const std::string macroView = GENERATE("source", "expanded", "both");
    const bool inlineUnicode = GENERATE(false, true);
    CAPTURE(context, commentFormat, macroView, inlineUnicode);
    Workspace workspace;
    auto configure = [&](bool enabled) {
        bodyConfig(workspace, enabled);
        std::ifstream input(workspace.root / ".slang/server.json");
        std::string config{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        REQUIRE(config.ends_with("}"));
        config.pop_back();
        config += ",\"hovers\":{\"docCommentFormat\":\"" + commentFormat +
                  "\",\"macroView\":\"" + macroView + "\"}}";
        workspace.write(".slang/server.json", config);
    };
    configure(true);
    const bool task = context == "module-task";
    const bool nonAnsi = context == "non-ansi";
    const std::string begin = context == "package" ? "" : task ? "module M;\n" : "class C;\n";
    const std::string end = context == "package" ? "" : task ? "endmodule\n" : "endclass\n";
    const std::string prototype = std::string(task ? "task automatic run" : "function automatic int run") +
        (nonAnsi ? "(arg);\n input int arg;\n" : "(input int arg=3);\n");
    const std::string body = " int local_value=arg+1;\n" +
                             std::string(task ? " arg=local_value;\n" : " return local_value;\n");
    const auto original = begin + "`define SIGNATURE_DOC_CONTEXT 1\n"
                          "/* Header block text. */\n"
                          "// Signature **documentation**.\n// Second line.\n" +
                          (inlineUnicode ? "/* 中文🙂 */ " : "") + prototype + body + (task ? "endtask\n" : "endfunction\n") + end;
    workspace.write("item.svh", original);
    auto text = original;
    text.replace(text.find(body), body.size(),
        " longint local_value=arg+2;\n // current body line\n" +
        std::string(task ? " arg=local_value+1;\n" : " return local_value+1;\n"));
    const std::vector<size_t> probes{text.find("run(")+1, text.find("arg")+1};
    std::vector<std::string> actual;
    {
        ServerHarness server(workspace.params());
        auto doc = server.openFile("item.svh", original);
        doc.doc->getAnalysis();
        change(server, doc.m_uri, 2, text);
        doc.m_text = text;
        REQUIRE(state(server).active);
        CHECK_FALSE(doc.doc->hasAnalysis());
        for (auto offset : probes) {
            actual.push_back(rfl::json::write(server.getDocHover(
                {.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(offset)})));
            CHECK(actual.back() != "null");
            CHECK(state(server).active);
            CHECK_FALSE(doc.doc->hasAnalysis());
        }
    }
    configure(false);
    ServerHarness native(workspace.params());
    auto oracle = native.openFile("item.svh", text);
    for (size_t i = 0; i < probes.size(); ++i)
        CHECK(actual[i] == rfl::json::write(native.getDocHover(
            {.textDocument = {.uri = oracle.m_uri}, .position = oracle.getPosition(probes[i])})));
}

TEST_CASE("Local hover preserves local type and field values in package functions and module tasks",
          "[method-body][general-body][body-hover]") {
    for (int context : {0, 1, 2}) {
        const bool module = context != 0, local = context != 2;
        DYNAMIC_SECTION(context) {
            Workspace workspace;
            bodyConfig(workspace);
            const std::string original =
                (module ? (local ? "module M; task automatic run(input int arg);\n"
                                 : "module M; task run(input int arg);\n")
                        : "function automatic int run(input int arg);\n") +
                std::string(" typedef struct packed { logic [7:0] value; } payload_t;\n"
                            " payload_t payload;\n payload.value = arg;\n") +
                (module ? "endtask endmodule\n" : "return payload.value; endfunction\n");
            workspace.write("item.svh", original);
            auto text = original;
            text.replace(text.find("[7:0]"), 5, "[15:0]");
            std::vector<std::string> actual;
            const std::vector<size_t> offsets{text.find("payload.value =") + 2,
                                              text.find("payload.value =") + 10};
            {
                ServerHarness server(workspace.params());
                auto doc = server.openFile("item.svh", original);
                doc.doc->getAnalysis();
                change(server, doc.m_uri, 2, text);
                doc.m_text = text;
                REQUIRE(state(server).active);
                for (auto offset : offsets) {
                    actual.push_back(rfl::json::write(
                        server.getDocHover({.textDocument = {.uri = doc.m_uri},
                                            .position = doc.getPosition(offset)})));
                    CHECK(actual.back() != "null");
                    CHECK(state(server).active == local);
                    CHECK(doc.doc->hasAnalysis() == !local);
                }
            }
            bodyConfig(workspace, false);
            ServerHarness native(workspace.params());
            auto doc = native.openFile("item.svh", text);
            for (size_t i = 0; i < offsets.size(); ++i)
                CHECK(actual[i] == rfl::json::write(native.getDocHover(
                                       {.textDocument = {.uri = doc.m_uri},
                                        .position = doc.getPosition(offsets[i])})));
        }
    }
}

TEST_CASE("Method certificate renewal preserves one diagnostic baseline across edit windows",
          "[method-certificate-renewal]") {
    const bool enabled = GENERATE(false, true);
    Workspace workspace;
    auto config = localDiagnosticConfig;
    config.insert(config.size() - 1, std::string(",\"experimentalMethodBodyCertificateRenewal\":") +
                                         (enabled ? "true" : "false"));
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto* owner = server.m_driver.get();
    const auto epoch = owner->completions.getCompletionEpoch();
    struct RenewalState {
        uint64_t certificateRenewals;
        size_t retainedHandles;
    };
    std::optional<lsp::CompletionItem> stale;
    for (int edit = 1; edit <= (enabled ? 193 : 65); ++edit) {
        auto text = header;
        text.replace(text.find("local_1 + charl"), 15,
                     std::string(edit % 3 == 0 ? "local_0 + absent_local + "
                                               : "local_0 +\n charl + ") +
                         std::to_string(edit));
        change(server, doc.m_uri, edit + 1, text);
        doc.m_text = text;
        if (!enabled && edit == 65) {
            CHECK_FALSE(state(server).active);
            break;
        }
        REQUIRE(state(server).active);
        CHECK(state(server).accepted == uint64_t(edit));
        server.runBackgroundWork({});
        CHECK(server.m_driver.get() == owner);
        CHECK(owner->completions.getCompletionEpoch() == epoch);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(edit));
        auto status =
            rfl::from_generic<RenewalState>(
                *server.executeCommand({.command = "slang.methodScopeStatus",
                                        .arguments = std::vector<lsp::LSPAny>{rfl::to_generic(
                                            std::string("compact"))}}))
                .value();
        CHECK(status.certificateRenewals == uint64_t((edit - 1) / 64));
        CHECK(status.retainedHandles == 0);
        if (edit == 64 || edit == 65 || edit == 128 || edit == 129 || edit == 193) {
            auto at = text.find("return (") + 10;
            auto response = server.getDocCompletion(
                {.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(at)});
            auto items = rfl::get<std::vector<lsp::CompletionItem>>(response);
            auto found = std::ranges::find_if(items, [](const auto& item) {
                return item.label == "local_0";
            });
            REQUIRE(found != items.end());
            if (stale) {
                auto resolved = server.getCompletionItemResolve(*stale);
                CHECK_FALSE(resolved.data);
            }
            stale = *found;
            checkNativeDiagnostics(workspace, server, text);
            workspace.write(".slang/server.json", config);
        }
    }
}

TEST_CASE("Renewal retains cancellation safety and rejects changed outer inputs",
          "[method-certificate-renewal]") {
    auto invalidate = GENERATE("none", "signature", "outside", "dependency", "macro");
    Workspace workspace;
    auto config = localDiagnosticConfig;
    config.insert(config.size() - 1, ",\"experimentalMethodBodyCertificateRenewal\":true");
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    for (int edit = 1; edit <= 64; ++edit) {
        auto text = header;
        text.replace(text.find("local_1 + charl"), 15, "local_0 + " + std::to_string(edit));
        change(server, doc.m_uri, edit + 1, text);
    }
    REQUIRE(state(server).accepted == 64);
    auto text = header;
    lsp::RequestContext cancellation("textDocument/didChange", 23);
    bool reached = false;
    cancellation.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "method certificate renewal") {
            reached = true;
            cancellation.cancel();
        }
    });
    CHECK_THROWS_AS(change(server, doc.m_uri, 66, text, cancellation), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(doc.doc->getVersion() == 66);
    REQUIRE(state(server).accepted == 65);
    CHECK(state(server).active);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    if (std::string_view(invalidate) == "signature")
        text.replace(text.find("input int alpha"), 15, "input longint alpha");
    else if (std::string_view(invalidate) == "outside")
        text.replace(text.find("int marker"), 10, "longint marker");
    else if (std::string_view(invalidate) == "dependency")
        workspace.write("prelude.sv", "package prelude; int changed; endpackage\n");
    else if (std::string_view(invalidate) == "macro")
        text.insert(text.find("int local_0"), "`define UNSAFE_RENEWAL 1\n");
    else
        text.replace(text.find("local_1 + charl"), 15, "local_0 + charl");
    change(server, doc.m_uri, 67, text);
    CHECK(state(server).active == (std::string_view(invalidate) == "none"));
    // The disk witness rejects reuse before a watcher event. Notify native caches
    // as an editor would before checking the rebuilt full-design diagnostics.
    if (std::string_view(invalidate) == "dependency")
        server.onWorkspaceDidChangeWatchedFiles(
            {.changes = {{.uri = URI::fromFile(workspace.root / "prelude.sv"),
                          .type = lsp::FileChangeType::Changed}}});
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Cooperative Native and renewed methods retain the hard source-storage bound",
          "[method-certificate-renewal][frozen-retirement]") {
    const bool enabled = GENERATE(false, true);
    Workspace workspace;
    auto config = localDiagnosticConfig;
    config.insert(config.size() - 1, std::string(",\"experimentalCooperativeAnalysis\":true,"
                                                 "\"experimentalMethodBodyCertificateRenewal\":") +
                                         (enabled ? "true" : "false"));
    workspace.write(".slang/server.json", config);
    struct Harness : BodyTestServer {
        using BodyTestServer::BodyTestServer;
        using SlangServer::m_contextBudget;
        using SlangServer::m_contextEpoch;
        using SlangServer::m_contextHardBudget;
    };
    Harness server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto epoch = server.m_driver->completions.getCompletionEpoch();
    server.m_contextEpoch = epoch;
    server.m_contextBudget = 1;
    server.m_contextHardBudget = 1; // Above the hard high-water mark with no library experiments.
    auto text = edited();
    change(server, doc.m_uri, 2, text);
    CHECK(server.m_driver->completions.getCompletionEpoch() == epoch);
    server.runBackgroundWork({}); // Admit current text before any semantic consumer.
    CHECK(server.m_driver->completions.getCompletionEpoch() != epoch);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    server.m_driver->getDocument(doc.m_uri)->getAnalysis();
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, text);
}

namespace {
std::string certificationConfig(bool enabled = true) {
    auto config = localDiagnosticConfig;
    config.insert(config.size() - 1,
                  std::string(",\"experimentalMethodBodyCertificateRenewal\":true,"
                              "\"experimentalCooperativeAnalysis\":true,"
                              "\"experimentalMethodBodyCertification\":") +
                      (enabled ? "true" : "false"));
    return config;
}
struct CertificationServer : BodyTestServer {
    using BodyTestServer::BodyTestServer;
    using SlangServer::backgroundGracePeriod;
    using SlangServer::m_config;
    using SlangServer::m_contextBudget;
    using SlangServer::m_contextEpoch;
    using SlangServer::m_contextHardBudget;
    using SlangServer::m_methodScope;
};
} // namespace

TEST_CASE("Macro identity witness cancellation preserves incoming text and recovers",
          "[method-macro-proof][macro-boundaries][macro-cancellation]") {
    const bool cached = GENERATE(false, true);
    Workspace workspace;
    const std::string original = "class C; function int work(input int arg);\n"
        "return arg; endfunction\nfunction void report(input int n); endfunction\n"
        "function void later(); report(`__LINE__); endfunction endclass\n";
    workspace.write("item.svh", original);
    auto config = localDiagnosticConfig;
    config.insert(1, "\"experimentalMethodBodyMacroProof\":true,\"experimentalMethodBodyCertification\":true,");
    workspace.write(".slang/server.json", config);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto make = [&](int value, bool macro) {
        auto current = original;
        current.replace(current.find("return arg;"), 11,
            (macro ? "`define NEW_BODY_VALUE 7\n" : "") +
                std::string("return arg + ") + std::to_string(value) + ";");
        return current;
    };
    change(server, doc.m_uri, 2, make(1, false));
    server.runBackgroundWork({});
    REQUIRE(state(server).active);
    int version = 3;
    if (cached) {
        change(server, doc.m_uri, version++, make(1, true));
        server.runBackgroundWork({});
        REQUIRE(state(server).active);
    }
    const auto current = make(2, true);
    lsp::RequestContext ctx("macro-witness-cancel", version);
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == (cached ? "reuse outside macro lookup identity proof" :
                               "before outside macro lookup identity witness commit")) {
            reached = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(change(server, doc.m_uri, version, current, ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getVersion() == version);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == current + '\0');
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, current, true);
    const auto repaired = make(3, true);
    change(server, doc.m_uri, ++version, repaired);
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, repaired, true);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == version);
}

TEST_CASE("Cached macro identity properties never replace physical input authentication",
          "[method-macro-proof][macro-boundaries][macro-input-auth]") {
    Workspace workspace;
    workspace.write("dependency.svh", "`define OBSERVED 1\n");
    const std::string original = "`include \"dependency.svh\"\n"
        "class C; function int work(input int arg);\nreturn arg; endfunction\n"
        "function int later(); return `OBSERVED; endfunction endclass\n";
    workspace.write("item.svh", original);
    auto config = localDiagnosticConfig;
    config.insert(1, "\"experimentalMethodBodyMacroProof\":true,\"experimentalMethodBodyCertification\":true,");
    workspace.write(".slang/server.json", config);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto current = original;
    current.replace(current.find("return arg;"), 11, "`define NEW_BODY_VALUE 7\nreturn arg + 1;");
    change(server, doc.m_uri, 2, current);
    server.runBackgroundWork({});
    REQUIRE(state(server).active);
    REQUIRE(server.m_methodScope);
    doc.m_text = current;
    workspace.write("dependency.svh", "`define OBSERVED 2\n");
    CHECK_FALSE(server.m_methodScope->hover(doc.m_uri,
        doc.getPosition(current.find("return arg") + 8), server.m_config, {}).has_value());
    CHECK(server.m_methodScope->observedInputChange());
    server.getDocHover({.textDocument={.uri=doc.m_uri},
                       .position=doc.getPosition(current.find("return arg") + 8)});
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, current, true);
}

#if defined(__linux__) || defined(__APPLE__)
TEST_CASE("Macro body recovery after retirement preserves an incomplete incoming version",
          "[method-macro-proof][macro-boundaries][macro-retirement]") {
    Workspace workspace;
    const std::string original = "class C; function int work(input int arg);\n"
                                 "return arg; endfunction endclass\n";
    workspace.write("item.svh", original);
    auto config = localDiagnosticConfig;
    config.insert(1, "\"experimentalMethodBodyMacroProof\":true,\"experimentalMethodBodyCertification\":true,"
                     "\"experimentalUvmPrefixCache\":true,");
    workspace.write(".slang/server.json", config);
    std::optional<std::string> previous;
    if (auto value=std::getenv("SLANG_UVM_GENERATION_EDIT_LIMIT")) previous=value;
    setenv("SLANG_UVM_GENERATION_EDIT_LIMIT", "1", 1);
    auto restore=slang::ScopeGuard([&] {
        if(previous) setenv("SLANG_UVM_GENERATION_EDIT_LIMIT",previous->c_str(),1);
        else unsetenv("SLANG_UVM_GENERATION_EDIT_LIMIT");
    });
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc=server.openFile("item.svh",original);
    doc.doc->getAnalysis();
    const auto epoch=server.m_driver->completions.getCompletionEpoch();
    auto make=[&](std::string_view expression) {
        auto value=original;
        value.replace(value.find("return arg;"),11,
            "`define NEW_BODY_VALUE 7\nreturn "+std::string(expression)+";");
        return value;
    };
    change(server,doc.m_uri,2,make("arg + 1"));
    server.runBackgroundWork({});
    const auto incomplete=make("arg +");
    change(server,doc.m_uri,3,incomplete);
    server.runBackgroundWork({});
    CHECK(server.m_driver->completions.getCompletionEpoch()!=epoch);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText()==incomplete+'\0');
    CHECK(server.client.diagnosticVersions.at(doc.m_uri)==3);
    checkNativeDiagnostics(workspace,server,incomplete,true);
    unsetenv("SLANG_UVM_GENERATION_EDIT_LIMIT");
    const auto repaired=make("arg + 2");
    change(server,doc.m_uri,4,repaired);
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace,server,repaired,true);
    // Recovery deliberately does not speculate by rebuilding an old query
    // model on didChange. Establish the current model through the real query.
    doc.m_text=repaired;
    REQUIRE(server.getDocHover({.textDocument={.uri=doc.m_uri},
                                .position=doc.getPosition(repaired.find("return arg")+8)}));
    change(server,doc.m_uri,5,make("arg + 3"));
    server.runBackgroundWork({});
    INFO(state(server).decline);
    CHECK(state(server).active);
    CHECK(diagnosticState(server).localDiagnosticUpdates>0);
    checkNativeDiagnostics(workspace,server,make("arg + 3"),true);
}
#endif

TEST_CASE("Prepared cooperative local diagnostics do not wait for a settling window",
          "[body-certification][local-diagnostic-scheduling]") {
    const bool cooperative = GENERATE(false, true);
    Workspace workspace;
    auto config = certificationConfig();
    if (!cooperative) {
        const std::string old = "\"experimentalCooperativeAnalysis\":true";
        const auto offset = config.find(old);
        REQUIRE(offset != std::string::npos);
        config.replace(offset, old.size(), "\"experimentalCooperativeAnalysis\":false");
    }
    workspace.write(".slang/server.json", config);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    REQUIRE(server.m_driver->comp);
    CHECK(server.backgroundGracePeriod() == std::chrono::milliseconds(2));
    const auto text = edited();
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    REQUIRE(server.m_methodScope);
    REQUIRE(server.m_methodScope->hasPreparedLocalDiagnostics());
    CHECK(server.backgroundGracePeriod() == std::chrono::milliseconds(cooperative ? 0 : 2));
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Superseded first inputs retain one original native diagnostic baseline",
          "[body-certification]") {
    const bool enabled = GENERATE(false, true);
    const bool primary = GENERATE(false, true);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig(enabled));
    const auto filename = primary ? "item.sv" : "item.svh";
    if (primary) {
        workspace.write("item.sv", header);
        workspace.write("design.f", "prelude.sv\nitem.sv\ntail.sv\n");
    }
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile(filename, header);
    doc.doc->getAnalysis();
    auto initial = diagnosticMap(server);
    std::string text;
    for (int version = 2; version <= 10; ++version) {
        text = header;
        text.replace(text.find("local_1 + charl"), 15,
                     version == 10 ? "local_0 +\n absent_local"
                                   : "local_0 + " + std::to_string(version));
        lsp::RequestContext ctx("textDocument/didChange", version);
        bool reached = false;
        ctx.setCheckpointObserverForTesting([&](std::string_view where) {
            if (where == "before method edit work") {
                reached = true;
                ctx.cancel();
            }
        });
        CHECK_THROWS_AS(change(server, doc.m_uri, version, text, ctx), lsp::RequestCancelled);
        REQUIRE(reached);
        REQUIRE(state(server).active);
        CHECK(state(server).accepted == uint64_t(version - 1));
        CHECK(doc.doc->getVersion() == version);
        CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
        CHECK(diagnosticMap(server) == initial);
    }
    doc.m_text = text;
    auto hover = server.getDocHover({.textDocument = {.uri = doc.m_uri},
                                     .position = doc.getPosition(text.find("local_0 +") + 3)});
    REQUIRE(hover);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
    server.runBackgroundWork({});
    auto status = diagnosticState(server);
    INFO(status.localDiagnosticDecline);
    CHECK(status.localDiagnosticUpdates == (enabled ? 1 : 0));
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 10);
    if (enabled)
        CHECK_FALSE(server.m_driver->comp->containsCurrentSource(workspace.root / filename));
    auto actual = diagnosticMap(server);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    auto oracle = native.openFile(filename, text);
    CHECK(actual == diagnosticMap(native));
}

TEST_CASE("Method result construction ends with one full content certificate",
          "[body-certification][publication-only-proof]") {
    auto operation = GENERATE("complete", "define", "hover", "resolve");
    auto changeDuring = GENERATE(false, true);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = edited();
    lsp::RequestContext editContext("textDocument/didChange", 14);
    size_t editProofs = 0;
    editContext.setCheckpointObserverForTesting(
        [&](std::string_view where) { editProofs += where == "method inputs certified"; });
    change(server, doc.m_uri, 2, text, editContext);
    CHECK(editProofs == 0);
    doc.m_text = text;
    REQUIRE(server.m_methodScope);
    auto at = doc.getPosition(text.rfind("charl") + 2);
    auto items = server.m_methodScope->complete(
        {.textDocument = {.uri = doc.m_uri}, .position = at}, server.m_config, {});
    REQUIRE(items);
    auto found = std::ranges::find_if(*items,
                                      [](const auto& item) { return item.label == "charl"; });
    REQUIRE(found != items->end());
    auto item = *found;
    auto path = workspace.root / "prelude.sv";
    auto stamp = std::filesystem::last_write_time(path);
    size_t proofs = 0;
    bool reached = false;
    const auto boundary = "before method " + std::string(operation) + " certification";
    lsp::RequestContext ctx("query", 15);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        proofs += where == "method inputs certified";
        if (where == boundary) {
            reached = true;
            if (changeDuring) {
                workspace.write("prelude.sv", "package prelude; int other; endpackage\n");
                std::filesystem::last_write_time(path, stamp); // same timestamp and length
            }
        }
    });
    bool result = false;
    if (std::string_view(operation) == "complete")
        result = server.m_methodScope
                     ->complete({.textDocument = {.uri = doc.m_uri}, .position = at},
                                server.m_config, ctx)
                     .has_value();
    else if (std::string_view(operation) == "define")
        result = server.m_methodScope
                     ->define({.textDocument = {.uri = doc.m_uri}, .position = at}, server.m_config,
                              ctx)
                     .has_value();
    else if (std::string_view(operation) == "hover")
        result = server.m_methodScope->hover(doc.m_uri, at, server.m_config, ctx).has_value();
    else
        result = server.m_methodScope->resolve(item, server.m_config, ctx).has_value();
    REQUIRE(reached);
    CHECK(result == !changeDuring);
    CHECK(proofs == (changeDuring ? 0 : 1));
}

TEST_CASE("Deferred diagnostic certification cancels without losing its original baseline",
          "[body-certification]") {
    auto boundary = GENERATE("after local diagnostic binding", "after local diagnostic baseline",
                             "initial diagnostic equality candidates",
                             "native driver diagnostic location proof",
                             "class external driver effects unchanged",
                             "retained local diagnostic publication");
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto before = diagnosticMap(server);
    auto text = header;
    text.replace(text.find("alpha + bravo"), 13, "missing_initializer");
    change(server, doc.m_uri, 2, text);
    CHECK(diagnosticMap(server) == before);
    lsp::RequestContext ctx("background", 16);
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == boundary) {
            reached = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(diagnosticMap(server) == before);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
    server.runBackgroundWork({});
    INFO(diagnosticState(server).localDiagnosticDecline);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Pending diagnostic baselines cannot cross a hard context retirement",
          "[body-certification]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto epoch = server.m_driver->completions.getCompletionEpoch();
    server.m_contextEpoch = epoch;
    server.m_contextBudget = 1;
    server.m_contextHardBudget = 1;
    auto text = edited();
    change(server, doc.m_uri, 2, text);
    CHECK(server.m_driver->completions.getCompletionEpoch() == epoch);
    server.runBackgroundWork({}); // Admit current text before any semantic consumer.
    CHECK(server.m_driver->completions.getCompletionEpoch() != epoch);
    CHECK_FALSE(server.m_methodScope);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    server.m_driver->getDocument(doc.m_uri)->getAnalysis();
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Deferred original diagnostics replay changed external reference usage",
          "[body-certification]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = header;
    text.replace(text.find("local_1 + charl"), 15, "marker + charl");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    CHECK(diagnosticState(server).localDiagnosticDecline.empty());
    checkNativeDiagnostics(workspace, server, text, true);
    workspace.write(".slang/server.json", certificationConfig());
    text.replace(text.find("marker + charl"), 14, "marker + charl + 10");
    change(server, doc.m_uri, 3, text);
    server.runBackgroundWork({});
    INFO(diagnosticState(server).localDiagnosticDecline);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 2);
    checkNativeDiagnostics(workspace, server, text, true);
}

TEST_CASE("Deferred diagnostics verify disk contents after constructing the result",
          "[body-certification]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto before = diagnosticMap(server);
    auto text = edited();
    change(server, doc.m_uri, 2, text);
    REQUIRE(server.m_methodScope);
    lsp::RequestContext ctx("background", 17);
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (!reached && where == "after local diagnostic binding") {
            reached = true;
            auto stamp = std::filesystem::last_write_time(workspace.root / "prelude.sv");
            workspace.write("prelude.sv", "package prelude; int other; endpackage\n");
            std::filesystem::last_write_time(workspace.root / "prelude.sv", stamp);
        }
    });
    CHECK_FALSE(server.m_methodScope->publishLocalDiagnostics(server.m_config, ctx));
    REQUIRE(reached);
    CHECK(before == diagnosticMap(server));
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
    server.onWorkspaceDidChangeWatchedFiles(
        {.changes = {{.uri = URI::fromFile(workspace.root / "prelude.sv"),
                      .type = lsp::FileChangeType::Changed}}});
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Deferred preprocessing failure cannot reuse outside diagnostics after repair",
          "[body-certification]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write(
        "tail.sv",
        "module tail;\n`ifdef BAD_BODY_STATE\nint value = missing_tail;\n`endif\nendmodule\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto unsafe = header;
    unsafe.insert(unsafe.find("int local_0"), "`define BAD_BODY_STATE 1\n");
    lsp::RequestContext ctx("textDocument/didChange", 18);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "before method edit work")
            ctx.cancel();
    });
    CHECK_THROWS_AS(change(server, doc.m_uri, 2, unsafe, ctx), lsp::RequestCancelled);
    server.runBackgroundWork({}); // No intervening foreground query to discard the overlay.
    CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
    checkNativeDiagnostics(workspace, server, unsafe);
    workspace.write(".slang/server.json", certificationConfig());
    auto text = edited();
    change(server, doc.m_uri, 3, text);
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Native diagnostic routes avoid first-use retained formatting",
          "[body-certification][native-diagnostic-routes]") {
    const bool enabled = GENERATE(false, true);
    const bool selectInstance = GENERATE(false, true);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig(enabled));
    workspace.write("prelude.sv", "package prelude; unknown_type field; endpackage\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    if (selectInstance)
        REQUIRE(server.setActiveInstance("tail"));
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    const auto initial = diagnosticMap(server);
    REQUIRE_FALSE(initial.at(URI::fromFile(workspace.root / "prelude.sv").str()).empty());
    size_t reusedCapture = 0, reusedBuckets = 0, formattedBuckets = 0;
    lsp::RequestContext ctx("diagnostic routes", 51);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        reusedCapture += where == "native diagnostic routes reused";
        reusedBuckets += where == "native diagnostic buckets reused";
        formattedBuckets += where == "local diagnostic bucket formatting";
    });
    auto text = edited();
    change(server, doc.m_uri, 2, text, ctx);
    server.runBackgroundWork(ctx);
    CHECK(reusedCapture == (enabled ? 1 : 0));
    CHECK(reusedBuckets == (enabled ? 1 : 0));
    CHECK((formattedBuckets == 0) == enabled);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Initial source witnesses read duplicate paths once with final content proof",
          "[body-certification][native-diagnostic-routes]") {
    const bool mutate = GENERATE(false, true);
    size_t reads[2]{};
    for (const bool enabled : {false, true}) {
        Workspace workspace;
        workspace.write(".slang/server.json", certificationConfig(enabled));
        workspace.write("sibling.svh", "class Sibling; int field; endclass\n");
        workspace.write("owner.sv", "package owner;\n`include \"sibling.svh\"\n"
                                    "`include \"item.svh\"\nendpackage\n");
        CertificationServer server(workspace.params());
        server.setBuildFile((workspace.root / "design.f").string());
        auto doc = server.openFile("item.svh", header);
        doc.doc->getAnalysis();
        lsp::RequestContext ctx("initial witnesses", 52);
        bool changed = false;
        ctx.setCheckpointObserverForTesting([&](std::string_view where) {
            reads[enabled] += where == "method source witness read";
            if (mutate && where == "method certificate sources ready") {
                REQUIRE_FALSE(changed);
                changed = true;
                workspace.write("sibling.svh", "class Sibling; int other; endclass\n");
            }
        });
        change(server, doc.m_uri, 2, edited(), ctx);
        // Optimized synchronization is provisional; it does not publish a
        // semantic result. The complete proof must reject the changed input
        // before a query can return a value from this certificate.
        CHECK(state(server).active == (!mutate || enabled));
        if (mutate && enabled) {
            REQUIRE(server.m_methodScope);
            CHECK_FALSE(server.m_methodScope->valid(server.m_config, {}));
            CHECK(server.m_methodScope->observedInputChange());
            CHECK_FALSE(server.m_methodScope
                            ->hover(doc.m_uri, doc.getPosition(edited().find("local_0") + 2),
                                    server.m_config, {})
                            .has_value());
        }
        CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == edited() + '\0');
        CHECK(doc.doc->getVersion() == 2);
        CHECK(reads[enabled] > 0);
        if (mutate)
            CHECK(changed);
    }
    // A mutation can make the legacy constructor abort before visiting later
    // paths. Compare read counts only when both constructors finish successfully.
    if (!mutate)
        CHECK(reads[1] < reads[0]);
}

TEST_CASE("Cancelled native route capture rolls back and restores its client observer",
          "[body-certification][native-diagnostic-routes]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv", "package prelude; unknown_type field; endpackage\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    const auto original = diagnosticMap(server);
    auto text = header;
    text.insert(0, "`define NATIVE_ONLY 1\n");
    change(server, doc.m_uri, 2, text);
    size_t observed = 0;
    auto client = server.m_driver->diagClient;
    auto previous = client->exchangeReportObserver([&](const URI&) { ++observed; });
    auto restore = slang::ScopeGuard([&] { client->exchangeReportObserver(std::move(previous)); });
    lsp::RequestContext ctx("native cancellation", 53);
    size_t routes = 0;
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "native diagnostic route capture" && ++routes == 2)
            ctx.cancel();
    });
    CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
    REQUIRE(routes == 2);
    CHECK(diagnosticMap(server) == original);
    auto restored = client->exchangeReportObserver({});
    REQUIRE(bool(restored));
    restored(doc.m_uri);
    CHECK(observed == 1);
    client->exchangeReportObserver(std::move(restored));
    server.runBackgroundWork({});
    restored = client->exchangeReportObserver({});
    REQUIRE(bool(restored));
    restored(doc.m_uri);
    CHECK(observed == 2);
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Native diagnostic physical routes follow editor aliases and macro filtering",
          "[body-certification][native-diagnostic-routes]") {
    const bool alias = GENERATE(false, true);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string prelude = "package prelude; missing_type field; endpackage\n";
    workspace.write("prelude.sv", prelude);
    auto original = std::string("`define LOCAL_READ(X) X\n") + header;
    original.replace(original.find("local_1 + charl"), 15, "`LOCAL_READ(local_1) + charl");
    workspace.write("item.svh", original);
    if (alias)
        std::filesystem::create_directory_symlink(workspace.root, workspace.root / "alias");
    const auto target = alias ? "alias/item.svh" : "item.svh";
    const auto other = alias ? "alias/prelude.sv" : "prelude.sv";
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto outside = server.openFile(other, prelude);
    auto doc = server.openFile(target, original);
    doc.doc->getAnalysis();
    size_t reused = 0;
    lsp::RequestContext ctx("macro route", 54);
    ctx.setCheckpointObserverForTesting(
        [&](std::string_view where) { reused += where == "native diagnostic buckets reused"; });
    auto text = original;
    text.replace(text.find("`LOCAL_READ(local_1)"), 20, "`LOCAL_READ(absent_local)");
    change(server, doc.m_uri, 2, text, ctx);
    server.runBackgroundWork(ctx);
    REQUIRE(state(server).active);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    CHECK(reused == 1);
    CHECK_FALSE(server.client.getDiagnostics(doc.m_uri).empty());
    CHECK_FALSE(server.client.getDiagnostics(outside.m_uri).empty());
    auto actual = diagnosticMap(server);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile(other, prelude);
    native.openFile(target, text);
    CHECK(actual == diagnosticMap(native));
}

TEST_CASE("Complete input proof rejects changed path identity and nonregular sources",
          "[body-certification][native-diagnostic-routes]") {
    auto kind = GENERATE("file-link", "directory-link", "directory", "missing");
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    std::filesystem::create_directory(workspace.root / "headers");
    const std::string sibling = "class Sibling; int field; endclass\n";
    workspace.write("headers/sibling.svh", sibling);
    workspace.write("replacement.svh", sibling);
    workspace.write("owner.sv", "package owner;\n`include \"headers/sibling.svh\"\n"
                                "`include \"item.svh\"\nendpackage\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    lsp::RequestContext ctx("input identity", 55);
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where != "method certificate sources ready")
            return;
        REQUIRE_FALSE(reached);
        reached = true;
        auto source = workspace.root / "headers/sibling.svh";
        if (std::string_view(kind) == "directory-link") {
            std::filesystem::rename(workspace.root / "headers", workspace.root / "actual");
            std::filesystem::create_directory_symlink(workspace.root / "actual",
                                                      workspace.root / "headers");
        }
        else {
            std::filesystem::remove(source);
            if (std::string_view(kind) == "file-link")
                std::filesystem::create_symlink(workspace.root / "replacement.svh", source);
            else if (std::string_view(kind) == "directory")
                std::filesystem::create_directory(source);
        }
    });
    change(server, doc.m_uri, 2, edited(), ctx);
    REQUIRE(reached);
    REQUIRE(server.m_methodScope);
    CHECK_FALSE(server.m_methodScope->valid(server.m_config, {}));
    CHECK(server.m_methodScope->observedInputChange());
    CHECK_FALSE(
        server.m_methodScope
            ->hover(doc.m_uri, doc.getPosition(edited().find("local_0") + 2), server.m_config, {})
            .has_value());
    CHECK(doc.doc->getVersion() == 2);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == edited() + '\0');
}

TEST_CASE("Observed dependency changes refresh public native fallback before watch events",
          "[body-certification][method-dependency-reload]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string before = "package prelude; typedef logic [7:0] word_t; endpackage\n";
    const std::string after = "package prelude; typedef logic [9:0] word_t; endpackage\n";
    workspace.write("prelude.sv", before);
    auto original = header;
    original.insert(original.find("    int local_0"), "    prelude::word_t local_word = '0;\n");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("local_1 +"), 7, "local_0");
    change(server, doc.m_uri, 2, text);
    REQUIRE(state(server).active);
    doc.m_text = text;
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(text.find("word_t") + 3)};
    const auto previous = server.getDocHover(params);
    REQUIRE(previous);
    auto stamp = std::filesystem::last_write_time(workspace.root / "prelude.sv");
    workspace.write("prelude.sv", after);
    std::filesystem::last_write_time(workspace.root / "prelude.sv", stamp);
    const auto actual = server.getDocHover(params);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", text);
    const auto expected = native.getDocHover(params);
    REQUIRE(expected);
    REQUIRE(rfl::json::write(previous) != rfl::json::write(expected));
    CHECK(rfl::json::write(actual) == rfl::json::write(expected));
}

TEST_CASE("Method dependency recovery is transactional and keeps later incremental input",
          "[body-certification][method-dependency-reload]") {
    const auto boundary = GENERATE("before method input reload", "before retirement inputs",
                                   "after retirement inputs", "before retirement commit");
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv", "package prelude; typedef logic [7:0] word_t; endpackage\n");
    auto original = header;
    original.insert(original.find("    int local_0"), "    prelude::word_t local_word = '0;\n");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("local_1 +"), 7, "local_0");
    change(server, doc.m_uri, 2, text);
    REQUIRE(state(server).active);
    doc.m_text = text;
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(text.find("word_t") + 3)};
    const auto oldMap = diagnosticMap(server);
    auto* oldDriver = server.m_driver.get();
    workspace.write("prelude.sv", "package prelude; typedef logic [9:0] word_t; endpackage\n");
    lsp::RequestContext ctx("textDocument/hover", 61);
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == boundary) {
            reached = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(server.getDocHover(params, ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(server.m_driver.get() == oldDriver);
    CHECK(diagnosticMap(server) == oldMap);
    // A superseded notification still has to apply its range exactly once.
    const auto at = text.find("local_0 + charl");
    REQUIRE(at != std::string::npos);
    lsp::DidChangeTextDocumentParams notification{
        .textDocument = {.version = 3, .uri = doc.m_uri},
        .contentChanges = {lsp::TextDocumentContentChangePartial{
            .range = {.start = doc.getPosition(at), .end = doc.getPosition(at + 7)},
            .text = "local_1"}}};
    lsp::RequestContext superseded("textDocument/didChange", 62);
    superseded.cancel();
    CHECK_THROWS_AS(server.SlangServer::onDocDidChange(notification, superseded),
                    lsp::RequestCancelled);
    text.replace(at, 7, "local_1");
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    CHECK(server.m_driver->getDocument(doc.m_uri)->getVersion() == 3);
    CHECK(diagnosticMap(server) == oldMap);
    const auto actual = server.getDocHover(params);
    server.runBackgroundWork({});
    const auto actualMap = diagnosticMap(server);
    CHECK(server.m_driver.get() != oldDriver);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", text);
    CHECK(rfl::json::write(actual) == rfl::json::write(native.getDocHover(params)));
    CHECK(actualMap == diagnosticMap(native));
}

TEST_CASE("Invalid config after dependency rejection cannot return the old native result",
          "[body-certification][method-dependency-reload]") {
    Workspace workspace;
    const auto config = certificationConfig();
    workspace.write(".slang/server.json", config);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    change(server, doc.m_uri, 2, edited());
    REQUIRE(state(server).active);
    const auto before = diagnosticMap(server);
    auto* driver = server.m_driver.get();
    workspace.write(".slang/server.json", "{invalid");
    doc.m_text = edited();
    lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                            .position = doc.getPosition(edited().find("local_0 +") + 3)};
    CHECK_THROWS_AS(server.getDocHover(params), lsp::RequestFailed);
    CHECK_THROWS_AS(server.getDocHover(params), lsp::RequestFailed);
    CHECK_THROWS_AS(server.runBackgroundWork({}), lsp::RequestFailed);
    CHECK_THROWS_AS(server.executeCommand({.command = "slang.getScope",
                                           .arguments = std::vector<lsp::LSPAny>{rfl::to_generic(
                                               std::string("tail"))}}),
                    lsp::RequestFailed);
    REQUIRE(server.executeCommand(
        {.command = "slang.methodScopeStatus",
         .arguments = std::vector<lsp::LSPAny>{rfl::to_generic(std::string("state"))}}));
    CHECK(server.m_driver.get() == driver);
    CHECK(diagnosticMap(server) == before);
    workspace.write(".slang/server.json", config);
    REQUIRE(server.getDocHover(params));
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, edited());
}

TEST_CASE("First method certificate compares options with the bytes originally loaded",
          "[body-certification][method-dependency-reload]") {
    const auto kind = GENERATE("source", "filelist", "config", "new-local-config");
    INFO(kind);
    Workspace workspace;
    auto config = certificationConfig();
    if (std::string_view(kind) != "filelist")
        config.replace(config.find("--threads 1"), 11, "--threads 1 -D WIDTH=8");
    workspace.write(".slang/server.json", config);
    if (std::string_view(kind) == "filelist")
        workspace.write("design.f", "+incdir+.\n+define+WIDTH=8\nprelude.sv\nowner.sv\ntail.sv\n");
    workspace.write("prelude.sv", "package prelude;\n`ifdef ALT_WIDTH\n"
                                  "typedef logic [`ALT_WIDTH-1:0] word_t;\n`else\n"
                                  "typedef logic [`WIDTH-1:0] word_t;\n`endif\nendpackage\n");
    auto original = header;
    original.insert(original.find("    int local_0"), "    prelude::word_t local_word = '0;\n");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    doc.m_text = original;
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(original.find("word_t") + 3)};
    const auto old = server.getDocHover(params);
    if (std::string_view(kind) == "source")
        workspace.write("prelude.sv", "package prelude; typedef logic [9:0] word_t; endpackage\n");
    else if (std::string_view(kind) == "filelist")
        workspace.write("design.f", "+incdir+.\n+define+WIDTH=6\nprelude.sv\nowner.sv\ntail.sv\n");
    else if (std::string_view(kind) == "config") {
        config.replace(config.find("WIDTH=8"), 7, "WIDTH=6");
        workspace.write(".slang/server.json", config);
    }
    else {
        std::filesystem::create_directories(workspace.root / ".slang/local");
        workspace.write(".slang/local/server.json", "{\"flags\":\"-D ALT_WIDTH=6\"}");
    }
    auto text = original;
    text.replace(text.find("local_1 +"), 7, "local_0");
    change(server, doc.m_uri, 2, text);
    CHECK_FALSE(state(server).active);
    const auto actual = server.getDocHover(params);
    server.runBackgroundWork({});
    const auto actualMap = diagnosticMap(server);
    // Keep all newly loaded options in the oracle; only disable local experiments.
    auto nativeConfig = server.m_config;
    nativeConfig.experimentalMethodBodyQueries = false;
    nativeConfig.experimentalMethodBodyDiagnostics = false;
    nativeConfig.experimentalMethodBodyCertification = false;
    BodyTestServer native(workspace.params());
    REQUIRE(native.loadConfig(nativeConfig, true));
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", text);
    const auto expected = native.getDocHover(params);
    REQUIRE(expected);
    REQUIRE(rfl::json::write(old) != rfl::json::write(expected));
    CHECK(rfl::json::write(actual) == rfl::json::write(expected));
    CHECK(actualMap == diagnosticMap(native));
}

TEST_CASE("Fixed provider usage proves added and removed method references",
          "[method-body-diagnostics][fixed-provider-reference]") {
    const bool providerUses = GENERATE(false, true);
    const bool callerUses = GENERATE(false, true);
    CAPTURE(providerUses, callerUses);
    Workspace workspace;
    auto config = certificationConfig();
    config.insert(config.size() - 1,
                  ",\"experimentalScopeSemanticCache\":true,"
                  "\"frozenSources\":{\"packages\":[\"provider\"],\"maxMiB\":256}");
    workspace.write(".slang/server.json", config);
    workspace.write(
        "prelude.sv",
        std::string("package provider; class Base;\n"
                    "function int get_value(input int value); return value; endfunction\n") +
            (providerUses ? "function int fixed_caller(); return get_value(1); endfunction\n"
                          : "") +
            "endclass endpackage\n");
    auto initial = header;
    initial.replace(initial.find("class Accumulator;"), 18,
                    "class Accumulator extends provider::Base;");
    if (callerUses)
        initial.replace(initial.find("local_1 + charl"), 15, "get_value(local_1) + charl");
    workspace.write("item.svh", initial);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", initial);
    doc.doc->getAnalysis();
    auto library = server.executeCommand(
        {.command = "slang.scopeLibraryStatus", .arguments = std::vector<lsp::LSPAny>{}});
    REQUIRE(library);
    REQUIRE(rfl::json::write(*library).find("\"prepared\":true") != std::string::npos);
    for (int version = 2; version <= 4; ++version) {
        const bool use = version % 2 ? callerUses : !callerUses;
        auto text = initial;
        const std::string previous = callerUses ? "get_value(local_1) + charl" : "local_1 + charl";
        text.replace(text.find(previous), previous.size(),
                     use ? "get_value(local_0) + charl + 1" : "local_0 + charl + 1");
        change(server, doc.m_uri, version, text);
        server.runBackgroundWork({});
        const auto status = diagnosticState(server);
        INFO(status.localDiagnosticDecline);
        CHECK(status.localDiagnosticUpdates == uint64_t(version - 1));
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Fixed reference proofs retain package call driver boundaries",
          "[method-body-diagnostics][fixed-provider-reference]") {
    const bool writesGlobal = GENERATE(false, true);
    const auto callShape = GENERATE("direct", "member", "system");
    CAPTURE(writesGlobal, callShape);
    Workspace workspace;
    auto config = certificationConfig();
    config.insert(config.size() - 1,
                  ",\"experimentalScopeSemanticCache\":true,"
                  "\"frozenSources\":{\"packages\":[\"provider\"],\"maxMiB\":256}");
    workspace.write(".slang/server.json", config);
    workspace.write("prelude.sv",
                    std::string("package provider; int shared;\nfunction int peek();\n") +
                        (writesGlobal ? "shared = 1; return 1;\n" : "return shared;\n") +
                        "endfunction\nfunction int fixed_caller(); return peek(); endfunction\n"
                        "class Base;\n"
                        "function int get_value(input int value); return value; endfunction\n"
                        "function int fixed_caller(); return get_value(1); endfunction\n"
                        "endclass endpackage\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto initial = header;
    initial.replace(initial.find("class Accumulator;"), 18,
                    "class Accumulator extends provider::Base;");
    auto doc = server.openFile("item.svh", initial);
    doc.doc->getAnalysis();
    auto text = initial;
    const std::string call = std::string_view(callShape) == "member" ? "get_value(provider::peek())"
                             : std::string_view(callShape) == "system"
                                 ? "$countones(provider::peek())"
                                 : "provider::peek()";
    text.replace(text.find("local_1 + charl"), 15, call + " + local_0 + charl");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    const auto status = diagnosticState(server);
    INFO(status.localDiagnosticDecline);
    CHECK(status.localDiagnosticUpdates == 1);
    CHECK(status.localDiagnosticDecline.empty());
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Later runtime line arguments preserve native diagnostics across multiline edits",
          "[method-body-diagnostics][later-macro-lines]") {
    const bool certify = GENERATE(false, true);
    const uint32_t syntaxBudget = GENERATE(0u, 1024u);
    const bool nested = GENERATE(false, true);
    const bool primary = GENERATE(false, true);
    const std::string argument = GENERATE(std::string("`__LINE__"), std::string("`BODY_LINE"));
    Workspace workspace;
    if (primary)
        workspace.write("design.f", "+incdir+.\nprelude.sv\nitem.svh\ntail.sv\n");
    auto original = std::string("`define BODY_LINE `__LINE__\n") + header;
    original.insert(
        original.find("  function int combine"),
        "  function void report(input string message, input int line_number); endfunction\n");
    original.replace(original.rfind("return value;"), 13,
                     "report(\"test\", " + argument + "); return value;");
    if (nested) {
        original = "`define REPORT report(\"test\", `BODY_LINE)\n" + original;
        original.replace(original.rfind("report(\"test\", " + argument + ");"),
                         std::string("report(\"test\", " + argument + ");").size(), "`REPORT;");
    }
    workspace.write("item.svh", original);
    auto cfg = localDiagnosticConfig;
    if (certify)
        cfg.insert(cfg.size() - 1, ",\"experimentalMethodBodyCertification\":true");
    cfg.insert(cfg.size() - 1, ",\"methodBodyLineSyntaxCacheKiB\":" +
                                   std::to_string(syntaxBudget));
    workspace.write(".slang/server.json", cfg);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    for (int version = 2; version <= 4; ++version) {
        auto text = original;
        text.replace(text.find("local_1 + charl"), 15,
                     version == 3 ? "local_0 + charl" : "local_0 +\n charl");
        change(server, doc.m_uri, version, text);
        const auto beforeDiagnostics = server.m_driver->sm.getSourceLocationEntryCount();
        server.runBackgroundWork({});
        if (version == 4) {
            // Returning to a proven height must honor the configured memo budget
            // without allocating another neighbour replay buffer.
            if (certify && syntaxBudget)
                CHECK(server.m_driver->sm.getSourceLocationEntryCount() == beforeDiagnostics);
            else
                CHECK(server.m_driver->sm.getSourceLocationEntryCount() > beforeDiagnostics);
        }
        REQUIRE(state(server).active);
        const auto result = diagnosticState(server);
        INFO(result.localDiagnosticDecline);
        CHECK(result.localDiagnosticUpdates == uint64_t(version - 1));
        checkNativeDiagnostics(workspace, server, text);
    }
}

TEST_CASE("Later macro proof rejects semantic line uses and accepts reset logical lines",
          "[method-body-diagnostics][later-macro-lines]") {
    const bool certify = GENERATE(false, true);
    struct Variant {
        const char* declaration;
        const char* statement;
        bool local;
        const char* prefix;
    };
    const auto variant = GENERATE(
        Variant{"function void report(input int line_number); endfunction", "report(`__LINE__);",
                true, ""},
        Variant{"function void report(input int line_number); endfunction", "report(`__LINE__);",
                true, "`line 200 \"logical.svh\" 0\n"},
        Variant{"function void report(input int line_number); endfunction",
                "report(`__LINE__ + 1);", false, ""},
        Variant{"function void report(input int line_number); endfunction",
                "if (`__LINE__ > 0) report(1);", false, ""},
        Variant{"static function void report(input int line_number); endfunction",
                "report(`__LINE__);", false, ""},
        Variant{"task report(input int line_number); endtask", "report(`__LINE__);", false, ""},
        Variant{"function void report(input int unsigned line_number); endfunction",
                "report(`__LINE__);", false, ""},
        Variant{"function void report(input longint line_number); endfunction",
                "report(`__LINE__);", false, ""},
        Variant{"function int report(input int line_number); return line_number; endfunction",
                "report(`__LINE__);", false, ""},
        Variant{"function void report(input int line_number); endfunction", "$display(`__LINE__);",
                false, ""},
        Variant{"function void report(input int line_number); endfunction",
                "`MUTATE report(`__LINE__);", false, ""});
    Workspace workspace;
    auto original = std::string("`define MUTATE `define ALTERED 1\n") + header;
    original.insert(original.find("  function int combine"),
                    std::string(variant.declaration) + "\n");
    original.insert(original.find("  function int later"), variant.prefix);
    original.replace(original.rfind("return value;"), 13,
                     std::string(variant.statement) + " return value;");
    workspace.write("item.svh", original);
    auto cfg = localDiagnosticConfig;
    if (certify)
        cfg.insert(cfg.size() - 1, ",\"experimentalMethodBodyCertification\":true");
    workspace.write(".slang/server.json", cfg);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("local_1 + charl"), 15, "local_0 +\n charl");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    const auto status = diagnosticState(server);
    INFO(variant.statement);
    INFO(status.localDiagnosticDecline);
    CHECK(status.localDiagnosticUpdates == (variant.local ? 1 : 0));
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Later macro proof cancellation leaves the complete native publication intact",
          "[method-body-diagnostics][later-macro-lines]") {
    const auto boundary = GENERATE("later macro native contexts", "later macro token proof",
                                   "later nested macro proof", "before later macro proof",
                                   "after later macro proof");
    Workspace workspace;
    auto original = std::string("`define LINE `__LINE__\n") + header;
    original.insert(original.find("  function int combine"),
                    "function void report(input int line_number); endfunction\n");
    original.replace(original.rfind("return value;"), 13, "report(`LINE); return value;");
    workspace.write("item.svh", original);
    auto cfg = localDiagnosticConfig;
    cfg.insert(cfg.size() - 1, ",\"experimentalMethodBodyCertification\":true");
    workspace.write(".slang/server.json", cfg);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const auto before = diagnosticMap(server);
    auto text = original;
    text.replace(text.find("local_1 + charl"), 15, "local_0 +\n charl");
    change(server, doc.m_uri, 2, text);
    lsp::RequestContext ctx("background", 171);
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == boundary) {
            reached = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(diagnosticMap(server) == before);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
    server.runBackgroundWork({});
    INFO(diagnosticState(server).localDiagnosticDecline);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Multiline local diagnostics keep later method hover and resolve current",
          "[method-body-diagnostics][later-macro-lines]") {
    const auto view = GENERATE("source", "expanded", "both");
    Workspace workspace;
    auto original = header;
    original.insert(original.find("  function int combine"),
                    "function void report(input int line_number); endfunction\n");
    original.replace(original.rfind("return value;"), 13, "report(`__LINE__); return value;");
    original.replace(original.find("return (local_1 + charl);"), 25,
                     "return later(local_1 + charl);");
    workspace.write("item.svh", original);
    auto cfg = localDiagnosticConfig;
    cfg.insert(cfg.size() - 1,
               std::string(
                   ",\"experimentalMethodBodyCertification\":true,\"hovers\":{\"macroView\":\"") +
                   view + "\"}");
    workspace.write(".slang/server.json", cfg);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("local_1 + charl"), 15, "local_0 +\n charl");
    change(server, doc.m_uri, 2, text);
    doc.m_text = text;
    server.runBackgroundWork({});
    INFO(diagnosticState(server).localDiagnosticDecline);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    const auto at = text.find("return later(") + 9;
    const auto response = server.getDocCompletion(
        {.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(at)});
    const auto& items = rfl::get<std::vector<lsp::CompletionItem>>(response);
    const auto ticket = std::ranges::find_if(items, [](const auto& item) {
        return item.label == "charl";
    });
    REQUIRE(ticket != items.end());
    REQUIRE(ticket->data);
    const auto hover = rfl::json::write(
        server.getDocHover({.textDocument = {.uri = doc.m_uri}, .position = doc.getPosition(at)}));
    REQUIRE(state(server).active);
    CHECK(server.getCompletionItemResolve(*ticket).documentation.has_value());
    const auto completion = rfl::json::write(bodyCompletions(server, doc, at));
    checkNativeDiagnostics(workspace, server, text);
    bodyConfig(workspace, false);
    auto nativeConfig =
        std::string("{\"indexingThreads\":1,\"flags\":\"--single-unit --threads "
                    "1\",\"compilationFilelist\":\"design.f\",\"hovers\":{\"macroView\":\"") +
        view + "\"}}";
    workspace.write(".slang/server.json", nativeConfig);
    ServerHarness native(workspace.params());
    auto oracle = native.openFile("item.svh", text);
    CHECK(completion == rfl::json::write(bodyCompletions(native, oracle, at)));
    CHECK(hover == rfl::json::write(native.getDocHover({.textDocument = {.uri = oracle.m_uri},
                                                        .position = oracle.getPosition(at)})));
}

TEST_CASE("Nested edited-body macros cannot alter the retained following environment",
          "[method-body-diagnostics][later-macro-lines]") {
    const bool argument = GENERATE(false, true);
    const bool macroProof = GENERATE(false, true);
    Workspace workspace;
    auto original = std::string("`define AFTER 1\n`define STATE `define AFTER "
                                "missing_user_name\n`define NESTED `STATE\n`define WRAP(x) x\n") +
                    header;
    original.replace(original.rfind("return value;"), 13, "return `AFTER;");
    workspace.write("item.svh", original);
    auto cfg = localDiagnosticConfig;
    if (macroProof) cfg.insert(1, "\"experimentalMethodBodyMacroProof\":true,");
    cfg.insert(cfg.size() - 1, ",\"experimentalMethodBodyCertification\":true");
    workspace.write(".slang/server.json", cfg);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.insert(text.find("    int local_0"), argument ? "`WRAP(`STATE)\n" : "`NESTED\n");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    CHECK_FALSE(state(server).active);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Large private query text stays bounded until its last syntax owner closes",
          "[method-body-diagnostics][query-storage]") {
    const bool certify = GENERATE(false, true);
    Workspace workspace;
    auto original = std::string("`define LINE `__LINE__\n") + std::string(1024 * 1024, ' ') + "\n" +
                    header;
    original.insert(original.find("  function int combine"),
                    "function void report(input int line_number); endfunction\n");
    original.replace(original.rfind("return value;"), 13, "report(`LINE); return value;");
    workspace.write("item.svh", original);
    auto cfg = localDiagnosticConfig;
    cfg.insert(cfg.size() - 1,
               ",\"experimentalUvmPrefixCache\":false,\"experimentalUvmSemanticCache\":false,"
               "\"experimentalScopeSemanticCache\":false,\"experimentalFrozenSources\":false,"
               "\"experimentalMethodBodyCertificateRenewal\":false");
    if (certify)
        cfg.insert(cfg.size() - 1, ",\"experimentalMethodBodyCertification\":true");
    workspace.write(".slang/server.json", cfg);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const auto epoch = server.m_driver->completions.getCompletionEpoch();
    size_t maximum = 0;
    size_t stableBytes = 0;
    std::string text;
    for (int version = 2; version <= 25; ++version) {
        text = original;
        text.replace(text.find("local_1 + charl"), 15,
                     "local_0 +\n charl + " + std::to_string(version));
        change(server, doc.m_uri, version, text);
        doc.m_text = text;
        auto hover = server.getDocHover({.textDocument = {.uri = doc.m_uri},
                                         .position = doc.getPosition(text.find("return (") + 10)});
        REQUIRE(hover);
        server.runBackgroundWork({});
        REQUIRE(state(server).active);
        const auto memory = server.m_driver->sm.getMemoryUsageDetails();
        maximum = std::max(maximum, memory.queryBytes);
        CHECK(memory.queryBuffers <= 3);
        if (version == 2) {
            stableBytes = memory.queryBytes;
            CHECK(stableBytes < 6 * 1024 * 1024 + 8192);
        }
        CHECK(memory.queryBytes <= stableBytes + 4096);
        CHECK(server.m_driver->completions.getCompletionEpoch() == epoch);
    }
    CHECK(maximum > 1024 * 1024);
    INFO(diagnosticState(server).localDiagnosticDecline);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 24);
    checkNativeDiagnostics(workspace, server, text);
    text = "`define NONLOCAL 1\n" + text;
    change(server, doc.m_uri, 26, text);
    server.runBackgroundWork({});
    CHECK_FALSE(state(server).active);
    CHECK(server.m_driver->sm.getMemoryUsageDetails().queryBuffers == 0);
}

TEST_CASE("Cancelled query proof releases temporary roots without losing its baseline",
          "[method-body-diagnostics][query-storage]") {
    Workspace workspace;
    auto original = std::string("`define LINE `__LINE__\n") + header;
    original.insert(original.find("  function int combine"),
                    "function void report(input int line_number); endfunction\n");
    original.replace(original.rfind("return value;"), 13, "report(`LINE); return value;");
    workspace.write("item.svh", original);
    auto cfg = localDiagnosticConfig;
    cfg.insert(cfg.size() - 1, ",\"experimentalMethodBodyCertification\":true");
    workspace.write(".slang/server.json", cfg);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("local_1 + charl"), 15, "local_0 +\n charl");
    change(server, doc.m_uri, 2, text);
    lsp::RequestContext ctx("cancel parse", 177);
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "during disposable syntax parsing") {
            reached = true;
            ctx.cancel();
        }
    });
    const auto before = diagnosticMap(server);
    CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(before == diagnosticMap(server));
    CHECK(server.m_driver->sm.getMemoryUsageDetails().queryBuffers <= 2);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    CHECK(server.m_driver->sm.getMemoryUsageDetails().queryBuffers <= 2);
    checkNativeDiagnostics(workspace, server, text);
}

namespace {
void addProofWitnessFiles(Workspace& workspace) {
    std::string files = "+incdir+.\nprelude.sv\nowner.sv\ntail.sv\n";
    for (int i = 0; i < 80; ++i) {
        auto name = "proof_" + std::to_string(i) + ".sv";
        workspace.write(name, "package proof_" + std::to_string(i) + "; int fixed; endpackage\n");
        files += name + "\n";
    }
    workspace.write("design.f", files);
}
} // namespace

TEST_CASE("Large input proofs handle concurrent cancellation and retry",
          "[body-certification][batched-file-proof][parallel-file-proof]") {
    const auto delay = GENERATE(0, 50, 500);
    Workspace workspace;
    addProofWitnessFiles(workspace);
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    change(server, doc.m_uri, 2, edited());
    REQUIRE(server.m_methodScope);
    REQUIRE(server.m_methodScope->valid(server.m_config, {}));
    for (int iteration = 0; iteration < 8; ++iteration) {
        lsp::RequestContext ctx("concurrent physical proof cancellation", 284);
        std::atomic<bool> begin = false;
        std::thread canceller([&] {
            while (!begin.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (delay)
                std::this_thread::sleep_for(std::chrono::microseconds(delay));
            ctx.cancel();
        });
        bool accepted = false, cancelled = false;
        begin.store(true, std::memory_order_release);
        try {
            accepted = server.m_methodScope->valid(server.m_config, ctx);
        }
        catch (const lsp::RequestCancelled&) {
            cancelled = true;
        }
        canceller.join();
        // Either cancellation arrived before completion, or this proof finished
        // first. Neither outcome may leave an input failure or borrowed reads
        // referring to its invocation-local expected bytes and descriptors.
        CHECK((accepted || cancelled));
        CHECK(ctx.isCancelled());
        CHECK_FALSE(server.m_methodScope->observedInputChange());
        CHECK(server.m_methodScope->valid(server.m_config, {}));
        CHECK(doc.doc->getVersion() == 2);
    }
}

TEST_CASE("Large content proofs observe equal-size dependency changes before watch events",
          "[body-certification][batched-file-proof]") {
    const bool changed = GENERATE(false, true);
    Workspace workspace;
    addProofWitnessFiles(workspace);
    workspace.write(".slang/server.json", certificationConfig());
    const std::string before = "package prelude; typedef logic [7:0] word_t; endpackage\n";
    const std::string after = "package prelude; typedef logic [9:0] word_t; endpackage\n";
    REQUIRE(before.size() == after.size());
    workspace.write("prelude.sv", before);
    auto original = header;
    original.insert(original.find("    int local_0"), "    prelude::word_t local_word = '0;\n");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("local_1 +"), 7, "local_0");
    change(server, doc.m_uri, 2, text);
    REQUIRE(state(server).active);
    REQUIRE(server.m_methodScope);
    const auto caller = std::this_thread::get_id();
    size_t files = 0;
    lsp::RequestContext ctx("large proof", 201);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        CHECK(std::this_thread::get_id() == caller);
        files += where == "method input validation";
    });
    REQUIRE(server.m_methodScope->valid(server.m_config, ctx));
    REQUIRE(files >= 80);
    doc.m_text = text;
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(text.find("word_t") + 3)};
    auto previous = server.getDocHover(params);
    REQUIRE(previous);
    if (changed) {
        auto stamp = std::filesystem::last_write_time(workspace.root / "prelude.sv");
        workspace.write("prelude.sv", after);
        std::filesystem::last_write_time(workspace.root / "prelude.sv", stamp);
    }
    const auto actual = server.getDocHover(params);
    server.runBackgroundWork({});
    const auto actualDiagnostics = diagnosticMap(server);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", text);
    const auto expected = native.getDocHover(params);
    REQUIRE(expected);
    CHECK((rfl::json::write(previous) != rfl::json::write(expected)) == changed);
    CHECK(rfl::json::write(actual) == rfl::json::write(expected));
    CHECK(actualDiagnostics == diagnosticMap(native));
}

TEST_CASE("Batched absence proofs observe a newly available earlier include",
          "[body-certification][batched-file-proof][file-proof-absence]") {
    const auto kind = GENERATE("file", "symlink", "directory");
    Workspace workspace;
    addProofWitnessFiles(workspace);
    std::filesystem::create_directory(workspace.root / "fallback");
    std::filesystem::rename(workspace.root / "item.svh", workspace.root / "fallback/item.svh");
    auto recipe = std::string("+incdir+fallback\n");
    std::ifstream original(workspace.root / "design.f");
    recipe.append(std::istreambuf_iterator<char>(original), {});
    workspace.write("design.f", recipe);
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("fallback/item.svh", header);
    doc.doc->getAnalysis();
    change(server, doc.m_uri, 2, edited());
    REQUIRE(server.m_methodScope);
    REQUIRE(server.m_methodScope->valid(server.m_config, {}));
    bool appeared = false;
    lsp::RequestContext ctx("absent include became available", 283);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        // Directory names have already been checked. The per-request physical
        // absence proof must still observe a new candidate before publication.
        if (where != "method input validation" || appeared)
            return;
        appeared = true;
        if (std::string_view(kind) == "file")
            workspace.write("item.svh", header);
        else if (std::string_view(kind) == "symlink")
            std::filesystem::create_symlink(workspace.root / "fallback/item.svh",
                                            workspace.root / "item.svh");
        else
            std::filesystem::create_directory(workspace.root / "item.svh");
    });
    CHECK_FALSE(server.m_methodScope->valid(server.m_config, ctx));
    REQUIRE(appeared);
    CHECK(server.m_methodScope->observedInputChange());
    std::filesystem::remove_all(workspace.root / "item.svh");
    CHECK(server.m_methodScope->valid(server.m_config, {}));
}

TEST_CASE("Large content proof failure closes files before discarding its owner",
          "[body-certification][batched-file-proof]") {
    const auto failure = GENERATE("missing", "directory", "symlink");
    Workspace workspace;
    addProofWitnessFiles(workspace);
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = edited();
    change(server, doc.m_uri, 2, text);
    REQUIRE(state(server).active);
    REQUIRE(server.m_methodScope);
    const auto file = workspace.root / "proof_40.sv";
    REQUIRE(std::filesystem::remove(file));
    if (std::string_view(failure) == "directory")
        std::filesystem::create_directory(file);
    else if (std::string_view(failure) == "symlink")
        std::filesystem::create_symlink(workspace.root / "proof_41.sv", file);
    CHECK_FALSE(server.m_methodScope->valid(server.m_config, {}));
    CHECK(server.m_methodScope->observedInputChange());
    // No file descriptor or borrowed witness may outlive the failed proof.
    server.m_methodScope.reset();
    std::filesystem::remove_all(file);
    workspace.write("proof_40.sv", "package proof_40; int fixed; endpackage\n");
    CHECK(doc.doc->getVersion() == 2);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    doc.m_text = text;
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(text.rfind("charl") + 2)};
    const auto actual = server.getDocHover(params);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", text);
    CHECK(rfl::json::write(actual) == rfl::json::write(native.getDocHover(params)));
}

#if defined(__APPLE__)
TEST_CASE("Content proofs release directory access on cancellation and descriptor pressure",
          "[body-certification][batched-file-proof][file-proof-directories]") {
    const bool cancel = GENERATE(false, true);
    Workspace workspace;
    std::string recipe = "+incdir+.\nprelude.sv\nowner.sv\ntail.sv\n";
    for (int i = 0; i < 80; ++i) {
        const auto directory = "proof_parent_" + std::to_string(i);
        std::filesystem::create_directory(workspace.root / directory);
        const auto name = directory + "/dep.sv";
        workspace.write(name, "package proof_" + std::to_string(i) + "; int fixed; endpackage\n");
        recipe += name + "\n";
    }
    workspace.write("design.f", recipe);
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    change(server, doc.m_uri, 2, edited());
    REQUIRE(server.m_methodScope);
    auto descriptorCount = [] {
        return std::distance(std::filesystem::directory_iterator("/dev/fd"),
                             std::filesystem::directory_iterator{});
    };
    const auto before = descriptorCount();
    struct rlimit original;
    REQUIRE(::getrlimit(RLIMIT_NOFILE, &original) == 0);
    auto limited = original;
    limited.rlim_cur = std::min<rlim_t>(original.rlim_cur, 48);
    REQUIRE(before + 8 < limited.rlim_cur);
    REQUIRE(::setrlimit(RLIMIT_NOFILE, &limited) == 0);
    auto restore = slang::ScopeGuard([&] { ::setrlimit(RLIMIT_NOFILE, &original); });
    size_t reads = 0;
    lsp::RequestContext ctx("directory descriptor pressure", 281);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "method input validation" && ++reads == 12 && cancel)
            ctx.cancel();
    });
    if (cancel) {
        CHECK_THROWS_AS(server.m_methodScope->valid(server.m_config, ctx), lsp::RequestCancelled);
        CHECK_FALSE(server.m_methodScope->observedInputChange());
    }
    else {
        CHECK(server.m_methodScope->valid(server.m_config, ctx));
    }
    CHECK(descriptorCount() == before);
    CHECK(server.m_methodScope->valid(server.m_config, {}));
    CHECK(descriptorCount() == before);
}

TEST_CASE("Directory access cannot certify a file in a moved input directory",
          "[body-certification][batched-file-proof][file-proof-directories]") {
    Workspace workspace;
    addProofWitnessFiles(workspace);
    std::filesystem::create_directory(workspace.root / "proofs");
    std::string recipe = "+incdir+.\nprelude.sv\nowner.sv\ntail.sv\n";
    for (int i = 0; i < 80; ++i) {
        const auto name = "proof_" + std::to_string(i) + ".sv";
        std::filesystem::rename(workspace.root / name, workspace.root / "proofs" / name);
        recipe += "proofs/" + name + "\n";
    }
    workspace.write("design.f", recipe);
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    change(server, doc.m_uri, 2, edited());
    REQUIRE(server.m_methodScope);
    bool firstRead = false, moved = false;
    const auto first = (workspace.root / "proofs/proof_0.sv").string();
    lsp::RequestContext ctx("directory replacement during proof", 282);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == first)
            firstRead = true;
        if (firstRead && !moved && where == "method input validation") {
            moved = true;
            std::filesystem::rename(workspace.root / "proofs", workspace.root / "moved");
            std::filesystem::copy(workspace.root / "moved", workspace.root / "proofs",
                                  std::filesystem::copy_options::recursive);
        }
    });
    CHECK_FALSE(server.m_methodScope->valid(server.m_config, ctx));
    REQUIRE(moved);
    CHECK(server.m_methodScope->observedInputChange());
    // Retry must use the current namespace, not a directory handle from this proof.
    server.m_methodScope.reset();
    doc.m_text = edited();
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(doc.m_text.rfind("charl") + 2)};
    auto actual = server.getDocHover(params);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", edited());
    CHECK(rfl::json::write(actual) == rfl::json::write(native.getDocHover(params)));
}
#endif

TEST_CASE("Large content proof cancellation preserves publication and retries its latest input",
          "[body-certification][batched-file-proof]") {
#if defined(__APPLE__)
    const auto boundary = GENERATE("method input validation",
                                   "after batched method input validation");
#else
    const auto boundary = GENERATE("method input validation", "method inputs certified");
#endif
    Workspace workspace;
    addProofWitnessFiles(workspace);
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = edited();
    change(server, doc.m_uri, 2, text);
    REQUIRE(state(server).active);
    REQUIRE(server.m_methodScope);
    const auto previous = diagnosticMap(server);
    const auto caller = std::this_thread::get_id();
    bool reached = false;
    lsp::RequestContext ctx("cancel large proof", 202);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        CHECK(std::this_thread::get_id() == caller);
        if (where == boundary) {
            reached = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(server.m_methodScope->valid(server.m_config, ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(previous == diagnosticMap(server));
    CHECK_FALSE(server.m_methodScope->observedInputChange());
    REQUIRE(server.m_methodScope->valid(server.m_config, {}));
    auto next = header;
    next.replace(next.find("local_1 +"), 7, "local_0");
    next.insert(next.find("return ("), "int latest_local = 3;\n    ");
    change(server, doc.m_uri, 3, next);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == next + '\0');
    doc.m_text = next;
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(next.find("latest_local") + 3)};
    const auto actual = server.getDocHover(params);
    REQUIRE(actual);
    server.runBackgroundWork({});
    auto actualDiagnostics = diagnosticMap(server);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", next);
    CHECK(rfl::json::write(actual) == rfl::json::write(native.getDocHover(params)));
    CHECK(actualDiagnostics == diagnosticMap(native));
}

#if defined(__APPLE__)
TEST_CASE("Cancelled initial witness capture preserves unsaved input for the latest retry",
          "[body-certification][batched-file-proof]") {
    const auto cancelAt = GENERATE("before method input proof",
                                   "after batched method source witness read");
    Workspace workspace;
    addProofWitnessFiles(workspace);
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    const auto previous = diagnosticMap(server);
    auto text = edited();
    bool reached = false;
    lsp::RequestContext ctx("cancel initial witness batch", 203);
    const auto caller = std::this_thread::get_id();
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        CHECK(std::this_thread::get_id() == caller);
        if (where == cancelAt) {
            reached = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(change(server, doc.m_uri, 2, text, ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(previous == diagnosticMap(server));
    CHECK(doc.doc->getVersion() == 2);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    doc.m_text = text;
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(text.rfind("charl") + 2)};
    const auto actual = server.getDocHover(params);
    REQUIRE(actual);
    server.runBackgroundWork({});
    auto actualDiagnostics = diagnosticMap(server);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", text);
    CHECK(rfl::json::write(actual) == rfl::json::write(native.getDocHover(params)));
    CHECK(actualDiagnostics == diagnosticMap(native));
}
#endif

TEST_CASE("Initial reader errors reload changed dependencies before public native fallback",
          "[body-certification][batched-file-proof]") {
    const auto failure = GENERATE("symlink", "directory");
    Workspace workspace;
    addProofWitnessFiles(workspace);
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv", "package prelude;\n`include \"prelude.svh\"\nendpackage\n");
    workspace.write("prelude.svh", "typedef logic [7:0] word_t;\n");
    auto original = header;
    original.insert(original.find("    int local_0"), "    prelude::word_t local_word = '0;\n");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(original.find("word_t") + 3)};
    const auto previous = server.getDocHover(params);
    REQUIRE(previous);
    bool reached = false;
    lsp::RequestContext ctx("changed during initial witness read", 204);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (!reached && where == "method source witness read") {
            reached = true;
            workspace.write("replacement.svh", "typedef logic [9:0] word_t;\n");
            std::filesystem::remove(workspace.root / "prelude.svh");
            if (std::string_view(failure) == "directory")
                std::filesystem::create_directory(workspace.root / "prelude.svh");
            else
                std::filesystem::create_symlink(workspace.root / "replacement.svh",
                                                workspace.root / "prelude.svh");
        }
    });
    auto text = original;
    text.replace(text.find("local_1 +"), 7, "local_0");
    change(server, doc.m_uri, 2, text, ctx);
    REQUIRE(reached);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    const auto actual = server.getDocHover(params);
    server.runBackgroundWork({});
    const auto actualDiagnostics = diagnosticMap(server);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", text);
    const auto expected = native.getDocHover(params);
    if (std::string_view(failure) == "symlink")
        REQUIRE(expected);
    REQUIRE(rfl::json::write(previous) != rfl::json::write(expected));
    CHECK(rfl::json::write(actual) == rfl::json::write(expected));
    CHECK(actualDiagnostics == diagnosticMap(native));
}

TEST_CASE("Strict file proofs reject replaced parents before serving a fresh native type",
          "[body-certification][batched-file-proof]") {
    Workspace workspace;
    std::filesystem::create_directory(workspace.root / "inputs");
    workspace.write(".slang/server.json", certificationConfig());
    std::string prelude = "package prelude;\n";
    auto writeInputs = [&](bool changed) {
        for (int i = 0; i < 80; ++i) {
            const auto name = "inputs/type_" + std::to_string(i) + ".svh";
            if (i == 40)
                workspace.write(name, changed ? "typedef logic [9:0] word_t;\n"
                                              : "typedef logic [7:0] word_t;\n");
            else
                workspace.write(name, "typedef int type_" + std::to_string(i) + ";\n");
        }
    };
    writeInputs(false);
    for (int i = 0; i < 80; ++i)
        prelude += "`include \"inputs/type_" + std::to_string(i) + ".svh\"\n";
    workspace.write("prelude.sv", prelude + "endpackage\n");
    auto original = header;
    original.insert(original.find("    int local_0"), "    prelude::word_t local_word = '0;\n");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("local_1 +"), 7, "local_0");
    change(server, doc.m_uri, 2, text);
    REQUIRE(server.m_methodScope);
    doc.m_text = text;
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(text.find("word_t") + 3)};
    const auto old = server.getDocHover(params);
    REQUIRE(old);
    lsp::RequestContext ctx("replace proved parent", 205);
    bool replaced = false;
    const auto provedPath = (workspace.root / "inputs/type_0.svh").string();
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (!replaced && where == provedPath) {
            replaced = true;
            std::filesystem::rename(workspace.root / "inputs", workspace.root / "old_inputs");
            std::filesystem::create_directory(workspace.root / "inputs");
            writeInputs(true);
        }
    });
#if defined(__APPLE__)
    CHECK_FALSE(server.m_methodScope->valid(server.m_config, ctx));
    REQUIRE(replaced);
#else
    std::filesystem::rename(workspace.root / "inputs", workspace.root / "old_inputs");
    std::filesystem::create_directory(workspace.root / "inputs");
    writeInputs(true);
#endif
    const auto actual = server.getDocHover(params);
    server.runBackgroundWork({});
    const auto diagnostics = diagnosticMap(server);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", text);
    const auto expected = native.getDocHover(params);
    REQUIRE(expected);
    REQUIRE(rfl::json::write(old) != rfl::json::write(expected));
    CHECK(rfl::json::write(actual) == rfl::json::write(expected));
    CHECK(diagnostics == diagnosticMap(native));
}

#if defined(__APPLE__)
TEST_CASE("Complete file proof observes revoked read permission before publication",
          "[body-certification][batched-file-proof][file-proof-permissions]") {
    if (::geteuid() == 0)
        SKIP("The root credential can read files without owner read permission");
    Workspace workspace;
    addProofWitnessFiles(workspace);
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    change(server, doc.m_uri, 2, edited());
    REQUIRE(server.m_methodScope);
    REQUIRE(server.m_methodScope->valid(server.m_config, {}));
    const auto file = workspace.root / "proof_41.sv";
    const auto permissions = std::filesystem::status(file).permissions();
    auto restore = slang::ScopeGuard([&] { std::filesystem::permissions(file, permissions); });
    std::filesystem::permissions(file, std::filesystem::perms::none);
    const int fresh = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fresh >= 0)
        ::close(fresh);
    REQUIRE(fresh < 0);
    CHECK_FALSE(server.m_methodScope->valid(server.m_config, {}));
    CHECK(server.m_methodScope->observedInputChange());
    std::filesystem::permissions(file, permissions);
    CHECK(server.m_methodScope->valid(server.m_config, {}));
}

#endif

TEST_CASE("Strict file proofs retain exact filename identity after a case-only rename",
          "[body-certification][batched-file-proof]") {
    Workspace workspace;
    std::filesystem::create_directory(workspace.root / "inputs");
    workspace.write(".slang/server.json", certificationConfig());
    std::string prelude = "package prelude;\n";
    for (int i = 0; i < 80; ++i) {
        const auto name = i == 40 ? std::string("Types.svh") : "pad_" + std::to_string(i) + ".svh";
        workspace.write("inputs/" + name, i == 40 ? "typedef logic [7:0] word_t;\n"
                                                  : "typedef int pad_" + std::to_string(i) + ";\n");
        prelude += "`include \"inputs/" + name + "\"\n";
    }
    workspace.write("prelude.sv", prelude + "endpackage\n");
    auto original = header;
    original.insert(original.find("    int local_0"), "    prelude::word_t local_word = '0;\n");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("local_1 +"), 7, "local_0");
    change(server, doc.m_uri, 2, text);
    REQUIRE(server.m_methodScope);
    auto oldName = workspace.root / "inputs/Types.svh";
    auto newName = workspace.root / "inputs/tYPES.svh";
    auto stamp = std::filesystem::last_write_time(oldName);
    std::filesystem::rename(oldName, newName);
    std::filesystem::last_write_time(newName, stamp);
    CHECK_FALSE(server.m_methodScope->valid(server.m_config, {}));
    CHECK(server.m_methodScope->observedInputChange());
    doc.m_text = text;
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(text.find("word_t") + 3)};
    const auto actual = server.getDocHover(params);
    server.runBackgroundWork({});
    auto diagnostics = diagnosticMap(server);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", text);
    CHECK(rfl::json::write(actual) == rfl::json::write(native.getDocHover(params)));
    CHECK(diagnostics == diagnosticMap(native));
}

TEST_CASE("Full file proofs check late bytes without scanning unrelated directory names",
          "[body-certification][batched-file-proof]") {
    const bool largeDirectory = GENERATE(false, true);
    Workspace workspace;
    addProofWitnessFiles(workspace);
    workspace.write(".slang/server.json", certificationConfig());
    auto content = std::string("package proof_40; int fixed; endpackage\n/*") +
                   std::string(192 * 1024, 'x') + "*/\n";
    workspace.write("proof_40.sv", content);
    if (largeDirectory)
        for (int i = 0; i < 8200; ++i)
            workspace.write("unrelated_" + std::to_string(i) + ".tmp", "");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    change(server, doc.m_uri, 2, edited());
    REQUIRE(server.m_methodScope);
    REQUIRE(server.m_methodScope->valid(server.m_config, {}));
    auto path = workspace.root / "proof_40.sv";
    auto stamp = std::filesystem::last_write_time(path);
    content[content.size() - 10] = 'y';
    workspace.write("proof_40.sv", content);
    std::filesystem::last_write_time(path, stamp);
    CHECK_FALSE(server.m_methodScope->valid(server.m_config, {}));
    CHECK(server.m_methodScope->observedInputChange());
    server.runBackgroundWork({});
    const auto actual = diagnosticMap(server);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", edited());
    CHECK(actual == diagnosticMap(native));
}

#if defined(__APPLE__)
TEST_CASE("Final path metadata cannot certify bytes changed after descriptor validation",
          "[body-certification][batched-file-proof][file-proof-final-metadata]") {
    const auto mutation = GENERATE("none", "grow", "same-size");
    Workspace workspace;
    addProofWitnessFiles(workspace);
    std::string recipe = "000_dep.sv\n+incdir+.\nprelude.sv\nowner.sv\ntail.sv\n";
    for (int i = 0; i < 80; ++i)
        recipe += "proof_" + std::to_string(i) + ".sv\n";
    workspace.write("design.f", recipe);
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv", "");
    const std::string before = "package prelude; typedef logic [7:0] word_t; endpackage\n";
    std::string after = "package prelude; typedef logic [9:0] word_t; endpackage\n";
    REQUIRE(before.size() == after.size());
    if (std::string_view(mutation) == "grow")
        after += "// changed after the descriptor metadata check\n";
    workspace.write("000_dep.sv", before);
    auto original = header;
    original.insert(original.find("    int local_0"), "    prelude::word_t local_word = '0;\n");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("local_1 +"), 7, "local_0");
    change(server, doc.m_uri, 2, text);
    REQUIRE(server.m_methodScope);
    doc.m_text = text;
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(text.find("word_t") + 3)};
    const auto previous = server.getDocHover(params);
    REQUIRE(previous);
    const auto stamp = std::filesystem::last_write_time(workspace.root / "000_dep.sv");
    bool reached = false;
    lsp::RequestContext ctx("final path metadata", 280);
    const auto provedPath = (workspace.root / "000_dep.sv").string();
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (!reached && where == provedPath) {
            reached = true;
            if (std::string_view(mutation) != "none") {
                workspace.write("000_dep.sv", after);
                std::filesystem::last_write_time(workspace.root / "000_dep.sv", stamp);
            }
        }
    });
    CHECK(server.m_methodScope->valid(server.m_config, ctx) ==
          (std::string_view(mutation) == "none"));
    REQUIRE(reached);
    CHECK(server.m_methodScope->observedInputChange() == (std::string_view(mutation) != "none"));
    const auto actual = server.getDocHover(params);
    server.runBackgroundWork({});
    const auto diagnostics = diagnosticMap(server);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", text);
    const auto expected = native.getDocHover(params);
    REQUIRE(expected);
    CHECK((rfl::json::write(previous) != rfl::json::write(expected)) ==
          (std::string_view(mutation) != "none"));
    CHECK(rfl::json::write(actual) == rfl::json::write(expected));
    CHECK(diagnostics == diagnosticMap(native));
}
#endif

TEST_CASE("Local task timing diagnostics update transitive procedural callers",
          "[method-body-diagnostics][task-timing-closure]") {
    const bool certify = GENERATE(false, true);
    const auto caller = GENERATE(std::string(""), std::string("initial c.wrapper(1);"),
                                 std::string("always c.wrapper(1);"),
                                 std::string("always_comb c.wrapper(1);"),
                                 std::string("always_ff @(posedge clk) c.wrapper(1);"),
                                 std::string("always @(*) c.wrapper(1);"),
                                 std::string("always_comb begin c.wrapper(1); c.other(1); end"),
                                 std::string("always_comb begin c.other(1); c.wrapper(1); end"),
                                 std::string("instances"));
    INFO(caller);
    Workspace workspace;
    const std::string original = "class C;\n"
                                 " task update(input int arg);\n"
                                 "  int local_value = arg;\n"
                                 "  local_value += 1;\n"
                                 " endtask\n"
                                 " task wrapper(input int arg);\n"
                                 "  update(arg); if (arg > 0) wrapper(arg - 1);\n"
                                 " endtask\n"
                                 " task other(input int arg); #9; $display(arg); endtask\n"
                                 "endclass\n";
    workspace.write("item.svh", original);
    if (caller == "instances")
        workspace.write("tail.sv", "module unit; import owner::*; C c; "
                                   "always_comb c.wrapper(1); endmodule\n"
                                   "module tail; unit first(); unit second(); endmodule\n");
    else
        workspace.write("tail.sv",
                        "module tail; import owner::*; bit clk; C c; initial c = new;\n" + caller +
                            "\nendmodule\n");
    auto config = localDiagnosticConfig;
    if (certify)
        config.insert(config.size() - 1, ",\"experimentalMethodBodyCertification\":true");
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto& statement :
         {"local_value += 2;", "#1; local_value += 2;", "#3;\n  local_value += 2;",
          "$display(local_value); local_value += 2;", "local_value += 1;"}) {
        auto text = original;
        text.replace(text.find("local_value += 1;"), 17, statement);
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        auto status = diagnosticState(server);
        INFO(status.localDiagnosticDecline);
        CHECK(status.localDiagnosticUpdates == uint64_t(version - 1));
        CHECK(server.client.diagnosticVersions.at(doc.m_uri) == version);
        checkNativeDiagnostics(workspace, server, text);
    }
}

TEST_CASE("Cancelling task caller diagnostic updates preserves the published map",
          "[method-body-diagnostics][task-timing-closure][diagnostic-cancellation]") {
    Workspace workspace;
    const std::string original = "class C; task run(input int arg); int local_value = arg; "
                                 "local_value += 1; endtask endclass\n";
    workspace.write("item.svh", original);
    workspace.write("tail.sv", "module tail; import owner::*; C c; "
                               "always_comb c.run(1); endmodule\n");
    workspace.write(".slang/server.json", localDiagnosticConfig);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto before = diagnosticMap(server);
    auto text = original;
    text.insert(text.find("local_value +="), "#1; ");
    change(server, doc.m_uri, 2, text);
    lsp::RequestContext ctx("background", 92);
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "task diagnostic caller update") {
            reached = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(before == diagnosticMap(server));
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
    server.runBackgroundWork({});
    INFO(diagnosticState(server).localDiagnosticDecline);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Configured source history bounds actual Native growth and keeps current text",
          "[source-history-budget][frozen-retirement]") {
    const bool cooperative = GENERATE(false, true);
    const uint32_t budget = GENERATE(1u, 64u);
    Workspace workspace;
    auto config = localDiagnosticConfig;
    config.insert(config.size() - 1,
                  ",\"sourceHistoryMaxMiB\":" + std::to_string(budget) +
                      ",\"experimentalCooperativeAnalysis\":" + (cooperative ? "true" : "false"));
    workspace.write(".slang/server.json", config);
    struct Harness : BodyTestServer {
        using BodyTestServer::BodyTestServer;
        using SlangServer::onForegroundComplete;
    };
    Harness server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto epoch = server.m_driver->completions.getCompletionEpoch();
    const auto text = header + "/*" + std::string(2 * 1024 * 1024, 'x') + "*/\n";
    // This edit is outside the method; exercise Native source history with no
    // UVM, frozen scopes, prefix cache, or certificate renewal enabled.
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    REQUIRE(server.m_driver->sm.getMemoryUsageDetails().dataBytes >= 2 * 1024 * 1024);
    server.onForegroundComplete();
    CHECK((server.m_driver->completions.getCompletionEpoch() != epoch) ==
          (cooperative && budget == 1));
    const auto latest = header + "// recovered small input\n";
    change(server, doc.m_uri, 3, latest);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == latest + '\0');
    server.runBackgroundWork({});
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 3);
    checkNativeDiagnostics(workspace, server, latest);
}

TEST_CASE("Native completion tickets survive the first source history retirement",
          "[source-history-budget][completion-retirement]") {
    const bool parameterized = GENERATE(false, true);
    Workspace workspace;
    workspace.write(".slang/server.json",
                    "{\"indexingThreads\":1,\"flags\":\"--single-unit --threads 1\","
                    "\"compilationFilelist\":\"design.f\","
                    "\"experimentalCooperativeAnalysis\":true}");
    const std::string text =
        parameterized ? "class base #(int W=8); function void consume(bit[W-1:0] value); "
                        "endfunction endclass\n"
                        "class item extends base #(64);\n"
                        "function void run(); item receiver; receiver.; endfunction\nendclass\n"
                      : "class base; extern function void consume(int value); endclass\n"
                        "function void base::consume(int value); endfunction\n"
                        "class item extends base;\n"
                        "function void run(); item receiver; receiver.; endfunction\nendclass\n";
    workspace.write("item.svh", text);
    struct Harness : BodyTestServer {
        using BodyTestServer::BodyTestServer;
        void retireNow() {
            m_contextBudget = m_contextHardBudget = 1;
            onForegroundComplete();
            admitRetirementInputs({});
        }
    };
    auto params = workspace.params();
    params.capabilities.textDocument.emplace();
    params.capabilities.textDocument->completion.emplace();
    params.capabilities.textDocument->completion->completionItem.emplace();
    params.capabilities.textDocument->completion->completionItem->resolveSupport =
        lsp::ClientCompletionItemResolveOptions{.properties = {"documentation"}};
    Harness server(params);
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", text);
    auto items = doc.after("receiver.").getCompletions(".");
    auto item = std::ranges::find_if(items, [](const auto& item) {
        return item.m_item.label == "consume";
    });
    REQUIRE(item != items.end());
    REQUIRE_FALSE(item->m_item.documentation);
    auto reference = *item;
    reference.resolve();
    REQUIRE(reference.m_item.documentation);
    const auto expected = rfl::get<lsp::MarkupContent>(*reference.m_item.documentation).value;
    if (!parameterized)
        REQUIRE(expected.find("extern") != std::string::npos);
    const auto epoch = server.m_driver->completions.getCompletionEpoch();
    CAPTURE(parameterized, doc.doc->getBuffer().getId(), doc.doc->getCompletionTextIdentity());
    INFO(rfl::json::write(item->m_item));
    server.retireNow();
    auto current = server.m_driver->getDocument(doc.m_uri);
    CAPTURE(current->getBuffer().getId(), current->getCompletionTextIdentity(),
            current->getVersion());
    REQUIRE(server.m_driver->completions.getCompletionEpoch() != epoch);
    item->resolve();
    REQUIRE(item->m_item.documentation);
    CHECK(rfl::get<lsp::MarkupContent>(*item->m_item.documentation).value == expected);
}

TEST_CASE("Independent native receiver reads admit changed ordinary property use",
          "[method-body-diagnostics][independent-native-receiver]") {
    const auto peerKind = GENERATE("live", "absent", "static", "generic", "dead");
    const bool navigate = GENERATE(false, true);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const bool isStatic = std::string_view(peerKind) == "static";
    const bool isGeneric = std::string_view(peerKind) == "generic";
    const bool hasPeer = std::string_view(peerKind) != "absent";
    const bool isDead = std::string_view(peerKind) == "dead";
    const std::string token =
        isGeneric ? "class ReceiverToken #(int W=8); function int value(); return W; endfunction "
                    "endclass\n"
                  : "class ReceiverToken; function int value(); return 8; endfunction endclass\n";
    auto original = header;
    original.insert(original.find("  int marker;"),
                    std::string("  ") + (isStatic ? "static " : "") +
                        (isGeneric ? "ReceiverToken#(8)" : "ReceiverToken") + " receiver;\n");
    if (hasPeer)
        original.insert(original.rfind("endclass"),
                        "  function int unedited_peer(); " + std::string(isDead ? "if (0) " : "") +
                            "return receiver.value(); " + (isDead ? "return 0; " : "") +
                            "endfunction\n");
    original = token + original;
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    if (navigate)
        for (int i = 0; i < 3; ++i)
            server.getDocHover({.textDocument = {.uri = doc.m_uri},
                                .position = doc.getPosition(original.find("receiver;") + 3)});
    auto text = original;
    text.replace(text.find("local_1 + charl"), 15, "(receiver != null) + charl");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    const auto status = diagnosticState(server);
    INFO(status.localDiagnosticDecline);
    CHECK(status.localDiagnosticUpdates == 1);
    checkNativeDiagnostics(workspace, server, text, true);
    // Removal must restore diagnostics using the same independent peer evidence.
    change(server, doc.m_uri, 3, original);
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, original, true);
}

TEST_CASE("Receiver dependency capture cancellation retains the latest input",
          "[method-body-diagnostics][independent-native-receiver]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    auto original = "class ReceiverToken; function int value(); return 8; endfunction endclass\n" +
                    header;
    original.insert(original.find("  int marker;"), "  ReceiverToken receiver;\n");
    original.insert(original.rfind("endclass"),
                    "  function int unedited_peer(); return receiver.value(); endfunction\n");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const auto previous = diagnosticMap(server);
    auto text = original;
    text.replace(text.find("local_1 + charl"), 15, "(receiver != null) + charl");
    lsp::RequestContext ctx("textDocument/didChange", lsp::ID_t{715});
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view at) {
        if (at == "independent native receiver reads") {
            reached = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(change(server, doc.m_uri, 2, text, ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    CHECK(diagnosticMap(server) == previous);
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Class-outside peer evidence excludes repeated edited include contexts",
          "[method-body-diagnostics][independent-native-receiver]") {
    const bool duplicate = GENERATE(false, true);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    if (duplicate)
        workspace.write("owner.sv",
                        "package first_pkg;\n`include \"item.svh\"\nendpackage\npackage "
                        "second_pkg;\n`include \"item.svh\"\nendpackage\n");
    auto original = "class ReceiverToken; function int value(); return 8; endfunction endclass\n" +
                    header;
    original.insert(original.find("  int marker;"), "  ReceiverToken receiver;\n");
    original.insert(original.rfind("endclass"), "  extern function int unedited_peer();\n");
    original += "function int Accumulator::unedited_peer(); return receiver.value(); endfunction\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    if (duplicate)
        REQUIRE(server.m_driver->setIncludeContext(
            {.uri = doc.m_uri,
             .source = URI::fromFile(workspace.root / "owner.sv"),
             .occurrence = 0}));
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("local_1 + charl"), 15, "(receiver != null) + charl");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    const auto status = diagnosticState(server);
    INFO(status.localDiagnosticDecline);
    CHECK(status.localDiagnosticUpdates == (duplicate ? 0 : 1));
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Native consumers admit changed certificate inputs after provisional text sync",
          "[body-certification][publication-only-proof]") {
    const auto route = GENERATE("native-protocol", "semantic-command", "native-hover-fallback",
                                "background-only");
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv", "package prelude; typedef logic [7:0] word_t; endpackage\n");
    auto original = header;
    original.insert(original.find("  int marker;"), "  prelude::word_t stable_word;\n");
    workspace.write("item.svh", original);
    struct Server : CertificationServer {
        using CertificationServer::CertificationServer;
        using SlangServer::onBeforeMessage;
    };
    Server server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const lsp::HoverParams params{.textDocument = {.uri = doc.m_uri},
                                  .position = doc.getPosition(original.find("stable_word") + 3)};
    const auto previous = rfl::json::write(server.getDocHover(params));
    auto text = original;
    text.replace(text.find("local_1 + charl"), 15, "local_0 + charl");
    change(server, doc.m_uri, 2, text);
    REQUIRE(server.m_methodScope);
    auto* previousDriver = server.m_driver.get();
    const auto path = workspace.root / "prelude.sv";
    const auto stamp = std::filesystem::last_write_time(path);
    workspace.write("prelude.sv", "package prelude; typedef logic [9:0] word_t; endpackage\n");
    std::filesystem::last_write_time(path, stamp);
    const auto method = std::string_view(route);
    lsp::RequestContext ctx("textDocument/inlayHint", lsp::ID_t{716});
    if (method == "native-protocol") {
        server.onBeforeMessage(ctx);
        server.getDocInlayHint({.textDocument = {.uri = doc.m_uri}, .range = {{0, 0}, {100, 0}}});
    }
    else if (method == "semantic-command")
        server.executeCommand(
            {.command = "slang.getScope",
             .arguments = std::vector<lsp::LSPAny>{rfl::to_generic(std::string("prelude"))}},
            ctx);
    else if (method == "native-hover-fallback")
        server.getDocHover(params, ctx);
    else
        server.runBackgroundWork({});
    CHECK(server.m_driver.get() != previousDriver);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    const auto actual = rfl::json::write(server.getDocHover(params));
    CHECK(actual != previous);
    server.runBackgroundWork({});
    const auto actualDiagnostics = diagnosticMap(server);
    bodyConfig(workspace, false);
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    native.openFile("item.svh", text);
    CHECK(actual == rfl::json::write(native.getDocHover(params)));
    CHECK(actualDiagnostics == diagnosticMap(native));
}

TEST_CASE("Provisional sync cancels native input admission without publishing stale results",
          "[body-certification][publication-only-proof]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    struct Server : CertificationServer {
        using CertificationServer::CertificationServer;
        using SlangServer::onBeforeMessage;
    };
    Server server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = edited();
    change(server, doc.m_uri, 2, text);
    REQUIRE(server.m_methodScope);
    server.m_driver->deferDocumentSave({.textDocument = {.uri = doc.m_uri}});
    REQUIRE(server.m_driver->hasDeferredSaveIndexes());
    const auto previous = diagnosticMap(server);
    lsp::RequestContext ctx("textDocument/codeLens", lsp::ID_t{717});
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view at) {
        if (at == "method input validation") {
            reached = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(server.onBeforeMessage(ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(server.m_driver->hasDeferredSaveIndexes());
    CHECK(diagnosticMap(server) == previous);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    server.onBeforeMessage(lsp::RequestContext("textDocument/codeLens", lsp::ID_t{718}));
    CHECK_FALSE(server.m_driver->hasDeferredSaveIndexes());
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Independent completed Native callers certify newly used ordinary methods",
          "[method-body-diagnostics][independent-native-callee]") {
    const auto peerKind = GENERATE("live", "extern", "absent", "edited-only", "navigation",
                                   "static");
    INFO(peerKind);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    auto original = header;
    if (std::string_view(peerKind) == "live" || std::string_view(peerKind) == "static")
        original.insert(original.rfind("endclass"),
                        "  function int unedited_peer(); return later(7); endfunction\n");
    else if (std::string_view(peerKind) == "extern") {
        original.insert(original.rfind("endclass"), "  extern function int unedited_peer();\n");
        original += "function int Accumulator::unedited_peer(); return later(7); endfunction\n";
    }
    if (std::string_view(peerKind) == "static")
        original.insert(original.find("function int later"), "static ");
    const std::string previous = "(local_1 + charl)";
    const std::string next = "(later(local_1) + charl)";
    const bool removal = std::string_view(peerKind) == "edited-only";
    if (removal)
        original.replace(original.find(previous), previous.size(), next);
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    if (std::string_view(peerKind) == "navigation") {
        auto hover = server.getDocHover(
            {.textDocument = {.uri = doc.m_uri},
             .position = doc.getPosition(original.find("function int later") + 14)});
        REQUIRE(hover);
    }
    auto text = original;
    text.replace(text.find(removal ? next : previous), removal ? next.size() : previous.size(),
                 removal ? previous : next);
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    const auto status = diagnosticState(server);
    INFO(status.localDiagnosticDecline);
    CHECK(status.localDiagnosticUpdates == 1);
    checkNativeDiagnostics(workspace, server, text, true);
    // Removing the new call must also retain complete Native diagnostics.
    change(server, doc.m_uri, 3, original);
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, original, true);
}

TEST_CASE("Automatic package and module subroutines update local diagnostics",
          "[method-body-diagnostics][automatic-context-diagnostics]") {
    const auto context = GENERATE("package", "module");
    const bool task = GENERATE(false, true);
    const bool used = GENERATE(false, true);
    INFO(context << " task=" << task << " used=" << used);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string original =
        task ? "task automatic work(input int arg);\n  int local_value = arg;\n  local_value += "
               "1;\nendtask\n"
             : "function automatic int work(input int arg);\n  int local_value = arg;\n  return "
               "local_value + 1;\nendfunction\n";
    workspace.write("item.svh", original);
    const bool package = std::string_view(context) == "package";
    const std::string caller = task ? "always_comb work(runtime_value);\n"
                                    : "initial result = work(runtime_value);\n";
    const std::string variables = "int runtime_value; int result;\n";
    if (package) {
        workspace.write("owner.sv", "package owner;\n`include \"item.svh\"\nendpackage\n");
        workspace.write("tail.sv", "module tail; import owner::*;\n" + variables +
                                       (used ? caller : "") + "endmodule\n");
    }
    else {
        workspace.write("owner.sv", "module owner;\n" + variables + "`include \"item.svh\"\n" +
                                        (used ? caller : "") + "endmodule\n");
    }
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const std::vector<std::string> replacements =
        task ? std::vector<std::string>{"#1; local_value += 2;", "#3;\n  local_value += 3;",
                                        "local_value += 4;"}
             : std::vector<std::string>{"return local_value + 2;",
                                        "return local_value +\n    unknown_user_value;",
                                        "return local_value + 4;"};
    const std::string old = task ? "local_value += 1;" : "return local_value + 1;";
    uint64_t updates = 0;
    for (const auto& replacement : replacements) {
        auto text = original;
        text.replace(text.find(old), old.size(), replacement);
        change(server, doc.m_uri, int(++updates) + 1, text);
        server.runBackgroundWork({});
        const auto status = diagnosticState(server);
        INFO(status.localDiagnosticDecline);
        CHECK(status.localDiagnosticUpdates == updates);
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Automatic nonclass diagnostics preserve external dependency boundaries",
          "[method-body-diagnostics][automatic-context-diagnostics]") {
    const auto boundary = GENERATE("static-lifetime", "ref-formal", "output-formal", "inout-formal",
                                   "persistent-write", "constant-evaluation", "multiple-instances",
                                   "interface-context", "program-context");
    INFO(boundary);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    std::string original = "function automatic int work(input int arg);\n  int local_value = "
                           "arg;\n  return local_value + 1;\nendfunction\n";
    const auto kind = std::string_view(boundary);
    if (kind == "static-lifetime")
        original.replace(original.find("automatic "), 10, "");
    else if (kind == "ref-formal")
        original.replace(original.find("input int"), 9, "ref int");
    else if (kind == "output-formal")
        original.replace(original.find("input int"), 9, "output int");
    else if (kind == "inout-formal")
        original.replace(original.find("input int"), 9, "inout int");
    else if (kind == "persistent-write")
        original.insert(original.find("  return"), "  global_value = arg;\n");
    workspace.write("item.svh", original);
    if (kind == "persistent-write" || kind == "multiple-instances") {
        workspace.write("owner.sv",
                        "module owner; int global_value;\n`include \"item.svh\"\nendmodule\n");
        if (kind == "multiple-instances")
            workspace.write("tail.sv", "module tail; owner first(); owner second(); endmodule\n");
    }
    else if (kind == "interface-context" || kind == "program-context") {
        const std::string keyword = kind == "interface-context" ? "interface" : "program";
        workspace.write("owner.sv",
                        keyword + " owner;\n`include \"item.svh\"\nend" + keyword + "\n");
        workspace.write("tail.sv", "module tail; owner environment(); endmodule\n");
    }
    else {
        workspace.write("owner.sv", "package owner;\n`include \"item.svh\"\nendpackage\n");
        if (kind == "constant-evaluation")
            workspace.write("tail.sv",
                            "module tail; localparam int VALUE = owner::work(3); endmodule\n");
    }
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("local_value + 1"), 15, "local_value + 2");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == (kind == "persistent-write" ? 1 : 0));
    checkNativeDiagnostics(workspace, server, text, true);
}

TEST_CASE("Automatic nonclass edits preserve caller static-driver diagnostics",
          "[method-body-diagnostics][automatic-context-driver-closure]") {
    const auto context = GENERATE("package", "module");
    const bool initializer = GENERATE(false, true);
    const bool transitive = GENERATE(false, true);
    INFO(context << " initializer=" << initializer << " transitive=" << transitive);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string original = "function automatic int work(input int arg);\n"
                                 "  static int saved;\n"
                                 "  return arg;\n"
                                 "endfunction\n";
    workspace.write("item.svh", original);
    const std::string callee = transitive ? "wrapper" : "work";
    const std::string wrapper =
        transitive
            ? "function automatic int wrapper(input int value); return work(value); endfunction\n"
            : "";
    const std::string initializerCaller = initializer
                                              ? "int boot_result = " + callee + "(first_input);\n"
                                              : "";
    const std::string caller = "int first_input, second_input, first_result, second_result;\n"
                               "always_comb first_result = " +
                               callee +
                               "(first_input);\n"
                               "always_comb second_result = " +
                               callee + "(second_input);\n";
    if (std::string_view(context) == "package") {
        workspace.write("owner.sv",
                        "package owner;\n`include \"item.svh\"\n" + wrapper + "endpackage\n");
        workspace.write("tail.sv", "module tail; import owner::*;\n" + caller + initializerCaller +
                                       "endmodule\n");
    }
    else {
        workspace.write("owner.sv", "module owner;\n`include \"item.svh\"\n" + wrapper + caller +
                                        initializerCaller + "endmodule\n");
    }
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("return arg;"), 11, "saved = arg; return saved;");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    checkNativeDiagnostics(workspace, server, text, true);
    bool conflict = false;
    for (const auto& uri : server.client.diagnosticPublications)
        for (const auto& diagnostic : server.client.getDiagnostics(uri))
            conflict |= diagnostic.code &&
                        rfl::json::write(*diagnostic.code) == "\"multiple-always-assigns\"";
    CHECK(conflict);
    auto changed = text;
    changed.replace(changed.find("saved = arg;"), 12, "saved = arg + 1;");
    change(server, doc.m_uri, 3, changed);
    const auto beforeCancellation = diagnosticMap(server);
    lsp::RequestContext cancellation("effect replay cancellation", 991);
    bool reached = false;
    cancellation.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "local effect replay procedure") {
            reached = true;
            cancellation.cancel();
        }
    });
    CHECK_THROWS_AS(server.runBackgroundWork(cancellation), lsp::RequestCancelled);
    CHECK(reached);
    CHECK(diagnosticMap(server) == beforeCancellation);
    CHECK(server.m_driver->getDocument(doc.m_uri)->textMatches(changed));
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 2);
    checkNativeDiagnostics(workspace, server, changed, true);
    change(server, doc.m_uri, 4, original);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 3);
    checkNativeDiagnostics(workspace, server, original, true);
    change(server, doc.m_uri, 5, text);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 4);
    checkNativeDiagnostics(workspace, server, text, true);
}

TEST_CASE("Automatic body edits update previously unresolved external local lookups",
          "[method-body-diagnostics][automatic-context-external-lookup]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string original = "function automatic int work(input int arg);\n"
                                 "  return arg;\n"
                                 "endfunction\n";
    workspace.write("item.svh", original);
    workspace.write("owner.sv", "module owner;\n`include \"item.svh\"\nendmodule\n");
    workspace.write("tail.sv", "module tail; owner unit(); int result;\n"
                               "initial result = unit.work.introduced;\nendmodule\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.insert(text.find("  return"), "  static int introduced;\n");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, text, true);
    change(server, doc.m_uri, 3, original);
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, original, true);
}

TEST_CASE("Package body line edits retain independent later class macros",
          "[method-body-diagnostics][later-declaration-macros]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const auto declaration =
        GENERATE("typedef int member_t;",
                 "typedef int member_t; static function int get_kind(); return 0; endfunction",
                 "localparam string FILE_NAME = `__FILE__; typedef int member_t;");
    const std::string original =
        "`define MEMBER_TYPE " + std::string(declaration) +
        "\n"
        "`define REPORT(MSG) report(MSG, `__LINE__);\n"
        "function automatic int work(input int arg);\n"
        "  int local_value = arg;\n"
        "  return local_value + 1;\n"
        "endfunction\n"
        "class Later;\n"
        "  `MEMBER_TYPE\n"
        "  function void report(input string msg, input int line_number); endfunction\n"
        "  function void later(); `REPORT(\"hello\") endfunction\n"
        "endclass\n";
    workspace.write("item.svh", original);
    workspace.write("tail.sv", "module tail; int value, result;\n"
                               "initial result = owner::work(value); endmodule\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const std::string replaced = "return local_value + 1;";
    uint64_t updates = 0;
    for (const std::string replacement :
         {"return local_value +\n    2;", "return local_value +\n    unknown_user_value;",
          "return local_value + 3;"}) {
        auto text = original;
        text.replace(text.find(replaced), replaced.size(), replacement);
        change(server, doc.m_uri, int(++updates) + 1, text);
        server.runBackgroundWork({});
        const auto status = diagnosticState(server);
        INFO(status.localDiagnosticDecline);
        CHECK(status.localDiagnosticUpdates == updates);
        CHECK(state(server).active);
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Later declaration macro proof preserves semantic line dependencies",
          "[method-body-diagnostics][later-declaration-macros]") {
    const auto kind = GENERATE("direct", "indirect", "default-argument", "actual-argument",
                               "redefined", "dynamic-name", "dynamic-name-collision");
    INFO(kind);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    std::string definitions, invocation = "`MEMBER";
    const std::string width = "localparam int WIDTH = ";
    if (std::string_view(kind) == "direct")
        definitions = "`define MEMBER " + width + "`__LINE__; typedef logic[WIDTH-1:0] word_t;\n";
    else if (std::string_view(kind) == "indirect" || std::string_view(kind) == "redefined") {
        definitions = "`define LINE_VALUE `__LINE__\n`define MEMBER " + width +
                      "`LINE_VALUE; typedef logic[WIDTH-1:0] word_t;\n";
    }
    else if (std::string_view(kind) == "default-argument") {
        definitions = "`define MEMBER(W=`__LINE__) " + width +
                      "W; typedef logic[WIDTH-1:0] word_t;\n";
        invocation = "`MEMBER()";
    }
    else if (std::string_view(kind) == "actual-argument") {
        definitions = "`define MEMBER(W) " + width + "W; typedef logic[WIDTH-1:0] word_t;\n";
        invocation = "`MEMBER(`__LINE__)";
    }
    else {
        definitions = (std::string_view(kind) == "dynamic-name-collision" ? "`define M 1\n" : "") +
                      std::string("`define MEMBER(M) ") + width +
                      "`M; typedef logic[WIDTH-1:0] word_t;\n";
        invocation = "`MEMBER(__LINE__)";
    }
    const std::string original =
        definitions +
        "function automatic int work(input int arg);\n  return arg + 1;\nendfunction\n"
        "class Later;\n" +
        invocation +
        "\nfunction void sink(input word_t value); logic dst; dst = value; "
        "endfunction\nendclass\n" +
        (std::string_view(kind) == "redefined" ? "`undef LINE_VALUE\n`define LINE_VALUE 8\n" : "");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    const std::string replaced = "return arg + 1;";
    text.replace(text.find(replaced), replaced.size(), "return arg +\n    2;");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    if (std::string_view(kind) == "default-argument") {
        // A default macro argument retains its definition-site line. Prove the
        // unchanged type independently with two fresh native compilations.
        auto freshWidth = [](const std::string& content) {
            SourceManager sources;
            auto tree = syntax::SyntaxTree::fromText("package owner;\n" + content + "endpackage\n",
                                                     sources);
            ast::Compilation compilation;
            compilation.addSyntaxTree(tree);
            (void)compilation.getSemanticDiagnostics();
            auto package = compilation.getPackage("owner");
            REQUIRE(package);
            auto cls = package->find("Later");
            REQUIRE(cls);
            auto alias = cls->as<ast::ClassType>().find("word_t");
            REQUIRE(alias);
            return alias->as<ast::TypeAliasType>().getCanonicalType().getBitWidth();
        };
        CHECK(freshWidth(original) == freshWidth(text));
        CHECK(state(server).active);
        CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    }
    else {
        CHECK_FALSE(state(server).active);
        CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
    }
    checkNativeDiagnostics(workspace, server, text, true);
}

TEST_CASE("Later declaration journal cancellation preserves the newest editor text",
          "[method-body-diagnostics][later-declaration-macros][foreground-cancellation]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string original =
        "`define MEMBER typedef int word_t;\n"
        "function automatic int work(input int value); return value; endfunction\n"
        "class Later; `MEMBER endclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    const std::string old = "return value;";
    text.replace(text.find(old), old.size(), "return value +\n  2;");
    bool reached = false;
    lsp::RequestContext ctx("textDocument/didChange", std::nullopt);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "later macro definition journal") {
            reached = true;
            ctx.cancelForContentChange();
        }
    });
    CHECK_THROWS_AS(change(server, doc.m_uri, 2, text, ctx), lsp::RequestCancelled);
    CHECK(reached);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getVersion() == 2);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    change(server, doc.m_uri, 3, original);
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, original, true);
}
TEST_CASE("Package edits update methods generated across macro invocations",
          "[method-body-diagnostics][generated-macro-methods]") {
    const bool semanticLine = GENERATE(false, true);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string type = semanticLine ? "typedef logic[`__LINE__-1:0] word_t; "
                                          : "typedef int word_t; ";
    const std::string original =
        "`define FACTORY_BEGIN(T) " + type +
        "static function int get_kind(); return 0; endfunction "
        "function void automate(input int op);\n"
        "`define FIELD(V) report(\"field\", `__LINE__); V += op;\n"
        "`define FACTORY_END endfunction\n"
        "function automatic int work(input int arg);\n"
        "  return arg + 1;\nendfunction\n"
        "class Later;\nint state;\n"
        "function void report(input string msg, input int line_number); endfunction\n"
        "`FACTORY_BEGIN(Later)\n`FIELD(state)\n`FACTORY_END\n"
        "function void sink(input word_t value); logic dst; dst = value; endfunction\n"
        "endclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const std::string old = "return arg + 1;";
    uint64_t updates = 0;
    for (const std::string replacement :
         {"return arg +\n  2;", "return arg +\n  missing_user_value;", "return arg + 3;"}) {
        auto text = original;
        text.replace(text.find(old), old.size(), replacement);
        change(server, doc.m_uri, int(++updates) + 1, text);
        server.runBackgroundWork({});
        if (!semanticLine) {
            INFO(diagnosticState(server).localDiagnosticDecline);
            CHECK(state(server).active);
            CHECK(diagnosticState(server).localDiagnosticUpdates == updates);
        }
        else if (updates == 1) {
            CHECK_FALSE(state(server).active);
            CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
        }
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Generated macro line proof rejects a recovered syntax baseline",
          "[method-body-diagnostics][generated-macro-methods]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string original =
        "`define START function void automate();\n"
        "`define FIELD report(`__LINE__) report(1);\n"
        "`define FINISH endfunction\n"
        "function automatic int work(input int arg); return arg + 1; endfunction\n"
        "class Later;\nfunction void report(input int line_number); endfunction\n"
        "`START\n`FIELD\n`FINISH\nendclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    const std::string old = "return arg + 1;";
    text.replace(text.find(old), old.size(), "return arg +\n  2;");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    CHECK_FALSE(state(server).active);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
    checkNativeDiagnostics(workspace, server, text, true);
}

TEST_CASE("Generated macro line proof checks every generic specialization",
          "[method-body-diagnostics][generated-macro-methods]") {
    const bool narrowed = GENERATE(false, true);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string original =
        "`define START function void automate();\n"
        "`define FIELD report(`__LINE__);\n"
        "`define FINISH endfunction\n"
        "function automatic int work(input int arg); return arg + 1; endfunction\n"
        "class Later #(type T=int, int ID=0);\n"
        "function void report(input T line_number); endfunction\n"
        "`START\n`FIELD\n`FINISH\nendclass\n"
        "Later#(int) first;\nLater#(" +
        std::string(narrowed ? "byte" : "int") +
        ", 1) second;\n"
        "function automatic void invoke(); first.automate(); second.automate(); endfunction\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const std::string old = "return arg + 1;";
    uint64_t updates = 0;
    for (const std::string replacement :
         {"return arg +\n  2;", "return arg +\n  missing_user_value;", "return arg + 3;"}) {
        auto text = original;
        text.replace(text.find(old), old.size(), replacement);
        change(server, doc.m_uri, int(++updates) + 1, text);
        server.runBackgroundWork({});
        INFO(diagnosticState(server).localDiagnosticDecline);
        const auto overlay = state(server);
        if (narrowed && updates == 1) {
            CHECK_FALSE(overlay.active);
            CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
            CHECK(overlay.decline == "macro line value changes semantic structure");
        }
        else if (!narrowed) {
            CHECK(overlay.active);
            CHECK(diagnosticState(server).localDiagnosticUpdates == updates);
        }
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Generated macro replay cancellation preserves the latest text",
          "[method-body-diagnostics][generated-macro-methods][foreground-cancellation]") {
    const auto boundary = GENERATE("native generated macro contexts", "macro replay environment",
                                   "native macro expansion token evidence",
                                   "macro replay preprocessing");
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string original =
        "`define START function void automate();\n"
        "`define FIELD report(`__LINE__);\n"
        "`define FINISH endfunction\n"
        "function automatic int work(input int arg); return arg + 1; endfunction\n"
        "class Later;\nfunction void report(input int line_number); endfunction\n"
        "`START\n`FIELD\n`FINISH\nendclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    const std::string old = "return arg + 1;";
    text.replace(text.find(old), old.size(), "return arg +\n  2;");
    bool reached = false;
    lsp::RequestContext ctx("textDocument/didChange", std::nullopt);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == boundary) {
            reached = true;
            ctx.cancelForContentChange();
        }
    });
    CHECK_THROWS_AS(change(server, doc.m_uri, 2, text, ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getVersion() == 2);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    change(server, doc.m_uri, 3, original);
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, original, true);
}

TEST_CASE("Generic macro syntax identity cancellation retains current input",
          "[method-body-diagnostics][generated-macro-methods][foreground-cancellation]") {
    const auto boundary = GENERATE("generated macro query syntax identity",
                                   "before generated macro query syntax witness commit");
    const bool nativeView = GENERATE(false, true);
    CAPTURE(boundary, nativeView);
    bool declined = false;
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string original =
        "`define START function void automate();\n"
        "`define FIELD report(`__LINE__);\n"
        "`define FINISH endfunction\n"
        "function automatic int work(input int arg); return arg + 1; endfunction\n"
        "class Later #(int N=0);\nfunction void report(input int line_number); endfunction\n"
        "`START\n`FIELD\n`FINISH\nendclass\n"
        "Later#(0) first; Later#(1) second;\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    const std::string old = "return arg + 1;";
    text.replace(text.find(old), old.size(), "return arg +\n  2;");
    bool reached = false;
    lsp::RequestContext ctx("textDocument/didChange", std::nullopt);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (!nativeView && !declined && where == "before completed body query context indexing") {
            declined = true;
            throw std::runtime_error("injected unavailable Native query view");
        }
        if (where == boundary) {
            reached = true;
            ctx.cancelForContentChange();
        }
    });
    CHECK_THROWS_AS(change(server, doc.m_uri, 2, text, ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(declined == !nativeView);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getVersion() == 2);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    change(server, doc.m_uri, 3, original);
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, original, true);
}

TEST_CASE("Generic macro syntax witnesses batch declarations and retain input admission",
          "[method-body-diagnostics][generated-macro-methods][macro-syntax-witness]") {
    const bool nativeView = GENERATE(false, true);
    CAPTURE(nativeView);
    bool declined = false;
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string original =
        "`define START function void automate();\n"
        "`define FIELD report(`__LINE__);\n"
        "`define FINISH endfunction\n"
        "function automatic int work(input int arg); return arg + 1; endfunction\n"
        "class First #(int N=0);\nfunction void report(input int line_number); endfunction\n"
        "`START\n`FIELD\n`FINISH\nendclass\n"
        "class Second #(int N=0);\nfunction void report(input int line_number); endfunction\n"
        "`START\n`FIELD\n`FINISH\nendclass\n"
        "First#(0) first; Second#(1) second;\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const std::string old = "return arg + 1;";
    uint64_t updates = 0;
    for (const std::string replacement :
         {"return arg +\n  2;", "return arg +\n  missing_user_value;", "return arg +\n\n  3;"}) {
        auto text = original;
        text.replace(text.find(old), old.size(), replacement);
        size_t traversals = 0, identityChecks = 0;
        lsp::RequestContext ctx("textDocument/didChange", std::nullopt);
        ctx.setCheckpointObserverForTesting([&](std::string_view where) {
            if (!nativeView && !declined &&
                where == "before completed body query context indexing") {
                declined = true;
                throw std::runtime_error("injected unavailable Native query view");
            }
            if (where == "generated macro query syntax identity")
                ++identityChecks;
            if (where == "generated macro query syntax identity walk")
                ++traversals;
        });
        change(server, doc.m_uri, int(++updates) + 1, text, ctx);
        server.runBackgroundWork({});
        CHECK(declined == !nativeView);
        CHECK(traversals == (!nativeView && updates == 1 ? 1 : 0));
        if (nativeView)
            CHECK((identityChecks > 0) == (updates == 1));
        REQUIRE(state(server).active);
        CHECK(diagnosticState(server).localDiagnosticUpdates == updates);
        checkNativeDiagnostics(workspace, server, text, true);
    }
    // A structural CST witness must not certify fresh physical dependency bytes.
    workspace.write("prelude.sv", "package prelude; int changed_dependency; endpackage\n");
    auto text = original;
    text.replace(text.find(old), old.size(), "return arg +\n\n  4;");
    change(server, doc.m_uri, 5, text);
    server.runBackgroundWork({});
    CHECK_FALSE(state(server).active);
    checkNativeDiagnostics(workspace, server, text, true);
}

TEST_CASE("Macro evidence retains include ancestors and external macro definitions",
          "[method-body-diagnostics][generated-macro-methods][macro-source-reachability]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("macros.svh", "`define START function void automate();\n"
                                  "`define FIELD report(`__LINE__);\n"
                                  "`define FINISH endfunction\n");
    workspace.write("unrelated.svh", "class Unrelated; function int value(); return `__LINE__; "
                                     "endfunction endclass\n");
    workspace.write("owner.sv", "`include \"macros.svh\"\npackage owner;\n"
                                "`include \"unrelated.svh\"\n`include \"item.svh\"\nendpackage\n");
    const std::string original =
        "function automatic int work(input int arg); return arg + 1; endfunction\n"
        "class Later #(int N=0);\nfunction void report(input int line_number); endfunction\n"
        "`START\n`FIELD\n`FINISH\nendclass\nLater#(0) handle;\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    const std::string old = "return arg + 1;";
    text.replace(text.find(old), old.size(), "return arg +\n  2;");
    size_t excluded = 0;
    lsp::RequestContext ctx("textDocument/didChange", std::nullopt);
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "native macro evidence excludes unrelated declaration")
            ++excluded;
    });
    change(server, doc.m_uri, 2, text, ctx);
    server.runBackgroundWork({});
    CHECK(excluded > 0);
    REQUIRE(state(server).active);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    checkNativeDiagnostics(workspace, server, text, true);
}

TEST_CASE("Queries certify inputs without computing pending local diagnostics",
          "[body-certification][query-background]") {
    const auto kind = GENERATE("hover", "completion", "definition");
    const bool cancel = GENERATE(false, true);
    Workspace workspace;
    auto config = localDiagnosticConfig;
    config.insert(config.size() - 1, ",\"experimentalMethodBodyCertification\":true");
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = edited();
    change(server, doc.m_uri, 2, text);
    doc.m_text = text;
    REQUIRE(server.getDocHover({.textDocument = {.uri = doc.m_uri},
                                .position = doc.getPosition(text.rfind("local_0") + 3)}));
    // Cold diagnostic certification remains off the query path.
    CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
    server.runBackgroundWork({});
    REQUIRE(diagnosticState(server).localDiagnosticUpdates == 1);
    const auto before = diagnosticMap(server);
    text = header;
    text.replace(text.find("local_1 + charl"), 15, "local_0 + absent_local");
    change(server, doc.m_uri, 3, text);
    doc.m_text = text;
    const auto position = doc.getPosition(text.rfind("local_0") + 3);
    auto query = [&](const lsp::RequestContext& ctx) {
        if (std::string_view(kind) == "hover") {
            const auto hover =
                server.getDocHover({.textDocument = {.uri = doc.m_uri}, .position = position}, ctx);
            REQUIRE(hover);
        }
        else if (std::string_view(kind) == "completion")
            (void)server.getDocCompletion(
                {.textDocument = {.uri = doc.m_uri}, .position = position}, ctx);
        else
            (void)server.getDocDefinition(
                {.textDocument = {.uri = doc.m_uri}, .position = position}, ctx);
    };
    int proofs = 0, publications = 0, procedures = 0;
    lsp::RequestContext ctx("query-before-diagnostics", 301);
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "method inputs certified")
            ++proofs;
        if (phase == "before local diagnostic publication")
            ++publications;
        if (phase == "after local diagnostic procedure")
            ++procedures;
    });
    query(ctx);
    CHECK(proofs == 1);
    CHECK(publications == 0);
    CHECK(procedures == 0);
    CHECK(diagnosticMap(server) == before);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
    CHECK_FALSE(server.m_driver->hasCertifiedLocalDiagnostics());
    CHECK(server.m_driver->hasPendingDiagnostics());
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    int diagnosticProofs = 0;
    lsp::RequestContext diagnostic("independent-local-diagnostics", 303);
    diagnostic.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "method inputs certified")
            ++diagnosticProofs;
        if (phase == "before local diagnostic publication") {
            ++publications;
            if (cancel)
                diagnostic.cancel();
        }
    });
    if (cancel) {
        CHECK_THROWS_AS(server.runBackgroundWork(diagnostic), lsp::RequestCancelled);
        CHECK(diagnosticMap(server) == before);
        CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
        server.runBackgroundWork({});
    }
    else
        server.runBackgroundWork(diagnostic);
    CHECK(diagnosticProofs == 1); // The background dispatch proves its inputs afresh.
    CHECK(publications == 1);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 3);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 2);
    int nextProofs = 0;
    lsp::RequestContext next("joint-publication-next", 302);
    next.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "method inputs certified")
            ++nextProofs;
    });
    query(next);
    CHECK(nextProofs == 1); // A later request must perform a new full input proof.
    CHECK(diagnosticState(server).localDiagnosticUpdates == 2);
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Superseded certified transport cannot label old diagnostics with a new version",
          "[body-certification][joint-publication][diagnostic-transport]") {
    const bool indexOnly = GENERATE(false, true);
    Workspace workspace;
    auto config = localDiagnosticConfig;
    config.insert(config.size() - 1, ",\"experimentalMethodBodyCertification\":true");
    workspace.write(".slang/server.json", config);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = edited();
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    REQUIRE(diagnosticState(server).localDiagnosticUpdates == 1);
    text.replace(text.find("local_0 + charl"), 15, "local_0 + absent_local");
    change(server, doc.m_uri, 3, text);
    doc.m_text = text;
    REQUIRE(server.getDocHover({.textDocument = {.uri = doc.m_uri},
                                .position = doc.getPosition(text.rfind("local_0") + 3)}));
    REQUIRE(server.m_methodScope);
    REQUIRE(server.m_methodScope->publishLocalDiagnostics(server.m_config, {}, true));
    REQUIRE(server.m_driver->hasCertifiedLocalDiagnostics());
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
    auto repaired = edited();
    if (indexOnly) {
        const auto revision = server.m_driver->getContextInputRevision();
        const auto index = server.m_indexer.getRevision();
        auto prelude = server.m_driver->getDocument(URI::fromFile(workspace.root / "prelude.sv"));
        REQUIRE(prelude);
        server.m_indexer.updateDocument(prelude->getPath(), *prelude->getSyntaxTree(),
                                        prelude->getBuffer());
        REQUIRE(server.m_indexer.getRevision() != index);
        CHECK(server.m_driver->getContextInputRevision() == revision);
    }
    else
        change(server, doc.m_uri, 4, repaired);
    server.m_driver->flushCertifiedLocalDiagnostics();
    CHECK_FALSE(server.m_driver->hasCertifiedLocalDiagnostics());
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
    CHECK(server.m_driver->hasPendingDiagnostics());
    server.runBackgroundWork({});
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == (indexOnly ? 3 : 4));
    checkNativeDiagnostics(workspace, server, indexOnly ? text : repaired);
}

TEST_CASE("Current generated macro evidence is reused only after complete private binding",
          "[generated-macro-methods][current-macro-witness]") {
    const bool cancelBinding = GENERATE(false, true);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string original =
        "`define START function void automate();\n"
        "`define FIELD report(`__LINE__);\n"
        "`define FINISH endfunction\n"
        "function automatic int work(input int arg); return arg + 1; endfunction\n"
        "class Later; function void report(input int line_number); endfunction\n"
        "`START\n`FIELD\n`FINISH\nendclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const std::string old = "return arg + 1;";
    for (int version = 2; version <= 3; ++version) {
        auto text = original;
        text.replace(text.find(old), old.size(),
                     version == 2 ? "return arg +\n 2;" : "return arg +\n missing_user_value;");
        // Keep the latest text, but defer binding as the protocol does for
        // superseded notifications. Observe the private binding separately.
        lsp::RequestContext edit("textDocument/didChange", version + 100);
        edit.setCheckpointObserverForTesting([&](std::string_view phase) {
            if (phase == "before method edit work")
                edit.cancel();
        });
        CHECK_THROWS_AS(change(server, doc.m_uri, version, text, edit), lsp::RequestCancelled);
        lsp::RequestContext ctx("macro-witness", version);
        size_t replays = 0, reuses = 0;
        bool cancel = cancelBinding && version == 2;
        ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
            if (phase == "macro replay preprocessing")
                ++replays;
            if (phase == "reuse current macro syntax proof")
                ++reuses;
            if (cancel && phase == "before current macro proof witness commit")
                ctx.cancel();
        });
        if (cancel) {
            CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
            CHECK(replays > 0);
            CHECK(reuses == 0);
            CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
            replays = reuses = 0;
            ctx = lsp::RequestContext("macro-witness-retry", 99);
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == "macro replay preprocessing")
                    ++replays;
                if (phase == "reuse current macro syntax proof")
                    ++reuses;
            });
        }
        server.runBackgroundWork(ctx);
        CHECK(replays > 0); // Every new edited syntax must receive a fresh replay.
        CHECK(reuses == 1); // Its diagnostic stage can reuse that exact syntax evidence.
        CHECK(server.client.diagnosticVersions.at(doc.m_uri) == version);
        checkNativeDiagnostics(workspace, server, text);
    }
}

TEST_CASE("Private effect replay follows added and removed initializer calls",
          "[method-body-diagnostics][caller-effect-replay]") {
    const bool module = GENERATE(false, true);
    const bool transitive = GENERATE(false, true);
    const bool initializer = GENERATE(false, true);
    CAPTURE(module, transitive, initializer);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string original =
        "function automatic int work(input int arg); return arg; endfunction\n";
    workspace.write("item.svh", original);
    const std::string helpers =
        "int shared_effect;\n"
        "function automatic int bump(input int arg); shared_effect=arg; return arg; endfunction\n"
        "function automatic int bridge(input int arg); return bump(arg); endfunction\n"
        "function automatic int peer(input int arg); return bridge(arg); endfunction\n";
    const std::string callers = "int first_input, second_input, first_result, second_result;\n"
                                "int boot_result=work(first_input);\n"
                                "always_comb first_result=work(first_input);\n"
                                "always_comb second_result=work(second_input);\n";
    workspace.write("owner.sv", std::string(module ? "module owner;\n" : "package owner;\n") +
                                    "`include \"item.svh\"\n" + helpers +
                                    (module ? callers + "endmodule\n" : "endpackage\n"));
    if (!module)
        workspace.write("tail.sv", "module tail; import owner::*;\n" + callers + "endmodule\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const std::string callee = transitive ? "bridge" : "bump";
    const std::string statement = initializer
                                      ? "int local_value=" + callee + "(arg); return local_value;"
                                      : callee + "(arg); return arg;";
    auto called = original;
    called.replace(called.find("return arg;"), 11, statement);
    uint64_t updates = 0;
    int version = 1;
    for (const auto& text : {called, original, called}) {
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        const auto status = diagnosticState(server);
        INFO(status.localDiagnosticDecline);
        CHECK(status.localDiagnosticUpdates == ++updates);
        checkNativeDiagnostics(workspace, server, text, true);
        bool conflict = false;
        for (const auto& uri : server.client.diagnosticPublications)
            for (const auto& diagnostic : server.client.getDiagnostics(uri))
                conflict |= diagnostic.code &&
                            rfl::json::write(*diagnostic.code) == "\"multiple-always-assigns\"";
        CHECK(conflict == (text != original));
    }
}

TEST_CASE("Self-call changes update the declaration's Native reference diagnostics",
          "[method-body-diagnostics][caller-reference-replay]") {
    const bool classMethod = GENERATE(false, true);
    const bool removal = GENERATE(false, true);
    CAPTURE(classMethod, removal);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    auto source = [&](bool recursive) {
        return std::string(classMethod ? "class Recursive;\n" : "") +
               "function automatic int work(input int arg);\n" +
               (recursive ? "if (arg > 0) return work(arg-1); return 0;\n" : "return 0;\n") +
               "endfunction\n" + (classMethod ? "endclass\n" : "");
    };
    const auto original = source(removal);
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const auto changed = source(!removal);
    change(server, doc.m_uri, 2, changed);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    checkNativeDiagnostics(workspace, server, changed, true);
}

TEST_CASE("Private reference replay updates sole uses without losing signature uses",
          "[method-body-diagnostics][sole-reference-replay]") {
    const bool classMethod = GENERATE(false, true);
    const auto kind = GENERATE("read", "write", "call", "initializer", "wildcard");
    CAPTURE(classMethod, kind);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv",
                    "package prelude; typedef int param_t;\n"
                    "int header_value, read_value, write_value;\n"
                    "function automatic int helper(input int arg); return arg; endfunction\n"
                    "endpackage\n");
    auto source = [&](std::string_view body) {
        return std::string(std::string_view(kind) == "wildcard" ? "import prelude::*;\n" : "") +
               (classMethod ? "class Client;\n" : "") +
               "function automatic int work(input prelude::param_t arg=prelude::header_value);\n" +
               std::string(body) + "\nendfunction\n" + (classMethod ? "endclass\n" : "");
    };
    const auto original = source("return arg;");
    const auto body = std::string_view(kind) == "read"    ? "return arg+prelude::read_value;"
                      : std::string_view(kind) == "write" ? "prelude::write_value=arg; return arg;"
                      : std::string_view(kind) == "call"  ? "return prelude::helper(arg);"
                      : std::string_view(kind) == "initializer"
                          ? "int local_value=prelude::helper(arg); return local_value;"
                          : "return arg+read_value;";
    const auto edited = source(body);
    const auto invalid = source("return arg+missing_user_value;");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    uint64_t updates = 0;
    int version = 1;
    for (const auto& text : {edited, original, invalid, edited}) {
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        const auto status = diagnosticState(server);
        INFO(status.localDiagnosticDecline);
        CHECK(status.localDiagnosticUpdates == ++updates);
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Stable task calls keep local diagnostics without whole-design dataflow replay",
          "[method-body-diagnostics][stable-task-effects]") {
    const bool package = GENERATE(false, true);
    Workspace workspace;
    const std::string original =
        package ? "task automatic helper(input int arg); #1; $display(arg); endtask\n"
                  "task automatic work(input int arg);\n int local_value = arg;\n"
                  " helper(arg); local_value += 1;\nendtask\n"
                : "class Base; task helper(input int arg); #1; $display(arg); endtask endclass\n"
                  "class C extends Base; task work(input int arg);\n int local_value = arg;\n"
                  " super.helper(arg); local_value += 1;\nendtask\n"
                  "task wrapper(input int arg); work(arg); endtask endclass\n";
    workspace.write("item.svh", original);
    workspace.write(
        "tail.sv", package
                       ? "module tail; import owner::*; bit arg; always_comb work(arg); endmodule\n"
                       : "module tail; import owner::*; bit arg; C c; always_comb c.wrapper(arg); "
                         "endmodule\n");
    auto config = localDiagnosticConfig;
    config.insert(config.size() - 1, ",\"experimentalMethodBodyCertification\":true");
    workspace.write(".slang/server.json", config);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto* statement : {"local_value += 2;", "local_value +=\n 3;",
                                  "local_value += missing_value;", "local_value += 1;"}) {
        auto text = original;
        text.replace(text.find("local_value += 1;"), 17, statement);
        unsigned fullFlow = 0;
        lsp::RequestContext ctx("background", 177);
        ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
            if (phase == "before local effect replay")
                ++fullFlow;
        });
        change(server, doc.m_uri, ++version, text, ctx);
        server.runBackgroundWork(ctx);
        INFO(package << " " << statement);
        CHECK(fullFlow == 0);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        CHECK(server.client.diagnosticVersions.at(doc.m_uri) == version);
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Large local and effect replay cancellation preserves published diagnostics",
          "[method-body-diagnostics][procedure-cancellation]") {
    const bool effect = GENERATE(false, true);
    Workspace workspace;
    workspace.write("prelude.sv", "package prelude; int value, extra; endpackage\n");
    auto original = std::string("class C; function void work(input int arg);\n"
                                "int local_value = arg;\n");
    for (unsigned i = 0; i < 2000; ++i)
        original += "local_value += arg;\n";
    if (effect)
        original += "prelude::value = local_value;\n";
    original += "endfunction endclass\n";
    workspace.write("item.svh", original);
    workspace.write(".slang/server.json", localDiagnosticConfig);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const auto before = diagnosticMap(server);
    auto text = original;
    text.replace(text.find("local_value = arg;"), 18, "local_value = missing_arg;");
    if (effect)
        text.insert(text.find("endfunction"), "prelude::extra = local_value;\n");
    change(server, doc.m_uri, 2, text);
    lsp::RequestContext ctx("background", 178);
    unsigned polls = 0;
    const auto boundary = effect ? "during local effect procedure"
                                 : "during local diagnostic procedure";
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == boundary && ++polls == 3)
            ctx.cancel();
    });
    CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
    CHECK(polls == 3);
    CHECK(diagnosticMap(server) == before);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
    CHECK(doc.doc->getVersion() == 2);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
    checkNativeDiagnostics(workspace, server, text, true);
}

TEST_CASE("Caller effect diagnostics reuse unchanged peers with intrinsic warnings",
          "[method-body-diagnostics][procedure-summary-reuse]") {
    const bool package = GENERATE(false, true);
    CAPTURE(package);
    Workspace workspace;
    const std::string original =
        std::string(package ? "" : "class C;\n") +
        " function automatic int peer(input int arg); if (arg) return 1; endfunction\n"
        " function automatic void work(input int arg);\n"
        "  int local_value = arg; prelude::value = local_value + 1;\n"
        " endfunction\n" +
        (package ? "" : "endclass\n");
    workspace.write("item.svh", original);
    workspace.write(".slang/server.json", certificationConfig());
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto* statement :
         {"prelude::value = local_value + 2;", "prelude::value = local_value + missing_value;",
          "prelude::value = local_value +\n 3;", "prelude::value = local_value + 1;"}) {
        auto text = original;
        const auto expression = std::string("prelude::value = local_value + 1;");
        text.replace(text.find(expression), expression.size(), statement);
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        struct Counts {
            uint64_t localDiagnosticProceduresReused, localDiagnosticProceduresRecomputed;
        };
        auto counts =
            rfl::from_generic<Counts>(
                *server.executeCommand({.command = "slang.methodScopeStatus",
                                        .arguments = std::vector<lsp::LSPAny>{rfl::to_generic(
                                            std::string("compact"))}}))
                .value();
        if (package) {
            CHECK(counts.localDiagnosticProceduresReused >= 1);
            CHECK(counts.localDiagnosticProceduresRecomputed >= 1);
        }
        else {
            CHECK(counts.localDiagnosticProceduresReused == 0);
            CHECK(counts.localDiagnosticProceduresRecomputed == 0);
        }
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Large diagnostic ordering cancels without changing the published map",
          "[method-body-diagnostics][diagnostic-order-cancellation]") {
    Workspace workspace;
    std::string original = "class C;\n";
    for (unsigned i = 0; i < 500; ++i)
        original += "function int peer" + std::to_string(i) +
                    "(input int arg); if(arg) return 1; endfunction\n";
    original += "function void work(input int arg);\n"
                "int local_value=arg; prelude::value=local_value;\n"
                "endfunction endclass\n";
    workspace.write("item.svh", original);
    workspace.write(".slang/server.json", localDiagnosticConfig);
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const auto before = diagnosticMap(server);
    auto text = original;
    text.replace(text.find("local_value=arg;"), 16, "local_value=missing_arg;");
    change(server, doc.m_uri, 2, text);
    lsp::RequestContext ctx("background", 179);
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "during local diagnostic ordering") {
            reached = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(diagnosticMap(server) == before);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
    checkNativeDiagnostics(workspace, server, text, true);
}

TEST_CASE("Formatted native siblings are reused while target-related notes track edits",
          "[method-body-diagnostics][formatted-diagnostic-reuse]") {
    Workspace workspace;
    workspace.write(
        "prelude.sv",
        "package prelude; int value;\n"
        "class Stable; function int peer(input int arg); if(arg) return 1; endfunction endclass\n"
        "endpackage\n");
    workspace.write("owner.sv", "package owner; `include \"item.svh\"\nendpackage\n");
    workspace.write("tail.sv", "module tail; import owner::*; int a,b,x,y;\n"
                               "always_comb x=work(a); always_comb y=work(b); endmodule\n");
    const std::string original = "function automatic int work(input int arg);\n"
                                 "static int saved; int local_value=arg;\n"
                                 "saved=local_value + 1; return saved;\nendfunction\n";
    workspace.write("item.svh", original);
    workspace.write(".slang/server.json", certificationConfig());
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    bool sawRelatedTarget = false;
    for (const auto& diagnostic :
         server.client.getDiagnostics(URI::fromFile(workspace.root / "tail.sv")))
        if (diagnostic.relatedInformation)
            for (const auto& note : *diagnostic.relatedInformation)
                sawRelatedTarget |= note.location.uri == doc.m_uri;
    REQUIRE(sawRelatedTarget);
    int version = 1;
    for (const auto* expression : {"local_value + 2", "local_value + missing_value",
                                   "local_value +\n 3", "local_value + 1"}) {
        auto text = original;
        text.replace(text.find("local_value + 1"), 15, expression);
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        struct Counts {
            uint64_t localDiagnosticRenderingsReused, localDiagnosticRenderingsFresh;
        };
        auto counts =
            rfl::from_generic<Counts>(
                *server.executeCommand({.command = "slang.methodScopeStatus",
                                        .arguments = std::vector<lsp::LSPAny>{rfl::to_generic(
                                            std::string("compact"))}}))
                .value();
        CHECK(counts.localDiagnosticRenderingsReused > 0);
        CHECK(counts.localDiagnosticRenderingsFresh > 0);
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Unchanged direct class effects keep local diagnostics off global replay",
          "[method-body-diagnostics][stable-class-effects]") {
    const auto effect = GENERATE("field", "package", "static-call");
    CAPTURE(effect);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv",
                    "package prelude; int value;\n"
                    "function automatic int bump(input int arg); value=arg; return arg; "
                    "endfunction\nendpackage\n");
    const std::string statement = std::string_view(effect) == "field"     ? "field=arg;\n"
                                  : std::string_view(effect) == "package" ? "prelude::value=arg;\n"
                                                                          : "prelude::bump(arg);\n";
    const std::string original = "class C; int field;\n"
                                 "function int peer(); return field; endfunction\n"
                                 "function int work(input int arg);\n" +
                                 statement + "return arg + 1;\nendfunction\nendclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto* expression : {"arg + 2", "arg + missing_value", "arg +\n 3", "arg + 1"}) {
        auto text = original;
        text.replace(text.find("arg + 1"), 7, expression);
        change(server, doc.m_uri, ++version, text);
        unsigned replays = 0, indexes = 0;
        lsp::RequestContext ctx("stable class effects", version);
        ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
            if (phase == "before local effect replay")
                ++replays;
            if (phase == "before immutable diagnostic rendering index")
                ++indexes;
        });
        server.runBackgroundWork(ctx);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        if (std::string_view(expression).find("missing_value") == std::string_view::npos) {
            CHECK(replays == 0);
            CHECK(indexes == 0);
        }
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Changed class driver bounds and existing notes retain complete diagnostics",
          "[method-body-diagnostics][stable-class-effects]") {
    const auto kind = GENERATE("new-write", "new-bound", "existing-conflict");
    CAPTURE(kind);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("tail.sv", "module tail; assign prelude::value[0]=1'b0; endmodule\n");
    const std::string write = std::string_view(kind) == "new-write"   ? ""
                              : std::string_view(kind) == "new-bound" ? "prelude::value[1]=arg;\n"
                                                                      : "prelude::value[0]=arg;\n";
    const std::string original = "class C; function int work(input int arg);\n" + write +
                                 "return arg + 1;\nendfunction endclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    if (std::string_view(kind) == "new-write")
        text.insert(text.find("return arg"), "prelude::value[0]=arg;\n");
    else if (std::string_view(kind) == "new-bound")
        text.replace(text.find("value[1]"), 8, "value[0]");
    else
        text.insert(text.find("prelude::value"), "\n\n");
    change(server, doc.m_uri, 2, text);
    unsigned replays = 0, proofs = 0;
    lsp::RequestContext ctx("changed class effects", 201);
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        replays += phase == "before local effect replay";
        proofs += phase == "class external driver effects unchanged";
    });
    server.runBackgroundWork(ctx);
    if (std::string_view(kind) == "existing-conflict")
        CHECK(proofs == 0);
    else {
        CHECK(replays > 0);
        CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    }
    checkNativeDiagnostics(workspace, server, text, true);
    change(server, doc.m_uri, 3, original);
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, original, true);
}

TEST_CASE("Deferred rendering indexes cancel promotion and retry without partial state",
          "[method-body-diagnostics][lazy-diagnostic-index]") {
    const auto boundary = GENERATE("prepare immutable diagnostic rendering",
                                   "commit immutable diagnostic rendering index");
    CAPTURE(boundary);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv", "package prelude; function automatic int helper(input int arg); "
                                  "return arg; endfunction endpackage\n");
    const std::string original = "class C; function int work(input int arg);\n"
                                 "return arg + 1;\nendfunction endclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto local = original;
    local.replace(local.find("arg + 1"), 7, "arg + 2");
    change(server, doc.m_uri, 2, local);
    server.runBackgroundWork({});
    REQUIRE(diagnosticState(server).localDiagnosticUpdates == 1);
    const auto before = diagnosticMap(server);
    auto called = original;
    called.replace(called.find("arg + 1"), 7, "prelude::helper(arg)");
    change(server, doc.m_uri, 3, called);
    bool reached = false;
    lsp::RequestContext ctx("lazy rendering promotion", 202);
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == boundary) {
            reached = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(diagnosticMap(server) == before);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    unsigned retries = 0;
    lsp::RequestContext retry("lazy rendering retry", 203);
    retry.setCheckpointObserverForTesting([&](std::string_view phase) {
        retries += phase == "before immutable diagnostic rendering index";
    });
    server.runBackgroundWork(retry);
    CHECK(retries == 1);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 2);
    checkNativeDiagnostics(workspace, server, called, true);
    change(server, doc.m_uri, 4, local);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 3);
    checkNativeDiagnostics(workspace, server, local, true);
}

TEST_CASE("Every generic class function context participates in local diagnostics",
          "[method-body-diagnostics][multiple-body-contexts]") {
    const bool differentTypes = GENERATE(false, true);
    const bool externalConsumers = GENERATE(false, true);
    CAPTURE(differentTypes, externalConsumers);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string consumers =
        "C#(int,0) first; C#(" + std::string(differentTypes ? "byte" : "int") +
        ",1) second;\n"
        "function automatic void invoke(); void'(first.work(1)); void'(second.work(2)); "
        "endfunction\n";
    const std::string original = "class C #(type T=int, int ID=0);\n"
                                 "function int work(input T arg);\n"
                                 "int local_value=arg; return local_value + 1;\n"
                                 "endfunction endclass\n" +
                                 (externalConsumers ? std::string{} : consumers);
    workspace.write("item.svh", original);
    if (externalConsumers)
        workspace.write("tail.sv", "module tail; import owner::*;\n" + consumers + "endmodule\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto* expression : {"local_value + 2", "local_value + missing_value",
                                   "local_value +\n 3", "local_value + 1"}) {
        auto text = original;
        text.replace(text.find("local_value + 1"), 15, expression);
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Module function contexts coalesce all and instance-specific warnings",
          "[method-body-diagnostics][multiple-body-contexts]") {
    const bool differentWidths = GENERATE(false, true);
    CAPTURE(differentWidths);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("owner.sv", "module owner #(parameter int WIDTH=8);\n"
                                "`include \"item.svh\"\nendmodule\n");
    workspace.write("tail.sv", "module tail; owner#(8) first(); owner#(" +
                                   std::string(differentWidths ? "16" : "8") +
                                   ") second(); owner#(" +
                                   std::string(differentWidths ? "9" : "8") +
                                   ") third(); int a,b,c;\n"
                                   "initial begin a=first.work(1); b=second.work(2); "
                                   "c=third.work(3); end endmodule\n");
    const std::string original = "function automatic int work(input logic[WIDTH-1:0] arg);\n"
                                 "int never_used; int local_value=arg;\n"
                                 "if (WIDTH<=9) begin if(arg) return local_value + 1; end\n"
                                 "else return local_value + 1;\nendfunction\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto* expression : {"local_value + 2", "local_value + missing_value",
                                   "local_value +\n 3", "local_value + 1"}) {
        auto text = original;
        text.replace(text.find("local_value + 1"), 15, expression);
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Module task contexts replace transitive timing summaries together",
          "[method-body-diagnostics][multiple-body-contexts][task-timing-closure]") {
    const bool cancelFirst = GENERATE(false, true);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("owner.sv", "module owner #(parameter int WIDTH=8);\n"
                                "`include \"item.svh\"\nendmodule\n");
    workspace.write("tail.sv", "module tail; owner#(8) first(); owner#(16) second(); "
                               "owner#(9) third(); bit arg;\n"
                               "always_comb first.wrapper(arg);\n"
                               "always_comb second.wrapper(arg);\n"
                               "always_comb third.wrapper(arg); endmodule\n");
    const std::string original = "task automatic work(input int arg);\n"
                                 "int local_value=arg; $display(local_value);\n"
                                 "endtask\n"
                                 "task automatic wrapper(input int arg); work(arg); endtask\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto* statement :
         {"if(WIDTH<=9) #1; $display(local_value);", "#2; $display(local_value);",
          "if(WIDTH==16) #3;\n $display(local_value);", "$display(local_value);"}) {
        auto text = original;
        text.replace(text.find("$display(local_value);"), 22, statement);
        change(server, doc.m_uri, ++version, text);
        if (cancelFirst && version == 2) {
            const auto before = diagnosticMap(server);
            lsp::RequestContext ctx("multiple-task-callers", 201);
            bool reached = false;
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == "task diagnostic caller update") {
                    reached = true;
                    ctx.cancel();
                }
            });
            CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
            REQUIRE(reached);
            CHECK(diagnosticMap(server) == before);
            CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
        }
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Local virtual interface declarations finalize in the private body context",
          "[method-body-diagnostics][body-virtual-interface]") {
    const bool interfacePort = GENERATE(false, true);
    const unsigned cancelAt = GENERATE(0u, 1u, 2u);
    const bool genericBody = GENERATE(false, true);
    CAPTURE(interfacePort, cancelAt, genericBody);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("tail.sv", "interface endpoint; endinterface\n"
                               "interface channel #(parameter int WIDTH=8)" +
                                   std::string(interfacePort ? "(endpoint ep)" : "") +
                                   "; logic [WIDTH-1:0] data; modport sink(input data); " +
                                   (genericBody ? std::string("if (WIDTH==16) begin:g "
                                                              "class Hidden #(int P=0); "
                                                              "function int bad(); "
                                                              "return missing_in_iface; "
                                                              "endfunction endclass end ")
                                                : std::string{}) +
                                   "endinterface\n"
                                   "module tail; import owner::*; C c; "
                                   "initial void'(c.work(1)); endmodule\n");
    const std::string original = "class Resource #(type T=int); function new(); "
                                 "byte signed_value=-1; bit[7:0] unsigned_value=signed_value; "
                                 "endfunction endclass\n"
                                 "class C; function int work(input int arg);\n"
                                 "Resource#(int) unrelated;\n"
                                 "int result=arg; result += 1; return result;\n"
                                 "endfunction endclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    CHECK(std::ranges::any_of(server.client.getDiagnostics(doc.m_uri), [](const auto& d) {
        return d.message.find("implicit conversion changes signedness") != std::string::npos;
    }));
    int version = 1;
    const std::string old = "int result=arg; result += 1; return result;";
    for (const auto* statement :
         {"virtual channel#(8) local_vif; int result=arg; result += 2; return result;",
          "virtual channel#(16) local_vif; int result=local_vif.missing; return result;",
          "virtual channel#(16).sink local_vif;\n int result=arg; return result;",
          "virtual channel#(8).missing local_vif; int result=arg; return result;",
          "int result=arg; result += 1; return result;"}) {
        auto text = original;
        text.replace(text.find(old), old.size(), statement);
        change(server, doc.m_uri, ++version, text);
        if (cancelAt && version == 2) {
            const auto before = diagnosticMap(server);
            lsp::RequestContext ctx("private-virtual-interface", 202);
            bool reached = false;
            unsigned callbacks = 0;
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == "during local virtual interface finalization" &&
                    ++callbacks == cancelAt) {
                    reached = true;
                    ctx.cancel();
                }
            });
            CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
            REQUIRE(reached);
            CHECK(diagnosticMap(server) == before);
            CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
        }
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        if (interfacePort && version == 2)
            CHECK(std::ranges::any_of(server.client.getDiagnostics(doc.m_uri), [](const auto& d) {
                return d.message.find("ports that reference other interfaces") != std::string::npos;
            }));
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Generic class task contexts update every procedural caller",
          "[method-body-diagnostics][multiple-body-contexts][task-timing-closure]") {
    const bool externalConsumers = GENERATE(false, true);
    CAPTURE(externalConsumers);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string consumers = "C#(int,0) first; C#(byte,1) second;\n";
    const std::string original =
        "class C #(type T=int, int ID=0);\n"
        "task work(input T arg); int local_value=arg; $display(local_value); endtask\n"
        "task wrapper(input T arg); work(arg); endtask endclass\n" +
        (externalConsumers ? std::string{} : consumers);
    workspace.write("item.svh", original);
    workspace.write("tail.sv", "module tail; import owner::*; bit arg;\n" +
                                   (externalConsumers ? consumers : std::string{}) +
                                   "always_comb first.wrapper(arg); "
                                   "always_comb second.wrapper(arg); endmodule\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    const std::string old = "$display(local_value);";
    for (const auto* statement :
         {"if(ID==0) #1; $display(local_value);", "#2; $display(local_value);",
          "if(ID==1) #3;\n $display(local_value);", "$display(local_value);"}) {
        auto text = original;
        text.replace(text.find(old), old.size(), statement);
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Cancelling generic query context preparation preserves current input",
          "[body-certification][multiple-body-contexts][diagnostic-cancellation]") {
    const bool duringIndex = GENERATE(false, true);
    const bool externalConsumers = GENERATE(false, true);
    CAPTURE(duringIndex, externalConsumers);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string consumers = "C#(int,0) first; C#(byte,1) second;\n"
                                  "function automatic void invoke(); void'(first.work(1)); "
                                  "void'(second.work(2)); endfunction\n";
    const std::string original =
        "class C #(type T=int, int ID=0);\n"
        "function int work(input T arg); return arg + 1; endfunction endclass\n" +
        (externalConsumers ? std::string{} : consumers);
    workspace.write("item.svh", original);
    if (externalConsumers)
        workspace.write("tail.sv", "module tail; import owner::*;\n" + consumers + "endmodule\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const auto before = diagnosticMap(server);
    auto text = original;
    text.replace(text.find("arg + 1"), 7, "arg + 2");
    lsp::RequestContext ctx("generic-context-preparation", 203);
    bool reached = false;
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        const bool selectedBoundary =
            externalConsumers
                ? phase == (duringIndex ? "after completed generic query context indexing"
                                        : "before completed generic query context indexing")
                : (!duringIndex && phase == "bind generic query context declarations") ||
                      (duringIndex && (phase == "index generic method contexts" ||
                                       phase == "index imported generic method contexts"));
        if (selectedBoundary) {
            reached = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(change(server, doc.m_uri, 2, text, ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(diagnosticMap(server) == before);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    CHECK(server.m_driver->getDocument(doc.m_uri)->getVersion() == 2);
    // Cancellation happened before the certificate existed. Background work
    // may establish the latest Native baseline; the next edit must still use
    // the local path rather than retaining a cancelled preparation forever.
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, text, true);
    // Edit admission deliberately requires an already queried current view.
    // Diagnostics alone do not populate the per-file query cache.
    server.m_driver->getDocument(doc.m_uri)->getAnalysis();
    text.replace(text.find("arg + 2"), 7, "arg + missing_generic_value");
    change(server, doc.m_uri, 3, text);
    server.runBackgroundWork({});
    INFO(state(server).decline);
    INFO(diagnosticState(server).localDiagnosticDecline);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    checkNativeDiagnostics(workspace, server, text, true);
}

TEST_CASE("Method-local nominal types keep complete private diagnostics",
          "[method-body-diagnostics][body-local-types]") {
    const auto context = GENERATE("class", "package", "module");
    const auto kind = GENERATE("alias", "enum", "struct", "union", "unpacked-struct",
                               "unpacked-union", "unused");
    const bool task = GENERATE(false, true);
    const bool initialType = GENERATE(false, true);
    const bool cancelFirst = GENERATE(false, true);
    CAPTURE(context, kind, task, initialType, cancelFirst);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const bool cls = std::string_view(context) == "class";
    const bool module = std::string_view(context) == "module";
    auto taskify = [task](std::string text) {
        if (task) {
            size_t offset = 0;
            while ((offset = text.find("return ", offset)) != std::string::npos) {
                auto semi = text.find(';', offset);
                auto replacement = "$display(" + text.substr(offset + 7, semi - offset - 7) +
                                   "); return;";
                text.replace(offset, semi - offset + 1, replacement);
                offset += replacement.size();
            }
        }
        return text;
    };
    if (module)
        workspace.write("owner.sv", "module owner;\n`include \"item.svh\"\nendmodule\n");
    std::string call = cls ? "c.work(1)" : module ? "dut.work(1)" : "owner::work(1)";
    workspace.write("tail.sv", std::string(cls      ? "module tail; import owner::*; C c; "
                                           : module ? "module tail; owner dut(); "
                                                    : "module tail; ") +
                                   "initial " + (task ? call : "void'(" + call + ")") +
                                   "; endmodule\n");
    const std::string type = kind;
    std::string valid = type == "alias"
                            ? "typedef logic[7:0] local_t; local_t result=arg; return int'(result);"
                        : type == "enum"   ? "typedef enum int {READY=0,DONE=1} local_t; "
                                             "local_t result=READY; return int'(result);"
                        : type == "struct" ? "typedef struct packed {int data;} local_t; "
                                             "local_t result; result.data=arg; return result.data;"
                        : type == "union"
                            ? "typedef union packed {int data; bit[31:0] bits;} local_t; "
                              "local_t result; result.data=arg; return result.data;"
                        : type == "unpacked-struct" ? "typedef struct {int data;} local_t; local_t "
                                                      "result; result.data=arg; return result.data;"
                        : type == "unpacked-union"
                            ? "typedef union {int data; bit[31:0] bits;} local_t; local_t result; "
                              "result.data=arg; return result.data;"
                            : "typedef int local_t; return arg;";
    std::string invalid =
        type == "alias"  ? "typedef missing_local_type local_t; local_t result=arg; return result;"
        : type == "enum" ? "typedef enum int {READY=0,DONE=0} local_t; "
                           "local_t result=READY; return int'(result);"
        : type == "struct" ? "typedef struct packed {int data;} local_t; "
                             "local_t result; return result.missing;"
        : type == "union"  ? "typedef union packed {int data; bit[7:0] bits;} local_t; "
                             "local_t result; return result.data;"
        : type == "unpacked-struct"
            ? "typedef struct {int data;} local_t; local_t result; return result.missing;"
        : type == "unpacked-union"
            ? "typedef union {int data; int data;} local_t; local_t result; return result.data;"
            : "typedef struct packed {missing_local_type field;} local_t; return arg;";
    valid = taskify(std::move(valid));
    invalid = taskify(std::move(invalid));
    const std::string old = taskify("int result=arg; return result;");
    std::string original = (cls ? std::string("class C; ") : std::string{}) +
                           (task ? std::string(cls ? "task " : "task automatic ")
                                 : std::string(cls ? "function int " : "function automatic int ")) +
                           "work(input int arg);\n" + old + "\n" +
                           (task ? "endtask\n" : "endfunction\n") + (cls ? "endclass\n" : "");
    if (initialType)
        original.replace(original.find(old), old.size(), valid);
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto& statement : {valid + " ", invalid, valid + "\n", taskify("return arg;")}) {
        auto text = original;
        const auto& previous = initialType ? valid : old;
        text.replace(text.find(previous), previous.size(), statement);
        change(server, doc.m_uri, ++version, text);
        if (cancelFirst && version == 2) {
            auto before = diagnosticMap(server);
            lsp::RequestContext ctx("local-type-analysis", 204);
            bool reached = false;
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == "after local diagnostic procedure") {
                    reached = true;
                    ctx.cancel();
                }
            });
            CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
            REQUIRE(reached);
            CHECK(diagnosticMap(server) == before);
            CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
        }
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        auto hoverOffset = text.find("local_t");
        if (hoverOffset == std::string::npos)
            hoverOffset = text.rfind("arg");
        // change() sends directly to the server; DocumentHandle still stores
        // the initial text and cannot compute coordinates for a longer body.
        const auto position = server::toPosition(
            slang::SourceLocation(server.m_driver->getDocument(doc.m_uri)->getBuffer(),
                                  hoverOffset),
            server.m_driver->sm);
        checkNativeDiagnostics(workspace, server, text, true, position);
    }
}

TEST_CASE("Local imports and new package references preserve private semantic closure",
          "[method-body-diagnostics][body-local-imports]") {
    const auto context = GENERATE("class", "package", "module");
    const auto style = GENERATE("explicit", "wildcard", "qualified");
    const bool task = GENERATE(false, true);
    const bool initialImport = GENERATE(false, true);
    const bool cancelFirst = GENERATE(false, true);
    CAPTURE(context, style, task, initialImport, cancelFirst);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv",
                    "package prelude; typedef struct packed {int value;} word_t; "
                    "function automatic int helper(int arg); return arg+1; endfunction "
                    "endpackage\n"
                    "package alternate; typedef struct packed {byte value;} word_t; "
                    "function automatic int helper(int arg); return arg-1; endfunction "
                    "endpackage\n");
    auto statement = [&](std::string_view package) {
        std::string prefix;
        if (std::string_view(style) == "explicit")
            prefix = "import " + std::string(package) + "::word_t; import " + std::string(package) +
                     "::helper; ";
        else if (std::string_view(style) == "wildcard")
            prefix = "import " + std::string(package) + "::*; ";
        const std::string qualifier = std::string_view(style) == "qualified"
                                          ? std::string(package) + "::"
                                          : "";
        return prefix + qualifier + "word_t local_word; local_word.value=arg; " +
               (task ? "$display(" : "return ") + qualifier + "helper(local_word.value)" +
               (task ? "); return;" : ";");
    };
    const auto valid = statement("prelude");
    const auto alternate = statement("alternate");
    const auto invalid = statement("missing_package");
    const std::string removed = task ? "$display(arg); return;" : "return arg;";
    const auto old = initialImport ? valid : removed;
    const bool cls = std::string_view(context) == "class";
    const bool module = std::string_view(context) == "module";
    const std::string original = (cls ? std::string("class C; ") : std::string{}) +
                                 (task ? std::string(cls ? "task " : "task automatic ")
                                       : std::string(cls ? "function int "
                                                         : "function automatic int ")) +
                                 "work(input int arg);\n" + old + "\n" +
                                 (task ? "endtask\n" : "endfunction\n") + (cls ? "endclass\n" : "");
    workspace.write("item.svh", original);
    if (module)
        workspace.write("owner.sv", "module owner;\n`include \"item.svh\"\nendmodule\n");
    const std::string call = cls ? "c.work(1)" : module ? "dut.work(1)" : "owner::work(1)";
    workspace.write("tail.sv", std::string(cls      ? "module tail; import owner::*; C c; "
                                           : module ? "module tail; owner dut(); "
                                                    : "module tail; ") +
                                   "initial " + (task ? call : "void'(" + call + ")") +
                                   "; endmodule\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    auto checkOrigin = [&](std::string_view phase) {
        if (!initialImport)
            return;
        INFO(phase);
        auto design = server.m_driver->comp->createActiveDesignContext({});
        auto owner = design.retainCompilation();
        std::vector<const ast::SubroutineSymbol*> bodies;
        for (auto fn : design.getAnalysis().getAnalysisManager().getAnalyzedSubroutines())
            if (fn->name == "work")
                bodies.push_back(fn);
        REQUIRE(!bodies.empty());
        auto outside = owner->getReferenceUsageExcludingBodies(bodies);
        REQUIRE(outside);
        const auto* syntax = owner->getPackage("prelude")->find("word_t")->getSyntax();
        auto found = outside->find(syntax);
        CHECK((found == outside->end() ? std::pair{false, false} : found->second) ==
              std::pair{false, false});
    };
    checkOrigin("before query indexing");
    doc.doc->getAnalysis();
    checkOrigin("after query indexing");
    int version = 1;
    for (const auto& current : {valid + " ", alternate, invalid, valid + "\n", removed, valid}) {
        auto text = original;
        text.replace(text.find(old), old.size(), current);
        change(server, doc.m_uri, ++version, text);
        if (cancelFirst && version == 2) {
            const auto before = diagnosticMap(server);
            lsp::RequestContext ctx("local-import-analysis", 206);
            bool reached = false;
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == "after local diagnostic procedure") {
                    reached = true;
                    ctx.cancel();
                }
            });
            CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
            REQUIRE(reached);
            CHECK(diagnosticMap(server) == before);
            CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
        }
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        auto offset = text.find("local_word");
        if (offset == std::string::npos)
            offset = text.rfind("arg");
        const auto position = server::toPosition(
            slang::SourceLocation(server.m_driver->getDocument(doc.m_uri)->getBuffer(), offset),
            server.m_driver->sm);
        checkNativeDiagnostics(workspace, server, text, true, position);
    }
}

TEST_CASE("New package references preserve the selected module parameter context",
          "[method-body-diagnostics][body-local-imports][multiple-body-contexts]") {
    const auto instance = GENERATE("tail.first", "tail.second");
    CAPTURE(instance);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv", "package prelude; typedef int word_t; endpackage\n");
    workspace.write("tail.sv", "module tail; owner#(8) first(); owner#(16) second(); "
                               "initial begin void'(first.work(1)); void'(second.work(2)); "
                               "end endmodule\n");
    const std::string old = "logic[WIDTH-1:0] local_word=arg; return local_word;";
    const std::string original =
        "module owner #(parameter int WIDTH=8);\nfunction automatic logic[WIDTH-1:0] work("
        "input logic[WIDTH-1:0] arg);\n" +
        old + "\nendfunction\nendmodule\n";
    workspace.write("owner.sv", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    REQUIRE(server.setActiveInstance(instance));
    auto doc = server.openFile("owner.sv", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto* statement :
         {"import prelude::word_t; word_t imported=arg; "
          "logic[WIDTH-1:0] local_word=arg; return local_word+imported;",
          "import prelude::missing_type; logic[WIDTH-1:0] local_word=arg; return local_word;",
          "import prelude::*;\n word_t imported=arg; "
          "logic[WIDTH-1:0] local_word=arg; return local_word+imported;",
          "logic[WIDTH-1:0] local_word=arg; return local_word;"}) {
        auto text = original;
        text.replace(text.find(old), old.size(), statement);
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        const auto position = server::toPosition(
            slang::SourceLocation(server.m_driver->getDocument(doc.m_uri)->getBuffer(),
                                  text.rfind("local_word")),
            server.m_driver->sm);
        auto hover = server.getDocHover({.textDocument = {.uri = doc.m_uri}, .position = position});
        REQUIRE(hover);
        CHECK(rfl::json::write(hover).find(std::string_view(instance) == "tail.first"
                                               ? "Width: `8`"
                                               : "Width: `16`") != std::string::npos);
        checkNativeDiagnostics(workspace, server, text, true, position, instance, "owner.sv");
    }
}

TEST_CASE("Changed generic instances update their complete diagnostic closure",
          "[method-body-diagnostics][body-generic-closure]") {
    const bool cls = GENERATE(false, true);
    const auto initial = GENERATE("none", "int", "byte");
    const bool independentConsumer = GENERATE(false, true);
    CAPTURE(cls, initial, independentConsumer);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv",
                    std::string("package prelude; class Box #(type T=int); T value; "
                                "function T read(input int arg); return arg; endfunction "
                                "endclass\n") +
                        (independentConsumer ? "Box#(byte) independent_box;\n" : "") +
                        "endpackage\n");
    const auto body = [](std::string_view parameter) {
        return parameter == "none"
                   ? std::string("return arg;")
                   : "prelude::Box#(" + std::string(parameter) + ") local_box; return arg;";
    };
    const auto old = body(initial);
    const std::string original = std::string(
                                     cls ? "class C; function int work(input int arg);\n"
                                         : "function automatic int work(input int arg);\n") +
                                 old + "\nendfunction\n" + (cls ? "endclass\n" : "");
    workspace.write("item.svh", original);
    workspace.write("tail.sv", cls ? "module tail; import owner::*; C c; "
                                     "initial void'(c.work(1)); endmodule\n"
                                   : "module tail; initial void'(owner::work(1)); endmodule\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto& replacement : {body("int") + " ", body("byte"), body("byte") + "\n",
                                    body("missing_type"), body("none"), body("int")}) {
        auto text = original;
        text.replace(text.find(old), old.size(), replacement);
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        auto offset = text.find("local_box");
        if (offset == std::string::npos)
            offset = text.rfind("arg");
        const auto position = server::toPosition(
            slang::SourceLocation(server.m_driver->getDocument(doc.m_uri)->getBuffer(), offset),
            server.m_driver->sm);
        checkNativeDiagnostics(workspace, server, text, true, position);
    }
}

TEST_CASE("Method-local generic arguments retain private nominal types and current values",
          "[method-body-diagnostics][body-local-generic-arguments]") {
    const auto context = GENERATE("class", "package", "module");
    const auto kind = GENERATE("alias", "enum", "packed", "unpacked", "value");
    const bool task = GENERATE(false, true);
    const bool cancelFirst = GENERATE(false, true);
    CAPTURE(context, kind, task, cancelFirst);
    const bool cls = std::string_view(context) == "class";
    const bool module = std::string_view(context) == "module";
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv", "package prelude; class Box #(type T=int, int N=1); T value;\n"
                                  "function int width(); return $bits(T)+N; endfunction\n"
                                  "endclass endpackage\n");
    if (module)
        workspace.write("owner.sv", "module owner;\n`include \"item.svh\"\nendmodule\n");
    const std::string call = cls ? "c.work(1)" : module ? "dut.work(1)" : "owner::work(1)";
    workspace.write("tail.sv", std::string(cls      ? "module tail; import owner::*; C c; "
                                           : module ? "module tail; owner dut(); "
                                                    : "module tail; ") +
                                   "initial " + (task ? call : "void'(" + call + ")") +
                                   "; endmodule\n");
    auto body = [&](bool wider, bool invalid) {
        const std::string count = invalid ? "missing_local_count" : wider ? "2" : "1";
        const std::string width = wider ? "15" : "7";
        const std::string type = kind;
        const std::string declaration =
            type == "alias"      ? "typedef logic[" + width + ":0] local_t; "
            : type == "enum"     ? "typedef enum logic[" + width + ":0] {EMPTY=0,READY=1} local_t; "
            : type == "packed"   ? "typedef struct packed {logic[" + width + ":0] data;} local_t; "
            : type == "unpacked" ? "typedef struct {logic[" + width + ":0] data;} local_t; "
                                 : "";
        return "localparam int COUNT=" + count + "; " + declaration + "prelude::Box#(" +
               (type == "value" ? "int" : "local_t") + ",COUNT) local_box; local_box=new; " +
               (task ? "$display(arg+local_box.width());" : "return arg+local_box.width();");
    };
    const std::string old = task ? "$display(arg);" : "return arg;";
    const std::string original = (cls ? std::string("class C;\n") : std::string{}) +
                                 (task ? std::string(cls ? "task " : "task automatic ")
                                       : std::string(cls ? "function int "
                                                         : "function automatic int ")) +
                                 "work(input int arg);\n" + old + "\n" +
                                 (task ? "endtask\n" : "endfunction\n") + (cls ? "endclass\n" : "");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto& replacement : {body(false, false), body(true, false), body(true, true),
                                    body(false, false) + "\n", old, body(true, false)}) {
        auto text = original;
        text.replace(text.find(old), old.size(), replacement);
        change(server, doc.m_uri, ++version, text);
        const auto position = server::toPosition(
            slang::SourceLocation(server.m_driver->getDocument(doc.m_uri)->getBuffer(),
                                  text.rfind("arg")),
            server.m_driver->sm);
        // Exercise shallow query disposal before completing diagnostics and before
        // the next nominal type is created with the same local spelling.
        REQUIRE(server.getDocHover({.textDocument = {.uri = doc.m_uri}, .position = position}));
        if (cancelFirst && version == 2) {
            const auto before = diagnosticMap(server);
            lsp::RequestContext ctx("local-generic-analysis", 205);
            bool reached = false;
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == "after local diagnostic binding") {
                    reached = true;
                    ctx.cancel();
                }
            });
            CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
            REQUIRE(reached);
            CHECK(diagnosticMap(server) == before);
            CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
        }
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        checkNativeDiagnostics(workspace, server, text, true, position);
    }
}

TEST_CASE("Method-local let declarations preserve defaults and unused diagnostics",
          "[method-body-diagnostics][body-local-let]") {
    const auto context = GENERATE("class", "package", "module");
    const auto kind = GENERATE("simple", "default", "unused", "import", "nested", "generic");
    const bool task = GENERATE(false, true);
    const bool initialPresent = GENERATE(false, true);
    const bool cancelFirst = GENERATE(false, true);
    CAPTURE(context, kind, task, initialPresent, cancelFirst);
    const bool cls = std::string_view(context) == "class";
    const bool module = std::string_view(context) == "module";
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv",
                    "package prelude; int shared,other; class Box #(type T=int);\n"
                    "static function T read(input T value); return value; endfunction\n"
                    "endclass endpackage\n");
    if (module)
        workspace.write("owner.sv", "module owner;\n`include \"item.svh\"\nendmodule\n");
    const std::string call = cls ? "c.work(1)" : module ? "dut.work(1)" : "owner::work(1)";
    workspace.write("tail.sv", std::string(cls      ? "module tail; import owner::*; C c; "
                                           : module ? "module tail; owner dut(); "
                                                    : "module tail; ") +
                                   "initial " + (task ? call : "void'(" + call + ")") +
                                   "; endmodule\n");
    auto value = [task](std::string expression) {
        return task ? "$display(" + expression + ");" : "return " + expression + ";";
    };
    auto body = [&](bool changed, bool invalid) {
        const std::string type = kind;
        const std::string operand = invalid ? "missing_let_name" : changed ? "2" : "1";
        if (type == "unused")
            return "let unused=" +
                   std::string(invalid   ? "missing_let_name"
                               : changed ? "prelude::other"
                                         : "prelude::shared") +
                   "; " + value("arg");
        if (type == "default")
            return "let add(x,y=" +
                   std::string(invalid   ? "missing_let_default"
                               : changed ? "arg+1"
                                         : "arg") +
                   ")=x+y; " + value("add(arg)");
        if (type == "import")
            return "import prelude::*; let add(x)=x+" +
                   std::string(invalid   ? "missing_let_import"
                               : changed ? "other"
                                         : "shared") +
                   "; " + value("add(arg)");
        if (type == "generic")
            return "let read(x)=prelude::Box#(" +
                   std::string(invalid   ? "missing_let_type"
                               : changed ? "byte"
                                         : "int") +
                   ")::read(x); " + value("read(arg)");
        const auto expression = "let add(x)=x+" + operand + "; " + value("add(arg)");
        return type == "nested" ? "begin : inner " + expression + " end" : expression;
    };
    const auto old = value("arg");
    const auto valid = body(false, false);
    const std::string initial = initialPresent ? valid : old;
    const std::string original = (cls ? std::string("class C;\n") : std::string{}) +
                                 (task ? std::string(cls ? "task " : "task automatic ")
                                       : std::string(cls ? "function int "
                                                         : "function automatic int ")) +
                                 "work(input int arg);\n" + initial + "\n" +
                                 (task ? "endtask\n" : "endfunction\n") + (cls ? "endclass\n" : "");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto& replacement :
         {valid + " ", body(true, false), body(true, true), valid + "\n", old, body(true, false)}) {
        auto text = original;
        text.replace(text.find(initial), initial.size(), replacement);
        change(server, doc.m_uri, ++version, text);
        const auto position = server::toPosition(
            slang::SourceLocation(server.m_driver->getDocument(doc.m_uri)->getBuffer(),
                                  text.rfind("arg")),
            server.m_driver->sm);
        REQUIRE(server.getDocHover({.textDocument = {.uri = doc.m_uri}, .position = position}));
        if (cancelFirst && version == 2) {
            const auto before = diagnosticMap(server);
            lsp::RequestContext ctx("local-let-analysis", 206);
            bool reached = false;
            ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == "after local diagnostic binding") {
                    reached = true;
                    ctx.cancel();
                }
            });
            CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
            REQUIRE(reached);
            CHECK(diagnosticMap(server) == before);
            CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
        }
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        checkNativeDiagnostics(workspace, server, text, true, position);
    }
}

TEST_CASE("Task expect properties preserve local diagnostics and clock references",
          "[method-body-diagnostics][body-local-expect]") {
    const auto context = GENERATE("class", "package", "module");
    CAPTURE(context);
    const bool cls = std::string_view(context) == "class";
    const bool module = std::string_view(context) == "module";
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv", "package prelude; bit clk; endpackage\n");
    if (module)
        workspace.write("owner.sv", "module owner;\n`include \"item.svh\"\nendmodule\n");
    workspace.write("tail.sv", std::string(cls      ? "module tail; import owner::*; C c; "
                                           : module ? "module tail; owner dut(); "
                                                    : "module tail; ") +
                                   "initial " +
                                   (cls      ? "c"
                                    : module ? "dut"
                                             : "owner:") +
                                   (cls || module ? "." : ":") + "work(1); endmodule\n");
    const std::string old = "$display(arg);";
    const std::string original = (cls ? std::string("class C;\n") : std::string{}) +
                                 (cls ? "task " : "task automatic ") + "work(input int arg);\n" +
                                 old + "\nendtask\n" + (cls ? "endclass\n" : "");
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto& replacement : {"expect (@(posedge prelude::clk) arg>0);",
                                    "expect (@(posedge prelude::clk) arg>0 ##1 arg>=0);",
                                    "expect (@(posedge prelude::clk) missing_expect_name>0);",
                                    "expect (@(negedge prelude::clk) arg>0) $display(arg);",
                                    "$display(arg);", "expect (@(posedge prelude::clk) arg>0);"}) {
        auto text = original;
        text.replace(text.find(old), old.size(), replacement);
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("A query-only replacement cannot start full diagnostics during body capture",
          "[body-certification][query-only-baseline]") {
    const bool localPublication = GENERATE(false, true);
    const std::string cancelAt = GENERATE("", "after shared method body warmup",
                                          "after diagnostic procedure");
    const bool cancelled = !cancelAt.empty();
    CAPTURE(localPublication, cancelAt);
    Workspace workspace;
    const std::string original = "package owner;\nfunction automatic int work(input int arg);\n"
                                 "return arg + 1;\nendfunction\nendpackage\n";
    auto body = original;
    body.replace(body.find("arg + 1"), 7, "arg + 2");
    workspace.write("owner.sv", original);
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("owner.sv", original);
    (void)doc.doc->getAnalysis();
    int version = 1;
    if (localPublication) {
        change(server, doc.m_uri, ++version, body);
        server.runBackgroundWork({});
        REQUIRE(diagnosticState(server).localDiagnosticUpdates == 1);
        doc.m_text = body;
        REQUIRE(server.getDocHover({.textDocument = {.uri = doc.m_uri},
                                   .position = doc.getPosition(original.find("owner") + 2)}));
        REQUIRE_FALSE(state(server).active);
    }
    // Navigation can replace the root while the last diagnostic publication
    // still describes the same inputs, without preparing the new root's analysis.
    server.m_driver->comp->refresh();
    REQUIRE(server.m_driver->comp->containsCurrentSource(workspace.root / "owner.sv"));
    REQUIRE(server.m_driver->comp->needsDiagnosticPreparation());
    size_t foregroundPreparations = 0;
    lsp::RequestContext ctx("body capture", lsp::ID_t{974});
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        foregroundPreparations += where == "after diagnostic scope" ||
                                  where == "after diagnostic procedure" ||
                                  where == "before private full-design semantics";
    });
    const auto source = localPublication ? body : original;
    CHECK_THROWS_WITH(server::BodyDiagnosticCache::capture(
                          *server.m_driver, doc.m_uri, source.find("function"),
                          source.find("endfunction") + std::string("endfunction").size(), ctx),
                      "local diagnostics require a completed current full-design baseline");
    CHECK(server.m_driver->comp->needsDiagnosticPreparation());
    auto text = original;
    text.replace(text.find("arg + 1"), 7, "arg + missing_after_navigation");
    change(server, doc.m_uri, ++version, text, ctx);
    doc.m_text = text;
    REQUIRE(server.getDocHover({.textDocument = {.uri = doc.m_uri},
                               .position = doc.getPosition(text.rfind("arg +") + 1)}, ctx));
    CHECK(foregroundPreparations == 0);
    REQUIRE(state(server).active);
    size_t backgroundRebuilds = 0;
    bool observedCancellation = false;
    lsp::RequestContext background("complete deferred baseline", lsp::ID_t{975});
    background.setCheckpointObserverForTesting([&](std::string_view where) {
        backgroundRebuilds += where == "before full-model library import";
        if (cancelled && where == cancelAt) {
            observedCancellation = true;
            background.cancel();
        }
    });
    if (cancelled) {
        CHECK_THROWS_AS(server.runBackgroundWork(background), lsp::RequestCancelled);
        REQUIRE(observedCancellation);
        REQUIRE(state(server).active);
        CHECK(server.client.diagnosticVersions.at(doc.m_uri) != version);
        text.replace(text.find("missing_after_navigation"),
                     std::string("missing_after_navigation").size(),
                     "missing_after_cancellation");
        change(server, doc.m_uri, ++version, text, ctx);
        doc.m_text = text;
        CHECK(foregroundPreparations == 0);
        server.runBackgroundWork({});
    }
    else
        server.runBackgroundWork(background);
    CHECK(backgroundRebuilds == 0);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == version);
    checkNativeDiagnostics(workspace, server, text, true, {}, {}, "owner.sv");
    const auto updates = diagnosticState(server).localDiagnosticUpdates;
    change(server, doc.m_uri, ++version, body, ctx);
    CHECK(foregroundPreparations == 0);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == updates + 1);
    CHECK(diagnosticState(server).localDiagnosticDecline.empty());
    checkNativeDiagnostics(workspace, server, body, true, {}, {}, "owner.sv");
}

TEST_CASE("Deferred shared binding preserves new generic specializations and virtual interfaces",
          "[body-certification][query-only-baseline][shared-semantic-warmup]") {
    const bool frozenInterface = GENERATE(false, true);
    CAPTURE(frozenInterface);
    Workspace workspace;
    workspace.write("prelude.sv", "interface channel #(parameter int WIDTH=8); "
                                 "logic [WIDTH-1:0] data; endinterface\n");
    const std::string original =
        "package owner;\n"
        "class Box #(type T=int);\n"
        "virtual channel#(8) endpoint; T data;\n"
        "function int touch(input int value); Box#(bit) peer; "
        "return value + peer.touch(value) + $bits(T); endfunction\n"
        "endclass\n"
        "Box#(int) resident;\n"
        "function automatic int work(input int arg);\n"
        "return arg + 1;\nendfunction\nendpackage\n";
    workspace.write("owner.sv", original);
    auto config = certificationConfig();
    if (frozenInterface)
        config.insert(config.size() - 1,
                      ",\"experimentalScopeSemanticCache\":true,"
                      "\"frozenSources\":{\"modules\":[\"channel\"],\"maxMiB\":64}");
    workspace.write(".slang/server.json", config);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    if (frozenInterface) {
        const auto status = rfl::json::write(server.m_driver->scopeLibraryStatus());
        REQUIRE(status.find("\"prepared\":true") != std::string::npos);
    }
    auto doc = server.openFile("owner.sv", original);
    (void)doc.doc->getAnalysis();
    server.m_driver->comp->refresh();
    auto owner = server.m_driver->comp->createActiveDesignContext({}).retainCompilation();
    auto package = owner->getPackage("owner");
    REQUIRE(package);
    auto resident = package->find("resident");
    REQUIRE(resident);
    REQUIRE(resident->getDeclaredType());
    REQUIRE(resident->getDeclaredType()->getType().isClass());
    CHECK_FALSE(owner->isElaborated());
    auto text = original;
    text.replace(text.find("arg + 1"), 7, "arg + missing_after_warmup");
    change(server, doc.m_uri, 2, text);
    size_t warmed = 0;
    lsp::RequestContext background("cancel shared generic warmup", lsp::ID_t{976});
    background.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "after shared method body warmup") {
            ++warmed;
            auto generic = package->find("Box")->as_if<slang::ast::GenericClassDefSymbol>();
            if (generic && generic->numSpecializations() >= 2)
                background.cancel();
        }
    });
    CHECK_THROWS_AS(server.runBackgroundWork(background), lsp::RequestCancelled);
    REQUIRE(warmed >= 1);
    CHECK_FALSE(owner->isElaborated());
    auto definition = package->find("Box")->as_if<slang::ast::GenericClassDefSymbol>();
    REQUIRE(definition);
    CHECK(definition->numSpecializations() >= 2);
    server.runBackgroundWork({});
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    checkNativeDiagnostics(workspace, server, text, true, {}, {}, "owner.sv");
    change(server, doc.m_uri, 3, original);
    server.runBackgroundWork({});
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 3);
    checkNativeDiagnostics(workspace, server, original, true, {}, {}, "owner.sv");
}

TEST_CASE("Obsolete unborrowed models retire before a cooperative rebuild",
          "[method-body-diagnostics][model-overlap-retirement]") {
    const bool cooperative = GENERATE(false, true);
    const bool borrowed = GENERATE(false, true);
    const bool cancelled = GENERATE(false, true);
    CAPTURE(cooperative, borrowed, cancelled);
    Workspace workspace;
    workspace.write(".slang/server.json",
                    std::string("{\"indexingThreads\":1,\"flags\":\"--single-unit --threads 1\","
                                "\"compilationFilelist\":\"design.f\","
                                "\"experimentalCooperativeAnalysis\":") +
                        (cooperative ? "true}" : "false}"));
    BodyTestServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    auto old = server.m_driver->comp->createActiveDesignContext({}).retainCompilation();
    std::weak_ptr<slang::ast::Compilation> previous = old;
    if (!borrowed)
        old.reset();
    const auto text = header + "class Extra; int current_member; endclass\n";
    change(server, doc.m_uri, 2, text);
    bool observed = false;
    lsp::RequestContext ctx("obsolete model replacement", lsp::ID_t{972});
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "before full-model library import") {
            observed = true;
            CHECK(previous.expired() == (cooperative && !borrowed));
            CHECK(server.m_driver->comp->getPreferredSymbolPath("owner", ctx) ==
                  std::optional((workspace.root / "owner.sv").string()));
            if (cancelled)
                ctx.cancel();
        }
    });
    if (cancelled) {
        CHECK_THROWS_AS(server.m_driver->comp->prepareDiagnostics(ctx),
                        lsp::RequestCancelled);
        CHECK_FALSE(server.m_driver->comp->containsCurrentSource(workspace.root / "item.svh"));
        CHECK(server.m_driver->getDocument(doc.m_uri)->textMatches(text));
    }
    else
        server.m_driver->comp->prepareDiagnostics(ctx);
    REQUIRE(observed);
    server.runBackgroundWork({});
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
    REQUIRE(server.m_driver->comp->getActiveInstance("tail"));
    CHECK(server.m_driver->comp->getActiveInstance("tail")->instPath == "tail");
    checkNativeDiagnostics(workspace, server, text);
    if (borrowed) {
        REQUIRE_FALSE(previous.expired());
        REQUIRE(old->getPackage("owner"));
        CHECK(old->getPackage("owner")->find("Accumulator"));
        CHECK_FALSE(old->getPackage("owner")->find("Extra"));
        old.reset();
    }
    CHECK(previous.expired());
}

TEST_CASE("Diagnostic fallback preserves the original capture refusal",
          "[method-body-diagnostics][body-diagnostic-refusal]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    (void)doc.doc->getAnalysis();
    server.m_driver->diagEngine.setErrorLimit(1);
    const auto text = edited();
    change(server, doc.m_uri, 2, text);
    REQUIRE(state(server).active);
    // Capture happens before the edit invalidates the full model. Its precise
    // refusal must survive a later attempt against that now-invalid model.
    CHECK(diagnosticState(server).localDiagnosticDecline ==
          "bounded diagnostic streams require full analysis");
    server.m_driver->diagEngine.setErrorLimit(0);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
    CHECK(diagnosticState(server).localDiagnosticDecline ==
          "bounded diagnostic streams require full analysis");
    checkNativeDiagnostics(workspace, server, text);
    // A fresh completed baseline must allow recovery, without a stale refusal.
    change(server, doc.m_uri, 3, header);
    server.runBackgroundWork({});
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    CHECK(diagnosticState(server).localDiagnosticDecline.empty());
    checkNativeDiagnostics(workspace, server, header);
}

TEST_CASE("Full-model diagnostic boundaries retain current versions and Native results",
          "[method-body-diagnostics][body-full-model-boundaries]") {
    const auto mode = GENERATE("constant-dependency", "error-limit", "fatal-hierarchy");
    CAPTURE(mode);
    const bool constant = std::string_view(mode) == "constant-dependency";
    const bool bounded = std::string_view(mode) == "error-limit";
    const bool fatal = std::string_view(mode) == "fatal-hierarchy";
    Workspace workspace;
    const std::string flags = std::string("--single-unit --threads 1") +
                              (bounded ? " --error-limit 1" : "") +
                              (fatal ? " --max-hierarchy-depth 4" : "");
    auto config = certificationConfig();
    const auto option = config.find("--single-unit --threads 1");
    REQUIRE(option != std::string::npos);
    config.replace(option, std::string("--single-unit --threads 1").size(), flags);
    const std::string nativeConfig =
        "{\"indexingThreads\":1,\"flags\":\"" + flags +
        "\",\"compilationFilelist\":\"design.f\",\"experimentalMethodScopeOverlay\":false,"
        "\"experimentalMethodBodyQueries\":false,\"experimentalMethodBodyDiagnostics\":false,"
        "\"experimentalMethodBodyCertification\":false,\"experimentalCooperativeAnalysis\":false}";
    workspace.write(".slang/server.json", config);
    const std::string tail = constant
                                 ? "module tail; localparam int W=owner::work(1); "
                                   "logic[W-1:0] data; endmodule\n"
                                 : "module tail; " + std::string(fatal ? "recursive r(); " : "") +
                                       "initial void'(owner::work(1)); endmodule\n" +
                                       (fatal ? "module recursive; recursive child(); endmodule\n"
                                              : "");
    workspace.write("tail.sv", tail);
    const std::string initial = constant ? "4" : "arg";
    const std::string original = "function automatic int work(input int arg);\nreturn " + initial +
                                 ";\nendfunction\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    auto tailDoc = server.openFile("tail.sv", tail);
    (void)doc.doc->getAnalysis();
    {
        auto baseline = server.m_driver->comp->createActiveDesignContext({}).retainCompilation();
        (void)baseline->getAllDiagnostics();
        CHECK(baseline->hasFatalErrors() == fatal);
    }
    if (bounded) {
        // CLI limits are deliberately disabled by the server. The cache still
        // rejects bounded engine settings defensively; restore the supported
        // unlimited setting before testing actual diagnostic publication.
        CHECK(server.m_driver->diagEngine.captureSettings().errorLimit == 0);
        server.m_driver->diagEngine.setErrorLimit(1);
        CHECK_THROWS_WITH(server::BodyDiagnosticCache::capture(*server.m_driver, doc.m_uri, 0,
                                                               original.size(), {}),
                          "bounded diagnostic streams require full analysis");
        server.m_driver->diagEngine.setErrorLimit(0);
    }
    int version = 1;
    for (const auto* expression : {"8", "missing_first+missing_second", "4"}) {
        auto text = original;
        text.replace(text.find("return " + initial), 7 + initial.size(),
                     "return " + std::string(expression));
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates ==
              (bounded ? uint64_t(version - 1) : 0));
        REQUIRE(server.client.diagnosticVersions.contains(doc.m_uri));
        CHECK(server.client.diagnosticVersions.at(doc.m_uri) == version);
        const auto position =
            constant ? tailDoc.getPosition(tail.find("data;"))
                     : server::toPosition(slang::SourceLocation(
                                              server.m_driver->getDocument(doc.m_uri)->getBuffer(),
                                              text.find("return ") + 7),
                                          server.m_driver->sm);
        const auto hoverUri = constant ? tailDoc.m_uri : doc.m_uri;
        const auto actualHover = rfl::json::write(
            server.getDocHover({.textDocument = {.uri = hoverUri}, .position = position}));
        auto actual = diagnosticMap(server);
        workspace.write(".slang/server.json", nativeConfig);
        {
            BodyTestServer native(workspace.params());
            native.setBuildFile((workspace.root / "design.f").string());
            auto oracle = native.openFile("item.svh", text);
            native.openFile("tail.sv", tail);
            CHECK(actualHover == rfl::json::write(native.getDocHover(
                                     {.textDocument = {.uri = hoverUri}, .position = position})));
            std::unordered_set<URI> keys(server.client.diagnosticPublications.begin(),
                                         server.client.diagnosticPublications.end());
            keys.insert(native.client.diagnosticPublications.begin(),
                        native.client.diagnosticPublications.end());
            auto published = [](auto& client, const URI& uri) {
                std::vector<std::string> result;
                for (const auto& diagnostic : client.getDiagnostics(uri))
                    result.push_back(rfl::json::write(diagnostic));
                std::sort(result.begin(), result.end());
                return result;
            };
            for (const auto& uri : keys)
                CHECK(published(server.client, uri) == published(native.client, uri));
            auto expected = diagnosticMap(native);
            std::erase_if(actual, [](const auto& item) { return item.second.empty(); });
            std::erase_if(expected, [](const auto& item) { return item.second.empty(); });
            INFO(rfl::json::write(actual));
            INFO(rfl::json::write(expected));
            CHECK(actual == expected);
        }
        workspace.write(".slang/server.json", config);
    }
}

TEST_CASE("Completed contexts reuse syntax only for the same included occurrence",
          "[body-syntax-index-reuse]") {
    using namespace slang;
    Workspace workspace;
    workspace.write("item.svh",
                    "function automatic int work(input int arg); return arg; endfunction\n");
    workspace.write("owner.sv", "package first;\n`include \"item.svh\"\nendpackage\n"
                                "package second;\n`include \"item.svh\"\nendpackage\n");
    SourceManager sm;
    auto parsed = syntax::SyntaxTree::fromFile((workspace.root / "owner.sv").string(), sm);
    REQUIRE(parsed);
    auto tree = *parsed;
    auto owner = std::make_shared<ast::Compilation>();
    owner->addSyntaxTree(tree);
    REQUIRE(owner->getSemanticDiagnostics().empty());
    const auto& first = owner->getPackage("first")->find<ast::SubroutineSymbol>("work");
    const auto& second = owner->getPackage("second")->find<ast::SubroutineSymbol>("work");
    REQUIRE(first.location.buffer() != second.location.buffer());
    const ast::SubroutineSymbol* a[] = {&first};
    const ast::SubroutineSymbol* b[] = {&second};
    auto originalUnit = std::make_shared<server::ShallowCompilation>(sm, tree, owner, a);
    auto original = std::make_shared<server::ShallowAnalysis>(first.location.buffer(),
                                                              originalUnit);
    auto nextUnit = std::make_shared<server::ShallowCompilation>(sm, tree, owner, b);
    auto next = std::make_shared<server::ShallowAnalysis>(second.location.buffer(), nextUnit,
                                                          lsp::RequestContext{}, original.get());
    auto token = next->syntaxes.getWordTokenAt(second.location);
    REQUIRE(token);
    CHECK(token->location().buffer() == second.location.buffer());
    CHECK(token != original->syntaxes.getWordTokenAt(first.location));
    auto reused = std::make_shared<server::ShallowAnalysis>(second.location.buffer(), nextUnit,
                                                            lsp::RequestContext{}, next.get());
    CHECK(reused->syntaxes.getWordTokenAt(second.location) == token);
    const auto& syntax = second.getSyntax()->as<syntax::FunctionDeclarationSyntax>();
    auto contexts = reused->getSubroutines(syntax);
    REQUIRE(contexts.size() == 1);
    CHECK(contexts.front() == &second);
    original.reset();
    next.reset();
    CHECK(reused->syntaxes.getWordTokenAt(second.location) == token);
    CHECK(reused->getSubroutines(syntax).front() == &second);
}

TEST_CASE("Local nominal class arguments never escape into the retained compilation",
          "[body-local-generic-ownership]") {
    using namespace slang;
    const auto kind = GENERATE("enum", "packed", "unpacked", "value");
    CAPTURE(kind);
    ast::CompilationOptions options;
    options.flags |= ast::CompilationFlags::TrackSubroutineBodyDependencies;
    const std::string prefix = "package p;\nclass Box #(type T=int,int N=1); T value;\n"
                               "function int width(); return $bits(T)+N; endfunction\nendclass\n";
    auto function = [&](int width, int count) {
        const std::string type = kind;
        const std::string declaration =
            type == "enum"     ? "typedef enum logic[" + std::to_string(width - 1) +
                                     ":0] {ZERO=0,ONE=1} local_t;\n"
            : type == "packed" ? "typedef struct packed {logic[" + std::to_string(width - 1) +
                                     ":0] data;} local_t;\n"
            : type == "unpacked"
                ? "typedef struct {logic[" + std::to_string(width - 1) + ":0] data;} local_t;\n"
                : "";
        return "function automatic int work(input int arg);\n" + declaration +
               "localparam int COUNT=" + std::to_string(count) + ";\n" + "p::Box#(" +
               (type == "value" ? "int" : "local_t") +
               ",COUNT) local_box;\nreturn arg+$bits(local_box.value);\nendfunction\n";
    };
    SourceManager sm;
    auto owner = std::make_shared<ast::Compilation>(options);
    owner->addSyntaxTree(
        syntax::SyntaxTree::fromText(prefix + function(8, 1) + "endpackage\n", sm));
    REQUIRE(owner->getSemanticDiagnostics().empty());
    auto& definition = owner->getPackage("p")->find<ast::GenericClassDefSymbol>("Box");
    const auto& original = owner->getPackage("p")->find<ast::SubroutineSymbol>("work");
    const auto count = definition.numSpecializations();
    const auto bytes = owner->getTotalBytesAllocated();
    const auto references = owner->getReferenceUsageExcludingBodies({});
    REQUIRE(references);
    auto padding = prefix;
    for (auto& c : padding)
        if (c != '\n')
            c = ' ';
    for (const auto [width, value] : {std::pair{16, 2}, {8, 3}, {32, 4}, {16, 2}}) {
        CAPTURE(width, value);
        std::weak_ptr<ast::Compilation> released;
        {
            const auto current = function(width, value);
            auto tree = syntax::SyntaxTree::fromText(padding + current, sm);
            REQUIRE(tree->diagnostics().empty());
            const syntax::FunctionDeclarationSyntax* fn = nullptr;
            auto find = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
                if (auto found = node.as_if<syntax::FunctionDeclarationSyntax>()) {
                    fn = found;
                    return;
                }
                for (uint32_t i = 0; i < node.getChildCount(); ++i)
                    if (auto child = node.childNode(i))
                        self(self, *child);
            };
            find(find, tree->root());
            REQUIRE(fn);
            auto query = std::make_shared<ast::Compilation>(options);
            released = query;
            query->setBodyQueryContext(owner, tree);
            {
                ast::BodyQueryReferenceGuard guard(*query);
                const auto& replacement = ast::SubroutineSymbol::createBodyQuery(*query, *fn,
                                                                                 original);
                query->forceElaborate(replacement);
                const auto& cls = replacement.find<ast::VariableSymbol>("local_box")
                                      .getType()
                                      .as<ast::ClassType>();
                CHECK(&cls.getCompilation() == query.get());
                const auto& argument = cls.find<ast::VariableSymbol>("value").getType();
                if (std::string_view(kind) != "value") {
                    REQUIRE(argument.getCanonicalType().getParentScope());
                    CHECK(&argument.getCanonicalType().getParentScope()->getCompilation() ==
                          query.get());
                    CHECK(argument.getBitstreamWidth() == uint64_t(width));
                }
                const ast::SubroutineSymbol* replaced[] = {&original};
                auto roots = query->getClassSpecializationUses();
                REQUIRE(roots);
                auto unused = owner->getUnusedClassSpecializationsExcludingBodies(replaced, {},
                                                                                  *roots);
                REQUIRE(unused);
                const auto view = query->getBodyQuerySemanticView(replacement, replaced, *unused);
                CHECK(view.diagnostics.empty());
                REQUIRE(view.references);
                CHECK(cls.hasCompletedSemanticAnalysis());
            }
            ast::Compilation fresh(options);
            fresh.addSyntaxTree(
                syntax::SyntaxTree::fromText(prefix + current + "endpackage\n", sm));
            CHECK(fresh.getSemanticDiagnostics().empty());
        }
        CHECK(released.expired());
        CHECK(definition.numSpecializations() == count);
        CHECK(owner->getTotalBytesAllocated() == bytes);
        CHECK(owner->getSemanticDiagnostics().empty());
        CHECK(owner->getReferenceUsageExcludingBodies({}) == references);
    }
}

TEST_CASE("Class value specialization keys preserve inferred parameter types",
          "[body-generic-value-types]") {
    using namespace slang;
    const auto kind = GENERATE("width", "sign", "state");
    const bool typed = GENERATE(false, true);
    const bool reverse = GENERATE(false, true);
    CAPTURE(kind, typed, reverse);
    const std::string shape = kind;
    std::string arguments[] = {shape == "state" ? "bit'(1)" : "32'd2", shape == "width" ? "64'd2"
                                                                       : shape == "sign"
                                                                           ? "32'sd2"
                                                                           : "logic'(1)"};
    if (reverse)
        std::swap(arguments[0], arguments[1]);
    SourceManager sm;
    ast::Compilation compilation;
    const std::string definition = "package p; class Box #(parameter " +
                                   std::string(typed ? "int " : "") +
                                   "N=1); logic[$bits(N)-1:0] payload; endclass\n";
    compilation.addSyntaxTree(syntax::SyntaxTree::fromText(definition + "Box#(" + arguments[0] +
                                                               ") first; Box#(" + arguments[1] +
                                                               ") second; endpackage\n",
                                                           sm));
    for (const auto& diagnostic : compilation.getSemanticDiagnostics())
        REQUIRE_FALSE(diagnostic.isError());
    const auto* package = compilation.getPackage("p");
    const auto& first = package->find<ast::VariableSymbol>("first").getType().as<ast::ClassType>();
    const auto& second =
        package->find<ast::VariableSymbol>("second").getType().as<ast::ClassType>();
    const ast::ClassType* classes[] = {&first, &second};
    std::unique_ptr<ast::Compilation> oracles[2];
    const ast::Type* expectedTypes[2] = {};
    for (unsigned i = 0; i < 2; ++i) {
        const bool latter = reverse ? i == 0 : i == 1;
        const auto& type =
            classes[i]->genericParameters.front()->as<ast::ParameterSymbol>().getType();
        // Use an independent single-specialization compilation as the oracle
        // for inferred types of known constant parameters.
        oracles[i] = std::make_unique<ast::Compilation>();
        auto& fresh = *oracles[i];
        fresh.addSyntaxTree(syntax::SyntaxTree::fromText(
            definition + "Box#(" + arguments[i] + ") only; endpackage\n", sm));
        for (const auto& diagnostic : fresh.getSemanticDiagnostics())
            REQUIRE_FALSE(diagnostic.isError());
        const auto& expected = fresh.getPackage("p")
                                   ->find<ast::VariableSymbol>("only")
                                   .getType()
                                   .as<ast::ClassType>()
                                   .genericParameters.front()
                                   ->as<ast::ParameterSymbol>()
                                   .getType();
        expectedTypes[i] = &expected;
        const uint64_t width = typed              ? 32
                               : shape == "width" ? (latter ? 64 : 32)
                               : shape == "state" ? 1
                                                  : 32;
        CHECK(type.getBitWidth() == width);
        CHECK(type.isSigned() == expected.isSigned());
        CHECK(type.isFourState() == expected.isFourState());
        CHECK(classes[i]->find<ast::VariableSymbol>("payload").getType().getBitWidth() == width);
    }
    // Inference can erase the distinction between known bit/logic cast values.
    // Reuse follows the inferred types, rather than their source spellings.
    CHECK((&first == &second) == expectedTypes[0]->isMatching(*expectedTypes[1]));
}

TEST_CASE("Edits creating their own generic class use current methods in every specialization",
          "[method-body-diagnostics][body-generic-self-closure]") {
    const bool task = GENERATE(false, true);
    const bool external = GENERATE(false, true);
    const auto initial = GENERATE("none", "int", "byte");
    CAPTURE(task, external, initial);
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const auto body = [&](std::string_view parameter) {
        std::string result;
        if (parameter != "none")
            result = "C#(" + std::string(parameter) + ") local_box; ";
        return result + (task ? "arg++;" : "return arg;");
    };
    const auto old = body(initial);
    const std::string signature = task ? "task" : "function int";
    const std::string end = task ? "endtask" : "endfunction";
    const std::string method = signature + (external ? " C::work" : " work") +
                               "(input int arg);\n" + old + "\n" + end + "\n";
    const std::string original = "class C #(type T=int);\n" +
                                 (external ? "extern " + signature +
                                                 " work(input int arg); endclass\n" + method
                                           : method + "endclass\n");
    workspace.write("item.svh", original);
    workspace.write("tail.sv", "module tail; import owner::*; C#(int) c; initial " +
                                   std::string(task ? "c.work(1);" : "void'(c.work(1));") +
                                   " endmodule\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto& replacement : {body("int") + " ", body("byte"), body("byte") + "\n",
                                    body("missing_type"), body("none"), body("int")}) {
        auto text = original;
        text.replace(text.find(old), old.size(), replacement);
        change(server, doc.m_uri, ++version, text);
        server.runBackgroundWork({});
        INFO(state(server).decline);
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        auto offset = text.find("local_box");
        if (offset == std::string::npos)
            offset = text.rfind("arg");
        const auto position = server::toPosition(
            slang::SourceLocation(server.m_driver->getDocument(doc.m_uri)->getBuffer(), offset),
            server.m_driver->sm);
        // Native hover does not select a generic class context here (it returns
        // null even for the unchanged argument). Compare all diagnostics with
        // Native and assert the supported private hover independently.
        const auto hover = rfl::json::write(
            server.getDocHover({.textDocument = {.uri = doc.m_uri}, .position = position}));
        CHECK(hover != "null");
        CHECK(hover.find(offset == text.rfind("arg") ? "arg" : "local_box") != std::string::npos);
        if (replacement.find("C#(byte)") != std::string::npos)
            CHECK(hover.find("C#(byte)") != std::string::npos);
        if (replacement.find("C#(int)") != std::string::npos)
            CHECK(hover.find("C#(int)") != std::string::npos);
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Hover before generic diagnostics releases old query imports safely",
          "[method-body-diagnostics][body-query-origin-lifetime]") {
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    workspace.write("prelude.sv",
                    "package prelude; int imported_value; class Box #(type T=int); "
                    "T value; function T read(input int arg); return arg; endfunction "
                    "endclass endpackage\n");
    const auto body = [](std::string_view parameter) {
        return std::string(
                   parameter == "none" ? "" : "Box#(" + std::string(parameter) + ") local_box; ") +
               "int local_value=imported_value; return local_value+arg;";
    };
    const auto old = body("none");
    const std::string original = "import prelude::*; class C #(type T=int); "
                                 "function int work(input int arg);\n" +
                                 old + "\nendfunction endclass\n";
    workspace.write("item.svh", original);
    workspace.write("tail.sv", "module tail; import owner::*; C#(int) c; "
                               "initial void'(c.work(1)); endmodule\n");
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto* parameter : {"int", "byte", "missing_type", "none", "byte", "int"}) {
        auto text = original;
        const auto replacement = body(parameter);
        text.replace(text.find(old), old.size(), replacement);
        change(server, doc.m_uri, ++version, text);
        auto offset = text.find("local_box");
        if (offset == std::string::npos)
            offset = text.rfind("arg");
        const auto position = server::toPosition(
            slang::SourceLocation(server.m_driver->getDocument(doc.m_uri)->getBuffer(), offset),
            server.m_driver->sm);
        // Match the real LSP order: query first, then publish diagnostics, then
        // the next change destroys this query before another reference replay.
        const auto hover = server.getDocHover(
            {.textDocument = {.uri = doc.m_uri}, .position = position});
        CHECK(rfl::json::write(hover) != "null");
        server.runBackgroundWork({});
        INFO(diagnosticState(server).localDiagnosticDecline);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

namespace {
struct LineSyntaxFixture {
    Workspace workspace;
    std::string original = header;
    CertificationServer server;
    std::shared_ptr<ast::Compilation> owner;
    const ast::SubroutineSymbol* edited = nullptr;
    const ast::SubroutineSymbol* later = nullptr;
    size_t originalEnd = 0;
    slang::BufferID originalBuffer;

    lsp::InitializeParams configure() {
        original.insert(original.find("  function int combine"),
                        "  function void report(input string message, input int line_number); "
                        "endfunction\n");
        original.replace(original.rfind("return value;"), 13,
                         "report(\"test\", `__LINE__); return value;");
        workspace.write("item.svh", original);
        workspace.write(".slang/server.json", certificationConfig());
        return workspace.params();
    }

    LineSyntaxFixture() : server(configure()) {
        server.setBuildFile((workspace.root / "design.f").string());
        auto doc = server.openFile("item.svh", original);
        (void)doc.doc->getAnalysis();
        auto design = server.m_driver->comp->createActiveDesignContext({});
        owner = design.retainCompilation();
        for (auto fn : design.getAnalysis().getAnalysisManager().getAnalyzedSubroutines()) {
            if (fn->name == "combine")
                edited = fn;
            else if (fn->name == "later")
                later = fn;
        }
        REQUIRE(edited);
        REQUIRE(later);
        originalBuffer = edited->getSyntax()->getFirstToken().location().buffer();
        originalEnd = edited->getSyntax()->getLastToken().range().end().offset();
    }

    std::string text(std::string_view expression) const {
        auto result = original;
        result.replace(result.find("local_1 + charl"), 15, expression);
        return result;
    }
    size_t end(const std::string& current) const {
        return size_t(int64_t(originalEnd) + int64_t(current.size()) - int64_t(original.size()));
    }
    size_t endLine(const std::string& current) const {
        return size_t(std::count(current.begin(), current.begin() + end(current), '\n')) + 1;
    }
    auto proof(size_t budget) {
        return std::make_unique<server::BodyLineProof>(
            server.m_driver->sm, *owner, *edited, originalBuffer, originalEnd, original,
            (workspace.root / "item.svh").string(), lsp::RequestContext{}, budget);
    }
    size_t entries() const { return server.m_driver->sm.getSourceLocationEntryCount(); }
};
} // namespace

TEST_CASE("Neighbour syntax memo bounds replay storage while line coordinates change",
          "[method-body-diagnostics][body-line-syntax-cache]") {
    const size_t budget = GENERATE(size_t(0), size_t(1), size_t(1024 * 1024));
    LineSyntaxFixture fixture;
    auto proof = fixture.proof(budget);
    const auto first = fixture.text("local_0 +\n charl");
    const auto sameHeight = fixture.text("local_0 + 12 +\n charl");
    const auto newHeight = fixture.text("local_0 +\n 12 +\n charl");
    const auto before = fixture.entries();
    proof->validate(first, fixture.end(first), {}, fixture.endLine(first));
    const auto afterFirst = fixture.entries();
    CHECK(afterFirst > before);
    proof->validate(sameHeight, fixture.end(sameHeight), {}, fixture.endLine(sameHeight));
    const auto afterSame = fixture.entries();
    if (budget == 1024 * 1024)
        CHECK(afterSame == afterFirst);
    else
        CHECK(afterSame > afterFirst);
    proof->validate(newHeight, fixture.end(newHeight), {}, fixture.endLine(newHeight));
    const auto afterNewHeight = fixture.entries();
    CHECK(afterNewHeight > afterSame);
    // One cached coordinate, never a growing history of successful edits.
    proof->validate(sameHeight, fixture.end(sameHeight), {}, fixture.endLine(sameHeight));
    CHECK(fixture.entries() > afterNewHeight);
}

TEST_CASE("Neighbour syntax memo rejects changed suffixes and late semantic dependents",
          "[method-body-diagnostics][body-line-syntax-cache]") {
    LineSyntaxFixture fixture;
    auto proof = fixture.proof(1024 * 1024);
    const auto first = fixture.text("local_0 +\n charl");
    auto sameHeight = fixture.text("local_0 + 12 +\n charl");
    proof->validate(first, fixture.end(first), {}, fixture.endLine(first));
    const auto before = fixture.entries();
    fixture.later->bodyHasExternalDependents.store(true, std::memory_order_relaxed);
    auto restore = slang::ScopeGuard([&] {
        fixture.later->bodyHasExternalDependents.store(false, std::memory_order_relaxed);
    });
    CHECK_THROWS(proof->validate(sameHeight, fixture.end(sameHeight), {},
                                 fixture.endLine(sameHeight)));
    CHECK(fixture.entries() == before);
    fixture.later->bodyHasExternalDependents.store(false, std::memory_order_relaxed);
    sameHeight.replace(sameHeight.rfind("report(\"test\""), 6, "retort");
    CHECK_THROWS(proof->validate(sameHeight, fixture.end(sameHeight), {},
                                 fixture.endLine(sameHeight)));
}

TEST_CASE("Cancelled neighbour replay cannot commit a partially checked syntax key",
          "[method-body-diagnostics][body-line-syntax-cache]") {
    LineSyntaxFixture fixture;
    auto proof = fixture.proof(1024 * 1024);
    const auto first = fixture.text("local_0 +\n charl");
    lsp::RequestContext cancelled("line-proof", 810);
    bool reached = false;
    cancelled.setCheckpointObserverForTesting([&](std::string_view checkpoint) {
        if (checkpoint == "before later macro syntax cache commit") {
            reached = true;
            cancelled.cancel();
        }
    });
    CHECK_THROWS_AS(proof->validate(first, fixture.end(first), cancelled, fixture.endLine(first)),
                    lsp::RequestCancelled);
    REQUIRE(reached);
    const auto afterCancelled = fixture.entries();
    proof->validate(first, fixture.end(first), {}, fixture.endLine(first));
    const auto afterCompleted = fixture.entries();
    CHECK(afterCompleted > afterCancelled);
    lsp::RequestContext reuse("line-proof", 811);
    reached = false;
    reuse.setCheckpointObserverForTesting([&](std::string_view checkpoint) {
        if (checkpoint == "reuse later macro syntax proof") {
            reached = true;
            reuse.cancel();
        }
    });
    CHECK_THROWS_AS(proof->validate(first, fixture.end(first), reuse, fixture.endLine(first)),
                    lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(fixture.entries() == afterCompleted);
    proof->validate(first, fixture.end(first), {}, fixture.endLine(first));
    CHECK(fixture.entries() == afterCompleted);
}


TEST_CASE("Withdrawn query diagnostic option keeps foreground work separate and certified",
          "[body-certification][query-diagnostic-transaction]") {
    const auto operation = GENERATE("hover", "complete", "define", "resolve");
    const auto mode = GENERATE("success", "cancel", "changed");
    INFO(operation << " / " << mode);
    Workspace workspace;
    auto config = certificationConfig();
    config.insert(config.size() - 1, ",\"experimentalQueryDiagnosticTransactions\":true");
    workspace.write(".slang/server.json", config);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = edited();
    change(server, doc.m_uri, 2, text);
    doc.m_text = text;
    // An opt-in query must still leave cold diagnostic preparation in background.
    size_t coldPreparations = 0;
    lsp::RequestContext cold("textDocument/hover", 700);
    cold.setCheckpointObserverForTesting([&](std::string_view phase) {
        coldPreparations += phase == "before optional diagnostic preparation";
    });
    REQUIRE(server.getDocHover({.textDocument = {.uri = doc.m_uri},
                                .position = doc.getPosition(text.rfind("local_0") + 3)}, cold));
    CHECK(coldPreparations == 0);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 0);
    server.runBackgroundWork({});
    REQUIRE(diagnosticState(server).localDiagnosticUpdates == 1);
    text = header;
    text.replace(text.find("local_1 + charl"), 15, "local_0 + absent_local");
    change(server, doc.m_uri, 3, text);
    doc.m_text = text;
    REQUIRE(server.m_methodScope);
    REQUIRE(server.m_methodScope->hasPreparedLocalDiagnostics());
    const auto at = doc.getPosition(text.rfind("local_0") + 3);
    auto items = server.m_methodScope->complete(
        {.textDocument = {.uri = doc.m_uri}, .position = at}, server.m_config, {});
    REQUIRE(items);
    auto found = std::ranges::find_if(*items, [](const auto& item) { return item.label == "charl"; });
    REQUIRE(found != items->end());
    const auto item = *found;
    const auto before = diagnosticMap(server);
    const auto clientBefore = rfl::json::write(server.client.getDiagnostics(doc.m_uri));
    const auto stamp = std::filesystem::last_write_time(workspace.root / "prelude.sv");
    size_t proofs = 0, publications = 0, preparation = 0;
    bool changed = false;
    lsp::RequestContext ctx("transaction-query", 701);
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        proofs += phase == "method inputs certified";
        publications += phase == "before local diagnostic publication";
        preparation += phase == "before optional diagnostic preparation";
        if (phase == "method input validation" && std::string_view(mode) == "cancel")
            ctx.cancelForContentChange();
        if (phase == "method input validation" && std::string_view(mode) == "changed" &&
            !changed) {
            changed = true;
            workspace.write("prelude.sv", "package prelude; int other; endpackage\n");
            std::filesystem::last_write_time(workspace.root / "prelude.sv", stamp);
        }
    });
    auto query = [&](const lsp::RequestContext& request) {
        if (std::string_view(operation) == "hover")
            return server.m_methodScope->hover(doc.m_uri, at, server.m_config, request).has_value();
        if (std::string_view(operation) == "complete")
            return server.m_methodScope->complete(
                {.textDocument = {.uri = doc.m_uri}, .position = at}, server.m_config, request).has_value();
        if (std::string_view(operation) == "define")
            return server.m_methodScope->define(
                {.textDocument = {.uri = doc.m_uri}, .position = at}, server.m_config, request).has_value();
        return server.m_methodScope->resolve(item, server.m_config, request).has_value();
    };
    if (std::string_view(mode) == "cancel") {
        CHECK_THROWS_AS(query(ctx), lsp::RequestCancelled);
        CHECK(ctx.isContentModified());
        CHECK(proofs == 0);
    }
    else {
        CHECK(query(ctx) == (std::string_view(mode) != "changed"));
        CHECK_FALSE(ctx.isCancelled());
        CHECK(proofs == (std::string_view(mode) == "changed" ? 0 : 1));
    }
    CHECK(preparation == 0);
    CHECK(publications == 0);
    CHECK(diagnosticMap(server) == before);
    CHECK(rfl::json::write(server.client.getDiagnostics(doc.m_uri)) == clientBefore);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
    CHECK(diagnosticState(server).localDiagnosticUpdates == 1);
    CHECK_FALSE(server.m_driver->hasCertifiedLocalDiagnostics());
    CHECK(server.m_driver->hasPendingDiagnostics());
    if (std::string_view(mode) != "changed") {
        size_t backgroundProofs = 0;
        lsp::RequestContext background("independent diagnostics", std::nullopt);
        background.setCheckpointObserverForTesting([&](std::string_view phase) {
            backgroundProofs += phase == "method inputs certified";
        });
        server.runBackgroundWork(background);
        CHECK(backgroundProofs == 1);
        CHECK(diagnosticState(server).localDiagnosticUpdates == 2);
        CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 3);
        size_t nextProofs = 0;
        lsp::RequestContext next("later-query", 702);
        next.setCheckpointObserverForTesting([&](std::string_view phase) {
            nextProofs += phase == "method inputs certified";
        });
        CHECK(query(next));
        CHECK(nextProofs == 1);
        checkNativeDiagnostics(workspace, server, text, true);
    }
    else
        CHECK(changed);
}

TEST_CASE("Stable target prefix renderings reuse Native payloads across body line edits",
          "[method-body-diagnostics][prefix-diagnostic-reuse]") {
    const bool cancel = GENERATE(false, true);
    Workspace workspace;
    auto config = certificationConfig();
    config.insert(config.size() - 1, ",\"methodBodyRenderingCacheKiB\":0");
    workspace.write(".slang/server.json", config);
    const std::string original =
        "class C;\n"
        "function int earlier(input int arg); if(arg) return 1; endfunction\n"
        "function int work(input int arg);\n return arg + 1;\nendfunction\n"
        "function int later(input int arg); if(arg) return 2; endfunction\n"
        "endclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    const auto before = diagnosticMap(server);
    size_t version = 1;
    for (const auto* expression : {"arg +\n 2", "arg + missing_name", "arg + 3", "arg + 1"}) {
        auto text = original;
        text.replace(text.find("arg + 1"), 7, expression);
        change(server, doc.m_uri, ++version, text);
        if (cancel && version == 2) {
            bool reached = false;
            lsp::RequestContext interrupted("prefix rendering", 850);
            interrupted.setCheckpointObserverForTesting([&](std::string_view phase) {
                if (phase == "before prefix diagnostic rendering commit") {
                    reached = true;
                    interrupted.cancel();
                }
            });
            CHECK_THROWS_AS(server.runBackgroundWork(interrupted), lsp::RequestCancelled);
            REQUIRE(reached);
            CHECK(diagnosticMap(server) == before);
            CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 1);
        }
        server.runBackgroundWork({});
        REQUIRE(server.m_methodScope);
        auto counts = server.m_methodScope->localDiagnosticRenderingCounts();
        CHECK(counts.first > 0); // Only the earlier function's immutable rendering.
        CHECK(counts.second > 0); // Later ranges still use the current projection.
        CHECK(diagnosticState(server).localDiagnosticUpdates == version - 1);
        checkNativeDiagnostics(workspace, server, text, true);
    }
}


TEST_CASE("Retained target display memo follows exact current end coordinates within its budget",
          "[method-body-diagnostics][target-rendering-memo]") {
    const auto budget = GENERATE(0u, 1u, 256u);
    const bool crlf = GENERATE(false, true);
    Workspace workspace;
    auto config = certificationConfig();
    config.insert(config.size() - 1, ",\"methodBodyRenderingCacheKiB\":" + std::to_string(budget));
    workspace.write(".slang/server.json", config);
    std::string original =
        "class C;\n"
        "function int earlier(input int arg); if(arg) return 1; endfunction\n"
        "function int work(input int arg); return arg + 1; endfunction\n"
        "function int later(input int arg); if(arg) return 2; endfunction\n"
        "endclass\n";
    if (crlf)
        for (size_t at = 0; (at = original.find('\n', at)) != std::string::npos; at += 2)
            original.insert(at, 1, '\r');
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    std::optional<lsp::Position> previous;
    for (auto expression : {std::string("arg + 2"), std::string("arg + 3"),
                            std::string("arg + 100"), std::string("arg + 200"),
                            std::string("arg + 2 /*界*/"), std::string("arg + 3 /*語*/"),
                            std::string("arg + 2 /*🚀*/"), std::string("arg + 3 /*😀*/"),
                            std::string("arg +\n 4"), std::string("arg +\n 5"), std::string("arg + 1")}) {
        if (crlf)
            for (size_t at = 0; (at = expression.find('\n', at)) != std::string::npos; at += 2)
                expression.insert(at, 1, '\r');
        auto text = original;
        text.replace(text.find("arg + 1"), 7, expression);
        change(server, doc.m_uri, ++version, text);
        const auto end = text.find("endfunction", text.find("function int work")) + 11;
        auto coordinate = server::toPosition(slang::SourceLocation(doc.doc->getBuffer(), end),
                                             server.m_driver->sm);
        size_t drafts = 0;
        lsp::RequestContext ctx("target memo background", version);
        ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
            drafts += phase == "before target rendering memo publication";
        });
        server.runBackgroundWork(ctx);
        REQUIRE(server.m_methodScope);
        auto usage = server.m_methodScope->localDiagnosticRenderingCacheUsage();
        CHECK(usage.first <= size_t(budget) * 1024);
        if (budget == 256) {
            REQUIRE(usage.second > 0);
            const bool same = previous && previous->line == coordinate.line &&
                              previous->character == coordinate.character;
            CHECK(drafts == (same ? 0 : 1));
            if (same)
                CHECK(server.m_methodScope->localDiagnosticRenderingCounts().first >= 2);
        }
        else
            CHECK(usage.second == 0);
        previous = coordinate;
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Target display memo publishes atomically after cancellation and rejects oversized data",
          "[method-body-diagnostics][target-rendering-memo]") {
    const auto boundary = GENERATE("before target rendering memo publication",
                                   "before local diagnostic publication");
    Workspace workspace;
    workspace.write(".slang/server.json", certificationConfig());
    const std::string original =
        "class C;\nfunction int work(input int arg); return arg + 1; endfunction\n"
        "function int later(input int arg); if(arg) return 2; endfunction\nendclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("arg + 1"), 7, "arg + 2");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    REQUIRE(server.m_methodScope);
    auto priorUsage = server.m_methodScope->localDiagnosticRenderingCacheUsage();
    REQUIRE(priorUsage.second > 0);
    const auto before = diagnosticMap(server);
    text.replace(text.find("arg + 2"), 7, "arg + 33333");
    change(server, doc.m_uri, 3, text);
    bool reached = false;
    lsp::RequestContext ctx("cancel target memo", 900);
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == boundary) {
            reached = true;
            ctx.cancelForContentChange();
        }
    });
    CHECK_THROWS_AS(server.runBackgroundWork(ctx), lsp::RequestCancelled);
    REQUIRE(reached);
    CHECK(diagnosticMap(server) == before);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 2);
    CHECK(server.m_methodScope->localDiagnosticRenderingCacheUsage() == priorUsage);
    size_t rebuilt = 0;
    lsp::RequestContext retry("retry target memo", 901);
    retry.setCheckpointObserverForTesting([&](std::string_view phase) {
        rebuilt += phase == "before target rendering memo publication";
    });
    server.runBackgroundWork(retry);
    CHECK(rebuilt == 1);
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 3);
    checkNativeDiagnostics(workspace, server, text, true);
}

TEST_CASE("Target display memo abandons a payload that exceeds its charged allowance",
          "[method-body-diagnostics][target-rendering-memo]") {
    Workspace workspace;
    auto config = certificationConfig();
    config.insert(config.size() - 1, ",\"methodBodyRenderingCacheKiB\":4");
    workspace.write(".slang/server.json", config);
    const std::string original =
        "class C;\nfunction int work(input int arg); return arg + 1; endfunction\n"
        "function int " + std::string(900, 'w') + "(); endfunction\nendclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    auto text = original;
    text.replace(text.find("arg + 1"), 7, "arg + 2");
    change(server, doc.m_uri, 2, text);
    server.runBackgroundWork({});
    REQUIRE(server.m_methodScope);
    auto usage = server.m_methodScope->localDiagnosticRenderingCacheUsage();
    CHECK(usage.first > 0);
    CHECK(usage.first <= 4096);
    CHECK(usage.second == 0);
    text.replace(text.find("arg + 2"), 7, "arg + 3");
    change(server, doc.m_uri, 3, text);
    size_t drafts = 0;
    lsp::RequestContext ctx("oversized memo retry", 902);
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        drafts += phase == "before target rendering memo publication";
    });
    server.runBackgroundWork(ctx);
    CHECK(drafts == 0); // One bounded negative entry, no repeated allocation for this coordinate.
    CHECK(server.m_methodScope->localDiagnosticRenderingCacheUsage() == usage);
    checkNativeDiagnostics(workspace, server, text, true);
}


TEST_CASE("Fixed Native bit-vector type formatting is reused while edited local types stay fresh",
          "[method-body-diagnostics][target-rendering-memo][fixed-native-type-rendering]") {
    const bool alias = GENERATE(false, true);
    Workspace workspace;
    auto config = certificationConfig();
    config.insert(config.size() - 1, ",\"methodBodyRenderingCacheKiB\":0");
    workspace.write(".slang/server.json", config);
    const std::string original =
        "class C; typedef logic[31:0] NativeWord;\n"
        "function void earlier(); NativeWord bits; int word; bits=bits|word; endfunction\n"
        "function void work();\n" +
        std::string(alias ? "typedef logic[7:0] LocalWord; LocalWord bits;"
                          : "logic[7:0] bits;") +
        " int word; bits=bits|word;\nendfunction\n"
        "endclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    int version = 1;
    for (const auto* width : {"15:0", "31:0", "7:0"}) {
        auto text = original;
        text.replace(text.find("7:0"), 3, width);
        change(server, doc.m_uri, ++version, text);
        size_t cachedTypes = 0;
        lsp::RequestContext ctx("fixed type rendering", version);
        ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
            cachedTypes += phase == "retain fixed Native type rendering";
        });
        server.runBackgroundWork(ctx);
        if (version == 2)
            CHECK(cachedTypes > 0);
        REQUIRE(server.m_methodScope);
        const auto counts = server.m_methodScope->localDiagnosticRenderingCounts();
        CHECK(counts.first > 0);
        CHECK(counts.second > 0); // The changed work() Type* is never reused from Native.
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

TEST_CASE("Retained frozen enum displays reuse provider ownership while local enums stay fresh",
          "[method-body-diagnostics][target-rendering-memo][fixed-native-type-rendering]") {
    const bool frozen = GENERATE(false, true);
    Workspace workspace;
    auto config = certificationConfig();
    config.insert(config.size() - 1, ",\"methodBodyRenderingCacheKiB\":0");
    if (frozen)
        config.insert(config.size() - 1,
                      ",\"experimentalScopeSemanticCache\":true,"
                      "\"frozenSources\":{\"packages\":[\"provider\"],\"maxMiB\":256}");
    workspace.write(".slang/server.json", config);
    // A signed enum and unsigned vector produce the real bitwise mismatch;
    // equal-width unsigned operands are deliberately not diagnosed by Native.
    workspace.write("prelude.sv",
                    "package provider; typedef enum int {Mode0=0,Mode1=1} Mode; "
                    "endpackage\n");
    const std::string original =
        "class C;\n"
        "function void earlier(); logic[31:0] bits; provider::Mode value; "
        "bits=bits|value; endfunction\n"
        "function int work(input int arg);\n"
        "typedef enum logic[7:0] {Local0=0,Local1=1} LocalMode;\n"
        "logic[31:0] bits; LocalMode value; bits=bits|value;\n"
        "return arg + 1;\nendfunction\nendclass\n";
    workspace.write("item.svh", original);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", original);
    doc.doc->getAnalysis();
    if (frozen) {
        auto status = server.executeCommand(
            {.command = "slang.scopeLibraryStatus", .arguments = std::vector<lsp::LSPAny>{}});
        REQUIRE(status);
        REQUIRE(rfl::json::write(*status).find("\"prepared\":true") != std::string::npos);
    }
    int version = 1;
    for (const auto* width : {"15:0", "31:0", "7:0"}) {
        auto text = original;
        text.replace(text.find("7:0"), 3, width);
        change(server, doc.m_uri, ++version, text);
        size_t cachedEnums = 0;
        lsp::RequestContext ctx("frozen enum rendering", version);
        ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
            cachedEnums += phase == "retain frozen enum rendering";
        });
        server.runBackgroundWork(ctx);
        REQUIRE(server.m_methodScope);
        if (version == 2) {
            INFO(rfl::json::write(diagnosticMap(server)));
            CHECK((cachedEnums > 0) == frozen);
        }
        CHECK(server.m_methodScope->localDiagnosticRenderingCounts().second > 0);
        CHECK(diagnosticState(server).localDiagnosticUpdates == uint64_t(version - 1));
        checkNativeDiagnostics(workspace, server, text, true);
    }
    if (frozen) {
        workspace.write("prelude.sv",
                        "package provider; typedef enum logic signed[15:0] {Mode0=0,Mode1=1} Mode; "
                        "endpackage\n");
        auto rebuilt = server.executeCommand(
            {.command = "slang.rebuildScopeLibrary", .arguments = std::vector<lsp::LSPAny>{}});
        REQUIRE(rebuilt);
        auto reopened = server.openFile("item.svh", original);
        reopened.doc->getAnalysis();
        auto text = original;
        text.replace(text.find("arg + 1"), 7, "arg + 2");
        change(server, reopened.m_uri, 2, text);
        server.runBackgroundWork({});
        checkNativeDiagnostics(workspace, server, text, true);
    }
}

#if defined(__APPLE__)
TEST_CASE("Method directory leases preserve serial retry and final mutable dependency proof",
          "[directory-proof-integration][body-certification]") {
    const auto budget = GENERATE(0, 512);
    const auto mode = GENERATE("success", "serial", "cancel", "changed", "serial-changed");
    CAPTURE(budget, mode);
    Workspace workspace;
    std::filesystem::create_directory(workspace.root / "defs");
    std::string prelude = "package prelude; int value;\n";
    for (int i = 0; i < 64; ++i) {
        const auto name = "stub" + std::to_string(i) + ".svh";
        workspace.write(name, "// mutable dependency\n");
        prelude += "`include \"" + name + "\"\n";
    }
    prelude += "endpackage\n";
    workspace.write("defs/prelude.sv", prelude);
    workspace.write("design.f", "+incdir+.\ndefs/prelude.sv\nowner.sv\ntail.sv\n");
    auto config = certificationConfig();
    config.insert(config.size() - 1, ",\"methodBodyDirectoryProofCacheKiB\":" +
                                      std::to_string(budget));
    workspace.write(".slang/server.json", config);
    CertificationServer server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = edited();
    change(server, doc.m_uri, 2, text);
    doc.m_text = text;
    REQUIRE(server.m_methodScope);
    const auto position = doc.getPosition(text.rfind("local_0") + 3);
    const std::string scenario = mode;
    bool injected = false, retried = false, changed = false;
    size_t scans = 0;
    lsp::RequestContext ctx("textDocument/hover", 860);
    ctx.setCheckpointObserverForTesting([&](std::string_view phase) {
        if (phase == "before missing input directory scan") {
            ++scans;
            if (scenario.starts_with("serial") && !injected) {
                injected = true;
                throw server::DirectoryProofUnavailable();
            }
        }
        if (phase == "before serial directory proof retry") {
            retried = true;
            if (scenario == "serial-changed") {
                workspace.write("defs/stub0.svh", "// appeared before complete retry\n");
                changed = true;
            }
        }
        if (phase == "method input validation") {
            if (scenario == "cancel") ctx.cancel();
            if (scenario == "changed" && !changed) {
                workspace.write("defs/stub0.svh", "// appeared during proof\n");
                changed = true;
            }
        }
    });
    auto query = [&](const auto& request) {
        return server.m_methodScope->hover(doc.m_uri, position, server.m_config, request);
    };
    if (scenario == "cancel") {
        CHECK_THROWS_AS(query(ctx), lsp::RequestCancelled);
        REQUIRE(query(lsp::RequestContext("retry", 861)));
    }
    else if (scenario == "changed" || (scenario == "serial-changed" && budget)) {
        CHECK_FALSE(query(ctx));
        CHECK(changed);
        CHECK(server.m_methodScope->observedInputChange());
    }
    else {
        REQUIRE(query(ctx));
    }
    CHECK((scans > 0) == (budget != 0));
    CHECK(retried == (budget != 0 && scenario.starts_with("serial")));
    CHECK(injected == retried);
    if (!changed) {
        server.runBackgroundWork({});
        checkNativeDiagnostics(workspace, server, text, true, position);
    }
}
#endif
