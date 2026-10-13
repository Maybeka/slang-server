// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"
#include <fstream>

namespace {
DocumentHandle openHeader(ServerHarness& server, std::string filename = "item.svh") {
    std::ifstream file(findSlangRoot() / "tests/data/included_package/item.svh");
    REQUIRE(file.good());
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // Unlike openFile(filename), this overload allows a legitimately empty outline
    // before the header's owning package has been opened.
    return server.openFile(std::move(filename), std::move(text));
}
} // namespace

TEST_CASE("Included package header uses owner scope and unsaved text", "[package-header]") {
    ServerHarness server("included_package");
    auto owner = server.openFile("owner.sv");
    auto header = openHeader(server);
    REQUIRE(server.m_driver->comp == nullptr);

    auto checkDefinition = [&]() {
        auto definitions = header.before("value_t value").getDefinitions();
        REQUIRE(definitions.size() == 1);
        CHECK(definitions.front().targetUri == owner.doc->getURI());
        CHECK(definitions.front().targetSelectionRange.start ==
              owner.before("value_t;").getPosition());
        CHECK(header.getHoverAt(header.before("value_t value").m_offset).has_value());
    };
    checkDefinition();
    CHECK(header.getDiagnostics().empty());

    auto analysis = header.doc->getAnalysis();
    auto trees = analysis->getCompilation()->getSyntaxTrees();
    REQUIRE(trees.size() == 1);
    CHECK(server.sourceManager().getFullPath(trees.front()->getSourceBufferIds().front()) ==
          std::filesystem::path(owner.doc->getPath()));

    auto original = header.getText();
    header.replaceAll("\n\n" + original);
    header.publishChanges();
    checkDefinition();
    auto member = header.after("return ").getDefinitions();
    REQUIRE(member.size() == 1);
    CHECK(member.front().targetUri == header.doc->getURI());
    CHECK(member.front().targetSelectionRange.start == header.before("value;").getPosition());

    auto broken = header.getText();
    broken.replace(broken.find("value_t value"), 7, "missing_t");
    header.replaceAll(broken);
    header.publishChanges();
    auto diagnostics = header.getDiagnostics();
    CHECK(std::ranges::any_of(diagnostics, [](const auto& diag) {
        return diag.message.find("missing_t") != std::string::npos;
    }));
    header.replaceAll(original);
    header.publishChanges();
    CHECK(header.getDiagnostics().empty());
    checkDefinition();

    auto completions = header.after("return ").getResolvedCompletions();
    auto value = std::ranges::find_if(completions,
                                      [](const auto& item) { return item.label == "value"; });
    REQUIRE(value != completions.end());
    CHECK(value->documentation.has_value());

    auto malformed = original;
    malformed.erase(malformed.find("value_t value;") + std::string("value_t value").size(), 1);
    header.replaceAll(malformed);
    header.publishChanges();
    auto parseDiags = header.getDiagnostics();
    CHECK(std::ranges::any_of(parseDiags, [](const auto& diag) {
        return diag.message.find("expected ';'") != std::string::npos;
    }));
    header.replaceAll(original);
    header.publishChanges();

    auto symbols = header.getSymbolTree();
    REQUIRE(symbols.size() == 1);
    CHECK(symbols.front().name == "item");
    CHECK(symbols.front().kind == lsp::SymbolKind::Class);
}

TEST_CASE("Included package header adopts a later opened owner", "[package-header]") {
    ServerHarness server("included_package");
    auto header = openHeader(server, "late_item.svh");
    CHECK(header.before("value_t value").getDefinitions().empty());
    auto owner = server.openFile("late_owner.sv",
                                 "package p; typedef int value_t;\n`define OWNER_CONTEXT\n"
                                 "`include \"late_item.svh\"\nendpackage\n");
    auto definitions = header.before("value_t value").getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == owner.doc->getURI());

    auto ownerText = owner.getText();
    auto headerText = header.getText();
    auto broken = headerText;
    broken.replace(broken.find("value_t value"), 7, "missing_t");
    header.replaceAll(broken);
    header.publishChanges();
    REQUIRE(!header.getDiagnostics().empty());

    owner.replaceAll("package p; typedef int value_t; endpackage\n");
    owner.publishChanges();
    CHECK(header.getDiagnostics().empty());
    CHECK(header.before("missing_t value").getDefinitions().empty());

    owner.replaceAll(ownerText);
    owner.publishChanges();
    CHECK(!header.getDiagnostics().empty());
    header.replaceAll(headerText);
    header.publishChanges();
    CHECK(header.getDiagnostics().empty());
}

TEST_CASE("Included package header retains required package dependencies", "[package-header]") {
    ServerHarness server("included_package");
    auto owner = server.openFile("owner.sv",
                                 "package p; typedef required_pkg::base_t value_t;\n"
                                 "`define OWNER_CONTEXT\n`include \"item.svh\"\nendpackage\n");
    auto header = openHeader(server);
    REQUIRE(server.m_driver->comp == nullptr);
    auto trees = header.doc->getAnalysis()->getCompilation()->getSyntaxTrees();
    REQUIRE(trees.size() == 2);
    for (const auto& tree : trees) {
        auto filename =
            server.sourceManager().getFullPath(tree->getSourceBufferIds().front()).filename();
        CHECK((filename == "owner.sv" || filename == "required_pkg.sv"));
    }
    CHECK(header.getDiagnostics().empty());
    REQUIRE(header.before("value_t value").getDefinitions().size() == 1);
}

TEST_CASE("Nested included package header uses the outer package", "[package-header]") {
    ServerHarness server("included_package");
    auto owner = server.openFile("owner.sv",
                                 "package p; typedef int value_t;\n"
                                 "`define OWNER_CONTEXT\n`include \"wrapper.svh\"\nendpackage\n");
    auto wrapper = server.openFile("wrapper.svh", "`include \"item.svh\"\n");
    auto header = openHeader(server);
    auto definitions = header.before("value_t value").getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == owner.doc->getURI());
    CHECK(header.getDiagnostics().empty());
}

TEST_CASE("Included package header honors diagnostic pragmas", "[package-header]") {
    ServerHarness server("included_package");
    auto owner = server.openFile("owner.sv");
    auto header = openHeader(server);
    auto original = header.getText();
    header.replaceAll(";\n" + original);
    header.publishChanges();
    auto diagnostics = header.getDiagnostics();
    CHECK(std::ranges::any_of(diagnostics, [](const auto& diag) {
        return diag.message.find("extra ';'") != std::string::npos;
    }));
    header.replaceAll("`pragma diagnostic ignore=\"-Wempty-member\"\n;\n" + original);
    header.publishChanges();
    CHECK(header.getDiagnostics().empty());
}

TEST_CASE("Global RTL include beside a package retains standalone analysis", "[package-header]") {
    ServerHarness server("included_package");
    auto owner = server.openFile("owner.sv", "package p; typedef int value_t; endpackage\n"
                                             "`include \"unrelated.sv\"\n");
    auto rtl = server.openFile("unrelated.sv");
    auto trees = rtl.doc->getAnalysis()->getCompilation()->getSyntaxTrees();
    REQUIRE(trees.size() == 1);
    CHECK(trees.front() == rtl.doc->getSyntaxTree());
    CHECK(server.m_driver->comp == nullptr);
}

TEST_CASE("Cancelled owner changes preserve pending header diagnostics", "[package-header]") {
    ServerHarness server("included_package");
    auto owner = server.openFile("owner.sv");
    auto header = openHeader(server);
    auto broken = header.getText();
    broken.replace(broken.find("value_t value"), 7, "missing_t");
    header.replaceAll(broken);
    header.publishChanges();
    REQUIRE(!header.getDiagnostics().empty());

    std::string replacement = "package p; typedef int value_t; endpackage\n";
    lsp::RequestContext cancelled("textDocument/didChange", std::nullopt);
    cancelled.cancel();
    CHECK_THROWS_AS(
        server.onDocDidChange(
            lsp::DidChangeTextDocumentParams{
                .textDocument = lsp::VersionedTextDocumentIdentifier{.version = 2,
                                                                     .uri = owner.doc->getURI()},
                .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{replacement}}},
            cancelled),
        lsp::RequestCancelled);
    CHECK(!header.getDiagnostics().empty());

    server.onDocDidChange(lsp::DidChangeTextDocumentParams{
        .textDocument = lsp::VersionedTextDocumentIdentifier{.version = 3,
                                                             .uri = owner.doc->getURI()},
        .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{replacement + "\n"}}});
    CHECK(header.getDiagnostics().empty());
}

TEST_CASE("Opening a package clears stale standalone header diagnostics", "[package-header]") {
    ServerHarness server("included_package");
    auto header = openHeader(server, "late_item.svh");
    auto text = header.getText();
    text.erase(text.find("`ifdef OWNER_CONTEXT\n"), std::string("`ifdef OWNER_CONTEXT\n").size());
    text.erase(text.find("`endif\n"), std::string("`endif\n").size());
    header.replaceAll(text);
    header.publishChanges();
    REQUIRE(!header.getDiagnostics().empty());

    auto owner = server.openFile("late_owner.sv",
                                 "package p; typedef int value_t;\n`define OWNER_CONTEXT\n"
                                 "`include \"late_item.svh\"\nendpackage\n");
    CHECK(header.getDiagnostics().empty());
    auto definitions = header.before("value_t value").getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == owner.doc->getURI());
}

TEST_CASE("Cancelled diagnostics do not leak through another document publication",
          "[package-header][scheduling]") {
    ServerHarness server("included_package");
    auto owner = server.openFile("owner.sv");
    auto header = openHeader(server);
    auto uri = header.doc->getURI();
    const auto original = header.getText();
    auto initial = original;
    initial.replace(initial.find("value_t value"), 7, "first_missing_t");
    server.onDocDidChange(lsp::DidChangeTextDocumentParams{
        .textDocument = {.version = 10, .uri = uri},
        .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{initial}}});
    REQUIRE(!server.client.getDiagnostics(uri).empty());
    REQUIRE(server.client.diagnosticVersions.at(uri) == 10);

    auto replacement = original;
    SECTION("cancel during parse diagnostic reporting") {
        replacement.erase(replacement.find("value_t value;") + 13, 1);
    }
    SECTION("cancel during semantic diagnostic reporting") {
        replacement.replace(replacement.find("value_t value"), 7, "second_missing_t");
    }
    lsp::RequestContext ctx("textDocument/didChange", std::nullopt);
    class CancelOnReport : public slang::DiagnosticClient {
    public:
        explicit CancelOnReport(lsp::RequestContext ctx) : ctx(std::move(ctx)) {}
        void report(const slang::ReportedDiagnostic&) override { ctx.cancel(); }
        lsp::RequestContext ctx;
    };
    auto cancel = std::make_shared<CancelOnReport>(ctx);
    server.m_driver->diagEngine.addClient(cancel);
    server.client.diagnosticPublications.clear();
    CHECK_THROWS_AS(
        server.onDocDidChange(
            lsp::DidChangeTextDocumentParams{
                .textDocument = {.version = 11, .uri = uri},
                .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{replacement}}},
            ctx),
        lsp::RequestCancelled);
    server.m_driver->diagEngine.removeClient(cancel);
    REQUIRE(ctx.isCancelled());
    CHECK(server.client.diagnosticPublications.empty());
    CHECK(header.doc->getVersion() == 11);

    server.openFile("unrelated_schedule.sv", "module unrelated_schedule; endmodule\n");
    CHECK(std::ranges::find(server.client.diagnosticPublications, uri) ==
          server.client.diagnosticPublications.end());
    CHECK(server.client.diagnosticVersions.at(uri) == 10);
    CHECK(std::ranges::any_of(server.client.getDiagnostics(uri), [](const auto& diag) {
        return diag.message.find("first_missing_t") != std::string::npos;
    }));

    server.onDocDidChange(lsp::DidChangeTextDocumentParams{
        .textDocument = {.version = 12, .uri = uri},
        .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{original}}});
    CHECK(server.client.getDiagnostics(uri).empty());
    CHECK(server.client.diagnosticVersions.at(uri) == 12);
}

TEST_CASE("Diagnostic versions follow open edit close and reopen", "[scheduling]") {
    ServerHarness server;
    auto uri = URI::fromFile(std::filesystem::current_path() / "versioned_schedule.sv");
    const std::string text = "class versioned_schedule; int value; endclass\n";
    auto open = [&](int version) {
        server.onDocDidOpen(lsp::DidOpenTextDocumentParams{
            .textDocument = {
                .uri = uri, .languageId = "systemverilog", .version = version, .text = text}});
    };
    open(7);
    CHECK(server.client.diagnosticVersions.at(uri) == 7);
    lsp::RequestContext cancelled("textDocument/didChange", std::nullopt);
    cancelled.cancel();
    CHECK_THROWS_AS(
        server.onDocDidChange(
            lsp::DidChangeTextDocumentParams{
                .textDocument = {.version = 8, .uri = uri},
                .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{"\n" + text}}},
            cancelled),
        lsp::RequestCancelled);
    CHECK(server.getDoc(uri)->getVersion() == 8);
    CHECK(server.client.diagnosticVersions.at(uri) == 7);
    server.onDocDidChange(lsp::DidChangeTextDocumentParams{
        .textDocument = {.version = 9, .uri = uri},
        .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{text}}});
    CHECK(server.client.diagnosticVersions.at(uri) == 9);
    server.onDocDidClose(lsp::DidCloseTextDocumentParams{.textDocument = {.uri = uri}});
    open(1);
    CHECK(server.client.diagnosticVersions.at(uri) == 1);
}
