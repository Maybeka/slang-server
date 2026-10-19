// SPDX-License-Identifier: MIT
#include "diagnostics/DiagnosticProcess.h"
#include "diagnostics/DiagnosticSnapshotCodec.h"
#include "diagnostics/DiagnosticWorker.h"
#include "utils/ServerHarness.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <rfl/json.hpp>
#include <thread>

#include "slang/driver/Driver.h"
#include "slang/util/ScopeGuard.h"
#ifndef _WIN32
#    include <signal.h>
#    include <unistd.h>
#endif

namespace {
std::vector<std::string> diagnosticObjects(const std::vector<lsp::Diagnostic>& diagnostics) {
    std::vector<std::string> result;
    for (const auto& diagnostic : diagnostics)
        result.push_back(rfl::json::write<rfl::UnderlyingEnums>(diagnostic));
    std::sort(result.begin(), result.end());
    return result;
}
void changeWithoutDiagnostics(ServerHarness& server, const URI& uri, int version,
                              const std::string& text) {
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = version, .uri = uri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
}
} // namespace

TEST_CASE("SDK driver option identities survive server snapshot capture") {
    slang::driver::Driver driver;
    driver.addStandardArgs();
    REQUIRE(driver.parseCommandLine("slang --single-unit --threads 1"));
    REQUIRE(driver.processOptions(false));
    driver.options.captureUvmPrefix = true;
    auto options = driver.createOptionBag();
    options.set(driver.getAnalysisOptions());
    REQUIRE(options.size() == 6);
    auto captured = server::diagnostic_snapshot::captureOptions(options);
    REQUIRE(captured.source);
    CHECK(captured.source->singleUnit);
    CHECK(captured.source->numThreads == 1);
    CHECK(captured.lexer);
    CHECK(captured.parser);
    REQUIRE(captured.preprocessor);
    CHECK(captured.preprocessor->captureUvmPrefix);
    CHECK(captured.compilation);
    CHECK(captured.analysis);
    server::diagnostic_snapshot::ReconstructedOptions restored(captured);
    CHECK(rfl::json::write(server::diagnostic_snapshot::captureOptions(restored.bag())) ==
          rfl::json::write(captured));
    struct UnsupportedOption {
        int value;
    };
    options.set(UnsupportedOption{1});
    CHECK_THROWS_AS(server::diagnostic_snapshot::captureOptions(options),
                    server::diagnostic_snapshot::CodecError);
}

TEST_CASE("Private diagnostic snapshots preserve complete errors and empty repairs") {
    ServerHarness server;
    auto header = server.openFile("private_item.svh", "class item; int value; endclass\n");
    auto owner = server.openFile("private_owner.sv",
                                 "package p;\n`include \"private_item.svh\"\nendpackage\n");
    std::string text = "class item; missing_private_t value; endclass\n";
    SECTION("semantic error") {
    }
    SECTION("included parse error occurs once") {
        text = "class item; int value endclass\n";
    }
    changeWithoutDiagnostics(server, header.m_uri, 1, text);
    auto snapshot = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(snapshot);
    auto output = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(snapshot->metadata, *snapshot->input));
    server.m_driver->runPendingDiagnostics();
    for (const auto& target : snapshot->metadata.targets) {
        auto found = std::find_if(output.documents.begin(), output.documents.end(),
                                  [&](const auto& item) { return item.uri == target.uri; });
        REQUIRE(found != output.documents.end());
        CHECK(diagnosticObjects(found->diagnostics) ==
              diagnosticObjects(server.client.getDiagnostics(target.uri)));
    }
    changeWithoutDiagnostics(server, header.m_uri, 2, "class item; int repaired; endclass\n");
    auto repair = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(repair);
    auto clean = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(repair->metadata, *repair->input));
    REQUIRE(clean.documents.size() >= repair->metadata.targets.size());
    server.m_driver->commitDiagnosticSnapshot(*repair, std::move(clean), {});
    CHECK(server.client.getDiagnostics(header.m_uri).empty());
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
}

TEST_CASE("Private diagnostic snapshot owns bytes and rejects stale publication") {
    ServerHarness server;
    auto header = server.openFile("private_bytes.svh", "class item; int value; endclass\n");
    auto owner = server.openFile("private_bytes_owner.sv",
                                 "package p;\n`include \"private_bytes.svh\"\nendpackage\n");
    changeWithoutDiagnostics(server, header.m_uri, 1,
                             "class item; old_missing_type value; endclass\n");
    auto snapshot = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(snapshot);
    const auto encoded = server::encodeDiagnosticJob(snapshot->metadata, *snapshot->input);
    changeWithoutDiagnostics(server, header.m_uri, 2, "class item; int newest_value; endclass\n");
    auto old = server::executeDiagnosticJob(encoded);
    bool sawOldError = false;
    for (const auto& document : old.documents)
        for (const auto& diagnostic : document.diagnostics)
            sawOldError |= diagnostic.message.find("old_missing_type") != std::string::npos;
    CHECK(sawOldError);
    CHECK_FALSE(server.m_driver->isDiagnosticSnapshotCurrent(*snapshot));
    CHECK_THROWS(server.m_driver->commitDiagnosticSnapshot(*snapshot, std::move(old), {}));
    CHECK(server.m_driver->hasPendingDiagnostics());
    auto latest = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(latest);
    auto current = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(latest->metadata, *latest->input));
    server.m_driver->commitDiagnosticSnapshot(*latest, std::move(current), {});
    CHECK(server.client.getDiagnostics(header.m_uri).empty());
}

#if defined(__linux__) || defined(__APPLE__)
TEST_CASE("Blocked private diagnostic child does not block newest foreground queries") {
    class IsolatedHarness : public ServerHarness {
    public:
        using SlangServer::hasBackgroundWork;
        using SlangServer::onForegroundComplete;
        using SlangServer::onProtocolStopped;
        using SlangServer::runBackgroundWork;
    };
    auto gate = std::filesystem::temp_directory_path() /
                ("slang-private-gate-" + std::to_string(getpid()));
    const auto ready = gate.string() + ".ready";
    const auto release = gate.string() + ".release";
    std::filesystem::remove(ready);
    std::filesystem::remove(release);
    setenv("SLANG_DIAGNOSTIC_TEST_GATE", gate.c_str(), 1);
    auto cleanup = slang::ScopeGuard([&] {
        unsetenv("SLANG_DIAGNOSTIC_TEST_GATE");
        std::filesystem::remove(ready);
        std::filesystem::remove(release);
    });
    IsolatedHarness server;
    server.enableIsolatedDiagnostics(isolated_diagnostics::currentDiagnosticExecutable());
    auto header = server.openFile("held_item.svh", "class item; int value; endclass\n");
    auto owner = server.openFile("held_owner.sv",
                                 "package p;\n`include \"held_item.svh\"\nendpackage\n");
    changeWithoutDiagnostics(server, header.m_uri, 1,
                             "class item; obsolete_type value; endclass\n");
    server.runBackgroundWork({});
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!std::filesystem::exists(ready) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(std::filesystem::exists(ready));
    int childPid = 0;
    std::ifstream(ready) >> childPid;
    REQUIRE(childPid > 0);
    REQUIRE_FALSE(std::filesystem::exists(release));
    CHECK_FALSE(header.doc->hasAnalysis());
    const std::string current = "class item; int newest_value; function int get(); return "
                                "newest_value; endfunction endclass\n";
    changeWithoutDiagnostics(server, header.m_uri, 2, current);
    // Queries execute while the original child is still held. No timing threshold
    // or diagnostic-drain helper is involved in this freshness assertion.
    const lsp::Position use{0, lsp::uint(current.rfind("newest_value"))};
    auto hover = server.getDocHover({.textDocument = {.uri = header.m_uri}, .position = use});
    REQUIRE(hover);
    CHECK(rfl::json::write(*hover).find("newest_value") != std::string::npos);
    auto completion = server.getDocCompletion(
        {.textDocument = {.uri = header.m_uri}, .position = use});
    CHECK(rfl::json::write<rfl::UnderlyingEnums>(completion).find("newest_value") !=
          std::string::npos);
    auto definition = server.getDocDefinition(
        {.textDocument = {.uri = header.m_uri}, .position = use});
    CHECK(rfl::json::write<rfl::UnderlyingEnums>(definition).find("character") !=
          std::string::npos);
    CHECK_FALSE(std::filesystem::exists(release));
    CHECK(kill(childPid, 0) == 0);
    server.onForegroundComplete(); // Cancels only the obsolete owned child.
    std::ofstream(release) << "release";
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (server.m_driver->hasPendingDiagnostics() &&
           std::chrono::steady_clock::now() < deadline) {
        if (server.hasBackgroundWork())
            server.runBackgroundWork({});
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(server.m_driver->hasPendingDiagnostics());
    CHECK(server.client.getDiagnostics(header.m_uri).empty());
    CHECK(server.client.diagnosticVersions.at(header.m_uri) == 2);
    server.onProtocolStopped();
    CHECK(kill(childPid, 0) == -1);
}
#endif

TEST_CASE("Snapshot discovery parses newly edited owner before earlier-sorted header") {
    ServerHarness server;
    auto header = server.openFile("a_adopted.svh", "class item; owner_type value; endclass\n");
    auto owner = server.openFile("z_adopting.sv",
                                 "package adoption; typedef int owner_type; endpackage\n");
    changeWithoutDiagnostics(
        server, owner.m_uri, 1,
        "package adoption; typedef int owner_type;\n`include \"a_adopted.svh\"\nendpackage\n");
    auto snapshot = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(snapshot);
    auto output = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(snapshot->metadata, *snapshot->input));
    for (const auto& document : output.documents) {
        if (document.uri == header.m_uri)
            CHECK(document.diagnostics.empty());
    }
    server.m_driver->commitDiagnosticSnapshot(*snapshot, std::move(output), {});
    CHECK(server.client.getDiagnostics(header.m_uri).empty());
}

TEST_CASE("Snapshot close reopen and incomplete output cannot overwrite diagnostics") {
    ServerHarness server;
    auto doc = server.openFile("private_close.sv", "package p; int value; endpackage\n");
    changeWithoutDiagnostics(server, doc.m_uri, 1, "package p; missing_type value; endpackage\n");
    auto snapshot = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(snapshot);
    CHECK_THROWS(server.m_driver->commitDiagnosticSnapshot(*snapshot, {}, {}));
    CHECK(server.m_driver->hasPendingDiagnostics());
    server.m_driver->closeDocument(doc.m_uri);
    CHECK_FALSE(server.m_driver->isDiagnosticSnapshotCurrent(*snapshot));
    server.m_driver->openDocument(doc.m_uri, "package p; int newest; endpackage\n", 1);
    CHECK_FALSE(server.m_driver->isDiagnosticSnapshotCurrent(*snapshot));
}

TEST_CASE("Private snapshots preserve unsaved preincludes and discard-close ownership") {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto root = std::filesystem::temp_directory_path() /
                ("slang-private-preinclude-" + std::to_string(stamp));
    REQUIRE(std::filesystem::create_directory(root));
    auto cleanup = slang::ScopeGuard([&] { std::filesystem::remove_all(root); });
    std::filesystem::create_directory(root / ".slang");
    std::ofstream(root / ".slang/server.json") << R"({"explorePreincludeFiles":["common.svh"]})";
    std::ofstream(root / "common.svh") << "`define FIELD_TYPE int\n";
    std::ofstream(root / "owner.sv") << "package p;\n`include \"item.svh\"\nendpackage\n";
    const std::string text = "class item; `FIELD_TYPE value; endclass\n";
    std::ofstream(root / "item.svh") << text;
    ServerHarness server(lsp::InitializeParams{
        .workspaceFolders = {
            {lsp::WorkspaceFolder{.uri = URI::fromFile(root), .name = "snapshot"}}}});
    auto header = server.openFile((root / "item.svh").string(), text);
    auto macro = server.openFile((root / "common.svh").string(), "`define FIELD_TYPE int\n");
    changeWithoutDiagnostics(server, macro.m_uri, 1, "`define FIELD_TYPE snapshot_missing_type\n");
    changeWithoutDiagnostics(server, header.m_uri, 2, text + "// unsaved consumer\n");
    auto bad = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(bad);
    auto broken = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(bad->metadata, *bad->input));
    bool foundError = false;
    for (const auto& document : broken.documents)
        for (const auto& diagnostic : document.diagnostics)
            foundError |= document.uri == header.m_uri &&
                          diagnostic.message.find("snapshot_missing_type") != std::string::npos;
    CHECK(foundError);
    server.m_driver->commitDiagnosticSnapshot(*bad, std::move(broken), {});
    server.onDocDidClose({.textDocument = {.uri = macro.m_uri}});
    CHECK_FALSE(server.m_driver->isDiagnosticSnapshotCurrent(*bad));
    CHECK(server.getDoc(header.m_uri)->getText().find("unsaved consumer") != std::string::npos);
    changeWithoutDiagnostics(server, header.m_uri, 3, text + "// unsaved consumer\n");
    auto repair = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(repair);
    auto clean = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(repair->metadata, *repair->input));
    server.m_driver->commitDiagnosticSnapshot(*repair, std::move(clean), {});
    CHECK(server.client.getDiagnostics(header.m_uri).empty());
    CHECK(server.client.diagnosticVersions.at(header.m_uri) == 3);
}

#ifndef _WIN32
TEST_CASE("Diagnostic publications retain opened URI aliases through close and reopen",
          "[diagnostic-uri-alias]") {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto root = fs::temp_directory_path() / ("slang-diagnostic-alias-" + std::to_string(stamp));
    fs::create_directories(root / "physical");
    auto cleanup = slang::ScopeGuard([&] { fs::remove_all(root); });
    fs::create_directory_symlink(fs::canonical(root / "physical"), root / "opened");
    const auto opened = root / "opened";
    const auto headerUri = URI::fromFile(opened / "item.svh");
    const auto physicalUri = URI::fromFile(fs::canonical(root / "physical") / "item.svh");
    const std::string clean = "class item; int value; endclass\n";
    const std::string broken = "class item; alias_missing_t value; endclass\n";
    std::ofstream(opened / "item.svh") << clean;
    std::ofstream(opened / "owner.sv") << "package p;\n`include \"item.svh\"\nendpackage\n";
    bool isolated = false;
    SECTION("sequential rendering") {
    }
    SECTION("private snapshot rendering") {
        isolated = true;
    }
    ServerHarness server(lsp::InitializeParams{
        .workspaceFolders = {
            {lsp::WorkspaceFolder{.uri = URI::fromFile(opened), .name = "alias"}}}});
    server.openFile((opened / "owner.sv").string());
    server.openFile((opened / "item.svh").string(), clean);
    auto publish = [&](int version, const std::string& text) {
        server.client.diagnosticPublications.clear();
        changeWithoutDiagnostics(server, headerUri, version, text);
        if (isolated) {
            auto snapshot = server.m_driver->captureDiagnosticSnapshot({});
            REQUIRE(snapshot);
            auto output = server::executeDiagnosticJob(
                server::encodeDiagnosticJob(snapshot->metadata, *snapshot->input));
            for (const auto& document : output.documents)
                CHECK(document.uri != physicalUri);
            server.m_driver->commitDiagnosticSnapshot(*snapshot, std::move(output), {});
        }
        else {
            server.m_driver->runPendingDiagnostics();
        }
        CHECK(server.client.diagnosticVersions.at(headerUri) == version);
        CHECK(std::ranges::count(server.client.diagnosticPublications, headerUri) == 1);
        CHECK(std::ranges::count(server.client.diagnosticPublications, physicalUri) == 0);
    };
    auto hasError = [&] {
        return std::ranges::any_of(server.client.getDiagnostics(headerUri), [](const auto& diag) {
            return diag.message.find("alias_missing_t") != std::string::npos;
        });
    };
    publish(7, broken);
    CHECK(hasError());
    publish(8, clean);
    CHECK(server.client.getDiagnostics(headerUri).empty());
    publish(9, "class item; int value; int value; endclass\n");
    bool foundRelated = false;
    for (const auto& diagnostic : server.client.getDiagnostics(headerUri)) {
        if (diagnostic.relatedInformation)
            for (const auto& related : *diagnostic.relatedInformation) {
                foundRelated = true;
                CHECK(related.location.uri == headerUri);
            }
    }
    CHECK(foundRelated);
    server.client.diagnosticPublications.clear();
    server.onDocDidClose({.textDocument = {.uri = headerUri}});
    CHECK(std::ranges::count(server.client.diagnosticPublications, physicalUri) == 0);
    server.openFile((opened / "item.svh").string(), clean);
    publish(20, broken);
    CHECK(hasError());
    publish(21, clean);
    CHECK(server.client.getDiagnostics(headerUri).empty());
}
#endif

#ifndef _WIN32
TEST_CASE("Opening an alias retires previously published physical include diagnostics",
          "[diagnostic-uri-alias]") {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto root = fs::temp_directory_path() / ("slang-diagnostic-adoption-" + std::to_string(stamp));
    fs::create_directories(root / "physical");
    auto cleanup = slang::ScopeGuard([&] { fs::remove_all(root); });
    fs::create_directory_symlink(fs::canonical(root / "physical"), root / "opened");
    auto opened = root / "opened";
    auto alias = URI::fromFile(opened / "item.svh");
    auto physical = URI::fromFile(fs::canonical(root / "physical") / "item.svh");
    const std::string broken = "class item; int value endclass\n";
    const std::string clean = "class item; int value; endclass\n";
    std::ofstream(opened / "item.svh") << broken;
    std::ofstream(opened / "owner.sv") << "package p;\n`include \"item.svh\"\nendpackage\n";
    bool isolated = false;
    SECTION("sequential repair") {
    }
    SECTION("isolated repair") {
        isolated = true;
    }
    ServerHarness server(lsp::InitializeParams{
        .workspaceFolders = {
            {lsp::WorkspaceFolder{.uri = URI::fromFile(opened), .name = "alias"}}}});
    server.openFile((opened / "owner.sv").string());
    REQUIRE_FALSE(server.client.getDiagnostics(physical).empty());
    CHECK_FALSE(server.client.diagnosticVersions.at(physical));
    server.client.diagnosticPublications.clear();
    server.openFile((opened / "item.svh").string(), broken);
    REQUIRE_FALSE(server.client.getDiagnostics(alias).empty());
    CHECK(server.client.diagnosticVersions.at(alias) == 1);
    CHECK(server.client.getDiagnostics(physical).empty());
    CHECK(std::ranges::count(server.client.diagnosticPublications, physical) == 1);
    CHECK(std::ranges::find(server.client.diagnosticPublications, physical) <
          std::ranges::find(server.client.diagnosticPublications, alias));
    CHECK_FALSE(server.client.diagnosticVersions.at(physical));
    server.client.diagnosticPublications.clear();
    changeWithoutDiagnostics(server, alias, 2, clean);
    if (isolated) {
        auto snapshot = server.m_driver->captureDiagnosticSnapshot({});
        REQUIRE(snapshot);
        auto output = server::executeDiagnosticJob(
            server::encodeDiagnosticJob(snapshot->metadata, *snapshot->input));
        server.m_driver->commitDiagnosticSnapshot(*snapshot, std::move(output), {});
    }
    else {
        server.m_driver->runPendingDiagnostics();
    }
    CHECK(server.client.getDiagnostics(alias).empty());
    CHECK(server.client.getDiagnostics(physical).empty());
    CHECK(server.client.diagnosticVersions.at(alias) == 2);
    CHECK(std::ranges::count(server.client.diagnosticPublications, physical) == 0);
}

TEST_CASE("Private preinclude clears use the open alias publication identity",
          "[diagnostic-uri-alias]") {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto root = fs::temp_directory_path() / ("slang-preinclude-alias-" + std::to_string(stamp));
    fs::create_directories(root / "physical/.slang");
    auto cleanup = slang::ScopeGuard([&] { fs::remove_all(root); });
    fs::create_directory_symlink(fs::canonical(root / "physical"), root / "opened");
    auto opened = root / "opened";
    auto alias = URI::fromFile(opened / "common.svh");
    auto physical = URI::fromFile(fs::canonical(root / "physical") / "common.svh");
    std::ofstream(opened / ".slang/server.json") << R"({"explorePreincludeFiles":["common.svh"]})";
    std::ofstream(opened / "common.svh") << "`define FIELD_TYPE int\n";
    std::ofstream(opened / "owner.sv") << "package p;\n`include \"item.svh\"\nendpackage\n";
    const std::string item = "class item; `FIELD_TYPE value; endclass\n";
    std::ofstream(opened / "item.svh") << item;
    ServerHarness server(lsp::InitializeParams{
        .workspaceFolders = {
            {lsp::WorkspaceFolder{.uri = URI::fromFile(opened), .name = "alias"}}}});
    server.openFile((opened / "common.svh").string(), "`define FIELD_TYPE int\n");
    server.openFile((opened / "item.svh").string(), item);
    for (int version : {3, 4}) {
        changeWithoutDiagnostics(server, alias, version,
                                 version == 3 ? "`unknown_alias_directive\n`define FIELD_TYPE int\n"
                                              : "`define FIELD_TYPE int\n// repaired\n");
        auto snapshot = server.m_driver->captureDiagnosticSnapshot({});
        REQUIRE(snapshot);
        REQUIRE(std::ranges::count(snapshot->metadata.clearUris, physical) > 0);
        auto output = server::executeDiagnosticJob(
            server::encodeDiagnosticJob(snapshot->metadata, *snapshot->input));
        for (const auto& document : output.documents)
            CHECK(document.uri != physical);
        server.client.diagnosticPublications.clear();
        server.m_driver->commitDiagnosticSnapshot(*snapshot, std::move(output), {});
        CHECK(std::ranges::count(server.client.diagnosticPublications, physical) == 0);
        CHECK(server.client.diagnosticVersions.at(alias) == version);
        if (version == 3)
            CHECK_FALSE(server.client.getDiagnostics(alias).empty());
        else
            CHECK(server.client.getDiagnostics(alias).empty());
    }
}
#endif

TEST_CASE("Configured initial open defers complete diagnostics and rejects superseded state",
          "[configured-open-deferral]") {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() /
                      ("slang-configured-open-" + std::to_string(stamp));
    REQUIRE(fs::create_directory(root));
    auto cleanup = slang::ScopeGuard([&] { fs::remove_all(root); });
    fs::create_directory(root / ".slang");
    std::ofstream(root / ".slang/server.json")
        << R"({"compilationFilelist":"files.f","flags":"--single-unit"})";
    std::ofstream(root / "files.f") << "owner.sv\n";
    std::ofstream(root / "owner.sv") << "package p;\n`include \"item.svh\"\nendpackage\n";
    const std::string clean = "class item; int value; endclass\n";
    std::string broken = "class item; missing_initial_t value; endclass\n";
    SECTION("complete semantic errors") {
    }
    SECTION("syntax errors") {
        broken = "class item; int value endclass\n";
    }
    SECTION("preprocessor errors") {
        broken = "`unknown_initial_directive\n" + clean;
    }
    std::ofstream(root / "item.svh") << broken;
    ServerHarness server(lsp::InitializeParams{
        .workspaceFolders = {
            {lsp::WorkspaceFolder{.uri = URI::fromFile(root), .name = "deferred"}}}});
    const auto uri = URI::fromFile(root / "item.svh");
    server.client.diagnosticPublications.clear();
    server.m_driver->openDocument(uri, broken, 7, true);
    CHECK(server.client.diagnosticPublications.empty());
    REQUIRE(server.m_driver->hasPendingDiagnostics());
    auto initial = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(initial);
    auto output = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(initial->metadata, *initial->input));
    auto document = std::find_if(output.documents.begin(), output.documents.end(),
                                 [&](const auto& d) { return d.uri == uri; });
    REQUIRE(document != output.documents.end());
    CHECK_FALSE(document->diagnostics.empty());
    server.m_driver->commitDiagnosticSnapshot(*initial, std::move(output), {});
    CHECK_FALSE(server.client.getDiagnostics(uri).empty());
    CHECK(server.client.diagnosticVersions.at(uri) == 7);
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
    server.m_driver->openDocument(uri, broken, 8, true);
    auto obsolete = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(obsolete);
    auto stale = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(obsolete->metadata, *obsolete->input));
    changeWithoutDiagnostics(server, uri, 9, clean);
    CHECK_FALSE(server.m_driver->isDiagnosticSnapshotCurrent(*obsolete));
    CHECK_THROWS(server.m_driver->commitDiagnosticSnapshot(*obsolete, std::move(stale), {}));
    REQUIRE(server.m_driver->hasPendingDiagnostics());
    auto latest = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(latest);
    auto repair = server::executeDiagnosticJob(
        server::encodeDiagnosticJob(latest->metadata, *latest->input));
    server.m_driver->commitDiagnosticSnapshot(*latest, std::move(repair), {});
    CHECK(server.client.getDiagnostics(uri).empty());
    CHECK(server.client.diagnosticVersions.at(uri) == 9);
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
}

// Fast cwd foundations are tested independently of timing measurements.
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/util/CwdCoordinator.h"
#include "slang/util/ThreadPool.h"
namespace {
struct FastCwdFixture {
    std::filesystem::path saved = std::filesystem::current_path();
    std::filesystem::path root, moved;
    FastCwdFixture() {
        static std::atomic<unsigned> serial{0};
        root = std::filesystem::temp_directory_path() /
               ("slang-fast-cwd-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(serial++));
        std::filesystem::create_directories(root / "work");
        root = std::filesystem::canonical(root);
        moved = root.string() + "-moved";
    }
    ~FastCwdFixture() {
        std::error_code ec;
        std::filesystem::current_path(saved, ec);
        std::filesystem::remove_all(root, ec);
        std::filesystem::remove_all(moved, ec);
    }
    void write(std::string_view name, std::string_view text) {
        auto path = root / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path);
        out << text;
        REQUIRE(out.good());
    }
};
} // namespace

TEST_CASE("Fast cwd boundary mismatch discards and poisons context", "[fast-cwd]") {
    FastCwdFixture f;
    std::filesystem::current_path(f.root / "work");
    slang::SourceManager sm;
    sm.setFastCwdSnapshotChecks(true);
    sm.enableSnapshotRecording();
    SECTION("begin detects ancestor rename") {
        std::filesystem::rename(f.root, f.moved);
        CHECK_THROWS_WITH(sm.beginFastCwdPhase(),
                          "fast cwd snapshot context changed or is unavailable; result discarded; "
                          "reload configuration");
    }
    SECTION("end detects ancestor rename") {
        auto phase = sm.beginFastCwdPhase();
        std::filesystem::rename(f.root, f.moved);
        CHECK_THROWS(phase.finish());
        std::filesystem::rename(f.moved, f.root);
        CHECK_THROWS(sm.beginFastCwdPhase()); // sticky even after pathname restored
    }
    SECTION("end detects changed current directory") {
        auto phase = sm.beginFastCwdPhase();
        std::filesystem::current_path(f.root); // simulate an uncoordinated writer
        CHECK_THROWS(phase.finish());
    }
    SECTION("end detects removed cwd") {
        auto phase = sm.beginFastCwdPhase();
        std::filesystem::remove(f.root / "work");
        CHECK_THROWS(phase.finish());
    }
    CHECK_THROWS(sm.captureSnapshot({}));
    // A failed / unwound lease must not strand the process coordinator.
    std::error_code ec;
    CHECK_NOTHROW(slang::CwdCoordinator::setCurrentPath(f.saved, ec));
    CHECK_FALSE(ec);
}

TEST_CASE("Fast cwd restored transient namespace changes are outside contract", "[fast-cwd]") {
    FastCwdFixture f;
    std::filesystem::current_path(f.root / "work");
    slang::SourceManager sm;
    sm.setFastCwdSnapshotChecks(true);
    sm.enableSnapshotRecording();
    auto phase = sm.beginFastCwdPhase();
    std::filesystem::rename(f.root, f.moved);
    std::filesystem::rename(f.moved, f.root);
    CHECK_NOTHROW(phase.finish()); // documented limitation, not detection coverage
}

TEST_CASE("Fast cwd pool workers borrow owner lease and reject writers without deadlock",
          "[fast-cwd]") {
    FastCwdFixture f;
    for (unsigned i = 0; i != 16; ++i)
        f.write("work/root" + std::to_string(i) + ".sv", "package p; int item; endpackage\n");
    std::filesystem::current_path(f.root / "work");
    slang::SourceManager sm;
    sm.setFastCwdSnapshotChecks(true);
    sm.enableSnapshotRecording();
    auto outer = sm.beginFastCwdPhase();
    auto nested = sm.beginFastCwdPhase();
    slang::ThreadPool pool(4);
    std::atomic<unsigned> loaded{0}, rejected{0}, phaseRejected{0};
    pool.detach_loop(size_t(0), size_t(16), [&](size_t i) {
        auto root = sm.readSource("root" + std::to_string(i) + ".sv");
        if (root) {
            auto tree = slang::syntax::SyntaxTree::fromBuffer(*root, sm);
            if (tree->diagnostics().empty())
                ++loaded;
        }
        try {
            std::error_code ec;
            slang::CwdCoordinator::setCurrentPath(f.root, ec);
        }
        catch (const std::runtime_error&) {
            ++rejected;
        }
        try {
            auto forbidden = sm.beginFastCwdPhase();
            forbidden.finish();
        }
        catch (const std::runtime_error&) {
            ++phaseRejected;
        }
    });
    pool.wait();
    CHECK(loaded == 16);
    CHECK(rejected == 16);
    CHECK(phaseRejected == 16);
    CHECK(std::filesystem::current_path() == f.root / "work");
    nested.finish();
    outer.finish();
    CHECK_NOTHROW(sm.captureSnapshot(sm.getAllBuffers()));
    std::error_code ec;
    CHECK_NOTHROW(slang::CwdCoordinator::setCurrentPath(f.saved, ec));
    CHECK_FALSE(ec);
}

TEST_CASE("Fast cwd cancellation unwinds lease and strict reads stay enabled outside phase",
          "[fast-cwd]") {
    FastCwdFixture f;
    f.write("work/root.sv", "module m; endmodule\n");
    std::filesystem::current_path(f.root / "work");
    slang::SourceManager sm;
    sm.setFastCwdSnapshotChecks(true);
    sm.enableSnapshotRecording();
    lsp::RequestContext ctx("fast-cwd-test", std::nullopt);
    ctx.cancel();
    CHECK_THROWS([&] {
        auto phase = sm.beginFastCwdPhase();
        ctx.throwIfCancelled("fast cwd cancellation test");
        phase.finish();
    }());
    CHECK_NOTHROW([&] {
        auto phase = sm.beginFastCwdPhase();
        phase.finish();
    }());
    std::error_code ec;
    slang::CwdCoordinator::setCurrentPath(f.root, ec);
    REQUIRE_FALSE(ec);
    REQUIRE(sm.readSource(f.root / "work/root.sv")); // outside lease observes different cwd
    slang::CwdCoordinator::setCurrentPath(f.root / "work", ec);
    REQUIRE_FALSE(ec);
    CHECK_THROWS(sm.captureSnapshot(sm.getAllBuffers()));
}

TEST_CASE("Fast and strict snapshot reads preserve aliases misses directives and replay",
          "[fast-cwd]") {
    FastCwdFixture f;
    f.write("work/root.sv",
            "`include \"header.svh\"\nmodule m; initial $display(`__FILE__); endmodule\n");
    f.write("work/header.svh", "`line 31 \"virtual.sv\" 0\nBAD SYNTAX\n");
    std::filesystem::create_directory_symlink(f.root / "work", f.root / "alias");
    std::filesystem::current_path(f.root / "work");
    std::vector<std::string> diagnostics;
    std::vector<size_t> lookupCounts;
    for (bool fast : {false, true}) {
        slang::SourceManager sm;
        sm.setFastCwdSnapshotChecks(fast);
        sm.enableSnapshotRecording();
        auto phase = sm.beginFastCwdPhase();
        REQUIRE_FALSE(sm.addUserDirectories((f.root / "work").string()));
        auto root = sm.readSource("root.sv");
        REQUIRE(root);
        auto tree = slang::syntax::SyntaxTree::fromBuffer(*root, sm);
        REQUIRE(sm.readSource(f.root / "alias/header.svh"));
        REQUIRE_FALSE(sm.readSource("missing.svh"));
        auto before = slang::DiagnosticEngine::reportAll(sm, tree->diagnostics());
        auto snapshot = sm.captureSnapshot(sm.getAllBuffers());
        phase.finish();
        diagnostics.push_back(before);
        lookupCounts.push_back(snapshot.lookups.size());
        slang::SourceManager replay;
        replay.loadSnapshot(snapshot);
        auto restored = replay.createRootBuffer(snapshot.graphFileIds.front());
        auto restoredTree = slang::syntax::SyntaxTree::fromBuffer(restored, replay);
        CHECK(slang::DiagnosticEngine::reportAll(replay, restoredTree->diagnostics()) == before);
        CHECK_FALSE(replay.readSource("missing.svh"));
        CHECK_THROWS_AS(replay.readSource("not-recorded.svh"),
                        slang::SourceManager::SnapshotIncompleteError);
    }
    CHECK(diagnostics[0] == diagnostics[1]);
    CHECK(lookupCounts[0] == lookupCounts[1]);
}

#include "slang/parsing/Preprocessor.h"
TEST_CASE("Fast cwd failed analysis never publishes diagnostics", "[fast-cwd]") {
    FastCwdFixture f;
    std::filesystem::current_path(f.root / "work");
    Config config;
    config.fastCwdSnapshotChecks = true;
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         (f.root / "work").string(), nullptr);
    bool renamed = false;
    auto callback = [&](slang::BufferID, bool, bool) {
        if (!renamed) {
            std::filesystem::rename(f.root, f.moved);
            renamed = true;
        }
    };
    driver->options.insertOrGet<slang::parsing::PreprocessorOptions>().bufferChangeCB = callback;
    const auto publications = client.diagnosticPublications.size();
    CHECK_THROWS(driver->openDocument(URI::fromFile(f.root / "work/root.sv"),
                                      "package p; missing_t value; endpackage\n", 1));
    CHECK(renamed);
    CHECK(client.diagnosticPublications.size() == publications);
    std::filesystem::rename(f.moved, f.root);
    CHECK_THROWS(driver->sm.beginFastCwdPhase());
}

TEST_CASE("Fast and strict preserve symlink target conflict rejection", "[fast-cwd]") {
    FastCwdFixture f;
    f.write("work/first.svh", "int first;\n");
    f.write("work/second.svh", "int second;\n");
    std::filesystem::current_path(f.root / "work");
    for (bool fast : {false, true}) {
        std::error_code ec;
        std::filesystem::remove(f.root / "work/alias.svh", ec);
        std::filesystem::create_symlink("first.svh", f.root / "work/alias.svh");
        slang::SourceManager sm;
        sm.setFastCwdSnapshotChecks(fast);
        sm.enableSnapshotRecording();
        auto phase = sm.beginFastCwdPhase();
        REQUIRE(sm.readSource("alias.svh"));
        std::filesystem::remove(f.root / "work/alias.svh");
        std::filesystem::create_symlink("second.svh", f.root / "work/alias.svh");
        REQUIRE(sm.readSource("alias.svh"));
        phase.finish();
        CHECK_THROWS(sm.captureSnapshot(sm.getAllBuffers()));
    }
}

TEST_CASE("Internal cwd writer serializes nested command files and preserves setter errors",
          "[fast-cwd]") {
    FastCwdFixture f;
    f.write("one/inner.f", "root.sv\n");
    f.write("one/outer.f", "-F inner.f\n");
    f.write("one/root.sv", "package one; endpackage\n");
    f.write("two/inner.f", "root.sv\n");
    f.write("two/outer.f", "-F inner.f\n");
    f.write("two/root.sv", "package two; endpackage\n");
    slang::driver::Driver a, b;
    auto& first = a.sourceManager;
    auto& second = b.sourceManager;
    a.addStandardArgs();
    b.addStandardArgs();
    std::atomic<unsigned> completed{0};
    std::thread t1([&] {
        if (a.processCommandFiles((f.root / "one/outer.f").string(), true, false))
            ++completed;
    });
    std::thread t2([&] {
        if (b.processCommandFiles((f.root / "two/outer.f").string(), true, false))
            ++completed;
    });
    t1.join();
    t2.join();
    CHECK(completed == 2);
    CHECK(std::filesystem::current_path() == f.saved);
    REQUIRE(a.processOptions(false));
    REQUIRE(b.processOptions(false));
    a.options.numThreads = 1;
    b.options.numThreads = 1;
    REQUIRE(a.parseAllSources());
    REQUIRE(b.parseAllSources());
    REQUIRE(a.syntaxTrees.size() == 1);
    REQUIRE(b.syntaxTrees.size() == 1);
    CHECK(first.getFullPath(a.syntaxTrees.front()->getSourceBufferIds().front()) ==
          f.root / "one/root.sv");
    CHECK(second.getFullPath(b.syntaxTrees.front()->getSourceBufferIds().front()) ==
          f.root / "two/root.sv");
    std::error_code nativeError, coordinatedError;
    std::filesystem::current_path(f.root / "nonexistent", nativeError);
    slang::CwdCoordinator::setCurrentPath(f.root / "nonexistent", coordinatedError);
    CHECK(nativeError == coordinatedError);
    CHECK(std::filesystem::current_path() == f.saved);
}

TEST_CASE("Fast cwd cancelled analysis cannot publish and retry releases phase", "[fast-cwd]") {
    FastCwdFixture f;
    std::filesystem::current_path(f.root / "work");
    Config config;
    config.fastCwdSnapshotChecks = true;
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         (f.root / "work").string(), nullptr);
    auto doc = server::SlangDoc::fromText(*driver, URI::fromFile(f.root / "work/root.sv"),
                                          "package p; class c; int field; endclass endpackage\n");
    std::string checkpoint;
    SECTION("after parse") {
        checkpoint = "before analysis construction";
    }
    SECTION("before publication") {
        checkpoint = "before analysis publication";
    }
    lsp::RequestContext ctx("fast-cancel", 1);
    ctx.setCheckpointObserverForTesting([&](std::string_view name) {
        if (name == checkpoint)
            ctx.cancel();
    });
    CHECK_THROWS_AS(doc->getAnalysis(ctx), lsp::RequestCancelled);
    std::error_code ec;
    CHECK_NOTHROW(slang::CwdCoordinator::setCurrentPath(f.root / "work", ec));
    CHECK_FALSE(ec);
    CHECK_NOTHROW(doc->getAnalysis());
    CHECK(client.diagnosticPublications.empty());
}

TEST_CASE("Fast cwd background failure reports error and retains pending diagnostics",
          "[fast-cwd]") {
    class FastHarness : public ServerHarness {
    public:
        using SlangServer::runBackgroundWork;
        void useFastMode() {
            Config c;
            c.fastCwdSnapshotChecks = true;
            loadConfig(c, false);
        }
    };
    FastCwdFixture f;
    std::filesystem::current_path(f.root / "work");
    FastHarness server;
    server.useFastMode();
    const auto header = URI::fromFile(f.root / "work/background.svh");
    server.onDocDidOpen({.textDocument = {.uri = header,
                                          .languageId = "systemverilog",
                                          .version = 1,
                                          .text = "class c; int field; endclass\n"}});
    server.onDocDidOpen(
        {.textDocument = {.uri = URI::fromFile(f.root / "work/background-owner.sv"),
                          .languageId = "systemverilog",
                          .version = 1,
                          .text = "package p;\n`include \"background.svh\"\nendpackage\n"}});
    changeWithoutDiagnostics(server, header, 2, "class c; missing_t field; endclass\n");
    const auto count = server.client.diagnosticPublications.size();
    REQUIRE(server.m_driver->hasPendingDiagnostics());
    std::filesystem::rename(f.root, f.moved);
    CHECK_THROWS_AS(server.runBackgroundWork({}), slang::SourceManager::FastCwdContextError);
    server.client.expectError("fast cwd snapshot context changed or is unavailable; result "
                              "discarded; reload configuration");
    CHECK(server.client.diagnosticPublications.size() == count);
    CHECK(server.m_driver->hasPendingDiagnostics());
    std::filesystem::rename(f.moved, f.root);
}

TEST_CASE("Fast SourceLoader joins parallel sources and separate units before release",
          "[fast-cwd]") {
    FastCwdFixture f;
    for (unsigned i = 0; i < 16; i++)
        f.write("work/root" + std::to_string(i) + ".sv",
                "package p" + std::to_string(i) + "; int field; endpackage\n");
    bool grouped = false;
    SECTION("parallel independent roots") {
    }
    SECTION("parallel native -C groups") {
        grouped = true;
    }
    for (unsigned group = 0; group < 4; group++) {
        std::string body;
        for (unsigned offset = 0; offset < 4; offset++)
            body += "root" + std::to_string(group * 4 + offset) + ".sv\n";
        f.write("work/group" + std::to_string(group) + ".f", body);
    }
    std::filesystem::current_path(f.root / "work");
    for (bool fast : {false, true}) {
        slang::driver::Driver driver;
        driver.addStandardArgs();
        std::string args = "--threads 4 ";
        if (grouped) {
            for (unsigned i = 0; i < 4; i++)
                args += "-C group" + std::to_string(i) + ".f ";
        }
        else {
            for (unsigned i = 0; i < 16; i++)
                args += "root" + std::to_string(i) + ".sv ";
        }
        slang::CommandLine::ParseOptions parseOpts;
        parseOpts.ignoreProgramName = true;
        REQUIRE(driver.parseCommandLine(args, parseOpts));
        REQUIRE(driver.processOptions(false));
        driver.sourceManager.setFastCwdSnapshotChecks(fast);
        driver.sourceManager.enableSnapshotRecording(true);
        REQUIRE(driver.parseAllSources());
        REQUIRE(driver.syntaxTrees.size() == (grouped ? 4 : 16));
        for (auto& tree : driver.syntaxTrees)
            CHECK(tree->diagnostics().empty());
        CHECK_NOTHROW(driver.sourceManager.captureSnapshot(driver.sourceManager.getAllBuffers()));
        std::error_code ec;
        CHECK_NOTHROW(slang::CwdCoordinator::setCurrentPath(f.root / "work", ec));
        CHECK_FALSE(ec);
    }
}

TEST_CASE("Fast cwd synchronous notification failure reaches client without pending background",
          "[fast-cwd][fast-cwd-notification]") {
    class NotificationHarness : public ServerHarness {
    public:
        using SlangServer::processMessage;
        void useFastMode() {
            Config c;
            c.fastCwdSnapshotChecks = true;
            loadConfig(c, false);
        }
    };
    FastCwdFixture f;
    std::filesystem::current_path(f.root / "work");
    NotificationHarness server;
    server.useFastMode();
    auto uri = URI::fromFile(f.root / "work/notification.sv");
    bool save = false;
    SECTION("didOpen") {
    }
    SECTION("didSave") {
        save = true;
    }
    lsp::DidOpenTextDocumentParams open{
        .textDocument = {.uri = uri,
                         .languageId = "systemverilog",
                         .version = 1,
                         .text = "package p; int field; endpackage\n"}};
    if (save)
        server.processMessage({.method = "textDocument/didOpen",
                               .params = rfl::to_generic<rfl::UnderlyingEnums>(open)});
    auto count = server.client.diagnosticPublications.size();
    std::filesystem::rename(f.root, f.moved);
    if (save)
        server.processMessage({.method = "textDocument/didSave",
                               .params = rfl::to_generic<rfl::UnderlyingEnums>(
                                   lsp::DidSaveTextDocumentParams{.textDocument = {.uri = uri}})});
    else
        server.processMessage({.method = "textDocument/didOpen",
                               .params = rfl::to_generic<rfl::UnderlyingEnums>(open)});
    CHECK(server.client.diagnosticPublications.size() == count);
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
    server.client.expectError("fast cwd snapshot context changed or is unavailable; result "
                              "discarded; reload configuration");
    std::filesystem::rename(f.moved, f.root);
}

TEST_CASE("Fast cwd synchronous failures report again after configuration reload",
          "[fast-cwd][fast-cwd-notification]") {
    class ReloadNotificationHarness : public ServerHarness {
    public:
        using SlangServer::m_diagnosticFallbackReason;
        using SlangServer::processMessage;
        void useFastMode() {
            Config config;
            config.fastCwdSnapshotChecks = true;
            loadConfig(config, false);
        }
    };
    FastCwdFixture fixture;
    std::filesystem::current_path(fixture.root / "work");
    ReloadNotificationHarness server;
    server.useFastMode();
    const auto uri = URI::fromFile(fixture.root / "work/reloaded.sv");
    lsp::DidOpenTextDocumentParams open{
        .textDocument = {.uri = uri,
                         .languageId = "systemverilog",
                         .version = 1,
                         .text = "package p; int field; endpackage\n"}};

    for (int cycle = 0; cycle < 2; ++cycle) {
        const auto count = server.client.diagnosticPublications.size();
        std::filesystem::rename(fixture.root, fixture.moved);
        if (cycle == 0) {
            server.processMessage({.method = "textDocument/didOpen",
                                   .params = rfl::to_generic<rfl::UnderlyingEnums>(open)});
        }
        else {
            server.processMessage(
                {.method = "textDocument/didSave",
                 .params = rfl::to_generic<rfl::UnderlyingEnums>(
                     lsp::DidSaveTextDocumentParams{.textDocument = {.uri = uri}})});
        }
        CHECK(server.client.diagnosticPublications.size() == count);
        CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
        server.client.expectError("fast cwd snapshot context changed or is unavailable; result "
                                  "discarded; reload configuration");
        std::filesystem::rename(fixture.moved, fixture.root);
        if (cycle == 0)
            server.useFastMode();
    }
    server.m_diagnosticFallbackReason = "unrelated diagnostic worker failure";
    server.useFastMode();
    CHECK(server.m_diagnosticFallbackReason == "unrelated diagnostic worker failure");
}
