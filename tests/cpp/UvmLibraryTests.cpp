// SPDX-License-Identifier: MIT
#include "ServerDriver.h"
#include "completions/CompletionContext.h"
#include "utils/ServerHarness.h"
#include <catch2/generators/catch_generators.hpp>
#include <fstream>
#include <rfl/json.hpp>

#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/util/OS.h"
#include "slang/util/ScopeGuard.h"

namespace {
namespace fs = std::filesystem;
using namespace slang;
struct LibraryWorkspace {
    fs::path oldCwd = fs::current_path();
    inline static unsigned next = 0;
    fs::path root = fs::weakly_canonical(fs::temp_directory_path()) /
                    fmt::format("slang-uvm-path-{}-{}", OS::getpid(), next++);
    fs::path library = root / "third party/uvm ?sources";
    fs::path entry = library / "vendor_entry.sv";
    std::string source = "`include \"uvm_macros.svh\"\npackage uvm_pkg; "
                         "localparam int X=`VALUE; class object; int field; endclass endpackage\n";
    LibraryWorkspace() {
        fs::create_directories(library);
        write(entry, source);
        write(library / "uvm_macros.svh", "`define PASS(a) a\n");
        write(root / "consumer.sv", "`include \"uvm_macros.svh\"\nmodule consumer; "
                                    "uvm_pkg::object obj; localparam int Y=`PASS(1); endmodule\n");
        fs::current_path(root);
    }
    ~LibraryWorkspace() {
        fs::current_path(oldCwd);
        fs::remove_all(root);
    }
    void write(const fs::path& path, std::string_view text) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path);
        REQUIRE(out.good());
        out << text;
    }
    Config config(bool cache = true) {
        Config result;
        result.uvm = Config::UvmConfig{.sourceDirectory = "third party/uvm ?sources",
                                       .entryFile = "vendor_entry.sv"};
        result.experimentalUvmPrefixCache = cache;
        result.flagsByFile = std::vector<Config::FlagSource>{
            {(root / "server.json").string(), "--single-unit --threads 1 -DVALUE=1"}};
        return result;
    }
};
std::shared_ptr<syntax::SyntaxTree> prepared(server::ServerDriver& driver) {
    for (auto& [_, doc] : driver.docs)
        if (auto tree = doc->getPreparedLibraryPrefix())
            return tree;
    return {};
}
int parameterValue(const std::shared_ptr<syntax::SyntaxTree>& tree) {
    ast::Compilation compilation;
    compilation.addSyntaxTree(tree);
    auto* package = compilation.getPackage("uvm_pkg");
    REQUIRE(package);
    auto* value = package->find("X");
    REQUIRE(value);
    return *value->as<ast::ParameterSymbol>().getValue().integer().as<int>();
}
struct LibraryAccess : server::CompletionQuery {
    using CompletionQuery::getIndexedSyntaxTree;
};
} // namespace

TEST_CASE("Explicit UVM paths preserve native root and wrapper order", "[uvm-library]") {
    const std::string mode = GENERATE("missing", "root", "wrapper", "nonprefix");
    const bool cache = GENERATE(false, true);
    CAPTURE(mode, cache);
    LibraryWorkspace files;
    auto config = files.config(cache);
    std::string inputs;
    fs::path expectedRoot = files.entry;
    if (mode == "root")
        inputs = "\"third party/uvm ?sources/vendor_entry.sv\"\n";
    else if (mode == "wrapper") {
        files.write(files.root / "wrapper.sv",
                    "`include \"third party/uvm ?sources/vendor_entry.sv\"\n");
        inputs = "wrapper.sv\n";
        expectedRoot = files.root / "wrapper.sv";
    }
    else if (mode == "nonprefix") {
        files.write(files.root / "prelude.sv", "module prelude; endmodule\n");
        inputs = "prelude.sv\n\"third party/uvm ?sources/vendor_entry.sv\"\n";
        expectedRoot = files.root / "prelude.sv";
    }
    files.write(files.root / "design.f", inputs + "consumer.sv\n");
    config.compilationFilelist = "design.f";
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(driver);
    const auto paths = driver->driver.sourceLoader.getFilePaths();
    REQUIRE(!paths.empty());
    CHECK(paths.front() == expectedRoot);
    CHECK(paths.back() == files.root / "consumer.sv");
    CHECK(paths.size() == (mode == "nonprefix" ? 3 : 2));
    CHECK(parameterValue(driver->driver.syntaxTrees.front()) == 1);
    auto pp = driver->options.getOrDefault<parsing::PreprocessorOptions>();
    CHECK(pp.uvmPrefixRoot == files.entry.string());
    CHECK(pp.captureUvmPrefix == cache);
    auto prefix = prepared(*driver);
    if (cache && (mode == "root" || mode == "missing")) {
        REQUIRE(prefix);
        CHECK(parameterValue(prefix) == 1);
        for (int repeat = 0; repeat < 3; ++repeat)
            CHECK(LibraryAccess::getIndexedSyntaxTree(driver->completions, files.entry.string()) ==
                  prefix);
    }
    else
        CHECK_FALSE(prefix);
}

TEST_CASE("Configured standalone UVM prepares once and sees unsaved library changes",
          "[uvm-library]") {
    const bool cache = GENERATE(false, true);
    LibraryWorkspace files;
    auto config = files.config(cache);
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(driver);
    CHECK(driver->driver.sourceLoader.getFilePaths() == std::vector<fs::path>{files.entry});
    auto prefix = prepared(*driver);
    CHECK(bool(prefix) == cache);
    if (prefix) {
        CHECK(parameterValue(prefix) == 1);
        CHECK(LibraryAccess::getIndexedSyntaxTree(driver->completions, files.entry.string()) ==
              prefix);
    }
    auto consumer = URI::fromFile(files.root / "consumer.sv");
    std::ifstream input(files.root / "consumer.sv");
    std::string text(std::istreambuf_iterator<char>(input), {});
    driver->openDocument(consumer, text, 1);
    auto analysis = driver->getDocument(consumer)->getAnalysis();
    auto* symbol = analysis->getCompilation()->getRoot().lookupName("consumer.obj");
    REQUIRE(symbol);
    CHECK(symbol->as<ast::VariableSymbol>().getType().isClass());
    if (cache)
        CHECK(prepared(*driver) == prefix);
    auto entryUri = URI::fromFile(files.entry);
    auto changed = files.source;
    changed.replace(changed.find("`VALUE"), 6, "2");
    driver->openDocument(entryUri, changed, 2);
    auto updated = LibraryAccess::getIndexedSyntaxTree(driver->completions, files.entry.string());
    REQUIRE(updated);
    CHECK(updated != prefix);
    CHECK(parameterValue(updated) == 2);
}

TEST_CASE("UVM distribution roots locate src and reject ambiguous or conflicting libraries",
          "[uvm-library]") {
    const std::string mode = GENERATE("distribution", "missing", "empty", "ambiguous", "conflict",
                                      "wrong-package");
    CAPTURE(mode);
    LibraryWorkspace files;
    auto config = files.config();
    if (mode == "distribution") {
        files.write(files.library / "src/uvm_pkg.sv", files.source);
        config.uvm.value()->entryFile = std::nullopt;
    }
    else if (mode == "missing")
        config.uvm.value()->entryFile = "missing.sv";
    else if (mode == "empty")
        config.uvm.value()->sourceDirectory = "";
    else if (mode == "ambiguous") {
        files.write(files.library / "uvm_pkg.sv", files.source);
        files.write(files.library / "src/uvm_pkg.sv", files.source);
        config.uvm.value()->entryFile = std::nullopt;
    }
    else if (mode == "conflict") {
        files.write(files.root / "other.sv", "package uvm_pkg; endpackage\n");
        files.write(files.root / "design.f", "other.sv\nconsumer.sv\n");
        config.compilationFilelist = "design.f";
    }
    else
        files.write(files.entry, "module not_uvm; endmodule\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    CHECK(bool(driver) == (mode == "distribution"));
    if (driver) {
        auto prefix = prepared(*driver);
        REQUIRE(prefix);
        CHECK(parameterValue(prefix) == 1);
    }
}

TEST_CASE("Failed UVM reconfiguration preserves the original unsaved context", "[uvm-library]") {
    LibraryWorkspace files;
    auto config = files.config();
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto uri = URI::fromFile(files.root / "consumer.sv");
    const std::string text = "module consumer; uvm_pkg::object obj; endmodule // unsaved\n";
    driver->openDocument(uri, text, 3);
    auto oldPrefix = prepared(*driver);
    config.uvm.value()->entryFile = "missing.sv";
    auto rejected = server::ServerDriver::createForExplore(indexer, client, config,
                                                           files.root.string(), driver.get(), true);
    CHECK_FALSE(rejected);
    CHECK(driver->getDocument(uri)->getText().starts_with(text));
    CHECK(prepared(*driver) == oldPrefix);
    CHECK(parameterValue(oldPrefix) == 1);
}

TEST_CASE("Explicit UVM preparation separates macro configurations and selected entries",
          "[uvm-library]") {
    LibraryWorkspace files;
    auto config = files.config();
    Indexer indexer;
    ClientHarness client;
    auto first = server::ServerDriver::createForExplore(indexer, client, config,
                                                        files.root.string(), nullptr, true);
    REQUIRE(first);
    auto original = prepared(*first);
    REQUIRE(original);
    config.flagsByFile.value()[0].flags = "--single-unit --threads 1 -DVALUE=9";
    auto second = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), first.get(), true);
    REQUIRE(second);
    auto differentMacros = prepared(*second);
    REQUIRE(differentMacros);
    CHECK(differentMacros != original);
    CHECK(parameterValue(differentMacros) == 9);
    files.write(files.library / "alternate.sv",
                "package uvm_pkg; localparam int X=3; endpackage\n");
    config.uvm.value()->entryFile = "alternate.sv";
    auto third = server::ServerDriver::createForExplore(indexer, client, config,
                                                        files.root.string(), second.get(), true);
    REQUIRE(third);
    auto differentEntry = prepared(*third);
    REQUIRE(differentEntry);
    CHECK(differentEntry != differentMacros);
    CHECK(parameterValue(differentEntry) == 3);
    CHECK(parameterValue(original) == 1);
}

TEST_CASE("UVM input watches repair missing entries and distribution directories",
          "[uvm-library]") {
    const bool missingDirectory = GENERATE(false, true);
    CAPTURE(missingDirectory);
    LibraryWorkspace files;
    auto config = files.config();
    config.flags = "--single-unit --threads 1 -DVALUE=1";
    fs::path target = files.library / "missing.sv";
    if (missingDirectory) {
        config.uvm.value()->sourceDirectory = "not-yet";
        config.uvm.value()->entryFile = std::nullopt;
        target = files.root / "not-yet/uvm_pkg.sv";
    }
    else
        config.uvm.value()->entryFile = "missing.sv";
    files.write(files.root / ".slang/server.json", rfl::json::write(config));
    ServerHarness server(lsp::InitializeParams{
        .workspaceFolders = {
            {lsp::WorkspaceFolder{.uri = URI::fromFile(files.root), .name = "test"}}}});
    server.expectError(missingDirectory
                           ? "UVM configuration: sourceDirectory is not a directory: " +
                                 target.parent_path().string()
                           : "UVM configuration: entry file is unavailable: " + target.string());
    REQUIRE(server.m_driver->configuredInputLoadFailed());
    auto paths = server.m_driver->getContextWatchPaths();
    CHECK(std::ranges::find(paths, target) != paths.end());
    files.write(target, files.source);
    files.write(target.parent_path() / "uvm_macros.svh", "`define PASS(a) a\n");
    server.onWorkspaceDidChangeWatchedFiles(
        {.changes = {{.uri = URI::fromFile(target), .type = lsp::FileChangeType::Created}}});
    CHECK_FALSE(server.m_driver->configuredInputLoadFailed());
    auto prefix = prepared(*server.m_driver);
    REQUIRE(prefix);
    CHECK(parameterValue(prefix) == 1);
    auto consumer = server.openFile("consumer.sv");
    REQUIRE(consumer.doc->getAnalysis()->getCompilation()->getPackage("uvm_pkg"));
}

TEST_CASE("UVM entry disk reload preserves unsaved user and library overlays", "[uvm-library]") {
    const bool libraryOpen = GENERATE(false, true);
    CAPTURE(libraryOpen);
    LibraryWorkspace files;
    auto config = files.config();
    config.flags = "--single-unit --threads 1 -DVALUE=1";
    files.write(files.root / ".slang/server.json", rfl::json::write(config));
    ServerHarness server(lsp::InitializeParams{
        .workspaceFolders = {
            {lsp::WorkspaceFolder{.uri = URI::fromFile(files.root), .name = "test"}}}});
    auto consumer = server.openFile("consumer.sv");
    consumer.append("// keep unsaved user text\n");
    consumer.publishChanges();
    auto entryUri = URI::fromFile(files.entry);
    if (libraryOpen) {
        auto library = server.openFile(files.entry.string());
        auto text = files.source;
        text.replace(text.find("`VALUE"), 6, "5");
        library.replaceAll(text);
        library.publishChanges();
    }
    auto disk = files.source;
    disk.replace(disk.find("`VALUE"), 6, "7");
    files.write(files.entry, disk);
    server.onWorkspaceDidChangeWatchedFiles(
        {.changes = {
             {.uri = URI::fromFile(files.entry.parent_path() / "." / files.entry.filename()),
              .type = lsp::FileChangeType::Changed}}});
    auto current = server.m_driver->getDocument(consumer.m_uri);
    CHECK(current->getText().find("// keep unsaved user text") != std::string::npos);
    auto* package = current->getAnalysis()->getCompilation()->getPackage("uvm_pkg");
    REQUIRE(package);
    auto* value = package->find("X");
    REQUIRE(value);
    CHECK(value->as<ast::ParameterSymbol>().getValue().integer().as<int>() ==
          (libraryOpen ? 5 : 7));
    if (libraryOpen) {
        auto entry = server.m_driver->getDocument(entryUri);
        CHECK(entry->getText().find("X=5") != std::string::npos);
    }
}
