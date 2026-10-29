// SPDX-License-Identifier: MIT
#include "diagnostics/DiagnosticSnapshotCodec.h"
#include "diagnostics/DiagnosticWorker.h"
#include "utils/ServerHarness.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <rfl/json.hpp>

namespace {
namespace fs = std::filesystem;
struct FrozenWorkerFixture {
    fs::path root = fs::weakly_canonical(fs::temp_directory_path()) /
                    ("slang-frozen-worker-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<ServerHarness> server;
    const std::string user = "class item; lib_first::word_t value; shared_pkg::shared_t extra; "
                             "int dimension[5-lib_second::WIDTH]; "
                             "localparam int VALUE=`LIB_WIDTH; endclass\n";
    FrozenWorkerFixture() {
        fs::create_directories(root / ".slang");
        write("files.f", "first.sv\nsecond.sv\nowner.sv\n");
        write("shared.svh", "`ifndef COMMON_GUARD\n`define COMMON_GUARD\n"
                            "package shared_pkg; typedef int shared_t; endpackage\n`endif\n");
        write("first.sv", "`include \"shared.svh\"\n`define LIB_WIDTH 4\n"
                          "package lib_first; typedef logic [`LIB_WIDTH-1:0] word_t; "
                          "localparam int WIDTH=`CLI_CONST; endpackage\n");
        write("second.sv", "package lib_second; "
                           "localparam int WIDTH=lib_first::WIDTH; endpackage\n");
        write("owner.sv", "`include \"shared.svh\"\npackage app;\n"
                          "`include \"item.svh\"\nendpackage\n");
        write("item.svh", user);
        initialize();
    }
    ~FrozenWorkerFixture() {
        server.reset();
        fs::remove_all(root);
    }
    void write(const fs::path& file, std::string_view text) {
        std::ofstream out(root / file, std::ios::binary);
        REQUIRE(out.good());
        out << text;
    }
    void initialize(int cli = 3, int budget = 64, bool enabled = true, bool modules = false) {
        server.reset();
        write(".slang/server.json",
              "{\"compilationFilelist\":\"files.f\",\"flags\":\"--single-unit --threads 1 "
              "-DCLI_CONST=" +
                  std::to_string(cli) +
                  "\",\"experimentalUvmPrefixCache\":false,\"experimentalFrozenSources\":" +
                  (enabled ? "true" : "false") +
                  ",\"frozenSources\":{\"packages\":[\"lib_first\",\"lib_second\"],\"maxMiB\":" +
                  std::to_string(budget) + (modules ? ",\"modules\":[\"library_leaf\"]}}" : "}}"));
        server = std::make_unique<ServerHarness>(lsp::InitializeParams{
            .workspaceFolders = {
                {lsp::WorkspaceFolder{.uri = URI::fromFile(root), .name = "test"}}}});
        server->m_driver->openDocument(URI::fromFile(root / "item.svh"), user, 1, true);
    }
    server::ServerDriver::DiagnosticSnapshot snapshot() {
        auto value = server->m_driver->captureDiagnosticSnapshot({});
        REQUIRE(value);
        return std::move(*value);
    }
};
std::string rendered(const server::DiagnosticJobOutput& output) {
    return rfl::json::write<rfl::UnderlyingEnums>(output);
}
void replaceFile(server::diagnostic_snapshot::Input& input, std::string_view filename,
                 std::string text) {
    bool found = false;
    text.push_back('\0');
    for (auto& file : input.sources.files)
        if (fs::path(file.fullPath).filename() == filename) {
            file.bytes = text;
            found = true;
        }
    REQUIRE(found);
}
} // namespace

TEST_CASE("Generic diagnostic prefixes reuse multiple ordinary packages and current suffixes",
          "[frozen-worker][frozen-source-integration]") {
    FrozenWorkerFixture fixture;
    auto snapshot = fixture.snapshot();
    REQUIRE(snapshot.metadata.schema == 2);
    REQUIRE(snapshot.metadata.trees.size() == 1);
    const auto& hint = snapshot.metadata.trees[0].frozenPrefix;
    REQUIRE(hint);
    CHECK(hint->roots == 2);
    CHECK(hint->packages == std::vector<std::string>{"lib_first", "lib_second"});
    REQUIRE(snapshot.input->treeOptions[0].preprocessor);
    CHECK_FALSE(snapshot.input->treeOptions[0].preprocessor->captureUvmPrefix);
    auto job = server::encodeDiagnosticJob(snapshot.metadata, *snapshot.input);
    auto native = server::executeDiagnosticJob(job);
    REQUIRE(native.documents.size() == 1);
    INFO(rendered(native));
    CHECK(native.documents[0].diagnostics.empty());
    server::DiagnosticJobSession worker;
    server::DiagnosticJobStatistics first, second;
    CHECK(rendered(worker.execute(job, {}, &first)) == rendered(native));
    CHECK(first.genericPrefixCaptureParses == 1);
    CHECK(first.genericSuffixParses == 1);
    CHECK(first.prefixCaptureParses == 0);
    CHECK(first.retainedGenericBytes > 0);
    CHECK(first.retainedGenericBytes <= hint->maxBytes);
    CHECK(rendered(worker.execute(job, {}, &second)) == rendered(native));
    CHECK(second.reusedGenericPrefixes == 1);
    CHECK(second.genericPrefixCaptureParses == 0);
    CHECK(second.genericSuffixParses == 1);

    auto edited = *snapshot.input;
    replaceFile(edited, "item.svh", "class item; missing_type value; endclass\n");
    auto changed = server::encodeDiagnosticJob(snapshot.metadata, edited);
    server::DiagnosticJobStatistics suffixStats;
    auto result = worker.execute(changed, {}, &suffixStats);
    CHECK(rendered(result) == rendered(server::executeDiagnosticJob(changed)));
    CHECK(suffixStats.reusedGenericPrefixes == 1);
    CHECK_FALSE(result.documents[0].diagnostics.empty());

    auto library = *snapshot.input;
    replaceFile(library, "first.sv",
                "`include \"shared.svh\"\n`define LIB_WIDTH 4\n"
                "package lib_first; localparam int WIDTH=`CLI_CONST; endpackage\n");
    changed = server::encodeDiagnosticJob(snapshot.metadata, library);
    server::DiagnosticJobStatistics libraryStats;
    result = worker.execute(changed, {}, &libraryStats);
    CHECK(rendered(result) == rendered(server::executeDiagnosticJob(changed)));
    CHECK(libraryStats.reusedGenericPrefixes == 0);
    CHECK(libraryStats.genericContextResets == 1);
    CHECK(libraryStats.genericPrefixCaptureParses == 1);
    CHECK_FALSE(result.documents[0].diagnostics.empty());

    fixture.initialize(7);
    auto options = fixture.snapshot();
    changed = server::encodeDiagnosticJob(options.metadata, *options.input);
    server::DiagnosticJobStatistics optionStats;
    const auto optionNative = server::executeDiagnosticJob(changed);
    REQUIRE(optionNative.documents.size() == 1);
    CHECK_FALSE(optionNative.documents[0].diagnostics.empty());
    CHECK(rendered(worker.execute(changed, {}, &optionStats)) == rendered(optionNative));
    CHECK(optionStats.genericContextResets == 1);
    CHECK(optionStats.reusedGenericPrefixes == 0);
    CHECK(optionStats.genericPrefixCaptureParses == 1);
}

TEST_CASE("Generic diagnostic decline restarts native parsing and can recover",
          "[frozen-worker][frozen-source-integration]") {
    FrozenWorkerFixture fixture;
    auto snapshot = fixture.snapshot();
    auto job = server::encodeDiagnosticJob(snapshot.metadata, *snapshot.input);
    server::DiagnosticJobSession worker;
    worker.execute(job);

    SECTION("source overrides a prefix macro") {
        auto input = *snapshot.input;
        replaceFile(input, "item.svh", "`undef LIB_WIDTH\n`define LIB_WIDTH 9\n" + fixture.user);
        job = server::encodeDiagnosticJob(snapshot.metadata, input);
    }
    SECTION("selection hint names an absent declaration") {
        snapshot.metadata.trees[0].frozenPrefix->packages = {"not_a_package"};
        job = server::encodeDiagnosticJob(snapshot.metadata, *snapshot.input);
    }
    SECTION("keyword stack effects cannot be replayed") {
        auto input = *snapshot.input;
        replaceFile(input, "first.sv",
                    "`begin_keywords \"1800-2017\"\n`include \"shared.svh\"\n`define LIB_WIDTH 4\n"
                    "package lib_first; typedef int word_t; localparam int WIDTH=3; endpackage\n");
        job = server::encodeDiagnosticJob(snapshot.metadata, input);
    }

    SECTION("prefix parser recovery uses fresh native state") {
        auto input = *snapshot.input;
        replaceFile(input, "first.sv",
                    "`include \"shared.svh\"\n`define LIB_WIDTH 4\n"
                    "package lib_first; typedef int word_t; localparam int WIDTH=; endpackage\n");
        job = server::encodeDiagnosticJob(snapshot.metadata, input);
    }

    server::DiagnosticJobStatistics declined;
    CHECK(rendered(worker.execute(job, {}, &declined)) ==
          rendered(server::executeDiagnosticJob(job)));
    CHECK(declined.genericDeclines >= 1);
    CHECK(declined.reusedGenericPrefixes == 0);
    CHECK(declined.parsedTrees == 1);
    CHECK(declined.retainedGenericBytes == 0);
    auto good = fixture.snapshot();
    auto repaired = server::encodeDiagnosticJob(good.metadata, *good.input);
    server::DiagnosticJobStatistics restored, hit;
    CHECK(rendered(worker.execute(repaired, {}, &restored)) ==
          rendered(server::executeDiagnosticJob(repaired)));
    CHECK(restored.genericPrefixCaptureParses == 1);
    worker.execute(repaired, {}, &hit);
    CHECK(hit.reusedGenericPrefixes == 1);
}

TEST_CASE("Generic diagnostic workers resume persistent prefix directives",
          "[frozen-worker][prefix-directives]") {
    FrozenWorkerFixture fixture;
    auto snapshot = fixture.snapshot();
    auto input = *snapshot.input;
    replaceFile(input, "second.sv",
        "package lib_second; localparam int WIDTH=lib_first::WIDTH; endpackage\n"
        "`timescale 1ns/1ps\n`default_nettype none\n`unconnected_drive pull1\n`celldefine\n");
    replaceFile(input, "owner.sv",
        "module inherited; assign implicit_net = 1'b0; endmodule\n"
        "`resetall\n`include \"shared.svh\"\npackage app;\n"
        "`include \"item.svh\"\nendpackage\n");
    auto job = server::encodeDiagnosticJob(snapshot.metadata, input);
    const auto expected = rendered(server::executeDiagnosticJob(job));
    server::DiagnosticJobSession worker;
    server::DiagnosticJobStatistics capture, hit;
    CHECK(rendered(worker.execute(job, {}, &capture)) == expected);
    CHECK(capture.genericPrefixCaptureParses == 1);
    CHECK(capture.genericDeclines == 0);
    CHECK(rendered(worker.execute(job, {}, &hit)) == expected);
    CHECK(hit.reusedGenericPrefixes == 1);
    CHECK(hit.genericDeclines == 0);
}

TEST_CASE("Generic worker hints are gated, validated and independently budgeted",
          "[frozen-worker][frozen-source-integration]") {
    FrozenWorkerFixture fixture;
    SECTION("feature off") {
        fixture.initialize(3, 64, false);
        auto snapshot = fixture.snapshot();
        CHECK_FALSE(snapshot.metadata.trees[0].frozenPrefix);
    }
    SECTION("zero budget") {
        fixture.initialize(3, 0);
        auto snapshot = fixture.snapshot();
        CHECK_FALSE(snapshot.metadata.trees[0].frozenPrefix);
    }
    SECTION("no retention fits a tiny budget") {
        auto snapshot = fixture.snapshot();
        snapshot.metadata.trees[0].frozenPrefix->maxBytes = 1;
        auto job = server::encodeDiagnosticJob(snapshot.metadata, *snapshot.input);
        server::DiagnosticJobSession worker;
        server::DiagnosticJobStatistics stats;
        auto native = server::executeDiagnosticJob(job);
        for (int n = 0; n < 2; ++n)
            CHECK(rendered(worker.execute(job, {}, &stats)) == rendered(native));
        CHECK(stats.genericDeclines == 2);
        CHECK(stats.reusedGenericPrefixes == 0);
        CHECK(stats.retainedGenericBytes == 0);
    }
    SECTION("malformed root count is rejected before parsing") {
        auto snapshot = fixture.snapshot();
        snapshot.metadata.trees[0].frozenPrefix->roots = 1000;
        auto job = server::encodeDiagnosticJob(snapshot.metadata, *snapshot.input);
        server::DiagnosticJobSession worker;
        CHECK_THROWS(worker.execute(job));
        CHECK_THROWS(server::executeDiagnosticJob(job));
    }
    SECTION("old private job schema is rejected explicitly") {
        auto snapshot = fixture.snapshot();
        snapshot.metadata.schema = 1;
        auto job = server::encodeDiagnosticJob(snapshot.metadata, *snapshot.input);
        server::DiagnosticJobSession worker;
        CHECK_THROWS(worker.execute(job));
        CHECK_THROWS(server::executeDiagnosticJob(job));
    }
    SECTION("source generation growth releases a context before exceeding retention") {
        auto snapshot = fixture.snapshot();
        auto seed = server::encodeDiagnosticJob(snapshot.metadata, *snapshot.input);
        server::DiagnosticJobSession probe;
        server::DiagnosticJobStatistics baseline;
        probe.execute(seed, {}, &baseline);
        REQUIRE(baseline.retainedGenericBytes > 0);
        auto budget = baseline.retainedGenericBytes + 4096;
        snapshot.metadata.trees[0].frozenPrefix->maxBytes = budget;
        server::DiagnosticJobSession worker;
        server::DiagnosticJobStatistics stats;
        bool releasedAfterReuse = false;
        for (int n = 0; n < 96; ++n) {
            auto input = *snapshot.input;
            replaceFile(input, "item.svh", fixture.user + "// edit " + std::to_string(n) + "\n");
            auto job = server::encodeDiagnosticJob(snapshot.metadata, input);
            auto resumes = stats.reusedGenericPrefixes;
            auto declines = stats.genericDeclines;
            CHECK(rendered(worker.execute(job, {}, &stats)) ==
                  rendered(server::executeDiagnosticJob(job)));
            CHECK(stats.retainedGenericBytes <= budget);
            releasedAfterReuse |= stats.reusedGenericPrefixes == resumes + 1 &&
                                  stats.genericDeclines == declines &&
                                  stats.retainedGenericBytes == 0;
        }
        CHECK(stats.reusedGenericPrefixes > 0);
        CHECK(stats.genericContextResets > 0);
        CHECK(releasedAfterReuse);
    }
}

TEST_CASE("A parent's declined prefix skips worker preparation until the source is repaired",
          "[frozen-worker][frozen-source-integration]") {
    FrozenWorkerFixture fixture;
    const auto uri = URI::fromFile(fixture.root / "item.svh");
    auto change = [&](int version, const std::string& text) {
        fixture.server->SlangServer::onDocDidChange(
            {.textDocument = {.version = version, .uri = uri},
             .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
    };
    server::DiagnosticJobSession worker;
    auto initial = fixture.snapshot();
    REQUIRE(initial.metadata.trees[0].frozenPrefix);
    CHECK_FALSE(initial.metadata.trees[0].frozenPrefix->nativeOnly);
    worker.execute(server::encodeDiagnosticJob(initial.metadata, *initial.input));

    for (int version : {2, 3}) {
        change(version, "`undef LIB_WIDTH\n`define LIB_WIDTH 9\n" + fixture.user + "// version " +
                            std::to_string(version));
        auto rejected = fixture.snapshot();
        REQUIRE(rejected.metadata.trees[0].frozenPrefix);
        CHECK(rejected.metadata.trees[0].frozenPrefix->nativeOnly);
        auto job = server::encodeDiagnosticJob(rejected.metadata, *rejected.input);
        server::DiagnosticJobStatistics stats;
        CHECK(rendered(worker.execute(job, {}, &stats)) ==
              rendered(server::executeDiagnosticJob(job)));
        CHECK(stats.genericPrefixCaptureParses == 0);
        CHECK(stats.reusedGenericPrefixes == 0);
        CHECK(stats.parsedTrees == 1);
        CHECK(stats.retainedGenericBytes == 0);
    }
    change(4, fixture.user);
    auto repaired = fixture.snapshot();
    REQUIRE(repaired.metadata.trees[0].frozenPrefix);
    CHECK_FALSE(repaired.metadata.trees[0].frozenPrefix->nativeOnly);
    auto job = server::encodeDiagnosticJob(repaired.metadata, *repaired.input);
    server::DiagnosticJobStatistics prepared, reused;
    CHECK(rendered(worker.execute(job, {}, &prepared)) ==
          rendered(server::executeDiagnosticJob(job)));
    CHECK(prepared.genericPrefixCaptureParses == 1);
    worker.execute(job, {}, &reused);
    CHECK(reused.reusedGenericPrefixes == 1);
}

TEST_CASE("Diagnostic workers reuse selected module definitions",
          "[frozen-worker][frozen-module]") {
    FrozenWorkerFixture fixture;
    fixture.write("second.sv",
                  "package lib_second; localparam int WIDTH=lib_first::WIDTH; endpackage\n"
                  "module library_leaf; logic[lib_first::WIDTH-1:0] word; endmodule\n");
    fixture.initialize(3, 64, true, true);
    auto snapshot = fixture.snapshot();
    REQUIRE(snapshot.metadata.trees[0].frozenPrefix);
    auto& hint = *snapshot.metadata.trees[0].frozenPrefix;
    CHECK(hint.modules == std::vector<std::string>{"library_leaf"});
    hint.packages.clear(); // Prove module-only eligibility, not a package-enabled false positive.
    auto job = server::encodeDiagnosticJob(snapshot.metadata, *snapshot.input);
    server::DiagnosticJobSession worker;
    server::DiagnosticJobStatistics first, second;
    auto native = rendered(server::executeDiagnosticJob(job));
    CHECK(rendered(worker.execute(job, {}, &first)) == native);
    CHECK(first.genericPrefixCaptureParses == 1);
    CHECK(rendered(worker.execute(job, {}, &second)) == native);
    CHECK(second.reusedGenericPrefixes == 1);
}
