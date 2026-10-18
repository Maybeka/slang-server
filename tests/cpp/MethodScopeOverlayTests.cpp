// SPDX-License-Identifier: MIT
#include "utils/ServerHarness.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <rfl/json/write.hpp>

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
        root = std::filesystem::temp_directory_path() /
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
