// SPDX-License-Identifier: MIT
#include "completions/MemberCompletions.h"
#include "util/Converters.h"
#include "utils/ServerHarness.h"
#include <atomic>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <rfl/json/write.hpp>

#include "slang/ast/symbols/ClassSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/util/ScopeGuard.h"

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
        root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
               ("slang-method-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(serial++));
        std::filesystem::create_directories(root / ".slang");
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
            CHECK(document.doc->getText() == edited() + '\0');
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
    CHECK(document.doc->getText() == expected + '\0');
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
            CHECK(document.doc->getText() == text + '\0');
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
            CHECK(document.doc->getText() == text + '\0');
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
            CHECK(document.doc->getText() == text + '\0');
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
    CHECK(document.doc->getText() == header + '\0');
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
    CHECK(document.doc->getText() == text + '\0');
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
    CHECK(document.doc->getText() == edited() + '\0');
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
    CHECK(document.doc->getText() == edited() + '\0');
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
void bodyConfig(Workspace& workspace, bool enabled = true, bool uvm = false) {
    workspace.write(".slang/server.json",
                    std::string("{\"indexingThreads\":1,\"flags\":\"--single-unit --threads 1\",") +
                        "\"compilationFilelist\":\"design.f\",\"experimentalMethodBodyQueries\":" +
                        (enabled ? "true" : "false") +
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
    Workspace workspace;
    bodyConfig(workspace);
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
    for (const auto& replacement : {std::string("local_1 + (charl"), std::string("marker + charl"),
                                    std::string("earlier(charl)"), std::string("`VALUE + charl")}) {
        DYNAMIC_SECTION(replacement) {
            Workspace workspace;
            bodyConfig(workspace);
            BodyTestServer server(workspace.params());
            auto doc = server.openFile("item.svh", header);
            doc.doc->getAnalysis();
            auto text = header;
            text.replace(text.find("local_1 + charl"), 15, replacement);
            change(server, doc.m_uri, 2, text);
            doc.m_text = text;
            CHECK(state(server).active);
            CHECK(doc.doc->getText() == text + '\0');
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
    bodyConfig(workspace);
    BodyTestServer server(workspace.params());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto text = header;
    for (size_t at = 0; (at = text.find("local_0", at)) != std::string::npos; at += 7)
        text.replace(at, 7, "value_0");
    change(server, doc.m_uri, 2, text);
    CHECK(state(server).active);
    CHECK(doc.doc->getText() == text + '\0');
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
            CHECK(doc.doc->getText() == text + '\0');
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
            CHECK(doc.doc->getText() == text + '\0');
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
            CHECK(doc.doc->getText() == text + '\0');
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
    REQUIRE(doc.doc->getText() == expected + '\0');
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
            CHECK(doc.doc->getText() == text + '\0');
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
    bodyConfig(workspace);
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
            CHECK(doc.doc->getText() == latest + '\0');
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
void checkNativeDiagnostics(Workspace& workspace, ServerHarness& server, const std::string& text) {
    auto actual = diagnosticMap(server);
    bodyConfig(workspace, false);
    auto restore = slang::ScopeGuard(
        [&] { workspace.write(".slang/server.json", localDiagnosticConfig); });
    BodyTestServer native(workspace.params());
    native.setBuildFile((workspace.root / "design.f").string());
    auto oracle = native.openFile("item.svh", text);
    CHECK(actual == diagnosticMap(native));
}
} // namespace

TEST_CASE("Local diagnostics reject changed outside usage and rebase on a native result",
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
    CHECK(first.localDiagnosticUpdates == 0);
    CHECK(first.localDiagnosticDecline == "method changes external reference usage");
    checkNativeDiagnostics(workspace, server, text);
    text.replace(text.find("marker + charl"), 14, "marker + charl + 10");
    change(server, doc.m_uri, 3, text);
    server.runBackgroundWork({});
    auto second = diagnosticState(server);
    INFO(second.localDiagnosticDecline);
    CHECK(second.localDiagnosticUpdates == 1);
    CHECK_FALSE(server.m_driver->comp->containsCurrentSource(workspace.root / "item.svh"));
    CHECK(server.client.diagnosticVersions.at(doc.m_uri) == 3);
    checkNativeDiagnostics(workspace, server, text);
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
    CHECK(second.localDiagnosticUpdates == 1);
    CHECK(second.localDiagnosticDecline == "task timing changes require caller analysis");
    checkNativeDiagnostics(workspace, server, text);
}

TEST_CASE("Local diagnostics inherit warning pragmas and current line macro values",
          "[method-body-diagnostics]") {
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
            workspace.write(".slang/server.json", localDiagnosticConfig);
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
                      "line shift affects diagnostics in later macro bodies");
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
            CHECK(result.localDiagnosticUpdates == 0);
            CHECK_FALSE(result.localDiagnosticDecline.empty());
            checkNativeDiagnostics(workspace, server, text);
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
            CHECK(result.localDiagnosticUpdates == 0);
            CHECK_FALSE(result.localDiagnosticDecline.empty());
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
    CHECK(result.localDiagnosticUpdates == 0);
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

TEST_CASE("Standalone method renewal retains the hard source-storage retirement bound",
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
    };
    Harness server(workspace.params());
    server.setBuildFile((workspace.root / "design.f").string());
    auto doc = server.openFile("item.svh", header);
    doc.doc->getAnalysis();
    auto epoch = server.m_driver->completions.getCompletionEpoch();
    server.m_contextEpoch = epoch;
    server.m_contextBudget = 1; // Above the hard high-water mark with no library experiments.
    auto text = edited();
    change(server, doc.m_uri, 2, text);
    CHECK((server.m_driver->completions.getCompletionEpoch() != epoch) == enabled);
    CHECK(server.m_driver->getDocument(doc.m_uri)->getText() == text + '\0');
    server.m_driver->getDocument(doc.m_uri)->getAnalysis();
    server.runBackgroundWork({});
    checkNativeDiagnostics(workspace, server, text);
}
