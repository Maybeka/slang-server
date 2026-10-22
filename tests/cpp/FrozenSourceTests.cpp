// SPDX-License-Identifier: MIT
#include "document/FrozenSourceRegistry.h"
#include "util/SlangExtensions.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <tuple>

#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"

using namespace slang;
namespace {
int packageConstant(ast::Compilation& compilation, std::string_view package,
                    std::string_view name) {
    auto* scope = compilation.getPackage(package);
    REQUIRE(scope);
    auto* member = scope->find(name);
    REQUIRE(member);
    return *member->as<ast::ParameterSymbol>().getValue().integer().as<int>();
}
} // namespace

TEST_CASE("A frozen input group preserves multiple packages and ordered macro state",
          "[frozen-source]") {
    SourceManager sm;
    auto a = sm.assignText("first.sv", "`define SCALE 3\npackage first; "
                                       "localparam int WIDTH=`SCALE; endpackage\n");
    auto b = sm.assignText("second.sv", "package second; import first::*; "
                                        "typedef enum {Zero=0,Value=WIDTH} code_t; "
                                        "localparam int RESULT=Value; endpackage\n"
                                        "`define END_VALUE 7\n");
    auto user = sm.assignText("user.sv", "package user_pkg; localparam int RESULT="
                                         "second::RESULT + `END_VALUE; endpackage\n");
    auto prefix = syntax::SyntaxTree::fromBuffersWithCheckpoint(std::vector{a, b}, sm);
    REQUIRE(prefix);
    REQUIRE(prefix->isPrefixResumeSafe().value_or(false));
    auto suffix = syntax::SyntaxTree::afterCachedPrefix(prefix, std::vector{user}, {});
    auto combined = syntax::SyntaxTree::withCachedPrefix(prefix, suffix);
    auto native = syntax::SyntaxTree::fromBuffers(std::vector{a, b, user}, sm);
    ast::Compilation cachedCompilation, nativeCompilation;
    cachedCompilation.addSyntaxTree(combined);
    nativeCompilation.addSyntaxTree(native);
    CHECK(cachedCompilation.getAllDiagnostics().empty());
    CHECK(nativeCompilation.getAllDiagnostics().empty());
    CHECK(packageConstant(cachedCompilation, "first", "WIDTH") == 3);
    CHECK(packageConstant(cachedCompilation, "second", "RESULT") == 3);
    CHECK(packageConstant(cachedCompilation, "user_pkg", "RESULT") == 10);
    CHECK(packageConstant(cachedCompilation, "user_pkg", "RESULT") ==
          packageConstant(nativeCompilation, "user_pkg", "RESULT"));
    const auto& borrowed = prefix->root().as<syntax::CompilationUnitSyntax>().members;
    const auto& composed = combined->root().as<syntax::CompilationUnitSyntax>().members;
    REQUIRE(borrowed.size() == 2);
    REQUIRE(composed.size() == 3);
    CHECK(borrowed[0] == composed[0]);
    CHECK(borrowed[1] == composed[1]);
    CHECK(borrowed[1]->parent == &prefix->root());
    CHECK(std::ranges::equal(combined->getSourceBufferIds(), native->getSourceBufferIds()));
    // Replacing an input invalidates the retained group; no source IDs are recycled.
    std::string_view edited = "package first; localparam int WIDTH=8; endpackage\n";
    SmallVector<char> replacement(edited.begin(), edited.end());
    replacement.push_back('\0');
    sm.replaceBuffer(a.id, std::move(replacement));
    CHECK_FALSE(server::hasValidBuffers(sm, prefix));
}

TEST_CASE("Generic prefix checkpoints decline unsupported directive and callback state",
          "[frozen-source]") {
    const std::string directive = GENERATE("`timescale 1ns/1ps\n", "`default_nettype none\n",
                                           "`begin_keywords \"1800-2017\"\n",
                                           "`pragma protect begin\n");
    CAPTURE(directive);
    SourceManager sm;
    auto root = sm.assignText(directive + "package selected; endpackage\n");
    auto tail = sm.assignText("module user; endmodule\n");
    auto prefix = syntax::SyntaxTree::fromBuffersWithCheckpoint(std::vector{root}, sm);
    REQUIRE(prefix->isPrefixResumeSafe());
    CHECK_FALSE(*prefix->isPrefixResumeSafe());
    CHECK_THROWS(syntax::SyntaxTree::afterCachedPrefix(prefix, std::vector{tail}, {}));
    parsing::PreprocessorOptions pp;
    pp.bufferChangeCB = [](BufferID, bool, bool) {};
    Bag options(pp);
    CHECK_THROWS(syntax::SyntaxTree::fromBuffersWithCheckpoint(std::vector{root}, sm, options));
}

TEST_CASE("Package selection resolves complete roots and reports ambiguous or missing names",
          "[frozen-source]") {
    SourceManager sm;
    auto leading = sm.assignText("setup.sv", "`define WIDTH 4\n");
    auto a = sm.assignText("a.sv", "package a; localparam int N=`WIDTH; endpackage\n");
    auto b = sm.assignText("b.sv", "package b; localparam int N=a::N; endpackage\n");
    auto user = sm.assignText("tail.sv", "module user; endmodule\n");
    auto tree = syntax::SyntaxTree::fromBuffers(std::vector{leading, a, b, user}, sm);
    auto selection = server::planFrozenSources(std::vector{tree},
                                               std::vector<std::string>{"a", "b", "missing"});
    REQUIRE(selection.units.size() == 1);
    CHECK(selection.units[0].packages == std::vector<std::string>{"a", "b"});
    CHECK(selection.units[0].frozenRootCount == 3);
    REQUIRE(selection.units[0].roots.size() == 4);
    CHECK(selection.units[0].roots[0] == sm.getFullPath(leading.id));
    CHECK(selection.selections[0].resolved);
    CHECK(selection.selections[1].resolved);
    CHECK_FALSE(selection.selections[2].resolved);
    auto duplicate = syntax::SyntaxTree::fromText("package a; endpackage\n", sm);
    auto ambiguous = server::planFrozenSources(std::vector{tree, duplicate},
                                               std::vector<std::string>{"a"});
    CHECK(ambiguous.units.empty());
    CHECK(ambiguous.selections[0].reason.find("ambiguous") != std::string::npos);
}

TEST_CASE("Frozen groups require a witness and release obsolete state within their budget",
          "[frozen-source]") {
    SourceManager sm;
    auto a = sm.assignText("a.sv", "package a; localparam int N=4; endpackage\n");
    auto b = sm.assignText("b.sv", "package b; localparam int N=a::N; endpackage\n");
    auto user = sm.assignText("tail.sv", "package consumer; localparam int N=b::N; endpackage\n");
    auto roots = std::vector{a, b, user};
    auto native = syntax::SyntaxTree::fromBuffers(roots, sm);
    auto selection = server::planFrozenSources(std::vector{native},
                                               std::vector<std::string>{"a", "b"});
    REQUIRE(selection.units.size() == 1);
    const auto& plan = selection.units[0];
    bool valid = true;
    auto witness = [&](const syntax::SyntaxTree&) { return valid; };
    server::FrozenSourceRegistry registry;
    CHECK_THROWS(registry.prepare(plan, roots, sm, {}, {}));
    auto prefix = registry.prepare(plan, roots, sm, {}, witness);
    REQUIRE(prefix);
    auto weak = std::weak_ptr(prefix);
    CHECK(registry.prepare(plan, roots, sm, {}, witness) == prefix);
    auto combined = registry.resume(plan, roots, sm, {}, witness);
    REQUIRE(combined);
    ast::Compilation compilation;
    compilation.addSyntaxTree(combined);
    CHECK(compilation.getAllDiagnostics().empty());
    CHECK(packageConstant(compilation, "consumer", "N") == 4);
    CHECK(registry.statistics().hits == 2);
    CHECK(registry.statistics().preparations == 1);
    CHECK(registry.statistics().retainedBytes <= 64 * 1024 * 1024);
    valid = false;
    CHECK_FALSE(registry.prepare(plan, roots, sm, {}, witness));
    CHECK(registry.statistics().invalidations == 1);
    CHECK(registry.statistics().retainedBytes == 0);
    CHECK_FALSE(weak.expired()); // The currently displayed compilation still owns its syntax.
    server::FrozenSourceRegistry noBudget(0);
    valid = true;
    CHECK_FALSE(noBudget.prepare(plan, roots, sm, {}, witness));
    CHECK(noBudget.statistics().retainedBytes == 0);
    registry.clear("manual refresh");
    CHECK(registry.statistics().lastReason == "manual refresh");
}

TEST_CASE("Frozen input groups retain include guards across the boundary", "[frozen-source]") {
    SourceManager sm;
    auto directory = std::filesystem::current_path() / "frozen-checkpoint-in-memory";
    sm.assignText((directory / "shared.svh").string(),
                  "`ifndef SHARED_GUARD\n`define SHARED_GUARD\n"
                  "package common; localparam int VALUE=6; endpackage\n`endif\n");
    auto a = sm.assignText((directory / "a.sv").string(),
                           "`include \"shared.svh\"\npackage a; endpackage\n");
    auto b = sm.assignText((directory / "b.sv").string(),
                           "`include \"shared.svh\"\npackage b; endpackage\n");
    auto user = sm.assignText((directory / "tail.sv").string(),
                              "`include \"shared.svh\"\npackage consumer; "
                              "localparam int N=common::VALUE; endpackage\n");
    auto prefix = syntax::SyntaxTree::fromBuffersWithCheckpoint(std::vector{a, b}, sm);
    auto suffix = syntax::SyntaxTree::afterCachedPrefix(prefix, std::vector{user}, {});
    auto combined = syntax::SyntaxTree::withCachedPrefix(prefix, suffix);
    ast::Compilation compilation;
    compilation.addSyntaxTree(combined);
    CHECK(compilation.getAllDiagnostics().empty());
    CHECK(packageConstant(compilation, "consumer", "N") == 6);
    auto selected = server::planFrozenSources(std::vector{combined},
                                              std::vector<std::string>{"common", "b"});
    REQUIRE(selected.units.size() == 1);
    CHECK(selected.units[0].frozenRootCount == 2);
    CHECK(selected.selections[0].resolved);
}

TEST_CASE("Frozen registry isolates macro options and evicts unreferenced input groups",
          "[frozen-source]") {
    SourceManager sm;
    auto a = sm.assignText("library.sv", "package selected; localparam int N=`VALUE; endpackage\n");
    auto user = sm.assignText("consumer.sv",
                              "package consumer; localparam int N=selected::N; endpackage\n");
    parsing::PreprocessorOptions pp;
    pp.predefines = {"VALUE=1"};
    Bag one(pp);
    auto roots = std::vector{a, user};
    auto native = syntax::SyntaxTree::fromBuffers(roots, sm, one);
    auto selected = server::planFrozenSources(std::vector{native},
                                              std::vector<std::string>{"selected"});
    REQUIRE(selected.units.size() == 1);
    const auto& plan = selected.units[0];
    auto proven = [](const syntax::SyntaxTree&) { return true; };
    server::FrozenSourceRegistry registry;
    auto first = registry.prepare(plan, roots, sm, one, proven);
    REQUIRE(first);
    const auto charge = registry.statistics().retainedBytes;
    pp.predefines = {"VALUE=2"};
    Bag two(pp);
    auto second = registry.resume(plan, roots, sm, two, proven);
    REQUIRE(second);
    ast::Compilation compilation;
    compilation.addSyntaxTree(second);
    CHECK(compilation.getAllDiagnostics().empty());
    CHECK(packageConstant(compilation, "consumer", "N") == 2);
    CHECK(registry.statistics().preparations == 2);
    server::FrozenSourceRegistry bounded(charge + 128);
    auto unreferenced = bounded.prepare(plan, roots, sm, one, proven);
    REQUIRE(unreferenced);
    auto weak = std::weak_ptr(unreferenced);
    unreferenced.reset();
    REQUIRE(bounded.prepare(plan, roots, sm, two, proven));
    CHECK(weak.expired());
    CHECK(bounded.statistics().evictions == 1);
    CHECK(bounded.statistics().retainedBytes <= charge + 128);
    auto swapped = roots;
    std::swap(swapped[0], swapped[1]);
    CHECK_THROWS(registry.prepare(plan, swapped, sm, one, proven));
}

TEST_CASE("Literal macro overrides decline before prefix certification and repair normally",
          "[frozen-source][frozen-preflight]") {
    const bool nativeSeed = GENERATE(false, true);
    const std::string directive = GENERATE(std::string("`undef WIDTH\n`define WIDTH 9\n"),
                                           std::string("`define WIDTH 9\n"),
                                           std::string("`undefineall\n`define WIDTH 9\n"));
    SourceManager sm;
    auto directory = std::filesystem::current_path() / "macro-preflight-memory";
    auto library = sm.assignText((directory / "uvm_pkg.sv").string(),
                                 "`define WIDTH 4\npackage uvm_pkg; endpackage\n");
    auto middle = sm.assignText((directory / "middle.sv").string(), "package middle; endpackage\n");
    const std::string consumer = "package consumer; localparam int N=`WIDTH; endpackage\n";
    auto user = sm.assignText((directory / "consumer.sv").string(), consumer);
    std::vector roots{library, middle, user};
    parsing::PreprocessorOptions pp;
    pp.captureUvmPrefix = true;
    Bag options(pp);
    auto native = syntax::SyntaxTree::fromBuffers(roots, sm, options);
    auto selections = server::planFrozenSources(std::vector{native},
                                                std::vector<std::string>{"uvm_pkg", "middle"});
    REQUIRE(selections.units.size() == 1);
    const auto& plan = selections.units[0];
    server::FrozenSourceRegistry registry;
    size_t witnesses = 0;
    auto witness = [&](const syntax::SyntaxTree&) {
        ++witnesses;
        return true;
    };
    if (nativeSeed) {
        auto seed = syntax::SyntaxTree::preparedUvmPrefix(native);
        REQUIRE(seed);
        auto seedPlan = plan;
        seedPlan.packages = {"uvm_pkg"};
        seedPlan.frozenRootCount = 1;
        REQUIRE(registry.offerNativePrefix(seedPlan, roots, sm, options, seed));
    }
    else
        REQUIRE(registry.prepare(plan, roots, sm, options, witness));
    auto replace = [&](const std::string& text) {
        SmallVector<char> bytes(text.begin(), text.end());
        bytes.push_back('\0');
        roots.back() = sm.replaceBuffer(roots.back().id, std::move(bytes));
    };
    replace(directive + consumer);
    witnesses = 0;
    CHECK_FALSE(registry.resume(plan, roots, sm, options, witness));
    CHECK(witnesses == 0); // Native decline cannot publish a reused tree.
    CHECK(registry.statistics().preflightDeclines == 1);
    CHECK(registry.statistics().resumes == 0);
    if (nativeSeed)
        CHECK(registry.statistics().preparations == 0);
    ast::Compilation changed;
    changed.addSyntaxTree(syntax::SyntaxTree::fromBuffers(roots, sm, options));
    CHECK(packageConstant(changed, "consumer", "N") == 9);
    replace(consumer);
    auto repaired = registry.resume(plan, roots, sm, options, witness);
    REQUIRE(repaired);
    CHECK(witnesses > 0);
    CHECK(registry.statistics().resumes == 1);
    ast::Compilation fixed;
    fixed.addSyntaxTree(repaired);
    CHECK(fixed.getAllDiagnostics().empty());
    CHECK(packageConstant(fixed, "consumer", "N") == 4);

    // A literal match in a comment is only a conservative native miss. Indirect
    // changes absent from the root text still undergo the existing validation.
    replace("// `undef WIDTH\n" + consumer);
    CHECK_FALSE(registry.resume(plan, roots, sm, options, witness));
    ast::Compilation comment;
    comment.addSyntaxTree(syntax::SyntaxTree::fromBuffers(roots, sm, options));
    CHECK(packageConstant(comment, "consumer", "N") == 4);
    sm.assignText((directory / "override.svh").string(), "`undef WIDTH\n`define WIDTH 11\n");
    replace("`include \"override.svh\"\n" + consumer);
    CHECK_THROWS(registry.resume(plan, roots, sm, options, witness));
    ast::Compilation indirect;
    indirect.addSyntaxTree(syntax::SyntaxTree::fromBuffers(roots, sm, options));
    CHECK(packageConstant(indirect, "consumer", "N") == 11);
}

#include "utils/ServerHarness.h"
#include <fstream>
#include <rfl/from_generic.hpp>
#include <rfl/json.hpp>

#include "slang/util/OS.h"
#include "slang/util/ScopeGuard.h"

namespace {
struct FrozenWorkspace {
    std::filesystem::path saved = std::filesystem::current_path();
    inline static unsigned serial = 0;
    std::filesystem::path root = std::filesystem::weakly_canonical(
                                     std::filesystem::temp_directory_path()) /
                                 fmt::format("slang-frozen-input-{}-{}", OS::getpid(), serial++);
    std::string consumer = "package consumer; localparam int OFFSET=1; "
                           "localparam int RESULT=b::WIDTH+OFFSET; endpackage\n";
    FrozenWorkspace(bool enabled = true) {
        std::filesystem::create_directories(root);
        write("defs.svh", "`define WIDTH 4\n");
        write("a.sv",
              "`include \"defs.svh\"\npackage a; localparam int WIDTH=`WIDTH; endpackage\n");
        write("b.sv", "package b; localparam int WIDTH=a::WIDTH; endpackage\n");
        write("user.sv", consumer);
        write("design.f", "a.sv\nb.sv\nuser.sv\n");
        Config config;
        config.flags = "--single-unit --threads 1";
        config.compilationFilelist = "design.f";
        config.experimentalFrozenSources = enabled;
        Config::FrozenSourcesConfig frozen;
        frozen.packages = std::vector<std::string>{"a", "b"};
        config.frozenSources = frozen;
        write(".slang/server.json", rfl::json::write(config));
        std::filesystem::current_path(root);
    }
    ~FrozenWorkspace() {
        std::filesystem::current_path(saved);
        std::filesystem::remove_all(root);
    }
    void write(const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories((root / path).parent_path());
        std::ofstream out(root / path, std::ios::binary);
        REQUIRE(out.good());
        out << text;
    }
    lsp::InitializeParams initialize() {
        return {.workspaceFolders = {
                    {lsp::WorkspaceFolder{.uri = URI::fromFile(root), .name = "frozen"}}}};
    }
};
class FrozenHarness : public ServerHarness {
    int next = 1;

public:
    using ServerHarness::ServerHarness;
    rfl::Generic command(std::string name, std::vector<rfl::Generic> arguments = {},
                         std::optional<int> rpcId = std::nullopt) {
        auto result = processMessage({.id = rpcId ? *rpcId : next++,
                                      .method = "workspace/executeCommand",
                                      .params = rfl::to_generic(lsp::ExecuteCommandParams{
                                          .command = name, .arguments = std::move(arguments)})});
        INFO(name);
        if (auto* error = std::get_if<lsp::RpcError>(&result))
            INFO(error->message);
        REQUIRE(std::holds_alternative<rfl::Generic>(result));
        return std::get<rfl::Generic>(result);
    }
    server::FrozenSourceRegistry::Statistics statistics() {
        auto root = command("slang.frozenSourcesStatus").to_object().value();
        auto sources = root.at("sources").to_object().value();
        return rfl::from_generic<server::FrozenSourceRegistry::Statistics>(sources.at("statistics"))
            .value();
    }
};
int currentResult(FrozenHarness& server, FrozenWorkspace& files) {
    auto document = server.m_driver->getDocument(URI::fromFile(files.root / "user.sv"));
    REQUIRE(document);
    auto* compilation = document->getAnalysis()->getCompilation().get();
    return packageConstant(*compilation, "consumer", "RESULT");
}
} // namespace

TEST_CASE("Configured frozen packages actually reuse across unsaved edits and refresh",
          "[frozen-source-integration]") {
    const bool enabled = GENERATE(false, true);
    FrozenWorkspace files(enabled);
    FrozenHarness server(files.initialize());
    auto document = server.openFile("user.sv");
    CHECK(currentResult(server, files) == 5);
    for (int offset : {2, 3, 4}) {
        auto text = files.consumer;
        text.replace(text.find("OFFSET=1"), 8, "OFFSET=" + std::to_string(offset));
        document.replaceAll(text);
        document.publishChanges();
        CHECK(currentResult(server, files) == 4 + offset);
    }
    auto statistics = server.statistics();
    CHECK(statistics.preparations == (enabled ? 1 : 0));
    if (enabled) {
        CHECK(statistics.hits >= 2);
        CHECK(statistics.retainedBytes > 0);
    }
    server.command("slang.refreshFrozenSources");
    CHECK(server.statistics().retainedBytes == 0);
    CHECK(currentResult(server, files) == 8);
}

TEST_CASE("Unwatched disk dependencies rebuild the native context and preserve user overlays",
          "[frozen-source-integration]") {
    FrozenWorkspace files;
    FrozenHarness server(files.initialize());
    auto document = server.openFile("user.sv");
    document.replaceAll("package consumer; localparam int RESULT=b::WIDTH+3; endpackage\n");
    document.publishChanges();
    CHECK(currentResult(server, files) == 7);
    files.write("defs.svh", "`define WIDTH 6\n");
    server.command("slang.frozenSourcesStatus");
    auto current = server.m_driver->getDocument(URI::fromFile(files.root / "user.sv"));
    CHECK(current->getText().find("WIDTH+3") != std::string::npos);
    CHECK(currentResult(server, files) == 9);
    auto library = server.openFile("a.sv");
    library.replaceAll("package a; localparam int WIDTH=8; endpackage\n");
    library.publishChanges();
    CHECK(currentResult(server, files) == 11);
    files.write("a.sv", "package a; localparam int WIDTH=9; endpackage\n");
    server.command("slang.frozenSourcesStatus");
    CHECK(currentResult(server, files) == 11);
    library.close();
    server.command("slang.frozenSourcesStatus");
    CHECK(currentResult(server, files) == 12);
}

TEST_CASE("Frozen certification detects new higher priority includes without watcher events",
          "[frozen-source-integration]") {
    FrozenWorkspace files;
    std::filesystem::create_directories(files.root / "first");
    files.write("second/defs.svh", "`define WIDTH 4\n");
    std::filesystem::remove(files.root / "defs.svh");
    files.write("design.f", "+incdir+first\n+incdir+second\na.sv\nb.sv\nuser.sv\n");
    FrozenHarness server(files.initialize());
    auto doc = server.openFile("user.sv");
    doc.append("// unsaved user text\n");
    doc.publishChanges();
    CHECK(currentResult(server, files) == 5);
    files.write("first/defs.svh", "`define WIDTH 7\n");
    server.command("slang.frozenSourcesStatus");
    CHECK(currentResult(server, files) == 8);
    auto current = server.m_driver->getDocument(URI::fromFile(files.root / "user.sv"));
    CHECK(current->getText().find("unsaved user text") != std::string::npos);
}

TEST_CASE("Frozen witnesses detect source and include-directory symlink retargeting",
          "[frozen-source-integration]") {
    const bool includeDirectory = GENERATE(false, true);
    FrozenWorkspace files;
    files.write("one/a.sv",
                "`include \"defs.svh\"\npackage a; localparam int WIDTH=`WIDTH; endpackage\n");
    files.write("two/a.sv",
                "`include \"defs.svh\"\npackage a; localparam int WIDTH=`WIDTH; endpackage\n");
    files.write("one/defs.svh", "`define WIDTH 4\n");
    files.write("two/defs.svh", "`define WIDTH 8\n");
    std::filesystem::create_directory_symlink(files.root / "one", files.root / "link");
    if (includeDirectory) {
        std::filesystem::remove(files.root / "defs.svh");
        files.write("design.f", "+incdir+link\na.sv\nb.sv\nuser.sv\n");
    }
    else
        files.write("design.f", "link/a.sv\nb.sv\nuser.sv\n");
    FrozenHarness server(files.initialize());
    auto document = server.openFile("user.sv");
    document.append("// unsaved\n");
    document.publishChanges();
    CHECK(currentResult(server, files) == 5);
    std::filesystem::remove(files.root / "link");
    std::filesystem::create_directory_symlink(files.root / "two", files.root / "link");
    server.command("slang.frozenSourcesStatus");
    CHECK(currentResult(server, files) == 9);
}

TEST_CASE("Frozen inputs preserve a deleted open root while rebuilding another dependency",
          "[frozen-source-integration]") {
    FrozenWorkspace files;
    FrozenHarness server(files.initialize());
    auto document = server.openFile("user.sv");
    auto library = server.openFile("a.sv");
    library.replaceAll("package a; localparam int WIDTH=8; endpackage\n");
    library.publishChanges();
    CHECK(currentResult(server, files) == 9);
    std::filesystem::remove(files.root / "a.sv");
    server.command("slang.frozenSourcesStatus");
    CHECK(currentResult(server, files) == 9);
    files.write("b.sv", "package b; localparam int WIDTH=a::WIDTH+1; endpackage\n");
    server.command("slang.frozenSourcesStatus");
    CHECK(currentResult(server, files) == 10);
    auto current = server.m_driver->getDocument(URI::fromFile(files.root / "a.sv"));
    REQUIRE(current);
    CHECK(current->getText().find("WIDTH=8") != std::string::npos);
}

TEST_CASE("Frozen certification reads changed bytes even if size and timestamp are restored",
          "[frozen-source-integration]") {
    FrozenWorkspace files;
    FrozenHarness server(files.initialize());
    auto document = server.openFile("user.sv");
    auto stamp = std::filesystem::last_write_time(files.root / "defs.svh");
    auto size = std::filesystem::file_size(files.root / "defs.svh");
    files.write("defs.svh", "`define WIDTH 9\n");
    std::filesystem::last_write_time(files.root / "defs.svh", stamp);
    REQUIRE(std::filesystem::file_size(files.root / "defs.svh") == size);
    server.command("slang.frozenSourcesStatus");
    CHECK(currentResult(server, files) == 10);
}

TEST_CASE("Frozen independent lookup keeps its original macro context within the shared budget",
          "[frozen-source-integration]") {
    FrozenWorkspace files;
    files.write("setup.sv", "`define FROM_PRELUDE 1\n");
    files.write("a.sv", "package a;\n`ifdef FROM_PRELUDE\nlocalparam int WIDTH=7;\n"
                        "`else\nlocalparam int WIDTH=4;\n`endif\nendpackage\n");
    files.write("design.f", "setup.sv\na.sv\nb.sv\nuser.sv\n");
    FrozenHarness server(files.initialize());
    auto document = server.openFile("user.sv");
    CHECK(currentResult(server, files) == 8);
    auto independent = server.m_driver->getFrozenIndependentTree((files.root / "a.sv").string(),
                                                                 server.m_driver->options);
    REQUIRE(independent);
    ast::Compilation compilation;
    compilation.addSyntaxTree(independent);
    CHECK(packageConstant(compilation, "a", "WIDTH") == 4);
    CHECK(server.m_driver->getFrozenIndependentTree((files.root / "a.sv").string(),
                                                    server.m_driver->options) == independent);
    CHECK(currentResult(server, files) == 8);
    auto statistics = server.statistics();
    CHECK(statistics.independentPreparations == 1);
    CHECK(statistics.independentHits == 1);
    CHECK(statistics.retainedBytes <= 64 * 1024 * 1024);
}

TEST_CASE(
    "Frozen configured package renames update selection status and recover without manual unfreeze",
    "[frozen-source-integration]") {
    FrozenWorkspace files;
    FrozenHarness server(files.initialize());
    auto consumer = server.openFile("user.sv");
    consumer.append("// first edit\n");
    consumer.publishChanges();
    auto library = server.openFile("b.sv");
    library.replaceAll("package renamed; localparam int WIDTH=a::WIDTH; endpackage\n");
    library.publishChanges();
    auto status = server.command("slang.frozenSourcesStatus").to_object().value();
    auto source = status.at("sources").to_object().value();
    auto selections = rfl::from_generic<std::vector<server::FrozenSelectionStatus>>(
                          source.at("selections"))
                          .value();
    REQUIRE(selections.size() == 2);
    CHECK(selections[0].resolved);
    CHECK_FALSE(selections[1].resolved);
    library.replaceAll("package b; localparam int WIDTH=a::WIDTH+1; endpackage\n");
    library.publishChanges();
    CHECK(currentResult(server, files) == 6);
    source = server.command("slang.frozenSourcesStatus")
                 .to_object()
                 .value()
                 .at("sources")
                 .to_object()
                 .value();
    selections = rfl::from_generic<std::vector<server::FrozenSelectionStatus>>(
                     source.at("selections"))
                     .value();
    CHECK(selections[1].resolved);
}

TEST_CASE("Frozen certification reloads changed filelists and effective JSON configuration",
          "[frozen-source-integration]") {
    const bool configuration = GENERATE(false, true);
    FrozenWorkspace files;
    FrozenHarness server(files.initialize());
    auto document = server.openFile("user.sv");
    document.append("// retained overlay\n");
    document.publishChanges();
    if (configuration) {
        std::ifstream input(files.root / ".slang/server.json");
        auto text = std::string(std::istreambuf_iterator<char>(input), {});
        auto config = rfl::json::read<Config>(text).value();
        config.experimentalFrozenSources = false;
        files.write(".slang/server.json", rfl::json::write(config));
    }
    else {
        files.write("replacement.sv", "package b; localparam int WIDTH=a::WIDTH+2; endpackage\n");
        files.write("design.f", "a.sv\nreplacement.sv\nuser.sv\n");
    }
    server.command("slang.frozenSourcesStatus");
    CHECK(server.m_driver->isFrozenSourcesEnabled() == !configuration);
    CHECK(currentResult(server, files) == (configuration ? 5 : 7));
    auto current = server.m_driver->getDocument(URI::fromFile(files.root / "user.sv"));
    CHECK(current->getText().find("retained overlay") != std::string::npos);
}

TEST_CASE("Frozen source configuration is opt in and zero budget preserves native analysis",
          "[frozen-source-integration]") {
    CHECK_FALSE(Config{}.experimentalFrozenSources.value());
    CHECK(Config::FrozenSourcesConfig{}.maxMiB.value() == 256);
    Config config;
    Config::FrozenSourcesConfig selection;
    selection.packages = std::vector<std::string>{"a", "b"};
    selection.maxMiB = 0;
    config.frozenSources = selection;
    auto decoded = rfl::json::read<Config>(rfl::json::write(config));
    REQUIRE(decoded);
    CHECK_FALSE(decoded->experimentalFrozenSources.value());
    CHECK(decoded->frozenSources.value()->packages.value() == std::vector<std::string>{"a", "b"});
    FrozenWorkspace files;
    config.flags = "--single-unit --threads 1";
    config.compilationFilelist = "design.f";
    config.experimentalFrozenSources = true;
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness server(files.initialize());
    auto document = server.openFile("user.sv");
    document.append("// edit\n");
    document.publishChanges();
    CHECK(currentResult(server, files) == 5);
    CHECK(server.statistics().retainedBytes == 0);
}

TEST_CASE("Frozen groups retain source library identity within one source manager",
          "[frozen-source]") {
    SourceManager sm;
    SourceLibrary firstLibrary, secondLibrary;
    auto a = sm.assignText((std::filesystem::current_path() / "same.sv").string(),
                           "package p; localparam int N=1; endpackage\n", {}, &firstLibrary);
    server::FrozenSourcePlan plan{.packages = {"p"},
                                  .roots = {sm.getFullPath(a.id)},
                                  .frozenRootCount = 1};
    server::FrozenSourceRegistry registry;
    auto proof = [](const syntax::SyntaxTree&) { return true; };
    auto first = registry.prepare(plan, std::vector{a}, sm, {}, proof);
    REQUIRE(first);
    auto secondBuffer = sm.readSource(sm.getFullPath(a.id), &secondLibrary).value();
    auto second = registry.prepare(plan, std::vector{secondBuffer}, sm, {}, proof);
    REQUIRE(second);
    CHECK(first != second);
    CHECK(first->getSourceLibrary() == &firstLibrary);
    CHECK(second->getSourceLibrary() == &secondLibrary);
}

TEST_CASE("Prepared groups pin replaced raw source until their final consumer releases it",
          "[frozen-source]") {
    SourceManager sm;
    auto buffer = sm.assignText("pinned.sv",
                                "package retained; localparam int OLD=7; endpackage\n");
    auto handles = sm.retainBuffers(std::vector{buffer.id});
    REQUIRE(handles.size() == 1);
    auto weak = std::weak_ptr<void>(handles.front());
    handles.clear();
    auto prefix = syntax::SyntaxTree::fromBuffersWithCheckpoint(std::vector{buffer}, sm);
    std::string_view text = "package replacement; endpackage\n";
    SmallVector<char> bytes(text.begin(), text.end());
    bytes.push_back('\0');
    sm.replaceBuffer(buffer.id, std::move(bytes));
    CHECK_FALSE(weak.expired());
    CHECK(prefix->root().toString().find("OLD=7") != std::string::npos);
    prefix.reset();
    CHECK(weak.expired());
}

TEST_CASE("Frozen refresh leaves native active design selections usable",
          "[frozen-source-integration]") {
    FrozenWorkspace files;
    files.write("rtl.sv", "module leaf #(parameter W=1); logic[W-1:0] value; endmodule\n"
                          "module top; leaf #(8) wide(); leaf #(2) narrow(); endmodule\n");
    files.write("design.f", "a.sv\nb.sv\nuser.sv\nrtl.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1 --top top";
    config.build = "design.f";
    config.experimentalFrozenSources = true;
    Config::FrozenSourcesConfig selected;
    selected.packages = std::vector<std::string>{"a", "b"};
    config.frozenSources = selected;
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness server(files.initialize());
    REQUIRE(server.m_driver->comp);
    REQUIRE(server.setActiveInstance("top.wide"));
    auto before = server.m_driver->comp.get();
    server.command("slang.refreshFrozenSources");
    CHECK(server.m_driver->comp.get() == before);
    CHECK(server.statistics().retainedBytes == 0);
    auto active = server.getActiveInstance("leaf");
    REQUIRE(active);
    CHECK(active->instPath == "top.wide");
    // A semantic consumer can now prepare a new syntax group for the design.
    CHECK(server.statistics().retainedBytes <= 64 * 1024 * 1024);
}

TEST_CASE("Unwitnessable contexts discard composed views and actually return to native parsing",
          "[frozen-source-integration]") {
    FrozenWorkspace files;
    FrozenHarness server(files.initialize());
    auto document = server.openFile("user.sv");
    document.append("// prepare group\n");
    document.publishChanges();
    auto before = server.statistics();
    REQUIRE(before.retainedBytes > 0);
    std::filesystem::create_directories(files.root / "late");
    REQUIRE_FALSE(server.m_driver->sm.addUserDirectories((files.root / "late").string()));
    auto status = server.command("slang.frozenSourcesStatus").to_object().value();
    auto source = status.at("sources").to_object().value();
    CHECK(source.at("mode").to_string().value() == "native: input witness unavailable");
    CHECK(server.statistics().retainedBytes == 0);
    document.append("// native edit\n");
    document.publishChanges();
    CHECK(currentResult(server, files) == 5);
    auto after = server.statistics();
    CHECK(after.preparations == before.preparations);
    CHECK(after.resumes == before.resumes);
}

TEST_CASE("Reference searches exclude indexed files outside complete compilation inputs",
          "[frozen-source-integration][explicit-references]") {
    const bool enabled = GENERATE(false, true);
    FrozenWorkspace files(enabled);
    files.write("b.sv", "package b; class Item; int field; endclass endpackage\n");
    files.write("user.sv", "package consumer; import b::*; Item object; "
                           "function int read(); return object.field; endfunction endpackage\n");
    files.write("omitted.sv", "package omitted; import b::*; Item other; "
                              "function int read(); return other.field; endfunction endpackage\n");
    FrozenHarness server(files.initialize());
    auto document = server.openFile("user.sv");
    auto at = document.before("field;");
    auto refs = server.getDocReferences(
        lsp::ReferenceParams{.context = {.includeDeclaration = true},
                             .textDocument = {.uri = URI::fromFile(files.root / "user.sv")},
                             .position = at.getPosition()});
    REQUIRE(refs);
    CHECK(std::ranges::any_of(*refs, [&](const auto& ref) {
        return ref.uri == URI::fromFile(files.root / "b.sv");
    }));
    CHECK(std::ranges::any_of(*refs, [&](const auto& ref) {
        return ref.uri == URI::fromFile(files.root / "user.sv");
    }));
    CHECK(std::ranges::none_of(*refs, [&](const auto& ref) {
        return ref.uri == URI::fromFile(files.root / "omitted.sv");
    }));
}

TEST_CASE("Frozen groups retain parser warnings without treating them as syntax recovery",
          "[frozen-source]") {
    SourceManager sm;
    auto library = sm.assignText("warning.sv", "package p; function void f(); int i; "
                                               "for(i=0;i<1;i++); endfunction endpackage\n");
    auto user = sm.assignText("consumer.sv", "module user; initial p::f(); endmodule\n");
    auto native = syntax::SyntaxTree::fromBuffers(std::vector{library, user}, sm);
    REQUIRE_FALSE(native->diagnostics().empty());
    REQUIRE(std::ranges::none_of(native->diagnostics(), [](const auto& d) { return d.isError(); }));
    auto plans = server::planFrozenSources(std::vector{native}, std::vector<std::string>{"p"});
    REQUIRE(plans.units.size() == 1);
    server::FrozenSourceRegistry registry;
    auto combined = registry.resume(plans.units[0], std::vector{library, user}, sm, {},
                                    [](const syntax::SyntaxTree&) { return true; });
    REQUIRE(combined);
    CHECK(combined->diagnostics().size() == native->diagnostics().size());
    CHECK(combined->diagnostics()[0].code == native->diagnostics()[0].code);
    CHECK(combined->diagnostics()[0].location == native->diagnostics()[0].location);
}

TEST_CASE("Whole-group preparation preserves worker capture options without redundant checkpoints",
          "[frozen-source]") {
    SourceManager sm;
    auto uvm = sm.assignText("uvm_pkg.sv", "package uvm_pkg; endpackage\n");
    auto selected = sm.assignText("selected.sv", "package selected; endpackage\n");
    parsing::PreprocessorOptions pp;
    pp.captureUvmPrefix = true;
    Bag options(pp);
    auto group = syntax::SyntaxTree::fromBuffersWithCheckpoint(std::vector{uvm, selected}, sm,
                                                               options);
    CHECK(group->options().getOrDefault<parsing::PreprocessorOptions>().captureUvmPrefix);
    CHECK(group->isPrefixResumeSafe().value_or(false));
    CHECK_FALSE(syntax::SyntaxTree::preparedUvmPrefix(group));
}

TEST_CASE("An initial native UVM arena extends into a resumable multi-package prefix",
          "[frozen-source][frozen-native-seed]") {
    SourceManager sm;
    auto dir = std::filesystem::current_path() / "native-seed-memory";
    sm.assignText((dir / "shared.svh").string(),
                  "`ifndef SHARED_NATIVE_GUARD\n`define SHARED_NATIVE_GUARD\n"
                  "class base; int value; endclass\n`endif\n");
    auto a = sm.assignText((dir / "uvm_pkg.sv").string(),
                           "`define OFFSET 3\npackage uvm_pkg;\n"
                           "`include \"shared.svh\"\n"
                           "localparam int VALUE=`OFFSET; endpackage\n`define TAIL_VALUE 7\n");
    auto b = sm.assignText((dir / "middle.sv").string(),
                           "`include \"shared.svh\"\n`undef OFFSET\n`define OFFSET 5\n"
                           "package middle; localparam int VALUE=`OFFSET+`CLI; endpackage\n");
    auto user = sm.assignText((dir / "user.sv").string(),
                              "`include \"shared.svh\"\npackage consumer; "
                              "localparam int RESULT=uvm_pkg::VALUE+middle::VALUE+"
                              "`OFFSET+`TAIL_VALUE; endpackage\n");
    parsing::PreprocessorOptions pp;
    pp.captureUvmPrefix = true;
    pp.predefines = {"CLI=2"};
    Bag options(pp);
    auto native = syntax::SyntaxTree::fromBuffers(std::vector{a, b, user}, sm, options);
    std::weak_ptr<syntax::SyntaxTree> nativeOwner = native;
    auto* original = native->root().as<syntax::CompilationUnitSyntax>().members[0];
    auto* parent = original->parent.get();
    auto seed = syntax::SyntaxTree::preparedUvmPrefix(native);
    REQUIRE(seed);
    auto ancestry = [](const syntax::SyntaxNode& root) {
        using Node = const syntax::SyntaxNode*;
        std::vector<std::tuple<Node, Node, Node>> result;
        auto visit = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
            result.emplace_back(&node, node.parent.get(), node.previewNode());
            auto trivia = [&](auto&& recur, std::span<const parsing::Trivia> values) -> void {
                for (const auto& value : values) {
                    if (value.kind == parsing::TriviaKind::Directive)
                        self(self, *value.syntax());
                    else if (value.kind == parsing::TriviaKind::SkippedTokens)
                        for (auto token : value.getSkippedTokens())
                            recur(recur, token.trivia());
                }
            };
            for (size_t i = 0; i < node.getChildCount(); ++i) {
                if (auto child = node.childNode(i))
                    self(self, *child);
                else if (auto token = node.childToken(i); token)
                    trivia(trivia, token.trivia());
            }
        };
        visit(visit, root);
        return result;
    };
    const auto nativeParents = ancestry(native->root());
    const auto seedParents = ancestry(seed->root());
    auto extended = syntax::SyntaxTree::extendCachedPrefix(seed, std::vector{b}, options);
    REQUIRE(extended->isPrefixResumeSafe().value_or(false));
    CHECK(extended->root().as<syntax::CompilationUnitSyntax>().members[0] == original);
    auto suffixOptions = options;
    pp.predefines.clear();
    pp.captureUvmPrefix = false;
    suffixOptions.set(pp);
    auto suffix = syntax::SyntaxTree::afterCachedPrefix(extended, std::vector{user}, suffixOptions);
    auto combined = syntax::SyntaxTree::withCachedPrefix(extended, suffix);
    {
        ast::Compilation reference, reused;
        reference.addSyntaxTree(native);
        reused.addSyntaxTree(combined);
        CHECK(reference.getAllDiagnostics().empty());
        CHECK(reused.getAllDiagnostics().empty());
        CHECK(packageConstant(reused, "consumer", "RESULT") == 22);
        CHECK(packageConstant(reference, "consumer", "RESULT") == 22);
    }
    CHECK(std::ranges::equal(native->getSourceBufferIds(), combined->getSourceBufferIds()));
    CHECK(original->parent.get() == parent);
    CHECK(ancestry(native->root()) == nativeParents);
    CHECK(ancestry(seed->root()) == seedParents);
    native.reset();
    CHECK_FALSE(nativeOwner.expired());
    auto replacement = std::string("package consumer; localparam int RESULT=99; endpackage\n");
    SmallVector<char> bytes(replacement.begin(), replacement.end());
    bytes.push_back('\0');
    sm.replaceBuffer(user.id, std::move(bytes));
    CHECK(server::hasValidBuffers(sm, extended));
    CHECK_FALSE(server::hasValidBuffers(sm, combined));
    CHECK(original->parent.get() == parent);
    combined.reset();
    suffix.reset();
    extended.reset();
    seed.reset();
    CHECK(nativeOwner.expired());
}

TEST_CASE("Staged native prefixes require a current witness and exact ordered context",
          "[frozen-source][frozen-native-seed]") {
    SourceManager sm;
    auto dir = std::filesystem::current_path() / "generic-native-seed";
    auto a = sm.assignText((dir / "a.sv").string(), "package a; localparam int N=3; endpackage\n");
    auto b = sm.assignText((dir / "b.sv").string(),
                           "package b; localparam int N=a::N+1; endpackage\n");
    auto user = sm.assignText((dir / "user.sv").string(),
                              "package consumer; localparam int N=b::N; endpackage\n");
    auto roots = std::vector{a, b, user};
    auto seed = syntax::SyntaxTree::fromBuffersWithCheckpoint(std::vector{a}, sm);
    server::FrozenSourcePlan partial{.packages = {"a"},
                                     .roots = {sm.getFullPath(a.id), sm.getFullPath(b.id),
                                               sm.getFullPath(user.id)},
                                     .frozenRootCount = 1};
    auto full = partial;
    full.packages = {"a", "b"};
    full.frozenRootCount = 2;
    server::FrozenSourceRegistry registry;
    REQUIRE(registry.offerNativePrefix(partial, roots, sm, {}, seed));
    CHECK(registry.statistics().nativeSeeds == 1);
    CHECK(registry.statistics().preparations == 0);
    CHECK(registry.statistics().nativeSeedUses == 0);
    CHECK_THROWS(registry.prepare(full, roots, sm, {}, {}));
    CHECK_FALSE(registry.prepare(full, roots, sm, {}, [](const auto&) { return false; }));
    CHECK(registry.statistics().nativeSeedUses == 0);
    auto composed = registry.prepare(full, roots, sm, {}, [](const auto&) { return true; });
    REQUIRE(composed);
    CHECK(composed->root().as<syntax::CompilationUnitSyntax>().members[0] ==
          seed->root().as<syntax::CompilationUnitSyntax>().members[0]);
    CHECK(registry.statistics().nativeSeedUses == 1);
    CHECK(registry.statistics().preparations == 1);
    ast::Compilation compilation;
    compilation.addSyntaxTree(composed);
    CHECK(compilation.getAllDiagnostics().empty());
    CHECK(packageConstant(compilation, "b", "N") == 4);
    auto sorted = sm.readSource(sm.getFullPath(a.id), nullptr, 17);
    REQUIRE(sorted);
    auto changed = roots;
    changed[0] = *sorted;
    server::FrozenSourceRegistry rejected;
    CHECK_FALSE(rejected.offerNativePrefix(partial, changed, sm, {}, seed));
    auto separate = registry.prepare(full, changed, sm, {}, [](const auto&) { return true; });
    REQUIRE(separate);
    CHECK(separate->root().as<syntax::CompilationUnitSyntax>().members[0] !=
          seed->root().as<syntax::CompilationUnitSyntax>().members[0]);
    CHECK(registry.statistics().nativeSeedUses == 1);
    server::FrozenSourceRegistry disabled(0);
    CHECK_FALSE(disabled.offerNativePrefix(partial, roots, sm, {}, seed));
    CHECK(disabled.statistics().retainedBytes == 0);
}

TEST_CASE("The driver uses its initial UVM nodes on the first ordinary edit",
          "[frozen-source-integration][frozen-native-seed]") {
    FrozenWorkspace files;
    files.write("uvm_pkg.sv", "package uvm_pkg; class base; int value; endclass endpackage\n");
    files.write("design.f", "uvm_pkg.sv\na.sv\nb.sv\nuser.sv\n");
    FrozenHarness server(files.initialize());
    REQUIRE(server.m_driver->driver.syntaxTrees.size() == 1);
    auto native = server.m_driver->driver.syntaxTrees.front();
    auto* node = native->root().as<syntax::CompilationUnitSyntax>().members[0];
    CHECK(server.statistics().nativeSeeds == 1);
    CHECK(server.statistics().nativeSeedUses == 0);
    auto uri = URI::fromFile(files.root / "user.sv");
    server.m_driver->openDocument(uri, files.consumer, 1, true);
    CHECK(currentResult(server, files) == 5);
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = 2, .uri = uri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
             .text = files.consumer + "// first unsaved edit\n"}}});
    CHECK(currentResult(server, files) == 5);
    auto actual = server.m_driver->getDocument(URI::fromFile(files.root / "user.sv"));
    REQUIRE(actual->getCachedAnalysisTree());
    CHECK(actual->getCachedAnalysisTree()->root().as<syntax::CompilationUnitSyntax>().members[0] ==
          node);
    CHECK(server.statistics().nativeSeedUses == 1);
    auto snapshot = server.m_driver->captureDiagnosticSnapshot({});
    REQUIRE(snapshot);
    for (const auto& options : snapshot->input->treeOptions) {
        REQUIRE(options.preprocessor);
        CHECK_FALSE(options.preprocessor->captureUvmPrefix);
    }
}

TEST_CASE("Module selections retain whole roots and distinguish package names",
          "[frozen-module][frozen-source]") {
    SourceManager sm;
    auto setup = sm.assignText("setup.sv", "package shared; localparam int N=8; endpackage\n");
    auto module = sm.assignText("module.sv",
                                "module selected; logic[shared::N-1:0] x; endmodule\n");
    auto tail = sm.assignText("tail.sv", "module top; selected u(); endmodule\n");
    auto tree = syntax::SyntaxTree::fromBuffers(std::vector{setup, module, tail}, sm);
    auto plans = server::planFrozenSources(std::vector{tree}, std::vector<std::string>{"shared"},
                                           std::vector<std::string>{"selected", "shared",
                                                                    "missing"});
    REQUIRE(plans.units.size() == 1);
    CHECK(plans.units[0].packages == std::vector<std::string>{"shared"});
    CHECK(plans.units[0].modules == std::vector<std::string>{"selected"});
    CHECK(plans.units[0].frozenRootCount == 2);
    CHECK(plans.selections[1].module == "selected");
    CHECK(plans.selections[1].package.empty());
    CHECK(plans.selections[1].resolved);
    CHECK_FALSE(plans.selections[2].resolved);
    CHECK_FALSE(plans.selections[3].resolved);
    auto duplicate = syntax::SyntaxTree::fromText("module selected; endmodule\n", sm);
    auto ambiguous = server::planFrozenSources(std::vector{tree, duplicate}, {},
                                               std::vector<std::string>{"selected"});
    CHECK(ambiguous.units.empty());
    CHECK(ambiguous.selections[0].reason.find("ambiguous") != std::string::npos);
}

TEST_CASE("Frozen modules share syntax while RTL instance semantics refresh",
          "[frozen-module][frozen-source-integration]") {
    const bool enabled = GENERATE(false, true);
    CAPTURE(enabled);
    FrozenWorkspace files(enabled);
    files.write("types.sv", "package types_pkg; parameter int BASE_WIDTH=8; "
                            "typedef logic[BASE_WIDTH-1:0] word_t; endpackage\n");
    files.write("bus.sv", "interface bus_if #(parameter int WIDTH=8)(); "
                          "logic[WIDTH-1:0] data; endinterface\n");
    files.write("leaf.sv", R"(
module stable_leaf #(parameter int WIDTH=types_pkg::BASE_WIDTH)(bus_if bus);
    logic[WIDTH-1:0] payload;
    types_pkg::word_t external_word;
    initial $display(bus.data);
    if(WIDTH>8) begin:wide
        logic[WIDTH-1:0] generated_word;
    end
endmodule
)");
    const std::string topText = R"(
module top;
    bus_if #(8) small_bus();
    bus_if #(16) big_bus();
    stable_leaf #(types_pkg::BASE_WIDTH) u_small(small_bus);
    stable_leaf #(16) u_big(big_bus);
endmodule
)";
    files.write("top.sv", topText);
    files.write("design.f", "types.sv\nbus.sv\nleaf.sv\ntop.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1";
    config.experimentalFrozenSources = enabled;
    Config::FrozenSourcesConfig selections;
    selections.modules = std::vector<std::string>{"stable_leaf"};
    config.frozenSources = selections;
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness server(files.initialize());
    server.setBuildFile("design.f");
    REQUIRE(server.m_driver->comp);
    auto leaf = server.openFile("leaf.sv");
    auto top = server.openFile("top.sv");
    auto width = [&](const std::string& token) {
        auto hover = server.m_driver->getDocHover(leaf.m_uri, leaf.before(token).getPosition());
        REQUIRE(hover);
        return rfl::get<lsp::MarkupContent>(hover->contents).value;
    };
    auto checkWidth = [&](const std::string& token, int bits) {
        auto content = width(token);
        CAPTURE(token, content);
        CHECK(content.find("Width: `" + std::to_string(bits) + "`") != std::string::npos);
    };
    REQUIRE(server.setActiveInstance("top.u_small"));
    checkWidth("payload;", 8);
    checkWidth("data);", 8);
    REQUIRE(server.setActiveInstance("top.u_big"));
    checkWidth("payload;", 16);
    checkWidth("data);", 16);
    REQUIRE(server.m_driver->comp->getHierLocation("top.u_big.wide.generated_word"));
    auto currentTree = [&]() {
        auto context = server.m_driver->comp->createActiveDesignContext({});
        auto trees = context.getAnalysis().compilation.getSyntaxTrees();
        REQUIRE(trees.size() == 1);
        return trees[0];
    };
    auto moduleNode = [&](const auto& tree) {
        for (const auto& [node, meta] : tree->getMetadata().nodeMeta)
            if (node->header->name.valueText() == "stable_leaf")
                return node;
        FAIL("module missing from current compilation");
        return decltype(tree->getMetadata().nodeMeta[0].first)(nullptr);
    };
    auto changeTop = [&](int bits) {
        auto text = topText;
        for (size_t pos = 0; (pos = text.find("16", pos)) != std::string::npos;)
            text.replace(pos, 2, std::to_string(bits)), pos += std::to_string(bits).size();
        top.replaceAll(text);
        top.publishChanges();
        REQUIRE(server.m_driver->comp);
        REQUIRE(server.setActiveInstance("top.u_big"));
        checkWidth("payload;", bits);
        checkWidth("data);", bits);
    };
    changeTop(24);
    auto first = currentTree();
    auto* reusedNode = moduleNode(first);
    changeTop(4);
    auto second = currentTree();
    CHECK((moduleNode(second) == reusedNode) == enabled);
    CHECK_FALSE(server.m_driver->comp->getHierLocation("top.u_big.wide.generated_word"));
    if (enabled)
        CHECK(server.statistics().resumes >= 2);
    else
        CHECK(server.statistics().preparations == 0);
    REQUIRE(server.setActiveInstance("top.u_small"));
    checkWidth("payload;", 8);
    auto package = server.openFile("types.sv");
    package.replaceAll("package types_pkg; parameter int BASE_WIDTH=12; "
                       "typedef logic[BASE_WIDTH-1:0] word_t; endpackage\n");
    package.publishChanges();
    REQUIRE(server.setActiveInstance("top.u_small"));
    checkWidth("payload;", 12);
    checkWidth("external_word;", 12);
    CHECK(moduleNode(currentTree()) != reusedNode);
    if (enabled) {
        auto status = server.command("slang.frozenSourcesStatus").to_object().value();
        auto sources = status.at("sources").to_object().value();
        CHECK(sources.at("selections")
                  .to_array()
                  .value()[0]
                  .to_object()
                  .value()
                  .at("module")
                  .to_string()
                  .value() == "stable_leaf");
        REQUIRE(server.setActiveInstance("top.u_big"));
        auto* previousDriver = server.m_driver.get();
        files.write("bus.sv", "interface bus_if #(parameter int WIDTH=8)(); "
                              "logic[WIDTH:0] data; endinterface\n");
        server.command("slang.frozenSourcesStatus");
        REQUIRE(server.m_driver->comp);
        CHECK(server.m_driver.get() != previousDriver);
        REQUIRE(server.m_driver->comp->getActiveInstance("stable_leaf"));
        CHECK(server.m_driver->comp->getActiveInstance("stable_leaf")->instPath == "top.u_big");
        checkWidth("payload;", 4);
        checkWidth("data);", 5);
        REQUIRE(server.setActiveInstance("top.u_small"));
        checkWidth("data);", 9);
        checkWidth("payload;", 12);
        auto beforeRefreshTree = currentTree();
        auto* beforeRefresh = moduleNode(beforeRefreshTree);
        server.command("slang.refreshFrozenSources");
        REQUIRE(server.setActiveInstance("top.u_small"));
        checkWidth("payload;", 12);
        CHECK(moduleNode(currentTree()) != beforeRefresh);
        config.flags = "--single-unit --threads 1 -DNEW_CONTEXT=1";
        files.write(".slang/server.json", rfl::json::write(config));
        server.command("slang.frozenSourcesStatus");
        REQUIRE(server.m_driver->comp);
        REQUIRE(server.setActiveInstance("top.u_small"));
        checkWidth("payload;", 12);
        checkWidth("data);", 9);
        files.write("design.f", "bus.sv\ntypes.sv\nleaf.sv\ntop.sv\n");
        server.command("slang.frozenSourcesStatus");
        REQUIRE(server.m_driver->comp);
        REQUIRE(server.setActiveInstance("top.u_small"));
        checkWidth("payload;", 12);
        checkWidth("data);", 9);
    }
}

TEST_CASE("A full-design selection without an editable suffix keeps native parsing",
          "[frozen-module][frozen-source-integration][frozen-no-suffix]") {
    FrozenWorkspace files(true);
    files.write("uvm_pkg.sv", "package uvm_pkg; endpackage\n");
    files.write("top.sv", "module top; localparam int N=4; logic[N-1:0] value; endmodule\n");
    files.write("design.f", "uvm_pkg.sv\ntop.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1";
    config.experimentalFrozenSources = true;
    Config::FrozenSourcesConfig selections;
    selections.modules = std::vector<std::string>{"top"};
    config.frozenSources = selections;
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness server(files.initialize());
    server.setBuildFile("design.f");
    REQUIRE(server.m_driver->comp);
    CHECK(server.statistics().nativeSeeds == 0);
    auto top = server.openFile("top.sv");
    for (int bits : {8, 3, 12}) {
        const auto text = "module top; localparam int N=" + std::to_string(bits) +
                          "; logic[N-1:0] value; endmodule\n";
        top.replaceAll(text);
        top.publishChanges();
        auto hover = server.m_driver->getDocHover(top.m_uri,
                                                  lsp::Position{0, uint32_t(text.find("value;"))});
        REQUIRE(hover);
        CHECK(rfl::get<lsp::MarkupContent>(hover->contents)
                  .value.find("Width: `" + std::to_string(bits) + "`") != std::string::npos);
        CHECK(server.statistics().preparations == 0);
        CHECK(server.statistics().resumes == 0);
        CHECK(server.statistics().retainedBytes == 0);
    }
    CHECK(server.statistics().declines >= 3);
    CHECK(server.statistics().lastReason.find("no editable suffix") != std::string::npos);
}

TEST_CASE("Unsaved full-design diagnostics validate bind and preserve unrelated documents",
          "[frozen-module][full-design-diagnostics]") {
    const bool enabled = GENERATE(false, true);
    FrozenWorkspace files(enabled);
    files.write("leaf.sv", "module leaf; logic value; endmodule\nmodule tap; endmodule\n");
    const std::string original = "module top; leaf u(); bind leaf tap observed(); "
                                 "initial $display(u.value); endmodule\n";
    files.write("top.sv", original);
    files.write("design.f", "leaf.sv\ntop.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1 --top top";
    config.build = "design.f";
    config.experimentalFrozenSources = enabled;
    Config::FrozenSourcesConfig selected;
    selected.modules = std::vector<std::string>{"leaf"};
    config.frozenSources = selected;
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness server(files.initialize());
    auto top = server.openFile("top.sv");
    CHECK(server.client.diagnosticVersions.at(top.m_uri) == 1);
    auto extra = server.openFile("unrelated.sv", "module unrelated; absent_type x; endmodule\n");
    const auto extraDiags = rfl::json::write(extra.getDiagnostics());
    REQUIRE(extraDiags.find("absent_type") != std::string::npos);
    int version = 2;
    for (auto [bound, bad] : std::vector<std::pair<bool, bool>>{
             {true, false}, {false, false}, {true, false}, {true, true}, {true, false}}) {
        auto text = "module top; leaf u(); " +
                    std::string(bound ? "bind leaf tap observed(); " : "") +
                    (bad ? "initial missing_user_signal=1; " : "initial $display(u.value); ") +
                    "endmodule\n";
        server.SlangServer::onDocDidChange(
            {.textDocument = {.version = version, .uri = top.m_uri},
             .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
        server.m_driver->runPendingDiagnostics();
        CHECK(bool(server.m_driver->comp->getHierLocation("top.u.observed")) == bound);
        auto diags = top.getDiagnostics();
        CHECK(std::ranges::any_of(diags, [](const auto& d) {
                  return d.severity == lsp::DiagnosticSeverity::Error;
              }) == bad);
        CHECK(server.client.diagnosticVersions.at(top.m_uri) == version);
        if (enabled && bad) {
            auto* previous = server.m_driver.get();
            files.write("leaf.sv", "module leaf; logic value; endmodule\nmodule tap; endmodule\n// "
                                   "changed dependency\n");
            server.command("slang.frozenSourcesStatus");
            CHECK(server.m_driver.get() != previous);
            CHECK(std::ranges::any_of(top.getDiagnostics(), [](const auto& d) {
                return d.message.find("missing_user_signal") != std::string::npos;
            }));
            CHECK(server.client.diagnosticVersions.at(top.m_uri) == version);
        }
        ++version;
        CHECK(rfl::json::write(extra.getDiagnostics()) == extraDiags);
        CHECK(server.client.diagnosticVersions.at(extra.m_uri) == 1);
    }
}

TEST_CASE("Exact frozen instance hierarchy survives unrelated class task edits",
          "[frozen-instance][frozen-source-integration]") {
    FrozenWorkspace files;
    files.write("types.sv", "package types_pkg; parameter int WIDTH=8; endpackage\n");
    files.write("rtl.sv", R"(
module child; logic flag; endmodule
module leaf #(parameter int W=types_pkg::WIDTH);
    logic[W-1:0] payload;
    child nested();
    if(W>4) begin:wide
        logic[W-1:0] generated_word;
    end
endmodule
module tap; logic observed; endmodule
)");
    const std::string originalTop = "module top; leaf u(); leaf #(16) u2(); "
                                    "leaf array[1:0](); endmodule\n";
    files.write("top.sv", originalTop);
    const std::string driverText = "class test_driver; task drive(); $display(1); "
                                   "endtask endclass\n";
    files.write("driver.svh", driverText);
    files.write("tb.sv", "package tb_pkg; `include \"driver.svh\"\nendpackage\n");
    files.write("design.f", "types.sv\nrtl.sv\ntop.sv\ntb.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1 --top top";
    config.build = "design.f";
    Config::FrozenSourcesConfig selected;
    selected.instances = std::vector<std::string>{"top.u", "top.missing", "leaf", "top.array",
                                                  "top.array[0]"};
    selected.maxMiB = 1;
    config.frozenSources = selected;
    config.experimentalFrozenSources = false;
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness native(files.initialize());
    config.experimentalFrozenSources = true;
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness enabled(files.initialize());
    REQUIRE(native.m_driver->comp);
    REQUIRE(enabled.m_driver->comp);
    auto& comp = *enabled.m_driver->comp;
    REQUIRE(comp.frozenInstances());
    auto statistics = [&]() { return comp.frozenInstances()->statistics(); };
    const auto& selections = comp.frozenInstances()->selections();
    REQUIRE(selections.size() == 5);
    CHECK(selections[0].resolved);
    CHECK_FALSE(selections[1].resolved);
    CHECK_FALSE(selections[2].resolved);
    CHECK_FALSE(selections[3].resolved);
    CHECK(selections[4].resolved);
    CHECK(selections[0].modules == std::vector<std::string>{"child", "leaf"});
    CHECK(comp.frozenInstances()->selected("top.u.nested"));
    CHECK_FALSE(comp.frozenInstances()->selected("top.u2"));
    auto open = [&](FrozenHarness& server, const char* file, const std::string& text) {
        const auto uri = URI::fromFile(files.root / file);
        server.SlangServer::onDocDidOpen(
            {.textDocument = {
                 .uri = uri, .languageId = "systemverilog", .version = 1, .text = text}});
        return uri;
    };
    auto taskUri = open(native, "driver.svh", driverText);
    open(enabled, "driver.svh", driverText);
    auto change = [&](const URI& uri, const std::string& text, int version) {
        for (auto* server : {&native, &enabled})
            server->SlangServer::onDocDidChange(
                {.textDocument = {.version = version, .uri = uri},
                 .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
    };
    auto equalScope = [&](const std::string& path) {
        const auto actual = rfl::json::write(comp.getScope(path));
        CHECK(actual == rfl::json::write(native.m_driver->comp->getScope(path)));
        return actual;
    };
    equalScope("top.u");
    equalScope("top.u.nested");
    CHECK(rfl::json::write(comp.getScopes("top.u")) ==
          rfl::json::write(native.m_driver->comp->getScopes("top.u")));
    const auto prepared = statistics().preparations;
    REQUIRE(prepared >= 3);
    const auto beforeHit = statistics().hits;
    equalScope("top.u");
    CHECK(statistics().hits == beforeHit + 1);
    equalScope("top.u2");
    CHECK(statistics().preparations == prepared);
    int version = 2;
    for (const auto& body : {std::string("$display(22);"), std::string("#3; $display(333);"),
                             std::string("missing_user_task();")}) {
        change(taskUri, "class test_driver; task drive(); " + body + " endtask endclass\n",
               version);
        CHECK_FALSE(comp.containsCurrentSource(files.root / "rtl.sv"));
        const auto previousHits = statistics().crossEditHits;
        equalScope("top.u");
        CHECK(statistics().crossEditHits > previousHits);
        // The hierarchy result did not rebuild the dirty complete compilation.
        CHECK_FALSE(comp.containsCurrentSource(files.root / "rtl.sv"));
        equalScope("top.u.nested");
        CHECK(rfl::json::write(comp.getScopes("top.u")) ==
              rfl::json::write(native.m_driver->comp->getScopes("top.u")));
        enabled.m_driver->runPendingDiagnostics();
        const auto& diagnostics = enabled.client.getDiagnostics(taskUri);
        CHECK(std::ranges::any_of(diagnostics, [](const auto& diag) {
                  return diag.message.find("missing_user_task") != std::string::npos;
              }) == (version == 4));
        CHECK(enabled.client.diagnosticVersions.at(taskUri) == version);
        CHECK(comp.containsCurrentSource(files.root / "rtl.sv"));
        CHECK(statistics().retainedBytes <= comp.frozenInstances()->budgetBytes());
        CHECK(statistics().retainedBytes + enabled.statistics().retainedBytes <= 1024 * 1024);
        ++version;
    }
    // Declarations and preprocessing effects inside a task are outside this prototype.
    for (const auto& body : {std::string("int local_value; $display(local_value);"),
                             std::string("`define TASK_MACRO 7\n$display(`TASK_MACRO); ")}) {
        change(taskUri, "class test_driver; task drive(); " + body + "endtask endclass\n",
               version++);
        const auto previous = statistics().crossEditHits;
        equalScope("top.u");
        CHECK(statistics().crossEditHits == previous);
        CHECK(comp.containsCurrentSource(files.root / "rtl.sv"));
    }
    change(taskUri, driverText, version++);
    equalScope("top.u");
    // All structural / elaboration edits must miss and match fresh native results.
    auto topUri = open(native, "top.sv", originalTop);
    open(enabled, "top.sv", originalTop);
    int topVersion = 2;
    for (const auto& edit : {std::string("defparam u.W=3;"), std::string("bind leaf tap bound();"),
                             std::string("logic [$bits(u2.payload)-1:0] cross_word;")}) {
        const auto text = "module top; leaf u(); leaf #(16) u2(); leaf array[1:0](); " + edit +
                          " endmodule\n";
        change(topUri, text, topVersion++);
        const auto previous = statistics().crossEditHits;
        equalScope("top.u");
        CHECK(statistics().crossEditHits == previous);
        CHECK(comp.containsCurrentSource(files.root / "rtl.sv"));
    }
    auto typesUri = open(native, "types.sv",
                         "package types_pkg; parameter int WIDTH=8; endpackage\n");
    open(enabled, "types.sv", "package types_pkg; parameter int WIDTH=8; endpackage\n");
    change(topUri, originalTop, topVersion++);
    change(typesUri, "package types_pkg; parameter int WIDTH=2; endpackage\n", 2);
    equalScope("top.u");
    CHECK_FALSE(comp.getHierLocation("top.u.wide.generated_word"));
    change(topUri, "module top; leaf renamed(); endmodule\n", topVersion++);
    equalScope("top.u");
    CHECK_FALSE(comp.frozenInstances()->selections()[0].resolved);
    change(topUri, originalTop, topVersion++);
    equalScope("top.u");
    CHECK(comp.frozenInstances()->selections()[0].resolved);
    enabled.command("slang.refreshFrozenSources");
    CHECK(statistics().retainedBytes == 0);
    equalScope("top.u");
}

TEST_CASE("Frozen instance values retain exact locations and require a bounded current witness",
          "[frozen-instance]") {
    SourceManager sm;
    const auto file = std::filesystem::current_path() / "instance-location-memory.sv";
    const std::string original = "class driver; task run(); $display(1); endtask endclass "
                                 "module top; logic payload; endmodule\n";
    auto buffer = sm.assignText(file.string(), original);
    auto tree = syntax::SyntaxTree::fromBuffer(buffer, sm);
    ast::Compilation compilation;
    compilation.addSyntaxTree(tree);
    bool valid = true;
    size_t checks = 0;
    server::FrozenInstanceCache cache({"top"}, 65536, sm, [&](const lsp::RequestContext& ctx) {
        ++checks;
        ctx.throwIfCancelled("during synthetic instance witness");
        return valid;
    });
    cache.resolveSelections(compilation);
    REQUIRE(cache.selected("top"));
    auto certificate = cache.certify(std::vector{tree}, {});
    REQUIRE(certificate);
    std::vector<hier::HierItem_t> values{
        hier::Var{.kind = hier::SlangKind::Logic,
                  .instName = "payload",
                  .instLoc = {.uri = URI::fromFile(file), .range = {{0, 90}, {0, 97}}},
                  .type = "logic"}};
    cache.store("scope:top", rfl::json::write(values), values, *certificate, {});
    REQUIRE(cache.find("scope:top", *certificate));
    CHECK(checks >= 2);
    CHECK(cache.statistics().retainedBytes <= cache.budgetBytes());
    const auto changed = "class driver; task run(); $display(123456); endtask endclass "
                         "module top; logic payload; endmodule\n";
    SmallVector<char> bytes(changed, changed + std::char_traits<char>::length(changed));
    bytes.push_back('\0');
    buffer = sm.replaceBuffer(buffer.id, std::move(bytes));
    tree = syntax::SyntaxTree::fromBuffer(buffer, sm);
    certificate = cache.certify(std::vector{tree}, {});
    REQUIRE(certificate); // Task-body equivalence alone cannot retain old LSP locations.
    CHECK_FALSE(cache.find("scope:top", *certificate));
    CHECK(cache.statistics().invalidations == 1);
    valid = false;
    CHECK_FALSE(cache.certify(std::vector{tree}, {}));
    CHECK(cache.statistics().retainedBytes == 0);
    valid = true;
    lsp::RequestContext cancelled("workspace/executeCommand", 12);
    cancelled.cancel();
    CHECK_THROWS_AS(cache.certify(std::vector{tree}, cancelled), lsp::RequestCancelled);
    server::FrozenInstanceCache disabled({"top"}, 0, sm, [](const auto&) { return true; });
    CHECK_FALSE(disabled.certify(std::vector{tree}, {}));
    CHECK(disabled.statistics().retainedBytes == 0);
    server::FrozenInstanceCache tiny({"top"}, 4, sm, [](const auto&) { return true; });
    CHECK_FALSE(tiny.certify(std::vector{tree}, {}));
    CHECK(tiny.statistics().retainedBytes == 0);
    SourceManager foreign;
    auto foreignTree = syntax::SyntaxTree::fromText("module foreign; endmodule", foreign);
    CHECK_FALSE(cache.certify(std::vector{foreignTree}, {}));
    CHECK(cache.statistics().retainedBytes == 0);
    cache.clear("refresh");
    CHECK(cache.statistics().retainedBytes == 0);
}

TEST_CASE("Class task line changes invalidate dependent RTL and incremental proofs stay bounded",
          "[frozen-instance][frozen-source-integration]") {
    FrozenWorkspace files;
    const std::string initial = "package dep_pkg;\nclass settings;\n"
                                "task run(); $display(1); endtask\n"
                                "localparam int WIDTH=`__LINE__;\nendclass\nendpackage\n";
    files.write("dep.sv", initial);
    files.write("rtl.sv", "module leaf #(parameter int W=dep_pkg::settings::WIDTH); "
                          "logic[W-1:0] payload; endmodule\n"
                          "module top; leaf u(); endmodule\n");
    files.write("design.f", "dep.sv\nrtl.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1 --top top";
    config.build = "design.f";
    Config::FrozenSourcesConfig selected;
    selected.instances = std::vector<std::string>{"top.u"};
    config.frozenSources = selected;
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness native(files.initialize());
    config.experimentalFrozenSources = true;
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness enabled(files.initialize());
    auto uri = URI::fromFile(files.root / "dep.sv");
    for (auto* server : {&native, &enabled})
        server->SlangServer::onDocDidOpen(
            {.textDocument = {
                 .uri = uri, .languageId = "systemverilog", .version = 1, .text = initial}});
    auto equal = [&]() {
        auto actual = rfl::json::write(enabled.m_driver->comp->getScope("top.u"));
        CHECK(actual == rfl::json::write(native.m_driver->comp->getScope("top.u")));
        return actual;
    };
    auto statistics = [&]() { return enabled.m_driver->comp->frozenInstances()->statistics(); };
    const auto original = equal();
    REQUIRE(original.find("logic[3:0]") != std::string::npos);
    const auto full = statistics().fullCertifications;
    REQUIRE(full == 1);
    int version = 2;
    auto changeBody = [&](const std::string& body) {
        auto text = initial;
        text.replace(text.find("$display(1);"), std::string("$display(1);").size(), body);
        for (auto* server : {&native, &enabled})
            server->SlangServer::onDocDidChange(
                {.textDocument = {.version = version, .uri = uri},
                 .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
        ++version;
    };
    changeBody("$display(222);");
    CHECK(equal() == original);
    CHECK(statistics().fullCertifications == full);
    CHECK(statistics().incrementalCertifications >= 1);
    CHECK(statistics().crossEditHits >= 1);
    CHECK(statistics().reusedSourceFiles >= 1);
    for (const auto& lineBreaks :
         {std::string("\n\n\n"), std::string("\r\n\r\n\r\n"), std::string("\r\r\r")}) {
        changeBody("$display(1);" + lineBreaks);
        const auto current = equal();
        CHECK(current.find("logic[6:0]") != std::string::npos);
        // The first line-count change rebuilds the certificate. Equivalent
        // newline encodings may then reuse values; actual values must agree.
        CHECK(statistics().fullCertifications > full);
        CHECK(statistics().retainedBytes <=
              enabled.m_driver->comp->frozenInstances()->budgetBytes());
    }
    changeBody("$display(1);");
    CHECK(equal() == original);
}

TEST_CASE("Input evidence is scoped to one dispatch owner and revision", "[frozen-instance]") {
    auto owner = std::make_shared<const int>(1);
    auto replacement = std::make_shared<const int>(2);
    lsp::RequestContext request("workspace/executeCommand", 77);
    request.recordInputWitness(owner, 4);
    CHECK_FALSE(request.hasInputWitness(owner, 4));
    {
        auto scope = request.inputWitnessScope();
        CHECK_FALSE(request.hasInputWitness({}, 0));
        request.recordInputWitness(owner, 4);
        CHECK(request.hasInputWitness(owner, 4));
        CHECK_FALSE(request.hasInputWitness(owner, 5));
        CHECK_FALSE(request.hasInputWitness(replacement, 4));
        CHECK(owner.use_count() == 1); // Evidence must not pin a retired context.
        auto copy = request;
        CHECK(copy.hasInputWitness(owner, 4));
        lsp::RequestContext another("workspace/executeCommand", 77);
        auto anotherScope = another.inputWitnessScope();
        CHECK_FALSE(another.hasInputWitness(owner, 4)); // Same client ID is insufficient.
        {
            auto nested = request.inputWitnessScope();
            CHECK_FALSE(request.hasInputWitness(owner, 4));
            request.recordInputWitness(owner, 5);
            CHECK(request.hasInputWitness(owner, 5));
        }
        CHECK_FALSE(request.hasInputWitness(owner, 5));
    }
    CHECK_FALSE(request.hasInputWitness(owner, 4));
    lsp::RequestContext notification("textDocument/didChange", std::nullopt);
    auto notificationScope = notification.inputWitnessScope();
    notification.recordInputWitness(owner, 4);
    CHECK_FALSE(notification.hasInputWitness(owner, 4));
}

TEST_CASE("Instance request witnesses do not cross repeated IDs or hide filesystem changes",
          "[frozen-instance][frozen-source-integration]") {
    FrozenWorkspace files;
    std::filesystem::create_directories(files.root / "first");
    files.write("second/instance_width.svh", "`define INSTANCE_WIDTH 4\n");
    files.write("types.sv", "`include \"instance_width.svh\"\n"
                            "package dep_pkg; parameter int WIDTH=`INSTANCE_WIDTH; endpackage\n");
    files.write("leaf.sv", "module leaf #(parameter int W=dep_pkg::WIDTH); "
                           "logic[W-1:0] payload; endmodule\n");
    files.write("top.sv", "module top; leaf u(); endmodule\n");
    files.write("design.f", "types.sv\nleaf.sv\ntop.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1 --top top";
    config.incdirs = std::vector<std::string>{"first", "second"};
    config.build = "design.f";
    config.experimentalFrozenSources = true;
    Config::FrozenSourcesConfig selected;
    selected.instances = std::vector<std::string>{"top.u"};
    config.frozenSources = selected;
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness server(files.initialize());
    auto query = [&]() {
        return rfl::json::write(
            server.command("slang.getScope", {rfl::Generic(std::string("top.u"))}, 901));
    };
    CHECK(query().find("logic[3:0]") != std::string::npos);
    CHECK(query().find("logic[3:0]") != std::string::npos);
    REQUIRE(server.m_driver->comp->frozenInstances());
    CHECK(server.m_driver->comp->frozenInstances()->statistics().requestWitnessReuses >= 1);
    auto* previous = server.m_driver.get();
    const auto header = files.root / "second/instance_width.svh";
    const auto stamp = std::filesystem::last_write_time(header);
    const auto size = std::filesystem::file_size(header);
    files.write("second/instance_width.svh", "`define INSTANCE_WIDTH 8\n");
    REQUIRE(std::filesystem::file_size(header) == size);
    std::filesystem::last_write_time(header, stamp);
    CHECK(query().find("logic[7:0]") != std::string::npos);
    CHECK(server.m_driver.get() != previous);
    // A previously failed higher-priority lookup becomes a real input, without a watch event.
    files.write("first/instance_width.svh", "`define INSTANCE_WIDTH 12\n");
    CHECK(query().find("logic[11:0]") != std::string::npos);
    files.write("one/instance_width.svh", "`define INSTANCE_WIDTH 12\n");
    files.write("two/instance_width.svh", "`define INSTANCE_WIDTH 16\n");
    const auto alias = files.root / "first/instance_width.svh";
    std::filesystem::remove(alias);
    std::filesystem::create_symlink(files.root / "one/instance_width.svh", alias);
    CHECK(query().find("logic[11:0]") != std::string::npos);
    std::filesystem::remove(alias);
    std::filesystem::create_symlink(files.root / "two/instance_width.svh", alias);
    CHECK(query().find("logic[15:0]") != std::string::npos);
    auto top = server.openFile("top.sv");
    top.replaceAll("module top; leaf #(.W(3)) u(); endmodule\n");
    top.publishChanges();
    CHECK(query().find("logic[2:0]") != std::string::npos);
}

TEST_CASE("Frozen instance interface aliases follow unsaved connections and interface edits",
          "[frozen-instance][frozen-source-integration]") {
    FrozenWorkspace files;
    files.write("bus.sv", "interface data_bus #(parameter int N=8)(); "
                          "logic[N-1:0] data; endinterface\n");
    files.write("leaf.sv", "module leaf(data_bus p); logic payload; endmodule\n");
    const std::string initial = "module top; data_bus #(8) a(); data_bus #(16) b(); "
                                "leaf u(a); endmodule\n";
    files.write("top.sv", initial);
    files.write("design.f", "bus.sv\nleaf.sv\ntop.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1 --top top";
    config.build = "design.f";
    Config::FrozenSourcesConfig selected;
    selected.instances = std::vector<std::string>{"top.u"};
    config.frozenSources = selected;
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness native(files.initialize());
    config.experimentalFrozenSources = true;
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness enabled(files.initialize());
    auto query = [&]() {
        const auto value = rfl::json::write(
            enabled.command("slang.getScope", {rfl::Generic(std::string("top.u.p"))}));
        CHECK(value == rfl::json::write(native.command("slang.getScope",
                                                       {rfl::Generic(std::string("top.u.p"))})));
        return value;
    };
    auto open = [&](const char* file, const std::string& text) {
        auto uri = URI::fromFile(files.root / file);
        for (auto* server : {&native, &enabled})
            server->SlangServer::onDocDidOpen(
                {.textDocument = {
                     .uri = uri, .languageId = "systemverilog", .version = 1, .text = text}});
        return uri;
    };
    auto change = [&](const URI& uri, const std::string& text, int version) {
        for (auto* server : {&native, &enabled})
            server->SlangServer::onDocDidChange(
                {.textDocument = {.version = version, .uri = uri},
                 .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
    };
    CHECK(query().find("logic[7:0]") != std::string::npos);
    CHECK(query().find("logic[7:0]") != std::string::npos);
    auto topUri = open("top.sv", initial);
    auto text = initial;
    text.replace(text.find("u(a)"), 4, "u(b)");
    change(topUri, text, 2);
    CHECK(query().find("logic[15:0]") != std::string::npos);
    const auto before = enabled.m_driver->comp->frozenInstances()->statistics().crossEditHits;
    auto busUri = open("bus.sv", "interface data_bus #(parameter int N=8)(); "
                                 "logic[N-1:0] data; endinterface\n");
    change(busUri,
           "interface data_bus #(parameter int N=8)(); "
           "logic[N:0] data; endinterface\n",
           2);
    CHECK(query().find("logic[16:0]") != std::string::npos);
    CHECK(enabled.m_driver->comp->frozenInstances()->statistics().crossEditHits == before);
    enabled.m_driver->runPendingDiagnostics();
    for (const auto& uri : {topUri, busUri})
        CHECK_FALSE(std::ranges::any_of(enabled.client.getDiagnostics(uri), [](const auto& diag) {
            return diag.severity == lsp::DiagnosticSeverity::Error;
        }));
}

TEST_CASE(
    "Incremental instance proof follows nested includes and rebuilds after macro context changes",
    "[frozen-instance][frozen-source-integration]") {
    SourceManager sm;
    const auto dir = std::filesystem::current_path() / "instance-nested-memory";
    auto switches = sm.assignText((dir / "switches.sv").string(), "`define KIND class\n");
    auto body = sm.assignText((dir / "body.svh").string(), "task run(); $display(1); endtask\n");
    sm.assignText((dir / "agent.svh").string(),
                  "`KIND agent;\n`include \"body.svh\"\n`ifdef AS_MODULE\n"
                  "endmodule\n`else\nendclass\n`endif\n");
    auto wrapper = sm.assignText((dir / "wrapper.sv").string(),
                                 "`include \"agent.svh\"\n"
                                 "class untouched; task run(); $display(2); endtask endclass\n");
    auto rtl = sm.assignText((dir / "rtl.sv").string(), "module top; logic payload; endmodule\n");
    std::vector roots{switches, wrapper, rtl};
    auto tree = syntax::SyntaxTree::fromBuffers(roots, sm);
    REQUIRE(tree->diagnostics().empty());
    ast::Compilation compilation;
    compilation.addSyntaxTree(tree);
    server::FrozenInstanceCache cache({"top"}, 65536, sm, [](const auto&) { return true; });
    cache.resolveSelections(compilation);
    auto certificate = cache.certify(std::vector{tree}, {});
    REQUIRE(certificate);
    std::vector<hier::HierItem_t> values{
        hier::Var{.kind = hier::SlangKind::Logic,
                  .instName = "payload",
                  .instLoc = {.uri = URI::fromFile(dir / "rtl.sv"), .range = {{0, 18}, {0, 25}}},
                  .type = "logic"}};
    const auto payload = rfl::json::write(values);
    cache.store("scope:top", payload, values, *certificate, {});
    REQUIRE(cache.find("scope:top", *certificate));
    auto replace = [&](SourceBuffer& buffer, const std::string& text) {
        SmallVector<char> bytes(text.begin(), text.end());
        bytes.push_back('\0');
        buffer = sm.replaceBuffer(buffer.id, std::move(bytes));
    };
    replace(body, "task run(); $display(333); endtask\n");
    tree = syntax::SyntaxTree::fromBuffers(roots, sm);
    REQUIRE(tree->diagnostics().empty());
    certificate = cache.certify(std::vector{tree}, {});
    REQUIRE(certificate);
    CHECK(cache.find("scope:top", *certificate) == payload);
    CHECK(cache.statistics().fullCertifications == 1);
    CHECK(cache.statistics().incrementalCertifications == 1);
    // An earlier input changes the declaration context of an unchanged include.
    // Its old class-task mask must not seed the next baseline.
    replace(switches, "`define KIND module\n`define AS_MODULE\n");
    roots[0] = switches;
    tree = syntax::SyntaxTree::fromBuffers(roots, sm);
    REQUIRE(tree->diagnostics().empty());
    certificate = cache.certify(std::vector{tree}, {});
    REQUIRE(certificate);
    CHECK_FALSE(cache.find("scope:top", *certificate));
    CHECK(cache.statistics().fullCertifications == 2);
    CHECK(cache.statistics().eligibleTaskBodies == 1); // Only the untouched class remains eligible.
    cache.store("scope:top", payload, values, *certificate, {});
    replace(body, "task run(); $display(444); endtask\n");
    tree = syntax::SyntaxTree::fromBuffers(roots, sm);
    REQUIRE(tree->diagnostics().empty());
    certificate = cache.certify(std::vector{tree}, {});
    REQUIRE(certificate);
    CHECK_FALSE(cache.find("scope:top", *certificate));
    CHECK(cache.statistics().fullCertifications == 3);
}

#if defined(__linux__) || defined(__APPLE__)
TEST_CASE("Full-design retirement preserves unsaved inputs and active selection across epochs",
          "[frozen-instance][frozen-retirement][frozen-source-integration]") {
    const auto mode = GENERATE("configured", "interactive", "top");
    const auto experiment = GENERATE("off", "uvm", "frozen");
    const bool bodyQueries = GENERATE(false, true);
    CAPTURE(mode, experiment, bodyQueries);
    std::optional<std::string> previous;
    if (auto* value = std::getenv("SLANG_UVM_GENERATION_EDIT_LIMIT"))
        previous = value;
    setenv("SLANG_UVM_GENERATION_EDIT_LIMIT", "1", 1);
    auto restore = ScopeGuard([&] {
        if (previous)
            setenv("SLANG_UVM_GENERATION_EDIT_LIMIT", previous->c_str(), 1);
        else
            unsetenv("SLANG_UVM_GENERATION_EDIT_LIMIT");
    });
    FrozenWorkspace files(true);
    const std::string package = "package uvm_pkg; class runner; int first; endclass endpackage\n";
    const std::string top = "import uvm_pkg::*;\n"
                            "module leaf #(parameter int WIDTH=8); "
                            "logic[WIDTH-1:0] payload; endmodule\n"
                            "module top; leaf #(4) a(); leaf #(16) b(); "
                            "runner h; initial $display(h.first); endmodule\n";
    files.write("uvm_pkg.sv", package);
    files.write("top.sv", top);
    files.write("design.f", "uvm_pkg.sv\ntop.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1";
    config.experimentalMethodBodyQueries = bodyQueries;
    config.experimentalFrozenSources = std::string_view(experiment) == "frozen";
    config.experimentalUvmPrefixCache = std::string_view(experiment) == "uvm";
    config.frozenSources = Config::FrozenSourcesConfig{
        .instances = std::vector<std::string>{"top.b"}};
    if (std::string_view(mode) == "configured")
        config.build = "design.f";
    else if (std::string_view(mode) == "top")
        config.compilationFilelist = "design.f";
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness server(files.initialize());
    const auto packageUri = URI::fromFile(files.root / "uvm_pkg.sv");
    const auto topUri = URI::fromFile(files.root / "top.sv");
    server.m_driver->openDocument(packageUri, package, 1, true);
    server.m_driver->openDocument(topUri, top, 1, true);
    if (std::string_view(mode) == "interactive")
        server.setBuildFile("design.f");
    else if (std::string_view(mode) == "top")
        server.setTopLevel(std::string(topUri.getPath()));
    REQUIRE(server.m_driver->comp);
    REQUIRE(server.setActiveInstance("top.b"));
    auto position = [](const std::string& text, std::string_view token) {
        const auto offset = text.find(token);
        REQUIRE(offset != std::string::npos);
        const auto start = text.rfind('\n', offset);
        return lsp::Position{uint32_t(std::count(text.begin(), text.begin() + offset, '\n')),
                             uint32_t(start == std::string::npos ? offset : offset - start - 1)};
    };
    const auto query = lsp::CompletionParams{.textDocument = {.uri = topUri},
                                             .position = position(top, "first);")};
    auto response = server.getDocCompletion(query);
    REQUIRE(rfl::holds_alternative<std::vector<lsp::CompletionItem>>(response));
    auto items = rfl::get<std::vector<lsp::CompletionItem>>(response);
    auto found = std::ranges::find_if(items,
                                      [](const auto& item) { return item.label == "first"; });
    REQUIRE(found != items.end());
    const auto oldItem = *found;
    REQUIRE(oldItem.data);
    CHECK_FALSE(oldItem.documentation);
    auto initialEpoch = server.m_driver->completions.getCompletionEpoch();
    const auto updatedPackage = "package uvm_pkg; class runner; logic[15:0] first; "
                                "endclass endpackage\n";
    auto change = [&](const URI& uri, int version, const std::string& text) {
        server.onDocDidChange(
            {.textDocument = {.version = version, .uri = uri},
             .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
        auto doc = server.m_driver->getDocument(uri);
        REQUIRE(doc);
        CHECK(doc->getVersion() == version);
        CHECK(doc->textMatches(text));
        CHECK(server.client.diagnosticVersions.at(uri) == version);
    };
    change(packageUri, 2, updatedPackage);
    CHECK(server.m_driver->completions.getCompletionEpoch() == initialEpoch);
    auto updatedTop = top;
    updatedTop.replace(updatedTop.find("#(16) b"), 7, "#(24) b");
    change(topUri, 2, updatedTop);
    const bool enabled = std::string_view(experiment) != "off";
    auto retiredEpoch = server.m_driver->completions.getCompletionEpoch();
    CHECK((retiredEpoch != initialEpoch) == enabled);
    REQUIRE(server.m_driver->comp);
    REQUIRE(server.m_driver->comp->getActiveInstance("leaf"));
    CHECK(server.m_driver->comp->getActiveInstance("leaf")->instPath == "top.b");
    auto width = server.m_driver->getDocHover(topUri, position(updatedTop, "payload;"));
    REQUIRE(width);
    CHECK(rfl::get<lsp::MarkupContent>(width->contents).value.find("Width: `24`") !=
          std::string::npos);
    CHECK(server.m_driver->getDocument(packageUri)->textMatches(updatedPackage));
    auto resolved = server.getCompletionItemResolve(oldItem);
    REQUIRE(resolved.documentation);
    CHECK(rfl::json::write(resolved.documentation).find("[15:0]") != std::string::npos);
    if (enabled) {
        auto raw = oldItem;
        auto payload = raw.data->to_object().value();
        raw.data = payload.at("payload");
        auto fenced = server.getCompletionItemResolve(raw);
        CHECK_FALSE(fenced.documentation);
        CHECK_FALSE(fenced.data);
    }
    const std::string invalid = "package uvm_pkg; class runner; logic[15:0] first; "
                                "function void use(); slang_missing_user_task(); "
                                "endfunction endclass endpackage\n";
    change(packageUri, 3, invalid);
    auto diagnostics = server.client.getDiagnostics(packageUri);
    CHECK(std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.message.find("slang_missing_user_task") != std::string::npos;
    }));
    change(packageUri, 4, updatedPackage);
    CHECK((server.m_driver->completions.getCompletionEpoch() != retiredEpoch) == enabled);
    CHECK(
        std::ranges::none_of(server.client.getDiagnostics(packageUri), [](const auto& diagnostic) {
            return diagnostic.severity == lsp::DiagnosticSeverity::Error;
        }));
    REQUIRE(server.m_driver->comp);
    REQUIRE(server.m_driver->comp->getActiveInstance("leaf"));
    CHECK(server.m_driver->comp->getActiveInstance("leaf")->instPath == "top.b");
    CHECK(server.m_driver->getDocument(topUri)->textMatches(updatedTop));
}
#endif

#if defined(__linux__) || defined(__APPLE__)
TEST_CASE("Failed full-design retirement keeps the edited owner and retries after repair",
          "[frozen-instance][frozen-retirement][frozen-source-integration]") {
    const bool topMode = GENERATE(false, true);
    CAPTURE(topMode);
    std::optional<std::string> previous;
    if (auto* value = std::getenv("SLANG_UVM_GENERATION_EDIT_LIMIT"))
        previous = value;
    setenv("SLANG_UVM_GENERATION_EDIT_LIMIT", "1", 1);
    auto restore = ScopeGuard([&] {
        if (previous)
            setenv("SLANG_UVM_GENERATION_EDIT_LIMIT", previous->c_str(), 1);
        else
            unsetenv("SLANG_UVM_GENERATION_EDIT_LIMIT");
    });
    FrozenWorkspace files(true);
    const std::string original = "module top; logic[7:0] payload; endmodule\n";
    files.write("uvm_pkg.sv", "package uvm_pkg; endpackage\n");
    files.write("top.sv", original);
    files.write("design.f", "uvm_pkg.sv\ntop.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1";
    config.experimentalFrozenSources = true;
    if (topMode)
        config.compilationFilelist = "design.f";
    else
        config.build = "design.f";
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness server(files.initialize());
    auto uri = URI::fromFile(files.root / "top.sv");
    server.m_driver->openDocument(uri, original, 1, true);
    if (topMode)
        server.setTopLevel(std::string(uri.getPath()));
    REQUIRE(server.m_driver->comp);
    const auto epoch = server.m_driver->completions.getCompletionEpoch();
    auto change = [&](int version, const std::string& text) {
        server.onDocDidChange(
            {.textDocument = {.version = version, .uri = uri},
             .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
        REQUIRE(server.m_driver->comp);
        auto doc = server.m_driver->getDocument(uri);
        REQUIRE(doc);
        CHECK(doc->getVersion() == version);
        CHECK(doc->textMatches(text));
        CHECK(server.client.diagnosticVersions.at(uri) == version);
    };
    change(2, original + "\n");
    const auto duringFailure = topMode ? "// no top module while editing\n" : original;
    if (!topMode)
        files.write("design.f", "unavailable-design-input.sv\n");
    change(3, duringFailure);
    CHECK(server.m_driver->completions.getCompletionEpoch() == epoch);
    files.write("design.f", "uvm_pkg.sv\ntop.sv\n");
    const std::string repaired = "module top; logic[23:0] payload; endmodule\n";
    change(4, repaired);
    CHECK(server.m_driver->completions.getCompletionEpoch() != epoch);
    auto hover = server.m_driver->getDocHover(uri, {0, uint32_t(repaired.find("payload"))});
    REQUIRE(hover);
    CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find("Width: `24`") !=
          std::string::npos);
}
#endif

TEST_CASE("An edited selected final root reuses its explicit UVM dependency prefix",
          "[frozen-module][frozen-dependency-prefix][frozen-source-integration]") {
    const auto mode = GENERATE("enabled", "zero-budget", "disabled");
    CAPTURE(mode);
    FrozenWorkspace files;
    files.write("library_defs.svh", "`define LIB_WIDTH 4\n");
    files.write("uvm_pkg.sv", "`include \"library_defs.svh\"\npackage uvm_pkg; "
                              "localparam int WIDTH=`LIB_WIDTH+`LIB_CFG; "
                              "class base; int base_field; endclass endpackage\n");
    const std::string originalClass = "class driver extends uvm_pkg::base; task run(); "
                                      "$display(base_field); endtask endclass\n";
    const std::string originalRoot = "`include \"driver.svh\"\n"
                                     "module dut; logic[uvm_pkg::WIDTH-1:0] payload; "
                                     "localparam int MACRO_VALUE=`LIB_WIDTH; endmodule\n"
                                     "module top; dut u(); driver obj; "
                                     "initial obj.run(); endmodule\n";
    files.write("driver.svh", originalClass);
    files.write("main.sv", originalRoot);
    files.write("design.f", "uvm_pkg.sv\nmain.sv\n");
    Config config;
    config.build = "design.f";
    config.flags = "--single-unit --threads 1 -DLIB_CFG=2";
    config.uvm = Config::UvmConfig{.sourceDirectory = files.root.string(),
                                   .entryFile = "uvm_pkg.sv"};
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"dut"},
                                    .maxMiB = std::string_view(mode) == "zero-budget" ? 0u : 64u};
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness native(files.initialize());
    config.experimentalFrozenSources = std::string_view(mode) != "disabled";
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness cached(files.initialize());
    const auto classUri = URI::fromFile(files.root / "driver.svh");
    const auto rootUri = URI::fromFile(files.root / "main.sv");
    for (auto* server : {&native, &cached}) {
        server->m_driver->openDocument(classUri, originalClass, 1, true);
        server->m_driver->openDocument(rootUri, originalRoot, 1, true);
    }
    auto diagnostics = [&](FrozenHarness& server) {
        std::set<std::string> uris;
        for (const auto& uri : server.client.diagnosticPublications)
            uris.emplace(uri.str());
        std::map<std::string, std::vector<std::string>> result;
        for (const auto& uri : uris) {
            auto& values = result[uri];
            for (const auto& diagnostic : server.client.getDiagnostics(URI(uri)))
                values.push_back(rfl::json::write(diagnostic));
            std::sort(values.begin(), values.end());
        }
        return result;
    };
    std::string classText = originalClass;
    auto compare = [&] {
        REQUIRE(native.loadConfig(native.getConfig(), false, true, false, true));
        cached.command("slang.frozenSourcesStatus");
        REQUIRE(native.m_driver->comp);
        REQUIRE(cached.m_driver->comp);
        CHECK(rfl::json::write(cached.getScope("top.u")) ==
              rfl::json::write(native.getScope("top.u")));
        CHECK(rfl::json::write(cached.getScopes("top.u")) ==
              rfl::json::write(native.getScopes("top.u")));
        auto language = [&](FrozenHarness& server) {
            const auto offset = uint32_t(classText.find("base_field"));
            const lsp::TextDocumentPositionParams query{.textDocument = {.uri = classUri},
                                                        .position = {0, offset + 1}};
            auto hover = server.getDocHover(
                {.textDocument = query.textDocument, .position = query.position});
            REQUIRE(hover);
            auto definition = server.getDocDefinition(
                {.textDocument = query.textDocument, .position = query.position});
            auto response = server.getDocCompletion(
                {.textDocument = query.textDocument, .position = {0, offset}});
            REQUIRE(rfl::holds_alternative<std::vector<lsp::CompletionItem>>(response));
            auto items = rfl::get<std::vector<lsp::CompletionItem>>(response);
            REQUIRE(std::ranges::any_of(items, [](const auto& item) {
                return item.label == "base_field";
            }));
            std::vector<std::string> resolved;
            for (auto item : items) {
                item = server.getCompletionItemResolve(item);
                item.data.reset();
                resolved.push_back(rfl::json::write(item));
            }
            return std::tuple{rfl::json::write(hover), rfl::json::write(definition), resolved};
        };
        CHECK(language(cached) == language(native));
        cached.m_driver->runPendingDiagnostics();
        native.m_driver->runPendingDiagnostics();
        CHECK(diagnostics(cached) == diagnostics(native));
    };
    compare();
    int version = 1;
    auto edit = [&](const std::string& body) {
        classText = "class driver extends uvm_pkg::base; task run(); " + body +
                    " endtask endclass\n";
        ++version;
        for (auto* server : {&native, &cached})
            server->onDocDidChange({.textDocument = {.version = version, .uri = classUri},
                                    .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                                        .text = classText}}});
        compare();
        CHECK(cached.client.diagnosticVersions.at(classUri) == version);
    };
    for (const auto& body : {"$display(base_field,22);", "missing_user_task(base_field);",
                             "$display(base_field,333);"})
        edit(body);
    const bool reuses = std::string_view(mode) == "enabled";
    auto stats = cached.statistics();
    CHECK((stats.dependencyResumes >= 3) == reuses);
    CHECK((stats.nativeSeedUses >= 1) == reuses);
    if (reuses) {
        CHECK(stats.preparations == 0);
        CHECK(stats.retainedBytes <= 64 * 1024 * 1024);
    }
    files.write("library_defs.svh", "`define LIB_WIDTH 7\n");
    if (!cached.getConfig().experimentalFrozenSources.value())
        REQUIRE(cached.loadConfig(cached.getConfig(), false, true, false, true));
    compare();
    auto payload = cached.m_driver->getDocHover(
        rootUri, {1, uint32_t(std::string("module dut; logic[uvm_pkg::WIDTH-1:0] ").size())});
    REQUIRE(payload);
    CHECK(rfl::get<lsp::MarkupContent>(payload->contents).value.find("Width: `9`") !=
          std::string::npos);
    const auto beforeOverride = cached.statistics().dependencyResumes;
    const auto overridden = "`undef LIB_WIDTH\n`define LIB_WIDTH 11\n" + originalRoot;
    for (auto* server : {&native, &cached})
        server->onDocDidChange(
            {.textDocument = {.version = 2, .uri = rootUri},
             .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = overridden}}});
    compare();
    CHECK(cached.statistics().dependencyResumes == beforeOverride);
    if (reuses)
        CHECK(cached.statistics().preflightDeclines > 0);
    for (auto* server : {&native, &cached})
        server->onDocDidChange({.textDocument = {.version = 3, .uri = rootUri},
                                .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                                    .text = originalRoot}}});
    cached.command("slang.refreshFrozenSources");
    compare();
}

#if defined(__linux__) || defined(__APPLE__)
TEST_CASE("Scope semantic completions retain epochs through retirement and explicit rebuild",
          "[scope-semantic][scope-completion-epoch]") {
    const bool enabled = GENERATE(false, true);
    std::optional<std::string> previous;
    if (auto* value = std::getenv("SLANG_UVM_GENERATION_EDIT_LIMIT"))
        previous = value;
    setenv("SLANG_UVM_GENERATION_EDIT_LIMIT", "1", 1);
    auto restore = ScopeGuard([&] {
        if (previous)
            setenv("SLANG_UVM_GENERATION_EDIT_LIMIT", previous->c_str(), 1);
        else
            unsetenv("SLANG_UVM_GENERATION_EDIT_LIMIT");
    });
    FrozenWorkspace files(false);
    files.write("fixed.sv", "module stable(input logic a, output wire y); assign y=a; endmodule\n");
    auto source = [](int width) {
        return fmt::format("module top; logic[{}:0] first; wire copy=first[0]; stable "
                           "u(.a(first[0]), .y()); endmodule\n",
                           width - 1);
    };
    auto text = source(2);
    files.write("top.sv", text);
    files.write("design.f", "fixed.sv\ntop.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1 --top top";
    config.build = "design.f";
    config.experimentalScopeSemanticCache = enabled;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness server(files.initialize());
    const auto uri = URI::fromFile(files.root / "top.sv");
    server.m_driver->openDocument(uri, text, 1, true);
    auto item = [&] {
        const auto offset = text.find("first[0]") + 3;
        auto response = server.getDocCompletion(
            {.textDocument = {.uri = uri}, .position = {0, uint32_t(offset)}});
        REQUIRE(rfl::holds_alternative<std::vector<lsp::CompletionItem>>(response));
        auto items = rfl::get<std::vector<lsp::CompletionItem>>(response);
        auto found = std::ranges::find_if(items, [](const auto& i) { return i.label == "first"; });
        REQUIRE(found != items.end());
        REQUIRE(found->data);
        if (enabled) {
            const auto data = found->data->to_object().value();
            CHECK(std::ranges::any_of(data, [](const auto& entry) {
                return entry.first == "contextEpoch";
            }));
            CHECK(std::ranges::any_of(data,
                                      [](const auto& entry) { return entry.first == "payload"; }));
        }
        return *found;
    };
    const auto old = item();
    const auto firstEpoch = server.m_driver->completions.getCompletionEpoch();
    for (int version = 2; version <= 4; ++version) {
        text = source(version + 1);
        server.onDocDidChange(
            {.textDocument = {.version = version, .uri = uri},
             .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
        auto resolved = server.getCompletionItemResolve(item());
        REQUIRE(resolved.documentation);
        CHECK(rfl::json::write(resolved.documentation).find(fmt::format("[{}:0]", version)) !=
              std::string::npos);
    }
    CHECK((server.m_driver->completions.getCompletionEpoch() != firstEpoch) == enabled);
    auto stale = server.getCompletionItemResolve(old);
    REQUIRE(stale.documentation);
    CHECK(rfl::json::write(stale.documentation).find("[4:0]") != std::string::npos);
    if (enabled) {
        auto raw = old;
        raw.data = old.data->to_object().value().at("payload");
        auto fenced = server.getCompletionItemResolve(raw);
        CHECK_FALSE(fenced.documentation);
        CHECK_FALSE(fenced.data);
        const auto beforeRebuild = item();
        const auto epoch = server.m_driver->completions.getCompletionEpoch();
        REQUIRE(rfl::from_generic<bool>(server.command("slang.rebuildScopeLibrary")).value());
        CHECK(server.m_driver->completions.getCompletionEpoch() != epoch);
        REQUIRE(server.m_driver->getDocument(uri)->textMatches(text));
        auto fresh = server.getCompletionItemResolve(item());
        REQUIRE(fresh.documentation);
        CHECK(rfl::json::write(fresh.documentation).find("[4:0]") != std::string::npos);
        auto oldResolved = server.getCompletionItemResolve(beforeRebuild);
        REQUIRE(oldResolved.documentation);
        CHECK(rfl::json::write(oldResolved.documentation).find("[4:0]") != std::string::npos);
    }
}
#endif

TEST_CASE("Frozen type-parameter completion preserves native broken-argument fallback and repair",
          "[scope-type-completion-fallback]") {
    const bool enabled = GENERATE(false, true);
    FrozenWorkspace files;
    files.write("fixed.sv", R"(
package fixed_types;
  typedef struct packed { logic [3:0] payload; } packet_t;
endpackage
module stable #(parameter type T=fixed_types::packet_t, parameter type U=T);
  U q;
endmodule
)");
    auto source = [](std::string_view type) {
        return fmt::format("module top; stable #(.T({})) u(); "
                           "initial $display(u.q); endmodule\n",
                           type);
    };
    files.write("consumer.sv", source("fixed_types::packet_t"));
    files.write("design.f", "fixed.sv\nconsumer.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1 --top top";
    config.compilationFilelist = "design.f";
    config.experimentalScopeSemanticCache = enabled;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness server(files.initialize());
    server.setBuildFile("design.f");
    REQUIRE(server.setActiveInstance("top.u"));
    auto user = server.openFile("consumer.sv");
    auto provider = server.openFile("fixed.sv");
    for (const auto type :
         {"fixed_types::packet_t", "MissingType", "logic [7:0]", "fixed_types::packet_t"}) {
        INFO(type);
        user.replaceAll(source(type));
        user.save();
        if (std::string_view(type) == "fixed_types::packet_t") {
            auto hover = user.getHoverAt(user.before("fixed_types::packet_t").m_offset);
            REQUIRE(hover);
            const auto& text = rfl::get<lsp::MarkupContent>(hover->contents).value;
            const std::string header = "**Package** `fixed_types`";
            REQUIRE(text.find(header) != std::string::npos);
            CHECK(text.find(header) == text.rfind(header));
        }
        auto items = provider.before("q;").getCompletions();
        auto q = std::ranges::find(items, "q", [](const auto& item) { return item.m_item.label; });
        REQUIRE(q != items.end());
        REQUIRE(q->m_item.labelDetails);
        CHECK(q->m_item.labelDetails->detail ==
              (std::string_view(type) == "logic [7:0]" ? " logic[7:0]" : " packet_t"));
        auto u = std::ranges::find(items, "U", [](const auto& item) { return item.m_item.label; });
        REQUIRE(u != items.end());
        u->resolve();
        REQUIRE(u->m_item.documentation);
        CHECK(rfl::get<lsp::MarkupContent>(*u->m_item.documentation)
                  .value.find("parameter type U=T") != std::string::npos);
    }
}

TEST_CASE("Projected nominal field references include the physical declaration when requested",
          "[scope-local-nominal-references]") {
    const bool enabled = GENERATE(false, true);
    const bool macro = GENERATE(false, true);
    FrozenWorkspace files;
    const std::string packet = "struct packed { logic [3:0] payload; }";
    files.write("fixed.sv",
                (macro ? "`define PACKET_T " + packet + "\n" : "") +
                    "module stable #(parameter type T=" + (macro ? "`PACKET_T" : packet) +
                    ")(input logic [3:0] a, output wire [3:0] y); T q; "
                    "assign q=a; assign y=q.payload; endmodule\n");
    files.write("consumer.sv", "module top; logic [3:0] a; wire [3:0] y; "
                               "stable u(.*); initial $display(u.q.payload); endmodule\n");
    files.write("design.f", "fixed.sv\nconsumer.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1 --top top";
    config.compilationFilelist = "design.f";
    config.experimentalScopeSemanticCache = enabled;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness server(files.initialize());
    server.setBuildFile("design.f");
    REQUIRE(server.setActiveInstance("top.u"));
    auto user = server.openFile("consumer.sv");
    auto provider = server.openFile("fixed.sv");
    const auto declaration =
        lsp::Location{.uri = provider.m_uri,
                      .range = lsp::Range{.start = provider.before("payload;").getPosition(),
                                          .end = provider.after("payload").getPosition()}};
    for (bool include : {true, false}) {
        auto references = server.m_driver->getDocReferences(user.m_uri,
                                                            user.before("payload").getPosition(),
                                                            include);
        REQUIRE(references);
        CHECK(references->size() == (include ? 3 : 2));
        CHECK(std::ranges::count_if(*references, [&](const auto& loc) {
                  return loc.uri == declaration.uri && loc.range == declaration.range;
              }) == (include ? 1 : 0));
    }
}

TEST_CASE("Forwarded array fields resolve documents and references after component discovery",
          "[scope-forwarded-field-queries]") {
    const bool enabled = GENERATE(false, true);
    FrozenWorkspace files;
    files.write("fixed.sv", R"(
package provider;
 function automatic void check_width(input int width);
   if(width<1) $error("invalid width");
 endfunction
 typedef enum logic [1:0] {Idle, Busy} state_t;
endpackage
module leaf #(parameter type T=logic)(input T value, output wire y);
 assign y=value.payload;
endmodule
module stable #(parameter type T=struct packed { logic [3:0] payload; })
 (input logic [3:0] a, output wire y);
 import provider::*;
 initial provider::check_width(4);
 state_t state=Idle;
 T words[2];
 function automatic T make_word(input logic [3:0] a);
   make_word.payload=a;
 endfunction
 assign words[0]=make_word(a);
 leaf #(.T(T)) child(.value(words[0]),.y(y));
endmodule
)");
    files.write("consumer.sv", "module top; logic [3:0] a; wire y; stable u(.*); endmodule\n");
    files.write("design.f", "fixed.sv\nconsumer.sv\n");
    Config config;
    config.flags = "--single-unit --threads 1 --top top";
    config.compilationFilelist = "design.f";
    config.experimentalScopeSemanticCache = enabled;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    files.write(".slang/server.json", rfl::json::write(config));
    FrozenHarness server(files.initialize());
    server.setBuildFile("design.f");
    REQUIRE(server.setActiveInstance("top.u"));
    auto user = server.openFile("consumer.sv");
    auto provider = server.openFile("fixed.sv");
    auto lexical = provider.before("words[2]").getCompletions();
    auto idle = std::ranges::find(lexical, "Idle", [](const auto& i) { return i.m_item.label; });
    REQUIRE(idle != lexical.end());
    idle->resolve();
    REQUIRE(idle->m_item.documentation);
    CHECK(rfl::get<lsp::MarkupContent>(*idle->m_item.documentation).value.find("Idle, Busy") !=
          std::string::npos);
    auto at = provider.before("payload;");
    auto items = at.getCompletions();
    auto field = std::ranges::find(items, "payload", [](const auto& i) { return i.m_item.label; });
    REQUIRE(field != items.end());
    field->resolve();
    REQUIRE(field->m_item.documentation);
    CHECK(rfl::get<lsp::MarkupContent>(*field->m_item.documentation)
              .value.find("logic [3:0] payload;") != std::string::npos);
    const auto declaration = lsp::Range{.start = provider.before("payload; }").getPosition(),
                                        .end = provider.before("; }").getPosition()};
    auto highlights = server.m_driver->getDocDocumentHighlight(provider.m_uri, at.getPosition());
    REQUIRE(highlights);
    CHECK(std::ranges::count_if(*highlights,
                                [&](const auto& h) { return h.range == declaration; }) == 1);
    for (bool include : {false, true}) {
        auto references = server.m_driver->getDocReferences(provider.m_uri, at.getPosition(),
                                                            include);
        REQUIRE(references);
        CHECK(std::ranges::count_if(*references, [&](const auto& loc) {
                  return loc.uri == provider.m_uri && loc.range == declaration;
              }) == (include ? 1 : 0));
        for (const auto& location : *references)
            CHECK(std::ranges::count_if(*references, [&](const auto& candidate) {
                      return candidate.uri == location.uri && candidate.range == location.range;
                  }) == 1);
    }
    auto functionReferences = server.m_driver->getDocReferences(
        provider.m_uri, provider.before("make_word(a)").getPosition(), false);
    REQUIRE(functionReferences);
    CHECK(functionReferences->size() == 2);
    CHECK(std::ranges::none_of(*functionReferences, [&](const auto& location) {
        return location.range.start == provider.before("make_word(input").getPosition();
    }));
    for (bool invalid : {true, false}) {
        user.replaceAll(
            invalid
                ? "module top; logic [3:0] a; wire y; stable #(.T(MissingType)) u(.*); endmodule\n"
                : "module top; logic [3:0] a; wire y; stable u(.*); endmodule\n");
        user.save();
        const auto position = provider.before("check_width(4)").getPosition();
        const auto declarationStart = provider.before("check_width(input").getPosition();
        for (bool include : {true, false}) {
            auto refs = server.m_driver->getDocReferences(provider.m_uri, position, include);
            REQUIRE(refs);
            CHECK(refs->size() == (include ? 2 : 1));
            CHECK(std::ranges::count_if(*refs, [&](const auto& r) {
                      return r.uri == provider.m_uri && r.range.start == declarationStart;
                  }) == (include ? 1 : 0));
        }
        auto marks = server.m_driver->getDocDocumentHighlight(provider.m_uri, position);
        REQUIRE(marks);
        CHECK(std::ranges::count_if(*marks, [&](const auto& m) {
                  return m.range.start == declarationStart;
              }) == 1);
    }
}
