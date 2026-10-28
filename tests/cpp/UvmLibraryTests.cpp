// SPDX-License-Identifier: MIT
#include "ServerDriver.h"
#include "ast/ScopeBodyCache.h"
#include "ast/ServerCompilationAnalysis.h"
#include "ast/TrustedUvmLibrary.h"
#include "completions/CompletionContext.h"
#include "completions/MemberCompletions.h"
#include "utils/ServerHarness.h"
#include "util/UvmParseArtifact.h"
#include "util/QueryText.h"
#include <catch2/generators/catch_generators.hpp>
#include <fstream>
#include <sstream>
#include <rfl/json.hpp>

#include "slang/analysis/AnalysisManager.h"
#include "slang/analysis/AnalysisQueries.h"
#include "slang/analysis/ValueDriver.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/ClassSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/ast/types/TypePrinter.h"
#include "slang/diagnostics/TextDiagnosticClient.h"
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

TEST_CASE("Full-unit macro facts respect opt-in and survive trusted library retirement",
          "[uvm-library][macro-proof]") {
    LibraryWorkspace files;
    const int mode = GENERATE(0, 1, 2, 3);
    auto config = files.config(false);
    CHECK_FALSE(config.experimentalMethodBodyMacroProof.value());
    config.experimentalUvmSemanticCache = true;
    config.experimentalMethodBodyQueries = true;
    config.experimentalMethodBodyMacroProof = mode != 0;
    config.methodBodyMacroProofMaxMiB = mode == 2 ? 0 : mode == 3 ? 1 : 16;
    config.compilationFilelist = "design.f";
    files.write(files.root / "design.f",
                "\"third party/uvm ?sources/vendor_entry.sv\"\nconsumer.sv\n");
    files.write(files.entry, "`ifdef ABSENT_UVM\n`define UNREACHED 1\n`endif\n" + files.source);
    files.write(files.root / "consumer.sv",
                "`ifdef ABSENT_USER\n`define USER_BRANCH 1\n`endif\n"
                "package user_pkg; class Test extends uvm_pkg::object; "
                "function int run(); return `PASS(1); endfunction endclass endpackage\n");
    Indexer indexer;
    ClientHarness client;
    auto current = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(current);
    auto setting = current->options.get<server::TrustedUvmImport>();
    REQUIRE(setting);
    auto library = setting->library;
    REQUIRE(library);
    REQUIRE(current->driver.precompiledUvm);
    const auto* root = &current->driver.precompiledUvm->root();
    const bool tracked = mode == 1 || mode == 3;
    auto verify = [&] {
        REQUIRE(current->driver.precompiledUvm);
        CHECK(&current->driver.precompiledUvm->root() == root);
        const auto& prefix = current->driver.precompiledUvm->getPreprocessorMetadata();
        CHECK(prefix.macroAccessTrackingComplete == tracked);
        CHECK(prefix.macroAccessNames.contains("ABSENT_UVM") == tracked);
        bool userFound = false;
        for (const auto& tree : current->driver.syntaxTrees) {
            if (!tree->getPreprocessorMetadata().macroAccessNames.contains("ABSENT_USER"))
                continue;
            userFound = true;
            CHECK(tree->getPreprocessorMetadata().macroAccessTrackingComplete);
            CHECK(tree->getPreprocessorMetadata().macroAccessEndedAtEof);
            CHECK(tree->getPreprocessorMetadata().macroAccessNames.contains("ABSENT_UVM"));
        }
        CHECK(userFound == tracked);
        // A successful private fragment EOF cannot certify the full source unit.
        auto options = current->driver.createParseOptionBag();
        server::QueryText fragment(current->sm, (files.root / "fragment.sv").string(),
                                    "module fragment; endmodule\n");
        auto tree = fragment.parse(options);
        CHECK_FALSE(tree->getPreprocessorMetadata().macroAccessTrackingComplete);
        CHECK(tree->getPreprocessorMetadata().macroAccessNames.empty());
    };
    verify();
    for (int i = 0; i < 3; ++i) {
        current->preserveTrustedUvmForRetirement();
        auto next = server::ServerDriver::createForExplore(indexer, client, config,
                                                           files.root.string(), current.get(),
                                                           true, true);
        REQUIRE(next);
        REQUIRE(next->options.get<server::TrustedUvmImport>());
        CHECK(next->options.get<server::TrustedUvmImport>()->library == library);
        current = std::move(next);
        verify();
    }
}

TEST_CASE("Retained UVM macro proof changes preserve the original syntax namespace",
          "[uvm-library][macro-proof-transition]") {
    LibraryWorkspace files;
    auto config = files.config(false);
    config.experimentalUvmSemanticCache = true;
    config.experimentalMethodBodyQueries = true;
    config.compilationFilelist = "design.f";
    files.write(files.root / "design.f",
                "\"third party/uvm ?sources/vendor_entry.sv\"\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto current = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(current);
    REQUIRE(current->options.get<server::TrustedUvmImport>());
    auto library = current->options.get<server::TrustedUvmImport>()->library;
    const auto* root = &current->driver.precompiledUvm->root();
    for (auto budget : {16u, 1u, 0u, 16u}) {
        config.experimentalMethodBodyMacroProof = true;
        config.methodBodyMacroProofMaxMiB = budget;
        for (int repeat = 0; repeat < 2; ++repeat) {
            current->preserveTrustedUvmForRetirement();
            auto next = server::ServerDriver::createForExplore(indexer, client, config,
                                                               files.root.string(), current.get(),
                                                               true, true);
            REQUIRE(next);
            REQUIRE(next->options.get<server::TrustedUvmImport>());
            CHECK(next->options.get<server::TrustedUvmImport>()->library == library);
            REQUIRE(next->driver.precompiledUvm);
            CHECK(&next->driver.precompiledUvm->root() == root);
            const auto& facts = next->driver.precompiledUvm->getPreprocessorMetadata();
            CHECK(facts.macroAccessTrackingComplete == (budget != 0));
            CHECK(facts.macroAccessNames.contains("VALUE") == (budget != 0));
            CHECK(parameterValue(next->driver.precompiledUvm) == 1);
            current = std::move(next);
        }
    }
}

TEST_CASE("Trusted UVM retains semantic identities through edits and source retirement",
          "[uvm-library][uvm-semantic]") {
    LibraryWorkspace files;
    auto config = files.config(false);
    config.experimentalUvmSemanticCache = true;
    config.compilationFilelist = "design.f";
    files.write(files.root / "design.f",
                "\"third party/uvm ?sources/vendor_entry.sv\"\nconsumer.sv\n");
    auto source = [](int value) {
        return fmt::format("package user_pkg; class Test extends uvm_pkg::object; "
                           "localparam int V={}; endclass endpackage\n",
                           value);
    };
    files.write(files.root / "consumer.sv", source(0));
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto setting = driver->options.get<server::TrustedUvmImport>();
    INFO(rfl::json::write(driver->trustedUvmStatus()));
    REQUIRE(setting);
    auto library = setting->library;
    REQUIRE(library);
    auto package = library->compilation->getPackage("uvm_pkg");
    auto object = package->find("object");
    const auto uri = URI::fromFile(files.root / "consumer.sv");
    driver->openDocument(uri, source(0), 1, true);
    auto verify = [&](server::ServerDriver& current, int value) {
        auto analysis = current.getDocument(uri)->getAnalysis();
        auto& compilation = *analysis->getCompilation();
        CHECK(compilation.getSemanticDiagnostics().empty());
        REQUIRE(compilation.getPackage("uvm_pkg") == package);
        auto type = compilation.getPackage("user_pkg")->find("Test");
        REQUIRE(type);
        CHECK(type->as<ast::ClassType>().getBaseClass() == object);
        auto parameter = type->as<ast::ClassType>().find("V");
        REQUIRE(parameter);
        CHECK(*parameter->as<ast::ParameterSymbol>().getValue().integer().as<int>() == value);
    };
    verify(*driver, 0);
    for (int value = 1; value <= 4; ++value) {
        driver->onDocDidChange({.textDocument = {.version = value + 1, .uri = uri},
                                .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                                    .text = source(value)}}});
        verify(*driver, value);
    }
    const auto generation = library->generation;
    CHECK(library->consumers.load() >= 5);
    driver->preserveTrustedUvmForRetirement();
    auto replacement = server::ServerDriver::createForExplore(indexer, client, config,
                                                              files.root.string(), driver.get(),
                                                              true, true);
    REQUIRE(replacement);
    REQUIRE(replacement->options.get<server::TrustedUvmImport>());
    CHECK(replacement->options.get<server::TrustedUvmImport>()->library == library);
    CHECK(&replacement->sm != &driver->sm);
    CHECK(replacement->sm.sharesImmutablePrefix(driver->sm));
    driver.reset();
    verify(*replacement, 4);
    CHECK(library->generation == generation);
    std::weak_ptr<const void> retiredSourceOwner =
        replacement->options.get<server::TrustedUvmImport>()->sourceOwner;
    std::weak_ptr<const void> retiredLibrarySource = library->sourceOwner;
    auto retainedAnalysis = replacement->getDocument(uri)->getAnalysis();
    auto retainedCompilation = retainedAnalysis->getCompilation();
    const auto retainedBuffer = retainedAnalysis->getBuffer();
    retainedAnalysis.reset();
    // Provider changes are intentionally unapplied until explicit reconstruction.
    files.write(files.entry, "package uvm_pkg; localparam int X=42; "
                             "class object; int newer; endclass endpackage\n");
    replacement->onWorkspaceDidChangeWatchedFiles(
        {.changes = {{.uri = URI::fromFile(files.entry), .type = lsp::FileChangeType::Changed}}});
    verify(*replacement, 4);
    replacement->invalidateUvmParseCache();
    auto rebuilt = server::ServerDriver::createForExplore(indexer, client, config,
                                                          files.root.string(), replacement.get(),
                                                          true, true);
    REQUIRE(rebuilt);
    auto refreshed = rebuilt->options.get<server::TrustedUvmImport>();
    REQUIRE(refreshed);
    CHECK(refreshed->library->generation != generation);
    auto newObject = refreshed->library->compilation->getPackage("uvm_pkg")->find("object");
    CHECK(newObject != object);
    REQUIRE(newObject->as<ast::ClassType>().find("newer"));
    replacement.reset();
    auto expectedSource = source(4);
    expectedSource.push_back('\0');
    CHECK(retainedCompilation->getSourceManager()->getSourceText(retainedBuffer) == expectedSource);
    CHECK_FALSE(retiredSourceOwner.expired());
    retainedCompilation.reset();
    CHECK(retiredSourceOwner.expired());
    library.reset();
    CHECK(retiredLibrarySource.expired());
}

TEST_CASE("Initial fixed provider isolation borrows the native parse and raw read history",
          "[scope-semantic][scope-initial-source-reuse]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.experimentalCooperativeAnalysis = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", "module stable; endmodule\n");
    files.write(files.root / "consumer.sv", "module top; stable u(); endmodule\n");
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto bootstrap = std::make_unique<server::ServerDriver>(indexer, client, config,
                                                            std::vector<std::string>{"design.f"},
                                                            files.root.string(), true);
    REQUIRE(bootstrap->options.get<server::TrustedUvmImport>());
    REQUIRE(bootstrap->driver.syntaxTrees.size() == 1);
    const auto tree = bootstrap->driver.syntaxTrees.front();
    auto consumer = std::make_unique<server::ServerDriver>(
        indexer, client, config, std::vector<std::string>{"design.f"}, files.root.string(), true,
        bootstrap.get(), nullptr, true, nullptr, true);
    REQUIRE(consumer->driver.syntaxTrees.size() == 1);
    CHECK(&consumer->driver.syntaxTrees.front()->root() == &tree->root());
    CHECK(&consumer->driver.syntaxTrees.front()->sourceManager() == &consumer->sm);
    CHECK(&consumer->sm != &bootstrap->sm);
    auto graph = tree->getSourceBufferIds();
    const auto recorded = consumer->sm.captureSnapshot(graph);
    CHECK(recorded.files.size() == bootstrap->sm.captureSnapshot(graph).files.size());
    CHECK_FALSE(consumer->sm.sourceWitnessFailure({}));
    // Promotion can borrow semantic providers without borrowing their native
    // parse. It must not create a stitched retirement prefix whose retained
    // syntax owners inflate admission for the first complete design.
    auto promotion = std::make_unique<server::ServerDriver>(
        indexer, client, config, std::vector<std::string>{"design.f"}, files.root.string(), true,
        bootstrap.get(), nullptr, true);
    REQUIRE(promotion->driver.syntaxTrees.size() == 1);
    CHECK(&promotion->driver.syntaxTrees.front()->root() != &tree->root());
    CHECK(rfl::json::write(promotion->scopeLibraryStatus()).find("\"syntaxResumes\":0") !=
          std::string::npos);
    auto* provider = bootstrap->options.get<server::TrustedUvmImport>()
                         ->library->compilation->getSourceManager();
    const auto bytes = provider->getMemoryUsage();
    consumer->sm.assignText("unrelated-edit.sv", std::string(1024 * 1024, 'x'));
    CHECK(provider->getMemoryUsage() == bytes);
    files.write(files.root / "consumer.sv", "module top; int changed; endmodule\n");
    CHECK(consumer->sm.sourceWitnessFailure({}).has_value());
}

TEST_CASE("Retirement resumes a fixed prefix with current native loader inputs",
          "[scope-semantic][scope-retirement-prefix]") {
    const bool reorder = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.experimentalCooperativeAnalysis = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv",
                "`define FIXED_VALUE 37\nmodule stable; endmodule\n");
    files.write(files.root / "side.sv", "module side; endmodule\n");
    const std::string disk =
        "module top; stable u(); localparam int V=`FIXED_VALUE+1; endmodule\n";
    const std::string edited =
        "module top; stable u(); localparam int V=`FIXED_VALUE+9; endmodule\n";
    files.write(files.root / "consumer.sv", disk);
    files.write(files.root / "design.f", "fixed.sv\nside.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(
        indexer, client, config, {"design.f"}, files.root.string(), nullptr, true);
    REQUIRE(driver);
    REQUIRE(driver->options.get<server::TrustedUvmImport>());
    const auto library = driver->options.get<server::TrustedUvmImport>()->library;
    auto uri = URI::fromFile(files.root / "consumer.sv");
    driver->openDocument(uri, edited, 2, true);
    if (reorder)
        files.write(files.root / "design.f", "side.sv\nfixed.sv\nconsumer.sv\n");
    driver->preserveTrustedUvmForRetirement();
    auto replacement = server::ServerDriver::createFromFileLists(
        indexer, client, config, {"design.f"}, files.root.string(), driver.get(), true, true);
    REQUIRE(replacement);
    CHECK_FALSE(bool(replacement->driver.sourceLoader.singleUnitParseOverride));
    CHECK(replacement->options.get<server::TrustedUvmImport>()->library == library);
    INFO(rfl::json::write(replacement->scopeLibraryStatus()));
    CHECK(rfl::json::write(replacement->scopeLibraryStatus())
              .find(reorder ? "\"syntaxResumes\":0" : "\"syntaxResumes\":1") !=
          std::string::npos);
    auto& body = replacement->comp->getInstancesOfModule("top").front()->body;
    CHECK(body.find("V")->as<ast::ParameterSymbol>().getValue().integer().as<int>() == 46);
    CHECK(body.getCompilation().getSemanticDiagnostics().empty());
    driver.reset();
    CHECK(body.find("V")->as<ast::ParameterSymbol>().getValue().integer().as<int>() == 46);
}

TEST_CASE("Native retirement rejects edited fixed roots and included dependencies",
          "[scope-semantic][scope-retirement-fixed-edit]") {
    const bool header = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.experimentalCooperativeAnalysis = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    const std::string rootText =
        "module stable;\n`include \"fixed.svh\"\nendmodule\n";
    const std::string headerText = "localparam int N=37;\n";
    files.write(files.root / "fixed.sv", rootText);
    files.write(files.root / "fixed.svh", headerText);
    files.write(files.root / "consumer.sv", "module top; stable u(); endmodule\n");
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(
        indexer, client, config, {"design.f"}, files.root.string(), nullptr, true);
    REQUIRE(driver);
    REQUIRE(driver->options.get<server::TrustedUvmImport>());
    const auto generation = driver->options.get<server::TrustedUvmImport>()->library->generation;
    const auto uri = URI::fromFile(files.root / (header ? "fixed.svh" : "fixed.sv"));
    driver->openDocument(uri, header ? headerText : rootText, 1, true);
    const std::string edited = header ? "localparam int N=99;\n"
                                      : "module stable; int edited;\n"
                                        "`include \"fixed.svh\"\nendmodule\n";
    driver->onDocDidChange(
        {.textDocument = {.version = 2, .uri = uri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = edited}}});
    driver->preserveTrustedUvmForRetirement();
    CHECK_THROWS_AS(server::ServerDriver::createFromFileLists(
                        indexer, client, config, {"design.f"}, files.root.string(), driver.get(),
                        true, true),
                    lsp::RequestFailed);
    CHECK(driver->getDocument(uri)->getText().starts_with(edited));
    CHECK(driver->options.get<server::TrustedUvmImport>()->library->generation == generation);
    driver->preserveTrustedUvmForRetirement(false);
    auto rebuilt = server::ServerDriver::createFromFileLists(
        indexer, client, config, {"design.f"}, files.root.string(), driver.get(), true, true);
    REQUIRE(rebuilt);
    CHECK(rebuilt->options.get<server::TrustedUvmImport>()->library->generation != generation);
    CHECK(rebuilt->comp->getInstancesOfModule("top").size() == 1);
}

TEST_CASE("Cancelled RTL consumers release models and evicted source generations",
          "[scope-semantic][scope-source-lifetime]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.experimentalCooperativeAnalysis = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 1};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv",
                "module stable #(parameter int N=1)(input int a, output int y);\n"
                "assign y=a+N; endmodule\n");
    auto source = [](int n) {
        return fmt::format("module top(input int a, output int y); "
                           "stable #(.N({})) u(.*); endmodule\n",
                           n);
    };
    files.write(files.root / "consumer.sv", source(0));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    constexpr size_t unrelatedBytes = 4 * 1024 * 1024;
    {
        // Reproduce the original ownership mechanism without the factory's
        // isolation step. The real editor factory must never expose this setup
        // driver to subsequent editing.
        auto bootstrap = std::make_unique<server::ServerDriver>(
            indexer, client, config, std::vector<std::string>{"design.f"}, files.root.string(),
            true);
        auto imports = bootstrap->options.get<server::TrustedUvmImport>();
        REQUIRE(imports);
        auto library = imports->library;
        auto* sources = library->compilation->getSourceManager();
        REQUIRE(sources == &bootstrap->sm);
        std::weak_ptr<const void> sourceOwner = bootstrap->driver.getSourceOwner();
        const auto before = sources->getMemoryUsage();
        bootstrap->sm.assignText("baseline-unrelated.sv", std::string(unrelatedBytes, 'x'));
        bootstrap.reset();
        CHECK_FALSE(sourceOwner.expired());
        CHECK(sources->getMemoryUsage() >= before + unrelatedBytes);
        fmt::print(stderr, "scope-source-lifetime baselineBefore={} baselineAfter={}\n", before,
                   sources->getMemoryUsage());
        library.reset();
        CHECK(sourceOwner.expired());
    }
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto setting = driver->options.get<server::ScopeBodyImport>();
    REQUIRE(setting);
    auto cache = setting->cache.lock();
    REQUIRE(cache);
    INFO(rfl::json::write(cache->status()));
    REQUIRE(cache->status().publications > 0);
    std::weak_ptr<const void> originalSource = driver->driver.getSourceOwner();
    const std::string_view names[] = {"stable"};
    std::weak_ptr<ast::Compilation> originalModel =
        driver->comp->createActiveDesignContext(names).retainCompilation();
    std::vector<std::weak_ptr<const void>> cancelledSources;
    driver->preserveTrustedUvmForRetirement();
    for (int n = 1; n <= 64; ++n) {
        CAPTURE(n);
        files.write(files.root / "consumer.sv", source(n));
        lsp::RequestContext construction("retirement construction", std::nullopt);
        auto candidate = server::ServerDriver::createFromFileLists(
            indexer, client, config, {"design.f"}, files.root.string(), driver.get(), true, true,
            &construction);
        REQUIRE(candidate);
        std::weak_ptr<ast::Compilation> model =
            candidate->comp->createActiveDesignContext(names).retainCompilation();
        cancelledSources.emplace_back(candidate->driver.getSourceOwner());
        const auto publications = cache->status().publications;
        lsp::RequestContext request("cancel candidate diagnostics", std::nullopt);
        bool reached = false;
        request.setCheckpointObserverForTesting([&](std::string_view where) {
            if (where == "after diagnostic scope") {
                reached = true;
                request.cancel();
            }
        });
        CHECK_THROWS_AS(candidate->comp->prepareDiagnostics(request), lsp::RequestCancelled);
        REQUIRE(reached);
        REQUIRE(cache->status().publications > publications);
        candidate.reset();
        CHECK(model.expired());
        // The cached body keeps its own namespace, never the cancelled editor's.
        CHECK(cancelledSources.back().expired());
        const auto usage = cache->status();
        CHECK(usage.baseBytes + usage.ownerBytes + usage.pinnedBytes + usage.admissionBytes <=
              usage.budgetBytes);
    }
    CHECK(cache->status().evictions > 0);
    CHECK(cancelledSources.front().expired());
    auto original = driver->comp->getActiveInstanceSymbol("stable");
    REQUIRE(original);
    CHECK(*original->body.find("N")->as<ast::ParameterSymbol>().getValue().integer().as<int>() ==
          0);
    CHECK_FALSE(originalModel.expired());
    CHECK_FALSE(originalSource.expired());
    // The provider must have a separate namespace before any editing begins.
    auto* providerSources =
        driver->options.get<server::TrustedUvmImport>()->library->compilation->getSourceManager();
    REQUIRE(providerSources != &driver->sm);
    const auto sourceBytesBefore = providerSources->getMemoryUsage();
    driver->sm.assignText("unrelated-generation.sv", std::string(unrelatedBytes, 'x'));
    CHECK(providerSources->getMemoryUsage() == sourceBytesBefore);
    files.write(files.root / "consumer.sv", source(100));
    auto replacement = server::ServerDriver::createFromFileLists(indexer, client, config,
                                                                 {"design.f"}, files.root.string(),
                                                                 driver.get(), true, true);
    REQUIRE(replacement);
    replacement->comp->prepareDiagnostics({});
    auto retained = replacement->comp->createActiveDesignContext(names).retainCompilation();
    std::weak_ptr<const void> replacementSource = replacement->driver.getSourceOwner();
    driver.reset();
    CHECK(originalModel.expired());
    CHECK(originalSource.expired());
    CHECK(providerSources->getMemoryUsage() == sourceBytesBefore);
    fmt::print(stderr, "scope-source-lifetime originBefore={} originAfter={} cacheBudget={}\n",
               sourceBytesBefore, providerSources->getMemoryUsage(), cache->status().budgetBytes);
    replacement.reset();
    CHECK_FALSE(replacementSource.expired());
    CHECK(retained->getSemanticDiagnostics().empty());
    retained.reset();
    cache.reset();
    CHECK(originalSource.expired());
    CHECK(replacementSource.expired());
    for (const auto& lease : cancelledSources)
        CHECK(lease.expired());
}

TEST_CASE("Stable RTL body reuse does not retain the retired editing source namespace",
          "[scope-semantic][scope-stable-source-lifetime]") {
    const bool fullContext = GENERATE(false, true);
    const bool edited = GENERATE(false, true);
    CAPTURE(fullContext);
    CAPTURE(edited);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.experimentalCooperativeAnalysis = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(),
         std::string("--single-unit --threads 1 --top top") + (fullContext ? " --top other" : "")}};
    files.write(files.root / "fixed.sv",
                "module stable #(parameter int N=1)(input int a, output int y); "
                "assign y=a+N; endmodule\n");
    files.write(files.root / "consumer.sv",
                "module top(input int a, output int y); stable u(.*); endmodule\n"
                "module other; endmodule\n");
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    REQUIRE(cache->status().publications > 0);
    CHECK(cache->status().compactPublications == (fullContext ? 0 : 1));
    if (edited) {
        const auto uri = URI::fromFile(files.root / "consumer.sv");
        const std::string first = "module top(input int a, output int y); stable u(.*); endmodule\n"
                                  "module other; endmodule\n";
        driver->openDocument(uri, first, 1, true);
        driver->onDocDidChange(
            {.textDocument = {.version = 2, .uri = uri},
             .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                 .text = "module top(input int a, output int y); stable #(.N(2)) u(.*); "
                         "endmodule\nmodule other; endmodule\n"}}});
        driver->comp->prepareDiagnostics({});
        REQUIRE(cache->status().publications == 2);
    }
    std::weak_ptr<const void> source = driver->driver.getSourceOwner();
    const std::string_view names[] = {"stable"};
    std::weak_ptr<ast::Compilation> model =
        driver->comp->createActiveDesignContext(names).retainCompilation();
    driver->sm.assignText("unrelated-edit.sv", std::string(4 * 1024 * 1024, 'x'));
    driver->preserveTrustedUvmForRetirement();
    auto replacement = server::ServerDriver::createFromFileLists(indexer, client, config,
                                                                 {"design.f"}, files.root.string(),
                                                                 driver.get(), true);
    REQUIRE(replacement);
    REQUIRE(cache->status().hits > 0);
    driver.reset();
    CHECK(model.expired());
    CHECK(source.expired());
    auto instance = replacement->comp->getActiveInstanceSymbol("stable");
    REQUIRE(instance);
    CHECK(*instance->body.find("N")->as<ast::ParameterSymbol>().getValue().integer().as<int>() ==
          (edited ? 2 : 1));
    const auto storage = replacement->sourceStorageUsage();
    CHECK(storage.retained.managers >= 2); // Fixed library and independent RTL body.
    CHECK(storage.total.source.dataBytes < 4 * 1024 * 1024);
    CHECK(storage.retained.source.queryBytes == 0);
    replacement.reset();
    cache.reset();
    CHECK(source.expired());
}

TEST_CASE("Trusted UVM stays opt-in and reports unavailable semantic preparation",
          "[uvm-library][uvm-semantic]") {
    CHECK_FALSE(Config{}.experimentalUvmSemanticCache.value());
    LibraryWorkspace files;
    auto config = files.config(false);
    config.experimentalUvmSemanticCache = true;
    config.uvm.value()->semanticMaxMiB = 0;
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(driver);
    CHECK_FALSE(driver->options.get<server::TrustedUvmImport>());
    CHECK(rfl::json::write(driver->trustedUvmStatus()).find("semanticMaxMiB") != std::string::npos);
    const auto uri = URI::fromFile(files.root / "consumer.sv");
    driver->openDocument(uri,
                         "`include \"uvm_macros.svh\"\nmodule consumer; "
                         "uvm_pkg::object obj; localparam int Y=`PASS(1); endmodule\n",
                         1, true);
    auto compilation = driver->getDocument(uri)->getAnalysis()->getCompilation();
    CHECK(compilation->getPackage("uvm_pkg"));
    CHECK(compilation->getSemanticDiagnostics().empty());
}

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

TEST_CASE("Bundled UVM is explicit and independently supports native and cached analysis",
          "[uvm-library][bundled-uvm]") {
    const bool cache = GENERATE(false, true);
    LibraryWorkspace files;
    files.write(files.root / "consumer.sv",
                "`include \"uvm_macros.svh\"\nimport uvm_pkg::*;\n"
                "class item extends uvm_sequence_item; `uvm_object_utils(item)\n"
                "function new(string name=\"item\"); super.new(name); endfunction endclass\n"
                "module consumer; item obj; initial begin obj=new(); obj.print(); end endmodule\n");
    files.write(files.root / "design.f", "consumer.sv\n");
    auto config = files.config(cache);
    config.uvm = Config::UvmConfig{.useBundled = true};
    config.compilationFilelist = "design.f";
    config.experimentalFrozenSources = true;
    config.frozenSources = Config::FrozenSourcesConfig{
        .packages = std::vector<std::string>{"uvm_pkg"}};
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(driver);
    CHECK_FALSE(driver->frozenInputWitnessFailure());
    REQUIRE(driver->driver.syntaxTrees.size() == 1);
    REQUIRE(driver->driver.precompiledUvm);
    CHECK_FALSE(driver->driver.precompiledUvm->getPreprocessorMetadata()
                    .macroAccessTrackingComplete);
    CHECK(driver->driver.precompiledUvm->getPreprocessorMetadata().macroAccessNames.empty());
    const auto root = driver->driver.precompiledUvmRoot;
    auto artifact = server::uvm_artifact::read(root.parent_path() / "preparsed/default.cst");
    parsing::PreprocessorOptions traceOptions;
    traceOptions.trackMacroAccesses = true;
    traceOptions.precompiledSourceRoot = root;
    SourceManager restoredSources;
    std::istringstream proofInput(std::move(artifact.syntax), std::ios::binary);
    auto withFacts = syntax::SyntaxTree::readParseArchive(proofInput, restoredSources, root,
                                                         Bag{traceOptions});
    CHECK(withFacts->getPreprocessorMetadata().macroAccessTrackingComplete);
    CHECK(withFacts->getPreprocessorMetadata().macroAccessEndedAtEof);
    CHECK(withFacts->getPreprocessorMetadata().macroAccessNames.contains("uvm_info"));
    // Indexing the installed library consumes its restored macro metadata.
    indexer.startIndexing(
        std::vector<std::string>{(driver->driver.precompiledUvmRoot / "**/*.svh").string()}, {});
    CHECK_FALSE(indexer.getFilesForMacro("uvm_object_utils").empty());
    ast::Compilation compilation(driver->options);
    compilation.addSyntaxTree(driver->driver.syntaxTrees.front());
    REQUIRE(compilation.getPackage("uvm_pkg"));
    CHECK(compilation.getRoot().lookupName("consumer.obj"));
    for (const auto& diagnostic : compilation.getAllDiagnostics())
        CHECK_FALSE(diagnostic.isError());
    auto paths = driver->driver.sourceLoader.getFilePaths();
    REQUIRE(paths.size() == 2);
    CHECK(paths.front().filename() == "uvm_pkg.sv");
    CHECK(paths.back() == files.root / "consumer.sv");
    CHECK(bool(prepared(*driver)) == cache);
}

TEST_CASE("Bundled UVM rejects conflicting input choices and project libraries",
          "[uvm-library][bundled-uvm]") {
    const auto mode = GENERATE("path", "entry", "project");
    CAPTURE(mode);
    LibraryWorkspace files;
    Config config;
    config.uvm = Config::UvmConfig{.useBundled = true};
    if (std::string_view(mode) == "path")
        config.uvm.value()->sourceDirectory = files.library.string();
    else if (std::string_view(mode) == "entry")
        config.uvm.value()->entryFile = "uvm_pkg.sv";
    else {
        files.write(files.root / "project.sv", "package uvm_pkg; endpackage\n");
        files.write(files.root / "design.f", "project.sv\n");
        config.compilationFilelist = "design.f";
    }
    Indexer indexer;
    ClientHarness client;
    CHECK_FALSE(server::ServerDriver::createForExplore(indexer, client, config, files.root.string(),
                                                       nullptr, true));
}

TEST_CASE("Bundled UVM does not activate with ordinary default project inputs",
          "[uvm-library][bundled-uvm]") {
    const bool explicitFalse = GENERATE(false, true);
    LibraryWorkspace files;
    files.write(files.root / "project.sv", "package uvm_pkg; localparam int USER=42; endpackage\n");
    files.write(files.root / "design.f", "project.sv\n");
    Config config;
    config.compilationFilelist = "design.f";
    if (explicitFalse)
        config.uvm = Config::UvmConfig{.sourceDirectory = files.root.string(),
                                       .entryFile = "project.sv",
                                       .useBundled = false};
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(driver);
    CHECK(driver->driver.sourceLoader.getFilePaths() ==
          std::vector<fs::path>{files.root / "project.sv"});
    ast::Compilation compilation;
    compilation.addSyntaxTree(driver->driver.syntaxTrees.front());
    auto* package = compilation.getPackage("uvm_pkg");
    REQUIRE(package);
    REQUIRE(package->find("USER"));
    CHECK_FALSE(package->find("uvm_object"));
    CHECK_FALSE(Config{}.experimentalUvmPrefixCache.value());
    CHECK_FALSE(Config{}.experimentalFrozenSources.value());
    CHECK_FALSE(Config::UvmConfig{}.useBundled.value());
}

TEST_CASE("UVM directory aliases remain part of exact entry resolution",
          "[uvm-library][bundled-uvm]") {
    LibraryWorkspace files;
    auto config = files.config(false);
    config.experimentalFrozenSources = true;
    files.write(files.library / "uvm_pkg.sv", files.source);
    auto other = files.root / "other-version";
    files.write(other / "uvm_pkg.sv", files.source);
    const auto alias = files.root / "selected";
    std::error_code error;
    fs::create_directory_symlink(files.library, alias, error);
    if (error)
        SKIP("Directory symlinks are unavailable on this host: " << error.message());
    config.uvm.value()->sourceDirectory = alias.string();
    config.uvm.value()->entryFile = std::nullopt;
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(driver);
    CHECK_FALSE(driver->frozenInputWitnessFailure());
    fs::remove(alias);
    fs::create_directory_symlink(other, alias);
    auto reason = driver->frozenInputWitnessFailure();
    REQUIRE(reason);
    CHECK(reason->find("UVM entry resolution changed") != std::string::npos);
}

TEST_CASE("Project UVM parse caches survive restart and require explicit content rebuild",
          "[uvm-library][uvm-semantic][uvm-project-cache]") {
    LibraryWorkspace files;
    auto config = files.config(false);
    config.experimentalUvmSemanticCache = true;
    Indexer indexer;
    ClientHarness client;
    auto open = [&] {
        auto result = server::ServerDriver::createForExplore(indexer, client, config,
                                                             files.root.string(), nullptr, true);
        REQUIRE(result);
        REQUIRE(result->driver.precompiledUvm);
        return result;
    };
    auto first = open();
    CHECK(parameterValue(first->driver.precompiledUvm) == 1);
    CHECK(rfl::json::write(first->trustedUvmStatus()).find("\"projectCacheHit\":false") !=
          std::string::npos);
    first.reset();
    fs::path cachePath;
    for (auto& item : fs::directory_iterator(files.root / ".slang/cache/uvm"))
        if (item.path().extension() == ".cst")
            cachePath = item.path();
    REQUIRE_FALSE(cachePath.empty());
    auto artifact = server::uvm_artifact::read(cachePath);
    parsing::PreprocessorOptions traceOptions;
    traceOptions.trackMacroAccesses = true;
    traceOptions.precompiledSourceRoot = files.library;
    SourceManager restoredSources;
    std::istringstream proofInput(std::move(artifact.syntax), std::ios::binary);
    auto withFacts = syntax::SyntaxTree::readParseArchive(proofInput, restoredSources,
                                                         files.library, Bag{traceOptions}, true);
    CHECK(withFacts->getPreprocessorMetadata().macroAccessTrackingComplete);
    CHECK(withFacts->getPreprocessorMetadata().macroAccessEndedAtEof);
    CHECK(withFacts->getPreprocessorMetadata().macroAccessNames.contains("VALUE"));
    CHECK(parameterValue(withFacts) == 1);
    auto again = open();
    CHECK(rfl::json::write(again->trustedUvmStatus()).find("\"projectCacheHit\":true") !=
          std::string::npos);
    CHECK(parameterValue(again->driver.precompiledUvm) == 1);
    again.reset();
    files.write(cachePath, "damaged cache envelope");
    auto repaired = open();
    CHECK(parameterValue(repaired->driver.precompiledUvm) == 1);
    CHECK(rfl::json::write(repaired->trustedUvmStatus()).find("\"projectCacheHit\":false") !=
          std::string::npos);
    repaired.reset();
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 -DVALUE=2"}};
    auto changed = open();
    CHECK(parameterValue(changed->driver.precompiledUvm) == 2);
    CHECK(rfl::json::write(changed->trustedUvmStatus()).find("\"projectCacheHit\":false") !=
          std::string::npos);
    changed.reset();
    files.write(files.entry, "package uvm_pkg; localparam int X=7; class object; "
                             "int field; endclass endpackage\n");
    auto trusted = open();
    CHECK(parameterValue(trusted->driver.precompiledUvm) == 2);
    trusted->invalidateUvmParseCache();
    trusted.reset();
    auto rebuilt = open();
    CHECK(parameterValue(rebuilt->driver.precompiledUvm) == 7);
    CHECK(rfl::json::write(rebuilt->trustedUvmStatus()).find("\"projectCacheHit\":false") !=
          std::string::npos);
}

TEST_CASE("Custom UVM wrappers with dependent package types retain native identities",
          "[uvm-library][uvm-semantic][uvm-project-cache]") {
    LibraryWorkspace files;
    files.write(files.entry, "package helper_pkg; class T; endclass endpackage\n"
                             "package uvm_pkg; function void consume(helper_pkg::T value); "
                             "endfunction endpackage\n");
    const std::string consumer = "module consumer; helper_pkg::T value; initial begin value=new(); "
                                 "uvm_pkg::consume(value); end endmodule\n";
    files.write(files.root / "consumer.sv", consumer);
    files.write(files.root / "design.f",
                "\"third party/uvm ?sources/vendor_entry.sv\"\nconsumer.sv\n");
    auto config = files.config(false);
    config.experimentalUvmSemanticCache = true;
    config.compilationFilelist = "design.f";
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(driver);
    REQUIRE(driver->driver.precompiledUvm);
    CHECK_FALSE(driver->options.get<server::TrustedUvmImport>());
    CHECK(rfl::json::write(driver->trustedUvmStatus()).find("additional declarations") !=
          std::string::npos);
    auto uri = URI::fromFile(files.root / "consumer.sv");
    driver->openDocument(uri, consumer, 1, true);
    auto compilation = driver->getDocument(uri)->getAnalysis()->getCompilation();
    for (const auto& diagnostic : compilation->getAllDiagnostics())
        CHECK_FALSE(diagnostic.isError());
}

TEST_CASE("Initial configured promotion reuses only matching fixed library inputs",
          "[scope-semantic][uvm-semantic][initial-library]") {
    LibraryWorkspace files;
    const int libraryMode = GENERATE(0, 1, 2);
    const bool withUvm = libraryMode != 0;
    const int change = GENERATE(0, 1, 2, 3, 4);
    const bool parallel = GENERATE(false, true);
    INFO("UVM " << withUvm << ", change " << change);
    Config config = withUvm ? files.config(false) : Config{};
    config.experimentalUvmSemanticCache = withUvm;
    config.experimentalParallelDiagnostics = parallel;
    config.parallelDiagnosticThreads = 2;
    config.experimentalScopeSemanticCache = libraryMode != 2;
    config.experimentalCooperativeAnalysis = change != 4;
    config.frozenSources = Config::FrozenSourcesConfig{
        .packages = std::vector<std::string>{"stable_pkg"}, .maxMiB = 64};
    config.compilationFilelist = "design.f";
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1"}};
    files.write(files.root / "stable.sv",
                "package stable_pkg;\n`ifdef SELECT_ALT\nlocalparam int X=2;\n"
                "`else\nlocalparam int X=`VALUE;\n`endif\nendpackage\n");
    files.write(files.root / "consumer.sv",
                "package user_pkg; localparam int Y=stable_pkg::X; endpackage\n");
    auto filelist = std::string("-DVALUE=1\n") +
                    (withUvm ? "\"third party/uvm ?sources/vendor_entry.sv\"\n" : "") +
                    "stable.sv\nconsumer.sv\n";
    files.write(files.root / "design.f", filelist);
    Indexer indexer;
    ClientHarness client;
    auto initial = server::ServerDriver::createForExplore(indexer, client, config,
                                                          files.root.string(), nullptr, true);
    REQUIRE(initial);
    REQUIRE_FALSE(initial->comp);
    auto setting = initial->options.get<server::TrustedUvmImport>();
    REQUIRE(setting);
    auto library = setting->library;
    // Consumer bytes are always reparsed, even when the fixed provider is reused.
    files.write(files.root / "consumer.sv",
                "package user_pkg; localparam int Y=stable_pkg::X+3; endpackage\n");
    if (change == 1)
        files.write(files.root / "design.f", "-DSELECT_ALT\n" + filelist);
    if (change == 2) {
        fs::create_directory(files.root / "other-includes");
        files.write(files.root / "design.f", "+incdir+other-includes\n" + filelist);
    }
    if (change == 3)
        files.write(files.root / "design.f", "--max-constexpr-steps 2048\n" + filelist);
    auto promoted = server::ServerDriver::createFromFileLists(indexer, client, config,
                                                              {(files.root / "design.f").string()},
                                                              files.root.string(), initial.get(),
                                                              true);
    REQUIRE(promoted);
    REQUIRE(promoted->comp);
    const auto* scheduling = promoted->options.get<server::ParallelDiagnosticOptions>();
    REQUIRE(scheduling);
    CHECK(scheduling->enabled == parallel);
    CHECK(scheduling->workers == 2);
    setting = promoted->options.get<server::TrustedUvmImport>();
    REQUIRE(setting);
    CHECK((setting->library == library) == (change == 0));
    // The real server destroys the exploration driver immediately after the
    // factory returns. Reused syntax and semantic owners must survive that.
    initial.reset();
    CHECK(promoted->diagClient->publishesOnDestruction());
    const auto uri = URI::fromFile(files.root / "consumer.sv");
    auto compilation = promoted->getDocument(uri)->getAnalysis()->getCompilation();
    auto user = compilation->getPackage("user_pkg");
    REQUIRE(user);
    CHECK(*user->find("Y")->as<ast::ParameterSymbol>().getValue().integer().as<int>() ==
          (change == 1 ? 5 : 4));
    for (const auto& diagnostic : compilation->getAllDiagnostics())
        CHECK_FALSE(diagnostic.isError());
    // A subsequent explicit rebuild must observe changed provider content.
    auto promotedLibrary = setting->library;
    files.write(files.root / "stable.sv", "package stable_pkg; localparam int X=43; endpackage\n");
    auto rebuilt = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                             files.root.string(), promoted.get(),
                                                             true);
    REQUIRE(rebuilt);
    setting = rebuilt->options.get<server::TrustedUvmImport>();
    REQUIRE(setting);
    CHECK(setting->library != promotedLibrary);
    compilation = rebuilt->getDocument(uri)->getAnalysis()->getCompilation();
    user = compilation->getPackage("user_pkg");
    REQUIRE(user);
    CHECK(*user->find("Y")->as<ast::ParameterSymbol>().getValue().integer().as<int>() == 46);
}

TEST_CASE("Configured library filelists keep identical options across rebuild and retirement",
          "[scope-semantic][uvm-semantic][retained-filelist]") {
    LibraryWorkspace files;
    const int mode = GENERATE(0, 1, 2);
    CAPTURE(mode);
    auto config = mode ? files.config(false) : Config{};
    config.experimentalCooperativeAnalysis = true;
    config.experimentalUvmSemanticCache = mode != 0;
    config.experimentalScopeSemanticCache = mode != 2;
    config.frozenSources = Config::FrozenSourcesConfig{
        .packages = std::vector<std::string>{"stable_pkg"}, .maxMiB = 64};
    config.compilationFilelist = "design.f";
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1"}};
    fs::create_directories(files.root / "includes");
    const auto filelist = std::string("-DVALUE=1\n+incdir+includes\n") +
                          (mode ? "\"third party/uvm ?sources/vendor_entry.sv\"\n" : "") +
                          "stable.sv\nconsumer.sv\n";
    files.write(files.root / "design.f", filelist);
    files.write(files.root / "stable.sv",
                "package stable_pkg; localparam int X=`VALUE; endpackage\n");
    const std::string text = "package user_pkg; localparam int Y=stable_pkg::X+3; endpackage\n";
    files.write(files.root / "consumer.sv", text);
    Indexer indexer;
    ClientHarness client;
    auto initial = server::ServerDriver::createForExplore(indexer, client, config,
                                                          files.root.string(), nullptr, true);
    REQUIRE(initial);
    auto current = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                             files.root.string(), initial.get(),
                                                             true);
    REQUIRE(current);
    initial.reset();
    const auto uri = URI::fromFile(files.root / "consumer.sv");
    const std::string incomplete =
        "package user_pkg; localparam int Y=stable_pkg::X+; endpackage\n";
    current->openDocument(uri, incomplete, 7, true);
    auto retire = [&] {
        auto setting = current->options.get<server::TrustedUvmImport>();
        REQUIRE(setting);
        auto library = setting->library;
        current->preserveTrustedUvmForRetirement();
        auto replacement = server::ServerDriver::createFromFileLists(
            indexer, client, config, {(files.root / "design.f").string()}, files.root.string(),
            current.get(), true);
        REQUIRE(replacement);
        REQUIRE(replacement->options.get<server::TrustedUvmImport>());
        CHECK(replacement->options.get<server::TrustedUvmImport>()->library == library);
        CHECK(replacement->getDocument(uri)->textMatches(incomplete));
        CHECK(replacement->getDocument(uri)->getVersion() == 7);
        current = std::move(replacement);
    };
    retire();
    auto library = current->options.get<server::TrustedUvmImport>()->library;
    current->preserveTrustedUvmForRetirement(false);
    files.write(files.root / "stable.sv", "package stable_pkg; localparam int X=43; endpackage\n");
    auto rebuilt = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                             files.root.string(), current.get(),
                                                             true);
    REQUIRE(rebuilt);
    REQUIRE(rebuilt->options.get<server::TrustedUvmImport>());
    CHECK(rebuilt->options.get<server::TrustedUvmImport>()->library != library);
    current = std::move(rebuilt);
    retire();
    current->openDocument(uri, text, 8, true);
    auto design = current->comp->createActiveDesignContext({});
    auto& compilation = design.getAnalysis().compilation;
    auto user = compilation.getPackage("user_pkg");
    REQUIRE(user);
    CHECK(*user->find("Y")->as<ast::ParameterSymbol>().getValue().integer().as<int>() == 46);
    for (const auto& diagnostic : compilation.getAllDiagnostics())
        CHECK_FALSE(diagnostic.isError());
    // Actual command-line changes still invalidate retained semantic inputs.
    files.write(files.root / "design.f", "-DCHANGED_CONFIGURATION\n" + filelist);
    current->preserveTrustedUvmForRetirement();
    auto rejected = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                              files.root.string(), current.get(),
                                                              true);
    CHECK_FALSE(rejected);
    CHECK(current->getDocument(uri)->textMatches(text));
}

TEST_CASE("Initial promotion separates RTL admission from borrowed exploration bodies",
          "[scope-semantic][initial-library]") {
    LibraryWorkspace files;
    const bool macroTypes = GENERATE(false, true);
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.experimentalCooperativeAnalysis = true;
    config.compilationFilelist = "design.f";
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    files.write(files.root / "fixed.sv",
                "module stable #(parameter type T=logic)(input T a, output wire T y); "
                "assign y=~a; endmodule\n");
    auto source = [&](std::string_view expression) {
        return fmt::format("`define PAYLOAD(N) typedef struct packed {{logic [3:0] bits;}} N;\n"
                           "module top(input logic [3:0] a, output wire [3:0] y);\n"
                           "{} stable #(.T(payload_t)) u({},y); endmodule\n",
                           macroTypes ? "`PAYLOAD(payload_t)"
                                      : "typedef struct packed {logic [3:0] bits;} payload_t;",
                           expression);
    };
    files.write(files.root / "consumer.sv", source("a"));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto initial = server::ServerDriver::createForExplore(indexer, client, config,
                                                          files.root.string(), nullptr, true);
    REQUIRE(initial);
    REQUIRE_FALSE(initial->comp);
    auto setting = initial->options.get<server::ScopeBodyImport>();
    REQUIRE(setting);
    auto exploration = setting->cache.lock();
    REQUIRE(exploration);
    std::unordered_map<std::string, std::shared_ptr<server::SlangDoc>> documents;
    for (const auto& [uri, doc] : initial->docs)
        documents.emplace(uri.getPath(), doc);
    auto borrowed = std::make_shared<server::ServerCompilationAnalysis>(
        documents, initial->options, initial->sm, lsp::RequestContext{}, false,
        initial->driver.getSourceOwner());
    borrowed->getAnalysisManager();
    INFO(DiagnosticEngine::reportAll(initial->sm, borrowed->compilation.getAllDiagnostics()));
    REQUIRE(exploration->status().ownerBuilds > 0);
    auto provider = initial->options.get<server::TrustedUvmImport>()->library;
    auto promoted = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                              files.root.string(), initial.get(),
                                                              true);
    REQUIRE(promoted);
    REQUIRE(promoted->comp);
    CHECK(promoted->options.get<server::TrustedUvmImport>()->library == provider);
    setting = promoted->options.get<server::ScopeBodyImport>();
    REQUIRE(setting);
    auto current = setting->cache.lock();
    REQUIRE(current);
    CHECK(current != exploration);
    CHECK(current->status().retainedRoots == std::vector<std::string>{"top.u"});
    initial.reset();
    documents.clear();
    // The old compilation owns its source lease independently of the driver.
    CHECK(borrowed->compilation.getRoot().topInstances.front()->body.find("u"));
    const auto uri = URI::fromFile(files.root / "consumer.sv");
    promoted->openDocument(uri, source("a"), 1, true);
    promoted->onDocDidChange(
        {.textDocument = {.version = 2, .uri = uri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = source("~a")}}});
    promoted->runPendingDiagnostics();
    CHECK(current->status().hits > 0);
    CHECK(current->status().retainedRoots == std::vector<std::string>{"top.u"});
}

TEST_CASE("Fixed package semantic closure shares nominal types across consumer edits",
          "[scope-semantic]") {
    LibraryWorkspace files;
    const bool withUvm = GENERATE(false, true);
    Config config = withUvm ? files.config(false) : Config{};
    config.experimentalUvmSemanticCache = withUvm;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources = Config::FrozenSourcesConfig{
        .packages = std::vector<std::string>{"stable_pkg"}, .maxMiB = 64};
    config.compilationFilelist = "design.f";
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 -DVALUE=1"}};
    files.write(files.root / "deps.sv", "package dep_pkg; class Base; int field; endclass "
                                        "typedef enum { A, B } Kind; endpackage\n");
    files.write(files.root / "stable.sv", "`undef VALUE\n`define VALUE 7\n"
                                          "package stable_pkg; class Object extends dep_pkg::Base; "
                                          "endclass class Holder #(type T=int); T value; endclass "
                                          "function int accept(dep_pkg::Base b); "
                                          "return b.field; endfunction endpackage\n");
    auto source = [](int value, bool error = false) {
        return fmt::format("package user_pkg; class Test extends stable_pkg::Object; "
                           "localparam int V={}; localparam int Macro=`VALUE; dep_pkg::Kind kind = "
                           "dep_pkg::A; stable_pkg::Holder#(Test) holder; "
                           "function int f(); return stable_pkg::accept(this){}; endfunction "
                           "endclass endpackage\n",
                           value, error ? "+missing" : "");
    };
    files.write(files.root / "consumer.sv", source(0));
    files.write(files.root / "design.f", "deps.sv\nstable.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(driver);
    INFO(rfl::json::write(driver->scopeLibraryStatus()));
    auto setting = driver->options.get<server::TrustedUvmImport>();
    REQUIRE(setting);
    auto library = setting->library;
    auto expectedPackages = std::vector<std::string>{"dep_pkg", "stable_pkg"};
    if (withUvm)
        expectedPackages.insert(expectedPackages.begin(), "uvm_pkg");
    REQUIRE(library->packages == expectedPackages);
    if (withUvm) {
        auto status = rfl::json::write(driver->trustedUvmStatus());
        CHECK(status.find("\"prepared\":true") != std::string::npos);
        CHECK(status.find("UVM shares the fixed scope semantic owner") != std::string::npos);
    }
    auto stable = library->compilation->getPackage("stable_pkg");
    auto dependency = library->compilation->getPackage("dep_pkg");
    auto uri = URI::fromFile(files.root / "consumer.sv");
    driver->openDocument(uri, source(0), 1, true);
    auto verify = [&](server::ServerDriver& current, int value, bool error) {
        auto analysis = current.getDocument(uri)->getAnalysis();
        auto compilation = analysis->getCompilation();
        CHECK(compilation->getPackage("stable_pkg") == stable);
        CHECK(compilation->getPackage("dep_pkg") == dependency);
        const auto& diagnostics = compilation->getSemanticDiagnostics();
        CHECK(std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }) == error);
        auto cls = compilation->getPackage("user_pkg")->find("Test");
        REQUIRE(cls);
        auto& type = cls->as<ast::ClassType>();
        CHECK(type.getBaseClass() == stable->find("Object"));
        CHECK(*type.find("V")->as<ast::ParameterSymbol>().getValue().integer().as<int>() == value);
        CHECK(*type.find("Macro")->as<ast::ParameterSymbol>().getValue().integer().as<int>() == 7);
    };
    verify(*driver, 0, false);
    const auto ownerBytes = library->compilation->getTotalBytesAllocated();
    for (int i = 1; i <= 6; ++i) {
        driver->onDocDidChange({.textDocument = {.version = i + 1, .uri = uri},
                                .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                                    .text = source(i, i % 2)}}});
        verify(*driver, i, i % 2);
        CHECK(library->compilation->getTotalBytesAllocated() == ownerBytes);
    }
    const auto generation = library->generation;
    std::weak_ptr<const void> originalOwner = library->sourceOwner;
    CHECK(library->consumers.load() >= 7);
    driver->preserveTrustedUvmForRetirement();
    auto replacement = server::ServerDriver::createForExplore(indexer, client, config,
                                                              files.root.string(), driver.get(),
                                                              true, true);
    REQUIRE(replacement);
    REQUIRE(replacement->options.get<server::TrustedUvmImport>());
    CHECK(replacement->options.get<server::TrustedUvmImport>()->library == library);
    if (withUvm) {
        REQUIRE(library->uvmParseTree);
        REQUIRE(replacement->driver.precompiledUvm);
        CHECK(&replacement->driver.precompiledUvm->root() == &library->uvmParseTree->root());
        CHECK(rfl::json::write(replacement->trustedUvmStatus()).find("\"archiveLoadMs\":0") !=
              std::string::npos);
    }
    driver.reset();
    verify(*replacement, 6, false);
    replacement->preserveTrustedUvmForRetirement(false);
    auto rebuilt = server::ServerDriver::createForExplore(indexer, client, config,
                                                          files.root.string(), replacement.get(),
                                                          true, true);
    REQUIRE(rebuilt);
    REQUIRE(rebuilt->options.get<server::TrustedUvmImport>());
    CHECK(rebuilt->options.get<server::TrustedUvmImport>()->library->generation != generation);
    replacement.reset();
    library.reset();
    CHECK(originalOwner.expired());
}

TEST_CASE("Fixed package semantic cache declines unsafe closures without hiding diagnostics",
          "[scope-semantic]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources = Config::FrozenSourcesConfig{
        .packages = std::vector<std::string>{"stable_pkg"}, .maxMiB = 64};
    config.compilationFilelist = "design.f";
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1"}};
    files.write(files.root / "stable.sv",
                "package stable_pkg; class Object; endclass endpackage\n");
    files.write(files.root / "consumer.sv", "module user_top; MissingType value; endmodule\n");
    files.write(files.root / "design.f", "stable.sv\nconsumer.sv\n");
    SECTION("disabled") {
        config.experimentalScopeSemanticCache = false;
    }
    SECTION("budget") {
        config.frozenSources.value()->maxMiB = 0;
    }
    SECTION("missing") {
        config.frozenSources.value()->packages = {"missing_pkg"};
    }
    SECTION("ambiguous") {
        files.write(files.root / "stable.sv", "package stable_pkg; endpackage\n"
                                              "package stable_pkg; endpackage\n");
    }
    SECTION("unsupported checker declaration") {
        files.write(files.root / "stable.sv", "checker fixed_checker; endchecker\n"
                                              "package stable_pkg; endpackage\n");
    }
    SECTION("ambiguous dependency definition") {
        files.write(files.root / "stable.sv", "module duplicate; endmodule\n"
                                              "module duplicate; endmodule\n"
                                              "package stable_pkg; endpackage\n");
    }
    SECTION("missing RTL subtree") {
        config.frozenSources.value()->instances = std::vector<std::string>{"user_top.missing"};
    }
    SECTION("package is not an RTL instance") {
        config.frozenSources.value()->instances = std::vector<std::string>{"stable_pkg"};
    }
    SECTION("mutable dependency") {
        files.write(files.root / "stable.sv",
                    "package stable_pkg; "
                    "class Object extends user_pkg::Base; endclass endpackage\n");
        files.write(files.root / "consumer.sv",
                    "package user_pkg; class Base; endclass endpackage\n"
                    "module user_top; MissingType value; endmodule\n");
    }
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(driver);
    CHECK_FALSE(driver->options.get<server::TrustedUvmImport>());
    const auto uri = URI::fromFile(files.root / "consumer.sv");
    driver->openDocument(uri, "module user_top; MissingType value; endmodule\n", 1, true);
    const auto& diagnostics =
        driver->getDocument(uri)->getAnalysis()->getCompilation()->getSemanticDiagnostics();
    CHECK(std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }));
}

TEST_CASE("Fixed definitions preserve virtual interface identity and consumer overrides",
          "[scope-semantic][scope-definitions]") {
    const bool byInstance = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.packages = std::vector<std::string>{"stable_pkg"},
                                    .modules = std::vector<std::string>{"stable"},
                                    .maxMiB = 64};
    if (byInstance) {
        config.frozenSources.value()->modules = std::vector<std::string>{};
        config.frozenSources.value()->instances = std::vector<std::string>{"top.u"};
    }
    config.compilationFilelist = "design.f";
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
interface bus_if #(parameter int W=4); logic [W-1:0] data; endinterface
package stable_pkg;
  typedef virtual bus_if#(4) bus_t;
  function int read(bus_t b); return int'(b.data); endfunction
endpackage
module stable #(parameter int W=4)(input logic [W-1:0] a, output logic [W-1:0] y);
  assign y=a;
endmodule
module default_cell;
  (* fixed_attribute=1 *) parameter int W=4;
  logic [W-1:0] data;
endmodule
)");
    auto source = [](int width, bool bind, bool bad = false) {
        return fmt::format(R"(
module observer; wire seen=1'b1; endmodule
module top;
  bus_if#(4) bus();
  logic [{}:0] a,y;
  stable u(.a(a),.y(y));
  logic [2:0] wrong;
  stable v(.a(wrong),.y());
  defparam u.W={};
  stable_pkg::bus_t handle;
  initial begin handle=bus; $display(stable_pkg::read(bus)); end
  {}
endmodule
{}
)",
                           width - 1, width, bad ? "MissingType broken;" : "",
                           bind ? "bind stable observer obs();" : "");
    };
    files.write(files.root / "consumer.sv", source(4, false));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(driver);
    INFO(rfl::json::write(driver->scopeLibraryStatus()));
    auto setting = driver->options.get<server::TrustedUvmImport>();
    REQUIRE(setting);
    auto library = setting->library;
    REQUIRE(library->definitions == std::vector<std::string>{"bus_if", "stable", "default_cell"});
    auto def = library->compilation->tryGetDefinition("stable", library->compilation->getRoot())
                   .definition;
    REQUIRE(def);
    auto uri = URI::fromFile(files.root / "consumer.sv");
    driver->openDocument(uri, source(4, false), 1, true);
    auto verify = [&](int width, bool bind, bool bad) {
        auto analysis = driver->getDocument(uri)->getAnalysis();
        auto compilation = analysis->getCompilation();
        const auto& diagnostics = compilation->getSemanticDiagnostics();
        INFO(DiagnosticEngine::reportAll(driver->sm, diagnostics));
        CHECK(std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }) == bad);
        const ast::InstanceSymbol* top = nullptr;
        for (const auto* candidate : compilation->getRoot().topInstances)
            if (candidate->name == "top")
                top = candidate;
        REQUIRE(top);
        const ast::InstanceSymbol* defaultCell = nullptr;
        for (const auto* candidate : compilation->getRoot().topInstances)
            if (candidate->name == "default_cell")
                defaultCell = candidate;
        REQUIRE(defaultCell);
        auto parameter = defaultCell->body.find("W");
        REQUIRE(parameter);
        CHECK(compilation->getAttributes(*parameter).size() == 1);
        auto instance = top->body.find("u");
        REQUIRE(instance);
        auto& inst = instance->as<ast::InstanceSymbol>();
        CHECK(&inst.getDefinition() == def);
        CHECK(inst.body.find("y")->as<ast::ValueSymbol>().getType().getBitWidth() ==
              unsigned(width));
        CHECK(bool(inst.body.find("obs")) == bind);
        CHECK(def->as<ast::DefinitionSymbol>().bindDirectives.empty());
        CHECK(def->as<ast::DefinitionSymbol>().getInstanceCount() == 0);
        auto nativeConfig = config;
        nativeConfig.experimentalScopeSemanticCache = false;
        Indexer nativeIndexer;
        ClientHarness nativeClient;
        auto native = server::ServerDriver::createForExplore(nativeIndexer, nativeClient,
                                                             nativeConfig, files.root.string(),
                                                             nullptr, true);
        REQUIRE(native);
        native->openDocument(uri, source(width, bind, bad), 1, true);
        const auto& expected =
            native->getDocument(uri)->getAnalysis()->getCompilation()->getSemanticDiagnostics();
        CHECK(DiagnosticEngine::reportAll(driver->sm, diagnostics) ==
              DiagnosticEngine::reportAll(native->sm, expected));
    };
    verify(4, false, false);
    const auto ownerBytes = library->compilation->getTotalBytesAllocated();
    for (int i = 1; i <= 6; ++i) {
        const auto width = i % 2 ? 5 : 4;
        driver->onDocDidChange({.textDocument = {.version = i + 1, .uri = uri},
                                .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                                    .text = source(width, i % 2, i % 3 == 1)}}});
        verify(width, i % 2, i % 3 == 1);
        CHECK(library->compilation->getTotalBytesAllocated() == ownerBytes);
    }
}

TEST_CASE("Fixed hierarchy queries follow current external port connections across edits",
          "[scope-semantic][scope-boundary]") {
    const bool byInstance = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources = Config::FrozenSourcesConfig{.maxMiB = 64};
    if (byInstance)
        config.frozenSources.value()->instances = std::vector<std::string>{"top.u"};
    else
        config.frozenSources.value()->modules = std::vector<std::string>{"stable"};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
module stable(input logic a, output logic y);
  wire internal_value = a;
  assign y = internal_value;
endmodule
)");
    auto source = [](bool alternate, bool renamed) {
        return fmt::format(R"(
module top(input logic first, second, output wire out_first, out_second);
  stable {}(.a({}), .y({}));
  stable other(.a(first), .y());
endmodule
)",
                           renamed ? "moved" : "u", alternate ? "second" : "first",
                           alternate ? "out_second" : "out_first");
    };
    files.write(files.root / "consumer.sv", source(false, false));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    REQUIRE(driver->comp);
    auto setting = driver->options.get<server::TrustedUvmImport>();
    REQUIRE(setting);
    auto library = setting->library;
    const auto uri = URI::fromFile(files.root / "consumer.sv");
    driver->openDocument(uri, source(false, false), 1, true);
    // Preserve a fully analyzed old consumer while new consumers change the
    // boundary. Reuse may retain this owner, but must not mutate its connections.
    std::unordered_map<std::string, std::shared_ptr<server::SlangDoc>> oldDocuments;
    for (const auto& [documentUri, document] : driver->docs)
        oldDocuments.emplace(documentUri.getPath(), document);
    auto oldConsumer = std::make_shared<server::ServerCompilationAnalysis>(oldDocuments,
                                                                           driver->options,
                                                                           driver->sm);
    CHECK(server::ConeResult(oldConsumer->getDriverCone("top.u.a"), driver->sm).getPaths() ==
          std::vector<std::string>{"top.first"});
    const auto ownerBytes = library->compilation->getTotalBytesAllocated();
    auto cacheSetting = driver->options.get<server::ScopeBodyImport>();
    REQUIRE(cacheSetting);
    auto cache = cacheSetting->cache.lock();
    REQUIRE(cache);
    for (int i = 0; i < 8; ++i) {
        const auto hitsBeforeState = cache->status().hits;
        const bool alternate = i % 2;
        const bool renamed = i == 3 || i == 4;
        if (i)
            driver->onDocDidChange({.textDocument = {.version = i + 1, .uri = uri},
                                    .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                                        .text = source(alternate, renamed)}}});
        const std::string instance = renamed ? "top.moved" : "top.u";
        const std::string input = alternate ? "top.second" : "top.first";
        const std::string output = alternate ? "top.out_second" : "top.out_first";
        const std::string unusedOutput = alternate ? "top.out_first" : "top.out_second";
        INFO("edit=" << i << " instance=" << instance << " input=" << input);
        CHECK(driver->comp->getDriverCone(instance + ".a").getPaths() ==
              std::vector<std::string>{input});
        CHECK(driver->comp->getDriverCone("top.other.a").getPaths() ==
              std::vector<std::string>{"top.first"});
        CHECK(driver->comp->getDriverCone(output).getPaths() ==
              std::vector<std::string>{instance + ".y"});
        CHECK(driver->comp->getDriverCone(unusedOutput).getPaths().empty());
        // A renamed module-selected path becomes reusable on the next edit,
        // even while another instance already imports an older independent owner.
        if (!byInstance && i == 4)
            CHECK(cache->status().hits >= hitsBeforeState + 2);
        CHECK(server::ConeResult(oldConsumer->getDriverCone("top.u.a"), driver->sm).getPaths() ==
              std::vector<std::string>{"top.first"});
        CHECK(server::ConeResult(oldConsumer->getDriverCone("top.out_first"), driver->sm)
                  .getPaths() == std::vector<std::string>{"top.u.y"});
        auto nativeConfig = config;
        nativeConfig.experimentalScopeSemanticCache = false;
        Indexer nativeIndexer;
        ClientHarness nativeClient;
        auto native = server::ServerDriver::createFromFileLists(nativeIndexer, nativeClient,
                                                                nativeConfig, {"design.f"},
                                                                files.root.string(), nullptr, true);
        REQUIRE(native);
        native->openDocument(uri, source(alternate, renamed), i + 1, true);
        for (const auto& path : {instance + ".a", instance + ".internal_value", instance + ".y",
                                 std::string("top.other.a"), output, unusedOutput}) {
            const auto actual = driver->comp->getDriverCone(path).getLocations();
            const auto expected = native->comp->getDriverCone(path).getLocations();
            REQUIRE(actual.size() == expected.size());
            for (size_t j = 0; j < actual.size(); ++j) {
                CHECK(actual[j].path == expected[j].path);
                CHECK(rfl::json::write(actual[j].location) ==
                      rfl::json::write(expected[j].location));
            }
        }
        CHECK(library->compilation->getTotalBytesAllocated() == ownerBytes);
    }
    const auto cacheStatus = cache->status();
    CHECK(cacheStatus.publications > 0);
    CHECK(cacheStatus.hits > 0);
    CHECK(cacheStatus.baseBytes + cacheStatus.ownerBytes + cacheStatus.pinnedBytes <=
          cacheStatus.budgetBytes);
    // Context retirement preserves independent owners and current unsaved text.
    const auto hitsBeforeRetirement = cache->status().hits;
    driver->preserveTrustedUvmForRetirement();
    auto replacement = server::ServerDriver::createFromFileLists(indexer, client, config,
                                                                 {"design.f"}, files.root.string(),
                                                                 driver.get(), true);
    REQUIRE(replacement);
    REQUIRE(replacement->options.get<server::ScopeBodyImport>());
    CHECK(replacement->options.get<server::ScopeBodyImport>()->cache.lock() == cache);
    CHECK(replacement->comp->getDriverCone("top.u.a").getPaths() ==
          std::vector<std::string>{"top.second"});
    CHECK(cache->status().hits > hitsBeforeRetirement);
    const auto generation =
        replacement->options.get<server::TrustedUvmImport>()->library->generation;
    replacement->preserveTrustedUvmForRetirement(false);
    auto rebuilt = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                             files.root.string(), replacement.get(),
                                                             true);
    REQUIRE(rebuilt);
    REQUIRE(rebuilt->options.get<server::ScopeBodyImport>());
    CHECK(rebuilt->options.get<server::ScopeBodyImport>()->cache.lock() != cache);
    CHECK(rebuilt->options.get<server::TrustedUvmImport>()->library->generation != generation);
    CHECK(rebuilt->comp->getDriverCone("top.u.a").getPaths() ==
          std::vector<std::string>{"top.second"});
}

TEST_CASE("Whole retained subtrees preserve current boundaries and native instance counts",
          "[scope-semantic][scope-subtree]") {
    const bool byInstance = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources = Config::FrozenSourcesConfig{.maxMiB = 64};
    if (byInstance)
        config.frozenSources.value()->instances = std::vector<std::string>{"top.u"};
    else
        config.frozenSources.value()->modules = std::vector<std::string>{"stable"};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
module leaf #(parameter int W=2)(input logic [W-1:0] a, output wire [W-1:0] y);
  wire [W-1:0] internal_value = a;
  assign y = internal_value;
endmodule
module mid #(parameter int W=2)(input logic [W-1:0] a, output wire [W-1:0] y);
  wire [W-1:0] internal_value;
  leaf #(.W(W)) bottom(.a(a), .y(internal_value));
  assign y=internal_value;
endmodule
module stable #(parameter int W=2)(input logic [W-1:0] a, output wire [W-1:0] y);
  wire [W-1:0] internal_value;
  logic captured;
  always_ff @(posedge a[0]) captured <= a[1];
  wire narrow_value;
  wire [1:0] lane_values, array_values;
  mid #(.W(W)) child(.a(a), .y(internal_value));
  leaf #(.W(1)) narrow(.a(a[0]), .y(narrow_value));
  for (genvar i=0; i<2; i++) begin:g
    leaf #(.W(1)) lane(.a(a[i]), .y(lane_values[i]));
  end
  leaf #(.W(1)) arr[1:0](.a(a[1:0]), .y(array_values));
  assign y = internal_value;
endmodule
)");
    auto source = [](bool alternate, bool renamed, int width = 2) {
        return fmt::format(R"(
module top(input logic [{}:0] first, second, output wire [{}:0] out_first, out_second);
  stable #(.W({})) {}(.a({}), .y({}));
  stable other(.a(first[1:0]), .y());
endmodule
)",
                           width - 1, width - 1, width, renamed ? "moved" : "u",
                           alternate ? "second" : "first", alternate ? "out_second" : "out_first");
    };
    files.write(files.root / "consumer.sv", source(false, false));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    REQUIRE(driver->comp);
    auto setting = driver->options.get<server::TrustedUvmImport>();
    REQUIRE(setting);
    auto library = setting->library;
    const auto uri = URI::fromFile(files.root / "consumer.sv");
    driver->openDocument(uri, source(false, false), 1, true);
    // Preserve a fully analyzed old consumer while new consumers change the
    // boundary. Reuse may retain this owner, but must not mutate its connections.
    std::unordered_map<std::string, std::shared_ptr<server::SlangDoc>> oldDocuments;
    for (const auto& [documentUri, document] : driver->docs)
        oldDocuments.emplace(documentUri.getPath(), document);
    auto oldConsumer = std::make_shared<server::ServerCompilationAnalysis>(oldDocuments,
                                                                           driver->options,
                                                                           driver->sm);
    CHECK(server::ConeResult(oldConsumer->getDriverCone("top.u.a"), driver->sm).getPaths() ==
          std::vector<std::string>{"top.first"});
    const auto ownerBytes = library->compilation->getTotalBytesAllocated();
    auto cacheSetting = driver->options.get<server::ScopeBodyImport>();
    REQUIRE(cacheSetting);
    auto cache = cacheSetting->cache.lock();
    REQUIRE(cache);
    auto diagnosticText = [](SourceManager& sources, const auto& diagnostics) {
        // Independent source namespaces can register files in different orders.
        // Compare full per-diagnostic text, including notes and instance context.
        std::vector<std::string> result;
        for (const auto& diagnostic : diagnostics) {
            std::vector<Diagnostic> single{diagnostic};
            result.push_back(DiagnosticEngine::reportAll(sources, single));
        }
        std::ranges::sort(result);
        return result;
    };
    std::map<std::pair<std::string, int>, const ast::Symbol*> childIdentities;
    std::map<const ast::Compilation*, size_t> retainedAstBytes;
    for (int i = 0; i < 10; ++i) {
        const auto hitsBeforeState = cache->status().hits;
        const bool alternate = i % 2;
        const int width = (i == 1 || i == 2 || i >= 8) ? 4 : (i >= 6 ? 8 : 2);
        const bool renamed = i == 3 || i == 4;
        if (i)
            driver->onDocDidChange({.textDocument = {.version = i + 1, .uri = uri},
                                    .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                                        .text = source(alternate, renamed, width)}}});
        const std::string instance = renamed ? "top.moved" : "top.u";
        const std::string input = alternate ? "top.second" : "top.first";
        const std::string output = alternate ? "top.out_second" : "top.out_first";
        const std::string unusedOutput = alternate ? "top.out_first" : "top.out_second";
        INFO("edit=" << i << " instance=" << instance << " input=" << input);
        CHECK(driver->comp->getDriverCone(instance + ".a").getPaths() ==
              std::vector<std::string>{input});
        CHECK(driver->comp->getDriverCone("top.other.a").getPaths() ==
              std::vector<std::string>{"top.first"});
        CHECK(driver->comp->getDriverCone(output).getPaths() ==
              std::vector<std::string>{instance + ".y"});
        CHECK(driver->comp->getDriverCone(unusedOutput).getPaths().empty());
        const auto inputLoads = driver->comp->getLoadCone(instance + ".a").getPaths();
        CHECK(std::ranges::find(inputLoads, instance + ".a") == inputLoads.end());
        CHECK(std::ranges::find(inputLoads, instance + ".captured") != inputLoads.end());
        CHECK(driver->comp->getDriverCone(instance + ".child.a").getPaths() ==
              std::vector<std::string>{instance + ".a"});
        for (auto suffix : {".g[0].lane.a", ".g[1].lane.a", ".arr[0].a", ".arr[1].a"})
            CHECK(driver->comp->getDriverCone(instance + suffix).getPaths() ==
                  std::vector<std::string>{instance + ".a"});
        bool parentShared = false;
        for (auto candidate : driver->comp->getInstancesOfModule("stable")) {
            auto& compilation = candidate->body.getCompilation();
            INFO("semantic=" << DiagnosticEngine::reportAll(driver->sm,
                                                            compilation.getSemanticDiagnostics()));
            REQUIRE(
                std::ranges::none_of(compilation.getSemanticDiagnostics(),
                                     [](const auto& diagnostic) { return diagnostic.isError(); }));
            CHECK(compilation.getDefinitionInstanceCount(candidate->getDefinition()) == 2);
            auto child = candidate->body.find("child");
            REQUIRE(child);
            CHECK(compilation.getDefinitionInstanceCount(
                      child->as<ast::InstanceSymbol>().getDefinition()) == 2);
            auto bottom = child->as<ast::InstanceSymbol>().body.find("bottom");
            REQUIRE(bottom);
            CHECK(compilation.getDefinitionInstanceCount(
                      bottom->as<ast::InstanceSymbol>().getDefinition()) == 10);
            if (candidate->name == (renamed ? "moved" : "u")) {
                parentShared = compilation.isImportedInstanceBody(candidate->body);
                if (parentShared) {
                    CHECK(
                        compilation.isImportedInstanceBody(child->as<ast::InstanceSymbol>().body));
                    auto [identity, inserted] = childIdentities.emplace(std::pair{instance, width},
                                                                        child);
                    CHECK(identity->second == child);
                    CHECK_FALSE(child->as<ast::InstanceSymbol>().getCanonicalBody());
                    CHECK_FALSE(bottom->as<ast::InstanceSymbol>().getCanonicalBody());
                    CHECK(
                        compilation.isImportedInstanceBody(bottom->as<ast::InstanceSymbol>().body));
                    auto& owner = child->as<ast::InstanceSymbol>().body.getCompilation();
                    auto [bytes, firstOwnerUse] =
                        retainedAstBytes.emplace(&owner, owner.getTotalBytesAllocated());
                    CHECK(bytes->second == owner.getTotalBytesAllocated());
                }
            }
        }
        INFO("body cache=" << rfl::json::write(cache->status()));
        if ((i == 2 || i == 4 || i == 5 || i >= 7) && (!byInstance || !renamed))
            REQUIRE(parentShared);
        // A renamed module-selected path becomes reusable on the next edit,
        // even while another instance already imports an older independent owner.
        if (!byInstance && i == 4)
            CHECK(cache->status().hits >= hitsBeforeState + 2);
        CHECK(server::ConeResult(oldConsumer->getDriverCone("top.u.a"), driver->sm).getPaths() ==
              std::vector<std::string>{"top.first"});
        CHECK(server::ConeResult(oldConsumer->getDriverCone("top.out_first"), driver->sm)
                  .getPaths() == std::vector<std::string>{"top.u.y"});
        CHECK(server::ConeResult(oldConsumer->getDriverCone("top.u.child.bottom.a"), driver->sm)
                  .getPaths() == std::vector<std::string>{"top.u.child.a"});
        auto nativeConfig = config;
        nativeConfig.experimentalScopeSemanticCache = false;
        Indexer nativeIndexer;
        ClientHarness nativeClient;
        auto native = server::ServerDriver::createFromFileLists(nativeIndexer, nativeClient,
                                                                nativeConfig, {"design.f"},
                                                                files.root.string(), nullptr, true);
        REQUIRE(native);
        native->openDocument(uri, source(alternate, renamed, width), i + 1, true);
        for (const auto& path :
             {instance + ".a", instance + ".internal_value", instance + ".y", instance + ".child.a",
              instance + ".child.internal_value", instance + ".child.bottom.a",
              instance + ".child.bottom.internal_value", instance + ".g[0].lane.a",
              instance + ".g[1].lane.a", instance + ".arr[0].a", instance + ".arr[1].a",
              std::string("top.other.a"), output, unusedOutput}) {
            auto loadResult = [&](server::ServerCompilation& compilation) {
                std::pair<std::vector<std::string>, std::string> result;
                try {
                    for (const auto& entry : compilation.getLoadCone(path).getLocations())
                        result.first.push_back(rfl::json::write(entry));
                    // Cone leaves are ordered by symbol identity, which differs
                    // between independent owners; preserve complete endpoints.
                    std::ranges::sort(result.first);
                }
                catch (const std::runtime_error& error) {
                    result.second = error.what();
                }
                return result;
            };
            CHECK(loadResult(*driver->comp) == loadResult(*native->comp));
            const auto actual = driver->comp->getDriverCone(path).getLocations();
            const auto expected = native->comp->getDriverCone(path).getLocations();
            REQUIRE(actual.size() == expected.size());
            for (size_t j = 0; j < actual.size(); ++j) {
                CHECK(actual[j].path == expected[j].path);
                CHECK(rfl::json::write(actual[j].location) ==
                      rfl::json::write(expected[j].location));
            }
        }
        auto& actualCompilation =
            driver->comp->getInstancesOfModule("stable").front()->body.getCompilation();
        auto& nativeCompilation =
            native->comp->getInstancesOfModule("stable").front()->body.getCompilation();
        CHECK(diagnosticText(driver->sm, actualCompilation.getSemanticDiagnostics()) ==
              diagnosticText(native->sm, nativeCompilation.getSemanticDiagnostics()));
        analysis::AnalysisOptions options{.flags = analysis::AnalysisFlags::CheckUnused |
                                                   analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager actualAnalysis(options), nativeAnalysis(options);
        actualCompilation.freeze();
        actualAnalysis.analyze(actualCompilation);
        actualCompilation.unfreeze();
        nativeCompilation.freeze();
        nativeAnalysis.analyze(nativeCompilation);
        nativeCompilation.unfreeze();
        const auto expected = diagnosticText(native->sm, nativeAnalysis.getDiagnostics());
        REQUIRE(std::ranges::any_of(expected, [](const auto& text) {
            return text.find("narrow_value") != std::string::npos;
        }));
        CHECK(diagnosticText(driver->sm, actualAnalysis.getDiagnostics()) == expected);
        CHECK(library->compilation->getTotalBytesAllocated() == ownerBytes);
    }
    const auto cacheStatus = cache->status();
    CHECK(cacheStatus.publications > 0);
    CHECK(cacheStatus.hits > 0);
    CHECK(cacheStatus.baseBytes + cacheStatus.ownerBytes + cacheStatus.pinnedBytes <=
          cacheStatus.budgetBytes);
    // Context retirement preserves independent owners and current unsaved text.
    const auto hitsBeforeRetirement = cache->status().hits;
    driver->preserveTrustedUvmForRetirement();
    auto replacement = server::ServerDriver::createFromFileLists(indexer, client, config,
                                                                 {"design.f"}, files.root.string(),
                                                                 driver.get(), true);
    REQUIRE(replacement);
    REQUIRE(replacement->options.get<server::ScopeBodyImport>());
    CHECK(replacement->options.get<server::ScopeBodyImport>()->cache.lock() == cache);
    CHECK(replacement->comp->getDriverCone("top.u.a").getPaths() ==
          std::vector<std::string>{"top.second"});
    CHECK(cache->status().hits > hitsBeforeRetirement);
    const auto generation =
        replacement->options.get<server::TrustedUvmImport>()->library->generation;
    replacement->preserveTrustedUvmForRetirement(false);
    auto rebuilt = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                             files.root.string(), replacement.get(),
                                                             true);
    REQUIRE(rebuilt);
    REQUIRE(rebuilt->options.get<server::ScopeBodyImport>());
    CHECK(rebuilt->options.get<server::ScopeBodyImport>()->cache.lock() != cache);
    CHECK(rebuilt->options.get<server::TrustedUvmImport>()->library->generation != generation);
    CHECK(rebuilt->comp->getDriverCone("top.u.a").getPaths() ==
          std::vector<std::string>{"top.second"});
}

TEST_CASE("Immutable instance body views retain symbols and bind current outside ports",
          "[scope-semantic][scope-body-view]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
package helpers;
  function logic ping; return 1'b1; endfunction
endpackage
module stable(input logic a, output wire y);
  (* keep=1 *) wire internal_value = a;
  assign y = internal_value;
endmodule
)");
    auto source = [](bool alternate, bool renamed) {
        return fmt::format(R"(
module top(input logic first, second, output wire out_first, out_second);
  stable {}(.a({}), .y({}));
endmodule
)",
                           renamed ? "moved" : "u", alternate ? "second" : "first",
                           alternate ? "out_second" : "out_first");
    };
    files.write(files.root / "consumer.sv", source(false, false));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    REQUIRE(driver->options.get<server::TrustedUvmImport>());
    auto makeConsumer = [&](bool alternate, bool renamed, bool oldUse = false) {
        auto text = source(alternate, renamed);
        if (oldUse)
            text.insert(text.rfind("endmodule"), "initial $display(helpers::ping());\n");
        auto compilation = std::make_shared<ast::Compilation>(driver->options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, driver->options));
        compilation->setInstanceBodyReuseHooks({}, {});
        compilation->addSyntaxTree(syntax::SyntaxTree::fromText(
            text, driver->sm, "consumer-edit.sv", "", driver->options));
        return compilation;
    };
    auto lookupInstance = [](ast::Compilation& compilation,
                             std::string_view name) -> const ast::InstanceSymbol& {
        const ast::InstanceSymbol* top = nullptr;
        for (auto candidate : compilation.getRoot().topInstances)
            if (candidate->name == "top")
                top = candidate;
        REQUIRE(top);
        auto symbol = top->body.find(name);
        REQUIRE(symbol);
        return symbol->as<ast::InstanceSymbol>();
    };
    auto original = makeConsumer(false, false, true);
    REQUIRE(original->getSemanticDiagnostics().empty());
    auto& retained = lookupInstance(*original, "u").body;
    auto retainedValue = retained.find("internal_value");
    REQUIRE(retainedValue);
    const auto ownerBytes = original->getTotalBytesAllocated();
    for (int i = 0; i < 8; ++i) {
        const bool alternate = i % 2;
        const bool renamed = i == 3 || i == 4;
        auto current = makeConsumer(alternate, renamed);
        auto& instance = lookupInstance(*current, renamed ? "moved" : "u");
        CHECK(current->tryImportInstanceBody(original, retained,
                                             const_cast<ast::InstanceBodySymbol&>(instance.body)) ==
              !renamed);
        CHECK(current->isImportedInstanceBody(instance.body) == !renamed);
        CHECK((instance.body.find("internal_value") == retainedValue) == !renamed);
        REQUIRE(current->getSemanticDiagnostics().empty());
        auto sharedValue = instance.body.find("internal_value");
        REQUIRE(sharedValue);
        CHECK(current->getAttributes(*sharedValue).size() == 1);
        analysis::AnalysisManager analysis(
            {.flags = analysis::AnalysisFlags::CheckUnused | analysis::AnalysisFlags::CheckShadow});
        current->freeze();
        analysis.analyze(*current);
        current->unfreeze();
        analysis::AnalysisQueries queries(*current, analysis);
        auto input = instance.body.find("a");
        REQUIRE(input);
        auto drivers = queries.getDrivers(input->as<ast::ValueSymbol>());
        REQUIRE(drivers.size() == 1);
        // The root port's driver must use the new frame even though its value
        // symbol and bound internal expressions are borrowed from the old body.
        CHECK(drivers.front()->containingSymbol == &instance.body);
        DriversTracer tracer(&input->as<ast::ValueSymbol>());
        instance.visit(tracer);
        CHECK(server::ConeResult(tracer.getLeaves(), driver->sm).getPaths() ==
              std::vector<std::string>{alternate ? "top.second" : "top.first"});
        CHECK(retained.parentInstance->getPortConnections()
                  .front()
                  ->getExpression()
                  ->getSymbolReference()
                  ->name == "first");
        // Compare all fresh flow / usage diagnostics. The retained consumer
        // calls helpers::ping, whereas this consumer does not: inheriting its
        // whole reference-status map would hide a current unused warning.
        auto native = makeConsumer(alternate, renamed);
        REQUIRE(native->getSemanticDiagnostics().empty());
        analysis::AnalysisManager nativeAnalysis(
            {.flags = analysis::AnalysisFlags::CheckUnused | analysis::AnalysisFlags::CheckShadow});
        native->freeze();
        nativeAnalysis.analyze(*native);
        native->unfreeze();
        const auto expectedDiagnostics =
            DiagnosticEngine::reportAll(driver->sm, nativeAnalysis.getDiagnostics());
        REQUIRE(expectedDiagnostics.find("ping") != std::string::npos);
        CHECK(DiagnosticEngine::reportAll(driver->sm, analysis.getDiagnostics()) ==
              expectedDiagnostics);
        CHECK(original->getTotalBytesAllocated() == ownerBytes);
    }
    // Once members have escaped, replacing a body would leave incompatible
    // identities in indexes and bound expressions. Decline without mutation.
    auto exposed = makeConsumer(false, false);
    auto& exposedBody = lookupInstance(*exposed, "u").body;
    auto nativeValue = exposedBody.find("internal_value");
    REQUIRE(nativeValue);
    CHECK_FALSE(exposed->tryImportInstanceBody(original, retained,
                                               const_cast<ast::InstanceBodySymbol&>(exposedBody)));
    CHECK(exposedBody.find("internal_value") == nativeValue);
    CHECK(original->getTotalBytesAllocated() == ownerBytes);
    std::weak_ptr<ast::Compilation> oldOwner = original;
    auto lease = makeConsumer(true, false);
    auto& leaseBody = lookupInstance(*lease, "u").body;
    REQUIRE(lease->tryImportInstanceBody(original, retained,
                                         const_cast<ast::InstanceBodySymbol&>(leaseBody)));
    original.reset();
    CHECK_FALSE(oldOwner.expired());
    lease.reset();
    CHECK(oldOwner.expired());
}

TEST_CASE("Automatic body variants match integral parameters and keep current argument references",
          "[scope-semantic][scope-body-parameters]") {
    const bool byInstance = GENERATE(false, true);
    const bool overrideMask = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources = Config::FrozenSourcesConfig{.maxMiB = 64};
    if (byInstance)
        config.frozenSources.value()->instances = std::vector<std::string>{"top.u"};
    else
        config.frozenSources.value()->modules = std::vector<std::string>{"stable"};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
module stable #(parameter int W=4, parameter logic [W-1:0] MASK='1)
               (input logic [W-1:0] a, output wire [W-1:0] y);
  localparam int LG=$clog2(W);
  wire [W-1:0] q=a & MASK;
  assign y=q;
endmodule
)");
    auto source = [overrideMask](int width, int edit) {
        return fmt::format(R"(
// current edit {}
module top;
  localparam int WIDTH={};
  logic [WIDTH-1:0] a;
  wire [WIDTH-1:0] y;
  stable #(.W(WIDTH){}) u(.a(a), .y(y));
endmodule
)",
                           edit, width ? std::to_string(width) : "MissingWidth",
                           overrideMask ? ", .MASK('1)" : "");
    };
    files.write(files.root / "consumer.sv", source(4, 0));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    REQUIRE(driver->options.get<server::ScopeBodyImport>());
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    REQUIRE(cache->status().publications > 0);
    auto make = [&](int width, int edit, bool native) {
        auto compilation = std::make_shared<ast::Compilation>(driver->options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, driver->options));
        if (native)
            compilation->setInstanceBodyReuseHooks({}, {});
        compilation->addSyntaxTree(syntax::SyntaxTree::fromText(
            source(width, edit), driver->sm, "parameter-consumer.sv", "", driver->options));
        const auto& diagnostics = compilation->getSemanticDiagnostics();
        REQUIRE(std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
                    return diagnostic.isError();
                }) == (width == 0));
        return compilation;
    };
    auto instance = [](ast::Compilation& compilation) -> const ast::InstanceSymbol& {
        const ast::InstanceSymbol* top = nullptr;
        for (auto candidate : compilation.getRoot().topInstances)
            if (candidate->name == "top")
                top = candidate;
        REQUIRE(top);
        auto value = top->body.find("u");
        REQUIRE(value);
        return value->as<ast::InstanceSymbol>();
    };
    std::map<int, const ast::Symbol*> identities;
    std::vector<int> widths{4, 8, 8, 0, 8, 4, 16, 16, 4};
    for (size_t edit = 0; edit < widths.size(); ++edit) {
        const auto width = widths[edit];
        auto current = make(width, int(edit), false);
        if (!width) {
            auto native = make(width, int(edit), true);
            CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
                  DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
            current->offerInstanceBodies(current);
            continue;
        }
        auto& body = instance(*current).body;
        auto q = body.find("q");
        REQUIRE(q);
        CHECK(q->as<ast::ValueSymbol>().getType().getBitWidth() == unsigned(width));
        auto w = body.find("W");
        auto lg = body.find("LG");
        REQUIRE(w);
        REQUIRE(lg);
        CHECK(w->as<ast::ParameterSymbol>().getValue().integer().as<int>() == width);
        CHECK(lg->as<ast::ParameterSymbol>().getValue().integer().as<int>() == (width == 4   ? 2
                                                                                : width == 8 ? 3
                                                                                             : 4));
        if (current->isImportedInstanceBody(body)) {
            auto [it, inserted] = identities.emplace(width, q);
            CHECK(it->second == q);
            auto boundary = body.getBoundaryParameterInitializers();
            REQUIRE(boundary.size() == (overrideMask ? 2 : 1));
            auto reference = boundary.front()->getSymbolReference();
            REQUIRE(reference);
            CHECK(reference->name == "WIDTH");
            CHECK(&reference->getParentScope()->getCompilation() == current.get());
            auto text = driver->sm.getSourceText(boundary.front()->sourceRange.start().buffer());
            CHECK(text.find(fmt::format("current edit {}", edit)) != std::string_view::npos);
            CHECK(w->as<ast::ParameterSymbol>().getInitializer()->kind ==
                  ast::ExpressionKind::IntegerLiteral);
            CHECK_FALSE(w->as<ast::ParameterSymbol>().getDeclaredType()->getInitializerSyntax());
        }
        if (edit == 2 || edit == 4 || edit == 5 || edit == 7 || edit == 8)
            REQUIRE(current->isImportedInstanceBody(body));
        auto native = make(width, int(edit), true);
        analysis::AnalysisOptions analysisOptions{.flags = analysis::AnalysisFlags::CheckUnused |
                                                           analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager actualAnalysis(analysisOptions), nativeAnalysis(analysisOptions);
        current->freeze();
        actualAnalysis.analyze(*current);
        current->unfreeze();
        native->freeze();
        nativeAnalysis.analyze(*native);
        native->unfreeze();
        auto expected = DiagnosticEngine::reportAll(driver->sm, nativeAnalysis.getDiagnostics());
        REQUIRE(expected.find("LG") != std::string::npos);
        CHECK(DiagnosticEngine::reportAll(driver->sm, actualAnalysis.getDiagnostics()) == expected);
        current->offerInstanceBodies(current);
        CHECK(cache->status().baseBytes + cache->status().ownerBytes +
                  cache->status().pinnedBytes <=
              cache->status().budgetBytes);
    }
    CHECK(identities.size() == 3);
    CHECK(cache->status().publications >= 3);
}

TEST_CASE("Body signatures distinguish inferred parameter widths and signedness",
          "[scope-semantic][scope-body-parameters]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
module stable #(parameter W=8)(input logic [W-1:0] a, output wire [W-1:0] y);
  localparam int PW=$bits(W);
  assign y=a;
endmodule
)");
    auto source = [](std::string_view value) {
        return fmt::format("module top; localparam WIDTH={}; "
                           "logic [WIDTH-1:0] a; wire [WIDTH-1:0] y; "
                           "stable #(.W(WIDTH)) u(.a(a),.y(y)); endmodule\n",
                           value);
    };
    files.write(files.root / "consumer.sv", source("8"));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    const std::vector<std::string> values{"8", "8'd8", "8'd8", "8'sd8", "8'sd8", "8"};
    for (size_t i = 0; i < values.size(); ++i) {
        auto current = std::make_shared<ast::Compilation>(driver->options);
        REQUIRE(server::importTrustedUvm(*current, driver->sm, driver->options));
        current->addSyntaxTree(syntax::SyntaxTree::fromText(
            source(values[i]), driver->sm, "inferred-parameter.sv", "", driver->options));
        REQUIRE(current->getSemanticDiagnostics().empty());
        const auto* top = current->getRoot().topInstances.front();
        REQUIRE(top->name == "top");
        auto symbol = top->body.find("u");
        REQUIRE(symbol);
        auto& body = symbol->as<ast::InstanceSymbol>().body;
        auto w = body.find("W");
        auto pw = body.find("PW");
        REQUIRE(w);
        REQUIRE(pw);
        const auto bits = values[i] == "8" ? 32 : 8;
        CHECK(w->as<ast::ValueSymbol>().getType().getBitWidth() == unsigned(bits));
        CHECK(w->as<ast::ValueSymbol>().getType().isSigned() == (values[i] != "8'd8"));
        CHECK(pw->as<ast::ParameterSymbol>().getValue().integer().as<int>() == bits);
        if (i == 2 || i == 4 || i == 5)
            CHECK(current->isImportedInstanceBody(body));
        current->offerInstanceBodies(current);
    }
    CHECK(cache->status().publications >= 3);
}

TEST_CASE("Frozen package imports preserve multidimensional packed types and constant usage",
          "[scope-semantic][scope-body-imports]") {
    const bool fixedPackage = GENERATE(false, true);
    const bool explicitImport = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    auto package = [](int mask) {
        return fmt::format("package constants_pkg; typedef logic bit_t; "
                           "localparam logic [1:0] MASK=2'd{}; "
                           "function automatic int width_for(input int x); return x; endfunction "
                           "function automatic int unused_helper(); return 1; endfunction "
                           "endpackage\n",
                           mask);
    };
    const auto fixed = fmt::format(R"(
module stable #(parameter int W=constants_pkg::width_for(2))
  (input constants_pkg::bit_t [1:0][W-1:0] a,
   output wire constants_pkg::bit_t [1:0][W-1:0] y);
  import constants_pkg::{};
  wire mask_bit = MASK[0];
  wire [1:0][W-1:0] q = a;
  assign y = mask_bit ? q : a;
endmodule
)",
                                   explicitImport ? "MASK" : "*");
    files.write(files.root / "fixed.sv", (fixedPackage ? package(3) : "") + fixed);
    auto source = [&](int width, int mask) {
        return (fixedPackage ? "" : package(mask)) + fmt::format(R"(
module top;
  localparam int WIDTH={};
  constants_pkg::bit_t [1:0][WIDTH-1:0] a;
  wire constants_pkg::bit_t [1:0][WIDTH-1:0] y;
  stable #(.W(WIDTH)) u(.a(a), .y(y));
endmodule
)",
                                                                 width);
    };
    auto initial = source(2, 3);
    initial.insert(initial.rfind("endmodule"),
                   "initial $display(constants_pkg::unused_helper());\n");
    files.write(files.root / "consumer.sv", initial);
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    auto make = [&](int width, int mask, bool native) {
        auto compilation = std::make_shared<ast::Compilation>(driver->options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, driver->options));
        if (native)
            compilation->setInstanceBodyReuseHooks({}, {});
        compilation->addSyntaxTree(syntax::SyntaxTree::fromText(
            source(width, mask), driver->sm, "package-import-consumer.sv", "", driver->options));
        INFO(DiagnosticEngine::reportAll(driver->sm, compilation->getSemanticDiagnostics()));
        REQUIRE(compilation->getSemanticDiagnostics().empty());
        return compilation;
    };
    std::map<int, const ast::Symbol*> identities;
    const std::vector<int> widths{2, 4, 4, 2, 4};
    for (size_t edit = 0; edit < widths.size(); ++edit) {
        auto current = make(widths[edit], int(edit % 4), false);
        auto native = make(widths[edit], int(edit % 4), true);
        const auto& top = *current->getRoot().topInstances.front();
        REQUIRE(top.name == "top");
        auto instance = top.body.find("u");
        REQUIRE(instance);
        const auto& body = instance->as<ast::InstanceSymbol>().body;
        auto q = body.find("q");
        REQUIRE(q);
        CHECK(q->as<ast::ValueSymbol>().getType().getBitWidth() == unsigned(2 * widths[edit]));
        if (fixedPackage && (edit == 2 || edit == 3 || edit == 4))
            REQUIRE(current->isImportedInstanceBody(body));
        if (!fixedPackage)
            CHECK_FALSE(current->isImportedInstanceBody(body));
        if (current->isImportedInstanceBody(body)) {
            auto [entry, inserted] = identities.emplace(widths[edit], q);
            CHECK(entry->second == q);
        }
        auto bit = body.find("mask_bit");
        REQUIRE(bit);
        auto initializer = bit->getDeclaredType()->getInitializer();
        REQUIRE(initializer);
        const ast::ParameterSymbol* mask = nullptr;
        initializer->visitSymbolReferences([&](const ast::Expression&, const ast::Symbol& symbol) {
            if (symbol.name == "MASK")
                mask = symbol.as_if<ast::ParameterSymbol>();
        });
        REQUIRE(mask);
        CHECK(mask->getValue().integer().as<int>() == (fixedPackage ? 3 : int(edit % 4)));
        CHECK((&mask->getParentScope()->getCompilation() != current.get()) == fixedPackage);
        analysis::AnalysisOptions options{.flags = analysis::AnalysisFlags::CheckUnused |
                                                   analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager actualAnalysis(options), nativeAnalysis(options);
        current->freeze();
        actualAnalysis.analyze(*current);
        current->unfreeze();
        native->freeze();
        nativeAnalysis.analyze(*native);
        native->unfreeze();
        const auto expected = DiagnosticEngine::reportAll(driver->sm,
                                                          nativeAnalysis.getDiagnostics());
        CHECK(DiagnosticEngine::reportAll(driver->sm, actualAnalysis.getDiagnostics()) == expected);
        CHECK(current->isReferenced(*mask->getSyntax()).first);
        auto helper = current->getPackage("constants_pkg")->find("width_for");
        REQUIRE(helper);
        CHECK(current->isReferenced(*helper->getSyntax()).first);
        REQUIRE(expected.find("unused_helper") != std::string::npos);
        current->offerInstanceBodies(current);
    }
    CHECK((cache->status().publications > 0) == fixedPackage);
    CHECK((cache->status().hits > 0) == fixedPackage);
}

TEST_CASE("Frozen package enum and aggregate parameter variants retain nominal identity",
          "[scope-semantic][scope-body-package-types]") {
    const bool useUnion = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", fmt::format(R"(
package constants_pkg;
  typedef enum logic [1:0] {{ ADD=0, SUB=1 }} op_t;
  typedef {} packed {{ logic [3:0] data; logic [3:0] tag; }} packet_t;
endpackage
module stable #(parameter int data=0,
                parameter constants_pkg::op_t OP=constants_pkg::ADD,
                parameter constants_pkg::packet_t MASK={})
               (input constants_pkg::packet_t a, output wire constants_pkg::packet_t y);
  import constants_pkg::*;
  packet_t q;
  op_t operation = OP;
  assign q.data = a.data ^ MASK.data;
  {}
  assign y = operation == ADD ? q : a;
endmodule
)",
                                                     useUnion ? "union" : "struct",
                                                     useUnion ? "'{data:'0}" : "'{data:'0,tag:'0}",
                                                     useUnion ? "" : "assign q.tag = a.tag;"));
    auto source = [](bool subtract, int mask) {
        return fmt::format(R"(
module top;
  constants_pkg::packet_t a;
  wire constants_pkg::packet_t y;
  stable #(.OP(constants_pkg::{}), .MASK({})) u(.a(a),.y(y));
endmodule
)",
                           subtract ? "SUB" : "ADD", mask ? "'1" : "'0");
    };
    files.write(files.root / "consumer.sv", source(false, 0));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    auto make = [&](bool subtract, int mask, bool native) {
        auto compilation = std::make_shared<ast::Compilation>(driver->options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, driver->options));
        if (native)
            compilation->setInstanceBodyReuseHooks({}, {});
        compilation->addSyntaxTree(syntax::SyntaxTree::fromText(
            source(subtract, mask), driver->sm, "package-types-consumer.sv", "", driver->options));
        INFO(DiagnosticEngine::reportAll(driver->sm, compilation->getSemanticDiagnostics()));
        REQUIRE(compilation->getSemanticDiagnostics().empty());
        return compilation;
    };
    std::map<std::pair<bool, int>, const ast::Symbol*> identities;
    const std::vector<std::pair<bool, int>> values{{false, 0}, {true, 0}, {true, 0},
                                                   {true, 1},  {true, 1}, {false, 0}};
    for (size_t edit = 0; edit < values.size(); ++edit) {
        auto [subtract, mask] = values[edit];
        auto current = make(subtract, mask, false);
        auto native = make(subtract, mask, true);
        const auto& body =
            current->getRoot().topInstances.front()->body.find("u")->as<ast::InstanceSymbol>().body;
        auto q = body.find("q");
        REQUIRE(q);
        const auto& type = q->as<ast::ValueSymbol>().getType();
        CHECK(type.getBitWidth() == (useUnion ? 4 : 8));
        auto declared = current->getPackage("constants_pkg")->find("packet_t");
        REQUIRE(declared);
        CHECK(&type.getCanonicalType() == &declared->as<ast::Type>().getCanonicalType());
        auto op = body.find("OP");
        REQUIRE(op);
        CHECK(op->as<ast::ParameterSymbol>().getValue().integer().as<int>() == int(subtract));
        if (edit == 2 || edit == 4 || edit == 5)
            REQUIRE(current->isImportedInstanceBody(body));
        if (current->isImportedInstanceBody(body)) {
            auto [identity, inserted] = identities.emplace(values[edit], q);
            CHECK(identity->second == q);
        }
        analysis::AnalysisOptions options{.flags = analysis::AnalysisFlags::CheckUnused |
                                                   analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager actualAnalysis(options), nativeAnalysis(options);
        current->freeze();
        actualAnalysis.analyze(*current);
        current->unfreeze();
        native->freeze();
        nativeAnalysis.analyze(*native);
        native->unfreeze();
        const auto expected = DiagnosticEngine::reportAll(driver->sm,
                                                          nativeAnalysis.getDiagnostics());
        REQUIRE(expected.find("unused parameter 'data'") != std::string::npos);
        CHECK(DiagnosticEngine::reportAll(driver->sm, actualAnalysis.getDiagnostics()) == expected);
        current->offerInstanceBodies(current);
    }
    std::shared_ptr<ast::Compilation> invalid, invalidNative;
    for (const bool nativeMode : {false, true}) {
        auto compilation = std::make_shared<ast::Compilation>(driver->options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, driver->options));
        if (nativeMode)
            compilation->setInstanceBodyReuseHooks({}, {});
        auto text = source(false, 0);
        auto position = text.find("constants_pkg::ADD");
        REQUIRE(position != std::string::npos);
        text.replace(position, std::string_view("constants_pkg::ADD").size(), "other_pkg::ADD");
        text = "package other_pkg; typedef enum logic [1:0] {ADD=0} op_t; endpackage\n" + text;
        compilation->addSyntaxTree(syntax::SyntaxTree::fromText(
            text, driver->sm, "foreign-enum-consumer.sv", "", driver->options));
        const auto& diagnostics = compilation->getSemanticDiagnostics();
        REQUIRE(std::ranges::any_of(diagnostics,
                                    [](const auto& diagnostic) { return diagnostic.isError(); }));
        (nativeMode ? invalidNative : invalid) = compilation;
    }
    CHECK(DiagnosticEngine::reportAll(driver->sm, invalid->getSemanticDiagnostics()) ==
          DiagnosticEngine::reportAll(driver->sm, invalidNative->getSemanticDiagnostics()));
    auto repaired = make(false, 0, false);
    const auto& repairedBody =
        repaired->getRoot().topInstances.front()->body.find("u")->as<ast::InstanceSymbol>().body;
    CHECK(repaired->isImportedInstanceBody(repairedBody));
    CHECK(cache->status().publications >= 3);
}

TEST_CASE("Fixed module source views share the selected current design and driver analysis",
          "[scope-semantic][scope-source-view]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    const std::string fixed = R"(
package shared_data; logic flag=1'b0; endpackage
module stable #(parameter int W=2)
  (input logic clk, input logic [W-1:0] a, output logic [W-1:0] y);
  always_ff @(posedge clk) y <= a;
endmodule
)";
    files.write(files.root / "fixed.sv", fixed);
    auto source = [](int width) {
        return fmt::format("module top; logic clk; logic [{}:0] a,y; "
                           "stable #(.W({})) u(.clk(clk),.a(a),.y(y)); endmodule\n",
                           width - 1, width);
    };
    files.write(files.root / "consumer.sv", source(4));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto library = driver->options.get<server::TrustedUvmImport>()->library;
    REQUIRE(library);
    const auto bytes = library->compilation->getTotalBytesAllocated();
    const auto consumerUri = URI::fromFile(files.root / "consumer.sv");
    const auto fixedUri = URI::fromFile(files.root / "fixed.sv");
    driver->openDocument(consumerUri, source(4), 1, true);
    driver->openDocument(fixedUri, fixed, 1, true);
    std::shared_ptr<ast::Compilation> oldView;
    for (int edit = 0; edit < 3; ++edit) {
        const int width = edit == 1 ? 8 : 4;
        if (edit)
            driver->onDocDidChange({.textDocument = {.version = edit + 1, .uri = consumerUri},
                                    .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                                        .text = source(width)}}});
        REQUIRE(driver->comp->setActiveInstance("top.u"));
        const auto& instances = driver->comp->getInstancesOfModule("stable");
        REQUIRE(instances.size() == 1);
        auto current = instances.front();
        auto analysis = driver->getDocument(fixedUri)->getAnalysis();
        const auto& projected = analysis->getShallowCompilation()->getSourceInstances();
        REQUIRE(projected.size() == 1);
        CHECK(projected.front() == current);
        CHECK(analysis->getCompilation().get() == &current->body.getCompilation());
        auto y = current->body.find("y");
        REQUIRE(y);
        CHECK(y->as<ast::ValueSymbol>().getType().getBitWidth() == unsigned(width));
        CHECK_FALSE(analysis->getDrivers(y->as<ast::ValueSymbol>()).empty());
        auto flag = analysis->getCompilation()->getPackage("shared_data")->find("flag");
        REQUIRE(flag);
        auto fullContext = driver->comp->createActiveDesignContext({});
        CHECK(analysis->getDrivers(flag->as<ast::ValueSymbol>()) ==
              fullContext.getAnalysis().getDrivers(flag->as<ast::ValueSymbol>()));
        CHECK(library->compilation->getTotalBytesAllocated() == bytes);
        if (edit == 0)
            oldView = analysis->getCompilation();
    }
    REQUIRE(oldView);
    auto oldY = oldView->getRoot()
                    .topInstances.front()
                    ->body.find("u")
                    ->as<ast::InstanceSymbol>()
                    .body.find("y");
    REQUIRE(oldY);
    CHECK(oldY->as<ast::ValueSymbol>().getType().getBitWidth() == 4);
    driver.reset();
    CHECK(oldY->as<ast::ValueSymbol>().getType().getBitWidth() == 4);
}

TEST_CASE("Parameter-only probing declines deferred type queries and mutable type aliases",
          "[scope-semantic][scope-body-parameters]") {
    const bool deferredQuery = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    const auto fixed = deferredQuery ? "module stable(output wire y); logic [3:0] q; "
                                       "localparam int N=$bits(q); assign y=q[0]; endmodule\n"
                                     : "module stable(output wire y); consumer_types::data_t q; "
                                       "assign y=q[0]; endmodule\n";
    files.write(files.root / "fixed.sv", fixed);
    auto source = [](int bits) {
        return fmt::format("package consumer_types; typedef logic [{}:0] data_t; endpackage\n"
                           "module top; wire y; stable u(.y(y)); endmodule\n",
                           bits - 1);
    };
    files.write(files.root / "consumer.sv", source(4));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    REQUIRE(driver->options.get<server::TrustedUvmImport>());
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    for (int bits : {4, 8, 4}) {
        auto current = std::make_shared<ast::Compilation>(driver->options);
        REQUIRE(server::importTrustedUvm(*current, driver->sm, driver->options));
        current->addSyntaxTree(syntax::SyntaxTree::fromText(
            source(bits), driver->sm, "deferred-parameter.sv", "", driver->options));
        REQUIRE(current->getSemanticDiagnostics().empty());
        const auto* top = current->getRoot().topInstances.front();
        REQUIRE(top->name == "top");
        auto instance = top->body.find("u");
        REQUIRE(instance);
        auto& body = instance->as<ast::InstanceSymbol>().body;
        auto q = body.find("q");
        REQUIRE(q);
        CHECK(q->as<ast::ValueSymbol>().getType().getBitWidth() ==
              unsigned(deferredQuery ? 4 : bits));
        if (deferredQuery) {
            auto n = body.find("N");
            REQUIRE(n);
            CHECK(n->as<ast::ParameterSymbol>().getValue().integer().as<int>() == 4);
        }
        CHECK_FALSE(current->isImportedInstanceBody(body));
        current->offerInstanceBodies(current);
        CHECK(cache->status().publications == 0);
    }
}

TEST_CASE("Automatic body budget counts evicted owners pinned by old consumers",
          "[scope-semantic][scope-body-budget]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv",
                "module stable #(parameter bit MODE=0)(input logic a, "
                "output wire y); wire [63:0] q; for(genvar i=0;i<64;i++) begin:g "
                "assign q[i]=a^MODE; end assign y=^q; endmodule\n");
    auto source = [](std::string_view name) {
        return fmt::format("module top(input logic a, output wire y); "
                           "stable #(.MODE({})) {}(.a(a),.y(y)); endmodule\n",
                           name == "v" ? 1 : 0, name);
    };
    files.write(files.root / "consumer.sv", source("u"));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto librarySetting = driver->options.get<server::TrustedUvmImport>();
    REQUIRE(librarySetting);
    auto library = librarySetting->library;
    auto newCache = [&](size_t limit) {
        return std::make_shared<server::ScopeBodyCache>(library, limit,
                                                        std::vector<std::string>{"stable"},
                                                        std::vector<std::string>{});
    };
    auto make = [&](const std::shared_ptr<server::ScopeBodyCache>& cache, std::string_view name) {
        auto options = driver->options;
        options.set(server::ScopeBodyImport{cache});
        auto compilation = std::make_shared<ast::Compilation>(options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, options));
        compilation->addSyntaxTree(syntax::SyntaxTree::fromText(source(name), driver->sm,
                                                                "budget-consumer.sv", "", options));
        REQUIRE(compilation->getSemanticDiagnostics().empty());
        return compilation;
    };
    auto probe = newCache(512 * 1024 * 1024);
    auto reference = make(probe, "u");
    reference->offerInstanceBodies(reference);
    REQUIRE(probe->status().publications == 1);
    const auto oneOwner = probe->status().ownerBytes;
    REQUIRE(oneOwner > 0);
    probe.reset();
    const auto limit = library->semanticBytes + oneOwner + oneOwner / 2;
    auto cache = newCache(limit);
    auto first = make(cache, "u");
    first->offerInstanceBodies(first);
    REQUIRE(cache->status().publications == 1);
    auto pinnedConsumer = make(cache, "u");
    REQUIRE(pinnedConsumer->hasImportedInstanceBodies());
    CHECK(cache->status().hits > 0);
    auto second = make(cache, "v");
    second->offerInstanceBodies(second);
    const auto pinned = cache->status();
    CHECK(pinned.evictions == 1);
    CHECK(pinned.owners == 0);
    CHECK(pinned.pinnedBytes > 0);
    CHECK(pinned.publications == 1);
    CHECK(pinned.declines > 0);
    CHECK(pinned.baseBytes + pinned.ownerBytes + pinned.pinnedBytes <= limit);
    // The old consumer remains valid even after its registry entry is evicted.
    CHECK(pinnedConsumer->getSemanticDiagnostics().empty());
    pinnedConsumer.reset();
    CHECK(cache->status().pinnedBytes == 0);
    auto retry = make(cache, "v");
    retry->offerInstanceBodies(retry);
    CHECK(cache->status().publications == 2);
    CHECK(cache->status().owners == 1);
    auto tiny = newCache(library->semanticBytes + 1);
    auto fallback = make(tiny, "u");
    fallback->offerInstanceBodies(fallback);
    CHECK(tiny->status().declines == 1);
    CHECK(tiny->status().owners == 0);
    CHECK_FALSE(fallback->hasImportedInstanceBodies());
    CHECK(fallback->getSemanticDiagnostics().empty());
}

TEST_CASE("Fixed library navigation uses retained syntax and explicit rebuild accepts overlays",
          "[scope-semantic][scope-navigation]") {
    const bool fullDesign = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources = Config::FrozenSourcesConfig{
        .packages = std::vector<std::string>{"stable_pkg"}, .maxMiB = 64};
    config.compilationFilelist = "design.f";
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1"}};
    files.write(files.root / "fixed.sv",
                "package stable_pkg; `include \"fixed.svh\"\nendpackage\n");
    const std::string original = "class C; int old_field; endclass\n";
    const std::string changed = "class C; int new_field; endclass\n";
    files.write(files.root / "fixed.svh", original);
    const std::string consumer = "module user_top; stable_pkg::C handle; endmodule\n";
    files.write(files.root / "consumer.sv", consumer);
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto create = [&](const server::ServerDriver* previous) {
        if (!fullDesign)
            return server::ServerDriver::createForExplore(indexer, client, config,
                                                          files.root.string(), previous, true,
                                                          true);
        auto buildConfig = config;
        buildConfig.compilationFilelist = std::nullopt;
        return server::ServerDriver::createFromFileLists(indexer, client, buildConfig, {"design.f"},
                                                         files.root.string(), previous, true);
    };
    auto driver = create(nullptr);
    REQUIRE(driver);
    auto setting = driver->options.get<server::TrustedUvmImport>();
    REQUIRE(setting);
    const auto generation = setting->library->generation;
    const auto libraryUri = URI::fromFile(files.root / "fixed.svh");
    const auto consumerUri = URI::fromFile(files.root / "consumer.sv");
    driver->openDocument(libraryUri, original, 1, true);
    driver->openDocument(consumerUri, consumer, 1, true);
    auto libraryAnalysis = driver->getDocument(libraryUri)->getAnalysis();
    CHECK(libraryAnalysis->getCompilation() == setting->library->compilation);
    CHECK(libraryAnalysis->getSyntaxTree() ==
          setting->library->compilation->getSyntaxTrees().front());
    REQUIRE(driver->getDocument(consumerUri)->getAnalysis());
    driver->onDocDidChange(
        {.textDocument = {.version = 2, .uri = libraryUri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = changed}}});
    CHECK_THROWS_AS(driver->getDocument(libraryUri)->getAnalysis(), lsp::RequestFailed);
    CHECK_THROWS_AS(driver->getDocument(consumerUri)->getAnalysis(), lsp::RequestFailed);
    if (fullDesign) {
        REQUIRE(driver->comp);
        CHECK_THROWS_AS(driver->comp->getScope("user_top"), lsp::RequestFailed);
    }
    auto rebuilt = create(driver.get());
    REQUIRE(rebuilt);
    auto next = rebuilt->options.get<server::TrustedUvmImport>();
    REQUIRE(next);
    CHECK(next->library->generation != generation);
    auto type = next->library->compilation->getPackage("stable_pkg")->find("C");
    REQUIRE(type);
    CHECK(type->as<ast::ClassType>().find("new_field"));
    CHECK_FALSE(type->as<ast::ClassType>().find("old_field"));
    CHECK(rebuilt->getDocument(libraryUri)->getVersion() == 2);
    REQUIRE(rebuilt->getDocument(consumerUri)->getAnalysis());
}

TEST_CASE("Retained subtree warnings are coalesced against current occurrences",
          "[scope-semantic][scope-body-diagnostics]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
module leaf #(parameter int W=2)(input logic [W-1:0] a, output logic y,
                                 input logic enabled=1'b1);
  always_comb begin
    unique case(a)
      0: y=0;
      1: y=1;
      default: y=0;
    endcase
  end
  if (W==2) begin:g
    wire [3:0] widened=a;
  end
endmodule
module stable #(parameter int W=2)(input logic [W-1:0] a, output wire y);
  leaf #(.W(W)) child(.a(a), .y(y), .enabled());
endmodule
)");
    auto source = [](int width, int count) {
        auto text = std::string("module top; logic [7:0] a; wire y;\n");
        text += fmt::format("stable #(.W({})) u(.a(a[{}:0]), .y(y));\n", width, width - 1);
        for (int i = 0; i < count; ++i)
            text += fmt::format("stable #(.W({})) v{}(.a(a[{}:0]), .y());\n", i % 2 ? 4 : 2, i,
                                i % 2 ? 3 : 1);
        return text + "endmodule\n";
    };
    files.write(files.root / "consumer.sv", source(2, 1));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    auto make = [&](int width, int count, bool native) {
        auto compilation = std::make_shared<ast::Compilation>(driver->options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, driver->options));
        if (native)
            compilation->setInstanceBodyReuseHooks({}, {});
        compilation->addSyntaxTree(syntax::SyntaxTree::fromText(
            source(width, count), driver->sm, "warning-consumer.sv", "", driver->options));
        (void)compilation->getSemanticDiagnostics();
        REQUIRE_FALSE(compilation->hasIssuedErrors());
        return compilation;
    };
    const std::vector<std::pair<int, int>> edits{{2, 1}, {2, 3}, {4, 2}, {4, 0}, {2, 0}, {2, 4}};
    for (size_t edit = 0; edit < edits.size(); ++edit) {
        auto [width, count] = edits[edit];
        auto current = make(width, count, false);
        auto native = make(width, count, true);
        const auto& actual = current->getSemanticDiagnostics();
        const auto& expected = native->getSemanticDiagnostics();
        REQUIRE_FALSE(expected.empty());
        REQUIRE(std::ranges::any_of(expected, [](const auto& diagnostic) {
            return !diagnostic.notes.empty();
        }));
        CHECK(DiagnosticEngine::reportAll(driver->sm, actual) ==
              DiagnosticEngine::reportAll(driver->sm, expected));
        // The same raw cache must obey current policy, including formerly
        // disabled diagnostics promoted to errors, without rebuilding its owner.
        for (auto severity : {DiagnosticSeverity::Ignored, DiagnosticSeverity::Warning,
                              DiagnosticSeverity::Error}) {
            auto report = [&](const Diagnostics& diagnostics) {
                DiagnosticEngine engine(driver->sm);
                auto client = std::make_shared<TextDiagnosticClient>();
                engine.addClient(client);
                for (const auto& diagnostic : expected)
                    engine.setSeverity(diagnostic.code, severity);
                engine.issue(diagnostics);
                if (severity == DiagnosticSeverity::Ignored) {
                    CHECK(engine.getNumWarnings() == 0);
                    CHECK(engine.getNumErrors() == 0);
                }
                if (severity == DiagnosticSeverity::Error)
                    CHECK(engine.getNumErrors() > 0);
                return client->getString();
            };
            CHECK(report(actual) == report(expected));
        }
        REQUIRE(actual.size() == expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            CHECK(actual[i].code == expected[i].code);
            CHECK(actual[i].coalesceCount == expected[i].coalesceCount);
            CHECK(actual[i].notes.size() == expected[i].notes.size());
        }
        const auto& top = *current->getRoot().topInstances.front();
        auto instance = top.body.find("u");
        REQUIRE(instance);
        if (edit == 1 || edit == 3 || edit >= 4)
            CHECK(current->isImportedInstanceBody(instance->as<ast::InstanceSymbol>().body));
        current->offerInstanceBodies(current);
    }
    REQUIRE(cache->status().hits > 0);
}

TEST_CASE("Local packed nominal types and pure system calls retain current semantics",
          "[scope-semantic][scope-local-types-calls]") {
    const bool impure = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    auto fixed = std::string(R"(
module stable #(parameter int W=2)(input logic [W-1:0] a, output wire [W-1:0] y);
  typedef logic [W-1:0] bits_t;
  typedef enum logic [1:0] { IDLE=0, ACTIVE=1 } state_t;
  typedef struct packed { bits_t data; logic tag; } packet_t;
  typedef union packed { packet_t packet; logic [W:0] flat; } cell_t;
  typedef logic unused_alias_t;
  state_t state=ACTIVE;
  cell_t q;
  wire [31:0] shape=$bits(packet_t)+$size(a)+$low(a)+$high(a)+$left(a)+$right(a)+
                         $increment(a)+$dimensions(a)+$unpacked_dimensions(a);
  wire [31:0] count=$countones(a)+$countbits(a,1'b1)+$clog2(W);
  assign q.packet.data=$unsigned($signed(a));
  assign q.packet.tag=$onehot(a)|$onehot0(a)|$isunknown(a)|(a inside {[0:W-1]});
  assign y=(state==ACTIVE) ? q.packet.data + bits_t'(count) : bits_t'(shape);
)");
    if (impure)
        fixed += "int seed=1; wire [31:0] random_value=$urandom(seed);\n";
    fixed += "endmodule\n";
    files.write(files.root / "fixed.sv", fixed);
    auto source = [](int width, bool invalid) {
        return fmt::format(R"(
package other_pkg; typedef enum logic [1:0] {{ BAD=1 }} other_t; endpackage
module top;
  logic [{}:0] a;
  wire [{}:0] y, z;
  stable #(.W({})) u(.a(a), .y(y));
  stable #(.W({})) v(.a(a), .y(z));
  {}
endmodule
)",
                           width - 1, width - 1, width, width,
                           invalid ? "initial u.state=other_pkg::BAD;" : "");
    };
    files.write(files.root / "consumer.sv", source(2, false));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    auto make = [&](int width, bool invalid, bool native) {
        auto compilation = std::make_shared<ast::Compilation>(driver->options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, driver->options));
        if (native)
            compilation->setInstanceBodyReuseHooks({}, {});
        compilation->addSyntaxTree(syntax::SyntaxTree::fromText(
            source(width, invalid), driver->sm, "local-type-consumer.sv", "", driver->options));
        (void)compilation->getSemanticDiagnostics();
        CHECK(compilation->hasIssuedErrors() == invalid);
        return compilation;
    };
    std::map<int, const ast::Symbol*> identities;
    const std::vector<int> widths{2, 4, 4, 2, 2, 2};
    for (size_t edit = 0; edit < widths.size(); ++edit) {
        const bool invalid = edit == 4;
        auto current = make(widths[edit], invalid, false);
        auto native = make(widths[edit], invalid, true);
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        const auto& top = *current->getRoot().topInstances.front();
        const auto& nativeTop = *native->getRoot().topInstances.front();
        const auto& body = top.body.find("u")->as<ast::InstanceSymbol>().body;
        const auto& sibling = top.body.find("v")->as<ast::InstanceSymbol>().body;
        auto q = body.find("q");
        REQUIRE(q);
        CHECK(q->as<ast::ValueSymbol>().getType().getBitWidth() == unsigned(widths[edit] + 1));
        const auto& type = body.find("state")->as<ast::ValueSymbol>().getType();
        const auto& siblingType = sibling.find("state")->as<ast::ValueSymbol>().getType();
        const auto& nativeType = nativeTop.body.find("u")
                                     ->as<ast::InstanceSymbol>()
                                     .body.find("state")
                                     ->as<ast::ValueSymbol>()
                                     .getType();
        const auto& nativeSiblingType = nativeTop.body.find("v")
                                            ->as<ast::InstanceSymbol>()
                                            .body.find("state")
                                            ->as<ast::ValueSymbol>()
                                            .getType();
        CHECK(type.isMatching(siblingType) == nativeType.isMatching(nativeSiblingType));
        if (!impure && edit >= 2) {
            REQUIRE(current->isImportedInstanceBody(body));
            auto [it, inserted] = identities.emplace(widths[edit], q);
            CHECK(it->second == q);
        }
        if (impure)
            CHECK_FALSE(current->isImportedInstanceBody(body));
        analysis::AnalysisOptions options{.flags = analysis::AnalysisFlags::CheckUnused |
                                                   analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager actualAnalysis(options), nativeAnalysis(options);
        current->freeze();
        actualAnalysis.analyze(*current);
        current->unfreeze();
        native->freeze();
        nativeAnalysis.analyze(*native);
        native->unfreeze();
        const auto expected = DiagnosticEngine::reportAll(driver->sm,
                                                          nativeAnalysis.getDiagnostics());
        CHECK(DiagnosticEngine::reportAll(driver->sm, actualAnalysis.getDiagnostics()) == expected);
        REQUIRE(expected.find("unused_alias_t") != std::string::npos);
        if (!invalid)
            current->offerInstanceBodies(current);
    }
    CHECK((cache->status().hits > 0) == !impure);
}

TEST_CASE("Local type dimensions cannot capture mutable package constants",
          "[scope-semantic][scope-local-type-dependencies]") {
    const bool fixedPackage = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    auto package = [](int width) {
        return fmt::format("package dims; localparam int WIDTH={}; endpackage\n", width);
    };
    files.write(files.root / "fixed.sv", (fixedPackage ? package(3) : "") + R"(
module stable(input logic [7:0] a, output wire y);
  typedef logic [dims::WIDTH-1:0] bits_t;
  bits_t q;
  assign q=a;
  assign y=q[0];
endmodule
)");
    auto source = [&](int width) {
        return (fixedPackage ? "" : package(width)) +
               "module top; logic [7:0] a; wire y; stable u(.a(a), .y(y)); endmodule\n";
    };
    files.write(files.root / "consumer.sv", source(3));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    for (int width : {3, 5, 3, 5}) {
        auto make = [&](bool native) {
            auto c = std::make_shared<ast::Compilation>(driver->options);
            REQUIRE(server::importTrustedUvm(*c, driver->sm, driver->options));
            if (native)
                c->setInstanceBodyReuseHooks({}, {});
            c->addSyntaxTree(syntax::SyntaxTree::fromText(
                source(width), driver->sm, "dimension-consumer.sv", "", driver->options));
            (void)c->getSemanticDiagnostics();
            REQUIRE_FALSE(c->hasIssuedErrors());
            return c;
        };
        auto current = make(false), native = make(true);
        auto& body =
            current->getRoot().topInstances.front()->body.find("u")->as<ast::InstanceSymbol>().body;
        CHECK(body.find("q")->as<ast::ValueSymbol>().getType().getBitWidth() ==
              unsigned(fixedPackage ? 3 : width));
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        if (!fixedPackage)
            CHECK_FALSE(current->isImportedInstanceBody(body));
        current->offerInstanceBodies(current);
    }
    CHECK((cache->status().hits > 0) == fixedPackage);
}

TEST_CASE("Frozen subtree reference profiles exclude mutable caller reads and writes",
          "[scope-semantic][scope-reference-origins]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
module leaf(input logic a, output wire y); assign y=a; endmodule
module stable(input logic a, output wire y);
  wire unused_inside=a;
  logic orphan;
  wire used=orphan;
  leaf child(.a(a|used), .y(y));
endmodule
)");
    auto source = [](bool outside) {
        return std::string("module top; logic a; wire y; stable u(.a(a), .y(y));\n") +
               (outside ? "initial begin $display(u.unused_inside); u.orphan=1'b1; end\n" : "") +
               "endmodule\n";
    };
    files.write(files.root / "consumer.sv", source(true));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    auto make = [&](bool outside, bool native) {
        auto c = std::make_shared<ast::Compilation>(driver->options);
        REQUIRE(server::importTrustedUvm(*c, driver->sm, driver->options));
        if (native)
            c->setInstanceBodyReuseHooks({}, {});
        c->addSyntaxTree(syntax::SyntaxTree::fromText(source(outside), driver->sm,
                                                      "origin-consumer.sv", "", driver->options));
        REQUIRE(c->getSemanticDiagnostics().empty());
        return c;
    };
    for (bool outside : {true, false, true, false}) {
        auto current = make(outside, false), native = make(outside, true);
        auto& body =
            current->getRoot().topInstances.front()->body.find("u")->as<ast::InstanceSymbol>().body;
        auto& nativeBody =
            native->getRoot().topInstances.front()->body.find("u")->as<ast::InstanceSymbol>().body;
        // Publication must work even when the initial producer has external
        // hierarchical accesses; later edits must retain the complete parent.
        REQUIRE(current->isImportedInstanceBody(body));
        for (const auto& name : {"unused_inside", "orphan", "a", "y"}) {
            auto value = body.find(name), expected = nativeBody.find(name);
            REQUIRE(value);
            REQUIRE(expected);
            CHECK(current->isReferenced(*value->getSyntax()) ==
                  native->isReferenced(*expected->getSyntax()));
        }
        CHECK(current->isReferenced(*body.find("unused_inside")->getSyntax()).first == outside);
        auto orphan = current->isReferenced(*body.find("orphan")->getSyntax());
        CHECK(orphan.first);
        CHECK(orphan.second == outside);
        analysis::AnalysisOptions options{.flags = analysis::AnalysisFlags::CheckUnused |
                                                   analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager actualAnalysis(options), nativeAnalysis(options);
        current->freeze();
        actualAnalysis.analyze(*current);
        current->unfreeze();
        native->freeze();
        nativeAnalysis.analyze(*native);
        native->unfreeze();
        const auto actual = DiagnosticEngine::reportAll(driver->sm,
                                                        actualAnalysis.getDiagnostics());
        const auto expected = DiagnosticEngine::reportAll(driver->sm,
                                                          nativeAnalysis.getDiagnostics());
        CHECK(actual == expected);
        if (!outside) {
            REQUIRE(expected.find("unused_inside") != std::string::npos);
            REQUIRE(expected.find("orphan") != std::string::npos);
            REQUIRE(expected.find("unassigned-variable") != std::string::npos);
        }
        current->offerInstanceBodies(current);
    }
    CHECK(cache->status().publications == 1);
}

TEST_CASE("Module lexical completion respects native compilation-unit import lookup positions",
          "[scope-semantic][scope-completion-import-order]") {
    struct CompletionProbe : server::completions::MemberCompletionQuery {
        using MemberCompletionQuery::addCompletions;
    };
    SourceManager sources;
    ast::Compilation compilation;
    compilation.addSyntaxTree(syntax::SyntaxTree::fromText(R"(
package early_pkg; localparam int Early=1; endpackage
package late_pkg; localparam int Late=2; localparam int ExplicitLate=3; endpackage
import early_pkg::*;
module before_import; logic marker; endmodule
localparam int BetweenDeclarations=0;
import late_pkg::*;
import late_pkg::ExplicitLate;
module after_import; logic marker; endmodule
)",
                                                           sources));
    REQUIRE(compilation.getSemanticDiagnostics().empty());
    for (const auto* instance : compilation.getRoot().topInstances) {
        std::vector<lsp::CompletionItem> items;
        CompletionProbe::addCompletions(items, &instance->body,
                                        server::CompletionContextKind::Expression, &instance->body,
                                        "file:///import-order.sv");
        auto has = [&](std::string_view label) {
            return std::ranges::any_of(items,
                                       [&](const auto& item) { return item.label == label; });
        };
        CHECK(has("Early"));
        CHECK(has("Late") == (instance->name == "after_import"));
        CHECK(has("ExplicitLate") == (instance->name == "after_import"));
    }
}

TEST_CASE("Independent compact owners preserve disjoint generated paths and enum variants",
          "[scope-semantic][scope-body-compact-branches]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.dut"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
package stable_types;
  typedef enum logic [1:0] {Zero=0, One=1} mode_t;
endpackage
module stable #(parameter int W=4, parameter stable_types::mode_t MODE=stable_types::Zero)
  (input wire [W-1:0] a, output wire [W-1:0] y);
  wire [W-1:0] value = a ^ (MODE == stable_types::One ? '1 : '0);
  logic unused_inside;
  always_comb unused_inside = a[0];
  assign y = value;
endmodule
module parent #(parameter int W=4, parameter type T=logic)
  (input wire [W-1:0] a, output wire [2*W-1:0] y);
  for (genvar i=-1; i<=1; i+=2) begin: g
    if (1) begin: inner
      stable #(.W(W), .MODE(i<0 ? stable_types::Zero : stable_types::One))
        u(.a(a), .y(y[((i+1)/2)*W +: W]));
    end
  end
endmodule
)");
    auto source = [](int width) {
        return fmt::format("module top(input wire [{}:0] a, output wire [{}:0] y); "
                           "parent #(.W({})) dut(.*); endmodule\n",
                           width - 1, 2 * width - 1, width);
    };
    files.write(files.root / "consumer.sv", source(4));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto library = driver->options.get<server::TrustedUvmImport>()->library;
    auto cache = std::make_shared<server::ScopeBodyCache>(library, 64 * 1024 * 1024,
                                                          std::vector<std::string>{"stable"},
                                                          std::vector<std::string>{});
    auto make = [&](int width, bool native) {
        auto options = driver->options;
        options.set(server::ScopeBodyImport{cache});
        auto compilation = std::make_shared<ast::Compilation>(options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, options));
        if (native)
            compilation->setInstanceBodyReuseHooks({}, {});
        compilation->addSyntaxTree(
            syntax::SyntaxTree::fromText(source(width), driver->sm, "branch-user.sv", "", options));
        const auto& diagnostics = compilation->getSemanticDiagnostics();
        REQUIRE_FALSE(std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }));
        return compilation;
    };
    auto first = make(4, false);
    std::weak_ptr<syntax::SyntaxTree> oldSyntax = first->getSyntaxTrees().front();
    first->offerInstanceBodies(first);
    REQUIRE(cache->status().publications == 2);
    REQUIRE(cache->status().compactPublications == 2);
    first.reset();
    CHECK(oldSyntax.expired());
    for (int width : {4, 8, 4}) {
        auto current = make(width, false);
        auto native = make(width, true);
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        analysis::AnalysisOptions analysisOptions{.flags = analysis::AnalysisFlags::CheckUnused |
                                                           analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager currentManager(analysisOptions), nativeManager(analysisOptions);
        current->freeze();
        currentManager.analyze(*current);
        current->unfreeze();
        native->freeze();
        nativeManager.analyze(*native);
        native->unfreeze();
        const auto expected = DiagnosticEngine::reportAll(driver->sm,
                                                          nativeManager.getDiagnostics());
        REQUIRE(expected.find("unused_inside") != std::string::npos);
        CHECK(DiagnosticEngine::reportAll(driver->sm, currentManager.getDiagnostics()) == expected);
        auto& parent = current->getRoot()
                           .topInstances.front()
                           ->body.find("dut")
                           ->as<ast::InstanceSymbol>()
                           .body;
        const auto& entries = parent.find("g")->as<ast::GenerateBlockArraySymbol>().entries;
        REQUIRE(entries.size() == 2);
        for (const auto* entry : entries) {
            auto& child = entry->find("inner")
                              ->as<ast::GenerateBlockSymbol>()
                              .find("u")
                              ->as<ast::InstanceSymbol>()
                              .body;
            CHECK(child.find("value")->as<ast::ValueSymbol>().getType().getBitWidth() ==
                  unsigned(width));
            CHECK(current->isImportedInstanceBody(child) == (width == 4));
        }
        current->offerInstanceBodies(current);
    }
    CHECK(cache->status().publications == 4);
    CHECK(cache->status().compactPublications == 4);
}

TEST_CASE("Compact body owners release mutable syntax and preserve nested resolved variants",
          "[scope-semantic][scope-body-compact]") {
    const bool arrayAncestor = GENERATE(false, true);
    const bool cliOverride = GENERATE(false, true);
    const bool compact = !arrayAncestor && !cliOverride;
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources = Config::FrozenSourcesConfig{
        .instances = std::vector<std::string>{arrayAncestor ? "top.wrap[0].u" : "top.wrap.u"},
        .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(),
         cliOverride ? "--single-unit --threads 1 --top top -GTOP_UNUSED=1"
                     : "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
module stable #(parameter int W=4, parameter N=2,
                parameter logic [7:0] MASK=0, parameter signed [39:0] BIAS=-1)
  (input wire [W-1:0] a, output wire [W-1:0] y);
  wire [W-1:0] value = a ^ MASK[W-1:0];
  logic unused_inside;
  always_comb unused_inside = a[0];
  assign y = value + N + BIAS;
endmodule
)");
    auto source = [&](int width) {
        return fmt::format(
            "module wrapper #(parameter int W=4)(input wire [W-1:0] a, output wire [W-1:0] y); "
            "stable #(.W(W),.N(4'hf),.MASK(8'b10xz0110),.BIAS(-40'sd3)) u(.a(a),.y(y)); "
            "endmodule\nmodule top #(parameter TOP_UNUSED=0)(input wire [{}:0] a, output wire "
            "[{}:0] y); "
            "wrapper #(.W({})) wrap{}(.a(a),.y(y)); endmodule\n",
            width - 1, width - 1, width, arrayAncestor ? "[0:0]" : "");
    };
    files.write(files.root / "consumer.sv", source(4));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto library = driver->options.get<server::TrustedUvmImport>()->library;
    auto cache = std::make_shared<server::ScopeBodyCache>(library, 64 * 1024 * 1024,
                                                          std::vector<std::string>{"stable"},
                                                          std::vector<std::string>{});
    auto make = [&](int width, bool native) {
        auto options = driver->options;
        options.set(server::ScopeBodyImport{cache});
        auto compilation = std::make_shared<ast::Compilation>(options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, options));
        if (native)
            compilation->setInstanceBodyReuseHooks({}, {});
        compilation->addSyntaxTree(syntax::SyntaxTree::fromText(source(width), driver->sm,
                                                                "compact-user.sv", "", options));
        const auto& diagnostics = compilation->getSemanticDiagnostics();
        REQUIRE_FALSE(std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }));
        return compilation;
    };
    auto first = make(4, false);
    std::weak_ptr<syntax::SyntaxTree> oldSyntax = first->getSyntaxTrees().front();
    first->offerInstanceBodies(first);
    REQUIRE(cache->status().publications == 1);
    CHECK(cache->status().compactPublications == (compact ? 1 : 0));
    first.reset();
    // Unsupported array / CLI contexts retain the ordinary independent syntax.
    CHECK(oldSyntax.expired() == compact);
    for (const int width : {4, 8, 4}) {
        auto current = make(width, false);
        auto native = make(width, true);
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        analysis::AnalysisOptions analysisOptions{.flags = analysis::AnalysisFlags::CheckUnused |
                                                           analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager currentManager(analysisOptions), nativeManager(analysisOptions);
        current->freeze();
        currentManager.analyze(*current);
        current->unfreeze();
        native->freeze();
        nativeManager.analyze(*native);
        native->unfreeze();
        const auto expected = DiagnosticEngine::reportAll(driver->sm,
                                                          nativeManager.getDiagnostics());
        REQUIRE(expected.find("unused_inside") != std::string::npos);
        CHECK(DiagnosticEngine::reportAll(driver->sm, currentManager.getDiagnostics()) == expected);
        auto& top = current->getRoot().topInstances.front()->body;
        auto instance = top.find("wrap");
        REQUIRE(instance);
        if (arrayAncestor)
            instance = instance->as<ast::InstanceArraySymbol>().elements.front();
        auto& body =
            instance->as<ast::InstanceSymbol>().body.find("u")->as<ast::InstanceSymbol>().body;
        CHECK(body.find("value")->as<ast::ValueSymbol>().getType().getBitWidth() ==
              unsigned(width));
        if (width == 4)
            CHECK(current->isImportedInstanceBody(body));
        current->offerInstanceBodies(current);
    }
    CHECK(cache->status().publications == 2);
    CHECK(cache->status().compactPublications == (compact ? 2 : 0));
}

TEST_CASE(
    "Package dependency closure excludes local class shadows and permits unrelated root edits",
    "[scope-semantic][scope-package-closure]") {
    const bool lateDependency = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.packages = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    auto unrelated = [](int value, bool error) {
        return fmt::format("package Shadow; {} endpackage\n"
                           "package mutable; localparam int WIDTH={}; endpackage\n",
                           error ? "missing_type broken;" : "int repaired;", value);
    };
    files.write(files.root / "unrelated.sv", unrelated(4, true));
    files.write(files.root / "deps.sv", R"(
package leaf; typedef enum logic [1:0] {Ready=1, Busy=2} state_t; endpackage
package middle; import leaf::*; typedef state_t state_alias_t; endpackage
)");
    files.write(files.root / "stable.sv", R"(
package stable;
  import middle::*;
  typedef state_alias_t state_t;
  class Shadow; localparam int VALUE=7; endclass
  localparam int N=Shadow::VALUE;
endpackage
)");
    files.write(files.root / "consumer.sv",
                "module top; stable::state_t state; "
                "localparam int V=mutable::WIDTH+stable::N; endmodule\n");
    files.write(files.root / "design.f", lateDependency
                                             ? "unrelated.sv\nstable.sv\ndeps.sv\nconsumer.sv\n"
                                             : "unrelated.sv\ndeps.sv\nstable.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    INFO(rfl::json::write(driver->scopeLibraryStatus()));
    REQUIRE(driver->options.get<server::TrustedUvmImport>());
    auto library = driver->options.get<server::TrustedUvmImport>()->library;
    REQUIRE(library);
    CHECK(std::set<std::string>(library->packages.begin(), library->packages.end()) ==
          std::set<std::string>{"leaf", "middle", "stable"});
    CHECK(library->definitions.empty());
    CHECK(library->files.size() == 2);
    CHECK_FALSE(library->files.contains((files.root / "unrelated.sv").generic_string()));
    auto enumType = &library->compilation->getPackage("leaf")
                         ->find("state_t")
                         ->as<ast::Type>()
                         .getCanonicalType();
    const auto generation = library->generation;
    const auto astBytes = library->compilation->getTotalBytesAllocated();
    auto uri = URI::fromFile(files.root / "unrelated.sv");
    driver->openDocument(uri, unrelated(4, true), 1, true);
    auto check = [&](server::ServerDriver& actual, int value, bool error) {
        const auto& instances = actual.comp->getInstancesOfModule("top");
        REQUIRE(instances.size() == 1);
        auto& body = instances.front()->body;
        CHECK(body.find("V")->as<ast::ParameterSymbol>().getValue().integer().as<int>() ==
              value + 7);
        CHECK(&body.find("state")->as<ast::ValueSymbol>().getType().getCanonicalType() == enumType);
        Config nativeConfig = config;
        nativeConfig.experimentalScopeSemanticCache = false;
        Indexer nativeIndexer;
        ClientHarness nativeClient;
        auto native = server::ServerDriver::createFromFileLists(nativeIndexer, nativeClient,
                                                                nativeConfig, {"design.f"},
                                                                files.root.string(), nullptr, true);
        REQUIRE(native);
        native->openDocument(uri, unrelated(value, error), 1, true);
        auto& nativeCompilation =
            native->comp->getInstancesOfModule("top").front()->body.getCompilation();
        CHECK(DiagnosticEngine::reportAll(actual.sm,
                                          body.getCompilation().getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(native->sm, nativeCompilation.getSemanticDiagnostics()));
        CHECK(actual.options.get<server::TrustedUvmImport>()->library == library);
        CHECK(library->generation == generation);
        CHECK(library->compilation->getTotalBytesAllocated() == astBytes);
    };
    check(*driver, 4, true);
    for (int edit = 1; edit < 4; ++edit) {
        const auto text = unrelated(edit == 1 ? 8 : 4, edit == 2);
        driver->onDocDidChange(
            {.textDocument = {.version = edit + 1, .uri = uri},
             .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
        check(*driver, edit == 1 ? 8 : 4, edit == 2);
    }
    CHECK(rfl::json::write(driver->scopeLibraryStatus()).find("\"syntaxResumes\":0") !=
          std::string::npos);
    driver->preserveTrustedUvmForRetirement();
    auto replacement = server::ServerDriver::createFromFileLists(indexer, client, config,
                                                                 {"design.f"}, files.root.string(),
                                                                 driver.get(), true);
    REQUIRE(replacement);
    check(*replacement, 4, false);
    const auto depsUri = URI::fromFile(files.root / "deps.sv");
    std::ifstream depsInput(files.root / "deps.sv");
    std::string depsText((std::istreambuf_iterator<char>(depsInput)), {});
    replacement->openDocument(depsUri, depsText, 1, false);
    depsText.replace(depsText.find("logic [1:0]"), std::string_view("logic [1:0]").size(),
                     "logic [2:0]");
    replacement->onDocDidChange(
        {.textDocument = {.version = 2, .uri = depsUri},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = depsText}}});
    CHECK_THROWS_AS(replacement->comp->getInstancesOfModule("top"), lsp::RequestFailed);
    replacement->preserveTrustedUvmForRetirement(false);
    auto rebuilt = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                             files.root.string(), replacement.get(),
                                                             true);
    REQUIRE(rebuilt);
    auto next = rebuilt->options.get<server::TrustedUvmImport>()->library;
    CHECK(next->generation != generation);
    auto& nextBody = rebuilt->comp->getInstancesOfModule("top").front()->body;
    CHECK(nextBody.find("state")->as<ast::ValueSymbol>().getType().getBitWidth() == 3);
    CHECK(nextBody.find("V")->as<ast::ParameterSymbol>().getValue().integer().as<int>() == 11);
    CHECK(enumType->getBitWidth() == 2);
}

TEST_CASE("Package closure observes qualified dependencies in uninstantiated generic classes",
          "[scope-semantic][scope-package-closure]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.packages = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "unrelated.sv",
                "package unrelated; missing_type broken; endpackage\n");
    files.write(files.root / "leaf.sv",
                "package leaf; typedef enum {Ready, Busy} state_t; endpackage\n");
    files.write(files.root / "stable.sv", "package stable; class Deferred #(type T); "
                                          "leaf::state_t state; endclass endpackage\n");
    files.write(files.root / "consumer.sv",
                "module top; stable::Deferred#(int) handle; endmodule\n");
    files.write(files.root / "design.f", "unrelated.sv\nleaf.sv\nstable.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    INFO(rfl::json::write(driver->scopeLibraryStatus()));
    REQUIRE(driver->options.get<server::TrustedUvmImport>());
    auto library = driver->options.get<server::TrustedUvmImport>()->library;
    CHECK(library->packages == std::vector<std::string>{"leaf", "stable"});
    CHECK_FALSE(library->files.contains((files.root / "unrelated.sv").generic_string()));
    auto& body = driver->comp->getInstancesOfModule("top").front()->body;
    auto& type = body.find("handle")->as<ast::ValueSymbol>().getType().getCanonicalType();
    auto field = type.as<ast::ClassType>().find("state");
    REQUIRE(field);
    CHECK(&field->as<ast::ValueSymbol>().getType().getCanonicalType() ==
          &library->compilation->getPackage("leaf")
               ->find("state_t")
               ->as<ast::Type>()
               .getCanonicalType());
}

TEST_CASE("Independent package reference analyses preserve header and macro declaration provenance",
          "[scope-semantic][scope-package-references]") {
    const bool cached = GENERATE(false, true);
    const bool macro = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = cached;
    config.frozenSources =
        Config::FrozenSourcesConfig{.packages = std::vector<std::string>{"A", "B"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "body.svh",
                macro ? "`define FIELD(name) int name;\nclass Item; `FIELD(state) endclass\n"
                      : "class Item; int state; endclass\n");
    files.write(files.root / "provider.sv", "package A;\n`include \"body.svh\"\nendpackage\n"
                                            "package B;\n`include \"body.svh\"\nendpackage\n");
    const std::string consumer = "module top; A::Item a; B::Item b; initial begin "
                                 "if(a!=null) $display(a.state); if(b!=null) $display(b.state); "
                                 "end endmodule\n";
    files.write(files.root / "consumer.sv", consumer);
    files.write(files.root / "design.f", "provider.sv\nconsumer.sv\n");
    Indexer indexer;
    indexer.setNumThreads(1);
    indexer.startIndexing({}, files.root.string());
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    const auto consumerUri = URI::fromFile(files.root / "consumer.sv");
    const auto headerUri = URI::fromFile(files.root / "body.svh");
    driver->openDocument(consumerUri, consumer, 1, true);
    const auto header =
        macro ? std::string("`define FIELD(name) int name;\nclass Item; `FIELD(state) endclass\n")
              : std::string("class Item; int state; endclass\n");
    driver->openDocument(headerUri, header, 1, false);
    auto offset = header.find("state");
    const auto prefix = std::string_view(header).substr(0, offset);
    lsp::Position position{.line = uint32_t(std::ranges::count(prefix, '\n')),
                           .character = uint32_t(prefix.size() -
                                                 (prefix.rfind('\n') == std::string_view::npos
                                                      ? 0
                                                      : prefix.rfind('\n') + 1))};
    const auto refs = driver->getDocReferences(headerUri, position, true);
    REQUIRE(refs);
    INFO(rfl::json::write(*refs));
    auto a = consumer.find("a.state") + 2, b = consumer.find("b.state") + 2;
    CHECK(std::ranges::any_of(*refs, [&](const auto& r) {
        return r.uri == consumerUri && r.range.start.character == a;
    }));
    CHECK_FALSE(std::ranges::any_of(*refs, [&](const auto& r) {
        return r.uri == consumerUri && r.range.start.character == b;
    }));
}

TEST_CASE("Body type parameters preserve fixed nominal identity and resolved builtin variants",
          "[scope-semantic][scope-body-type-parameters]") {
    const bool useDefault = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
package stable_types;
  typedef struct packed { logic [3:0] value; } packet_t;
  typedef struct packed { logic [3:0] value; } other_t;
  typedef enum logic [3:0] {Zero=0, One=1} mode_t;
  typedef logic [3:0] nibble_t;
endpackage
module stable #(parameter type T=stable_types::packet_t, parameter type U=T,
                parameter int N=$bits(U))
  (input T a, output var T y);
  U q;
  assign q = a;
  assign y = q;
  localparam int UNUSED=$bits(T);
  localparam U ZERO=U'('0);
endmodule
)");
    auto source = [&](std::string_view type, int edit) {
        return fmt::format(R"(
// edit {}
module top;
  localparam int WIDTH=4;
  typedef logic [WIDTH-1:0] mutable_alias_t;
  {} a;
  {} y;
  stable {} u(.a(a), .y(y));
endmodule
)",
                           edit, type, type, useDefault ? "" : fmt::format("#(.T({}))", type));
    };
    files.write(files.root / "consumer.sv", source("stable_types::packet_t", 0));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    std::vector<std::string> types = useDefault ? std::vector<std::string>{"stable_types::packet_t",
                                                                           "stable_types::packet_t"}
                                                : std::vector<std::string>{"stable_types::packet_t",
                                                                           "stable_types::other_t",
                                                                           "stable_types::mode_t",
                                                                           "logic [WIDTH-1:0]",
                                                                           "logic signed [3:0]",
                                                                           "bit [3:0]",
                                                                           "logic [0:3]",
                                                                           "logic [7:0]",
                                                                           "mutable_alias_t",
                                                                           "stable_types::packet_t",
                                                                           "logic [WIDTH-1:0]",
                                                                           "reg [3:0]",
                                                                           "logic [3:0]",
                                                                           "stable_types::nibble_t",
                                                                           "logic [3:0]"};
    std::map<std::string, const ast::Symbol*> identities;
    for (size_t edit = 0; edit < types.size(); ++edit) {
        auto make = [&](bool native) {
            auto compilation = std::make_shared<ast::Compilation>(driver->options);
            REQUIRE(server::importTrustedUvm(*compilation, driver->sm, driver->options));
            if (native)
                compilation->setInstanceBodyReuseHooks({}, {});
            compilation->addSyntaxTree(
                syntax::SyntaxTree::fromText(source(types[edit], int(edit)), driver->sm,
                                             "type-argument-user.sv", "", driver->options));
            INFO(DiagnosticEngine::reportAll(driver->sm, compilation->getSemanticDiagnostics()));
            REQUIRE_FALSE(
                std::ranges::any_of(compilation->getSemanticDiagnostics(),
                                    [](const auto& diagnostic) { return diagnostic.isError(); }));
            return compilation;
        };
        auto current = make(false);
        auto native = make(true);
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        auto bodyOf = [](auto& compilation) -> const ast::InstanceBodySymbol& {
            return compilation.getRoot()
                .topInstances.front()
                ->body.find("u")
                ->template as<ast::InstanceSymbol>()
                .body;
        };
        const auto& body = bodyOf(*current);
        const auto& type = body.find("q")->as<ast::ValueSymbol>().getType();
        const auto& expectedType = bodyOf(*native).find("q")->as<ast::ValueSymbol>().getType();
        CHECK(type.isMatching(expectedType));
        CHECK(body.find("T")->as<ast::TypeParameterSymbol>().targetType.getType().toString() ==
              bodyOf(*native)
                  .find("T")
                  ->as<ast::TypeParameterSymbol>()
                  .targetType.getType()
                  .toString());
        const bool mutableAlias = types[edit] == "mutable_alias_t";
        CHECK_FALSE((mutableAlias && current->isImportedInstanceBody(body)));
        if (edit == 0 || edit == 9 || edit == 10 || (useDefault && edit == 1))
            CHECK(current->isImportedInstanceBody(body));
        if (current->isImportedInstanceBody(body)) {
            auto [it, inserted] = identities.emplace(types[edit], body.find("q"));
            CHECK(it->second == body.find("q"));
        }
        analysis::AnalysisOptions analysisOptions{.flags = analysis::AnalysisFlags::CheckUnused |
                                                           analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager actualAnalysis(analysisOptions), nativeAnalysis(analysisOptions);
        current->freeze();
        actualAnalysis.analyze(*current);
        current->unfreeze();
        native->freeze();
        nativeAnalysis.analyze(*native);
        native->unfreeze();
        CHECK(DiagnosticEngine::reportAll(driver->sm, actualAnalysis.getDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, nativeAnalysis.getDiagnostics()));
        current->offerInstanceBodies(current);
    }
    CHECK(cache->status().compactPublications > 0);
}

TEST_CASE("Type argument defaults audit mutable dependencies and preserve native errors",
          "[scope-semantic][scope-body-type-dependencies]") {
    const auto defaultType = GENERATE(std::string("consumer_types::data_t"),
                                      std::string("struct packed { logic [3:0] data; }"),
                                      std::string("type(q)"));
    const bool overridden = GENERATE(false, true);
    const bool invalid = !overridden && defaultType == "type(q)";
    const bool reusable = defaultType.starts_with("struct");
    INFO(defaultType);
    INFO(overridden);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", fmt::format(R"(
module stable #(parameter type T={})(output wire y);
  logic [3:0] q;
  T value;
  assign y=q[0] ^ value[0];
endmodule
)",
                                                     defaultType));
    auto source = [&](int width) {
        return fmt::format("package consumer_types; typedef logic [{}:0] data_t; endpackage\n"
                           "module top; wire y; stable {} u(.y(y)); endmodule\n",
                           width - 1, overridden ? "#(.T(logic [3:0]))" : "");
    };
    files.write(files.root / "consumer.sv", source(4));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    for (int width : {4, 8, 4}) {
        auto make = [&](bool native) {
            auto compilation = std::make_shared<ast::Compilation>(driver->options);
            REQUIRE(server::importTrustedUvm(*compilation, driver->sm, driver->options));
            if (native)
                compilation->setInstanceBodyReuseHooks({}, {});
            compilation->addSyntaxTree(syntax::SyntaxTree::fromText(
                source(width), driver->sm, "type-default-user.sv", "", driver->options));
            INFO(DiagnosticEngine::reportAll(driver->sm, compilation->getSemanticDiagnostics()));
            CHECK(std::ranges::any_of(compilation->getSemanticDiagnostics(),
                                      [](const auto& diagnostic) {
                                          return diagnostic.isError();
                                      }) == invalid);
            return compilation;
        };
        auto current = make(false);
        auto native = make(true);
        const auto& body =
            current->getRoot().topInstances.front()->body.find("u")->as<ast::InstanceSymbol>().body;
        CHECK(current->isImportedInstanceBody(body) == reusable);
        if (!invalid)
            CHECK(body.find("value")->as<ast::ValueSymbol>().getType().getBitWidth() ==
                  unsigned(!overridden && defaultType == "consumer_types::data_t" ? width : 4));
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        current->offerInstanceBodies(current);
        CHECK(cache->status().publications == (reusable ? 1 : 0));
    }
}

TEST_CASE(
    "Local nominal defaults preserve declaration origin and instance identity across variants",
    "[scope-semantic][scope-body-local-nominal]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
module stable #(parameter int W=4,
  parameter type T=struct packed { logic [W-1:0] payload; },
  parameter type V=struct packed { logic [W-1:0] payload; },
  parameter type U=T,
  parameter type E=enum logic [1:0] {Idle=0, Busy=1},
  parameter U MASK=U'('0))
  (input logic [W-1:0] a, output wire [W-1:0] y);
  U q;
  V twin;
  E state=Idle;
  localparam int N=$bits(U);
  assign q=a ^ MASK;
  assign twin=a;
  assign y=q.payload ^ twin.payload ^ (state==Busy ? '1 : '0);
endmodule
)");
    auto source = [](int width) {
        return fmt::format("module top; localparam int WIDTH={}; logic [WIDTH-1:0] a; "
                           "wire [WIDTH-1:0] y, z; stable #(.W(WIDTH)) u(.a(a),.y(y)); "
                           "stable #(.W(WIDTH)) other(.a(a),.y(z)); endmodule\n",
                           width);
    };
    files.write(files.root / "consumer.sv", source(4));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    auto body = [](auto& comp, std::string_view name) -> const ast::InstanceBodySymbol& {
        return comp.getRoot()
            .topInstances.front()
            ->body.find(name)
            ->template as<ast::InstanceSymbol>()
            .body;
    };
    std::map<std::pair<int, std::string>, const ast::Symbol*> identities;
    for (int width : {4, 8, 8, 4}) {
        auto make = [&](bool native) {
            auto comp = std::make_shared<ast::Compilation>(driver->options);
            REQUIRE(server::importTrustedUvm(*comp, driver->sm, driver->options));
            if (native)
                comp->setInstanceBodyReuseHooks({}, {});
            comp->addSyntaxTree(syntax::SyntaxTree::fromText(
                source(width), driver->sm, "local-nominal-user.sv", "", driver->options));
            INFO(DiagnosticEngine::reportAll(driver->sm, comp->getSemanticDiagnostics()));
            REQUIRE_FALSE(std::ranges::any_of(comp->getSemanticDiagnostics(),
                                              [](const auto& d) { return d.isError(); }));
            return comp;
        };
        auto current = make(false), native = make(true);
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        for (const auto name : {"u", "other"}) {
            const auto& actual = body(*current, name);
            const auto& expected = body(*native, name);
            const auto& at = actual.find("q")->as<ast::ValueSymbol>().getType();
            const auto& et = expected.find("q")->as<ast::ValueSymbol>().getType();
            CHECK(at.getBitWidth() == unsigned(width));
            CHECK(at.isMatching(actual.find("twin")->as<ast::ValueSymbol>().getType()) ==
                  et.isMatching(expected.find("twin")->as<ast::ValueSymbol>().getType()));
            CHECK_FALSE(at.isMatching(actual.find("twin")->as<ast::ValueSymbol>().getType()));
            if (current->isImportedInstanceBody(actual)) {
                auto [it, inserted] = identities.emplace(std::pair{width, std::string(name)},
                                                         actual.find("q"));
                CHECK(it->second == actual.find("q"));
            }
        }
        CHECK(
            body(*current, "u")
                .find("q")
                ->as<ast::ValueSymbol>()
                .getType()
                .isMatching(body(*current, "other").find("q")->as<ast::ValueSymbol>().getType()) ==
            body(*native, "u")
                .find("q")
                ->as<ast::ValueSymbol>()
                .getType()
                .isMatching(body(*native, "other").find("q")->as<ast::ValueSymbol>().getType()));
        analysis::AnalysisOptions opts{.flags = analysis::AnalysisFlags::CheckUnused |
                                                analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager am(opts), nm(opts);
        current->freeze();
        am.analyze(*current);
        current->unfreeze();
        native->freeze();
        nm.analyze(*native);
        native->unfreeze();
        CHECK(DiagnosticEngine::reportAll(driver->sm, am.getDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, nm.getDiagnostics()));
        current->offerInstanceBodies(current);
    }
    CHECK(cache->status().publications == 4);
    CHECK(cache->status().compactPublications == 4);
    CHECK(identities.size() >= 2);
}

TEST_CASE("Closed fixed arrays retain nominal elements and native indexed drivers across edits",
          "[scope-semantic][scope-body-fixed-arrays]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
package array_provider;
 localparam int DEPTH=2, DISABLED_INDEX=0;
 typedef logic [7:0] disabled_t;
 function automatic int depth_value(input int n); return n; endfunction
 function automatic void check_width(input int width);
   if(width<1) $error("invalid width");
 endfunction
 function automatic logic [3:0] offset(input int n);
   return 4'(n);
 endfunction
endpackage
module array_depth #(parameter int DEPTH=1) (output wire [31:0] y);
 assign y=32'(DEPTH);
endmodule
module array_leaf #(parameter type dtype=logic, parameter int W=4)
 (input dtype words[2][2], output wire [W-1:0] y);
 logic probe, spare;
 assign probe=words[0][1].payload[0];
 assign spare=words[1][0].payload[0];
 assign y=words[0][0].payload ^ words[1][1].payload;
endmodule
module stable #(parameter int W=4,
 parameter type T=struct packed {logic [W-1:0] payload;})
 (input logic clk, input logic [W-1:0] a, output wire [W-1:0] y);
 localparam int SLICE=2;
 localparam T TABLE[2][2]='{'{T'(W),T'(W+1)},'{T'(W+2),T'(W+3)}};
 localparam logic [W-1:0] MASKS[4:5]='{W'(W),W'(W+1)};
 T words[2][2];
 logic [W-1:0] random_value;
 initial random_value=W'({2{$urandom()}});
 typedef string filename_t;
 filename_t fn;
 int fd;
 function automatic filename_t trace_name(input string suffix);
   return {"scope-cache-no-runtime-io-",suffix};
 endfunction
 initial begin
   $sformat(fn,"%s",trace_name("test"));
   fd=$fopen(fn,"w");
   $fwrite(fd,"%s %0h",fn,a);
   $fclose(fd);
 end
 function automatic void check_value(input logic [W-1:0] x);
   if(W<1) $fatal(1,"invalid value %0h",x);
 endfunction
 initial check_value(a);
 initial begin
   array_provider::check_width(W);
   if(W<=0) $fatal(1,"invalid width %0d",W);
   if(W<0) $error("negative %0d",W);
   if(W==0) $warning("zero width");
   if(W>0) $info("configured %0d %m",W);
   if(W<0) $stop(1);
   if(W<0) $finish(2);
 end
 function automatic T make_word(input logic [W-1:0] value, input int offset);
   int offsets[2];
   logic [W-1:0] mixed;
   foreach (offsets[k]) offsets[k]=offset;
   mixed = value + TABLE[0][0].payload + MASKS[5] + W'(array_provider::offset(offsets[0])) + {{0{1'b1}}, {W{1'b0}}};
   make_word.payload={<<SLICE{mixed}};
 endfunction
 for(genvar i=0;i<2;i++) begin:g
   for(genvar j=0;j<2;j++) begin:h
     always_ff @(posedge clk) words[i][j] <= make_word(a, i+j);
   end
 end
 for(genvar unused_iteration=0;unused_iteration<0;unused_iteration++) begin:empty_loop
   wire never_used;
 end
 if(W<0) begin:disabled
   localparam int BITS=$bits(array_provider::disabled_t);
   wire unused_bit=a[array_provider::DISABLED_INDEX];
 end
 wire [31:0] depth_y;
 array_depth #(.DEPTH(array_provider::depth_value(array_provider::DEPTH))) depth(.y(depth_y));
 typedef T local_t;
 wire [W-1:0] leaf_y;
 array_leaf #(.dtype(local_t), .W(W)) leaf(.words(words),.y(leaf_y));
 assign y={>>{leaf_y ^ leaf.words[0][1].payload ^ W'(leaf.probe) ^ random_value ^ W'(depth_y)}};
endmodule
)");
    auto source = [](int width, bool read, bool write) {
        return fmt::format("module top; logic clk; logic [{}:0] a; wire [{}:0] y; "
                           "stable #(.W({})) u(.*); {} {} endmodule\n",
                           width - 1, width - 1, width,
                           read ? "initial $display(u.words[0][1].payload, u.leaf.spare);" : "",
                           write ? "initial u.words[1][1].payload = '0;" : "");
    };
    files.write(files.root / "consumer.sv", source(4, false, false));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    std::vector<int> preparedWidths{4}; // initial build publishes the selected root
    for (const auto [width, read, write] :
         std::vector<std::tuple<int, bool, bool>>{{4, false, false},
                                                  {4, true, false},
                                                  {4, true, true},
                                                  {8, false, false},
                                                  {8, true, false},
                                                  {4, false, false}}) {
        auto make = [&](bool native) {
            auto comp = std::make_shared<ast::Compilation>(driver->options);
            REQUIRE(server::importTrustedUvm(*comp, driver->sm, driver->options));
            if (native)
                comp->setInstanceBodyReuseHooks({}, {});
            comp->addSyntaxTree(syntax::SyntaxTree::fromText(source(width, read, write), driver->sm,
                                                             "array-user.sv", "", driver->options));
            (void)comp->getSemanticDiagnostics();
            return comp;
        };
        auto current = make(false), native = make(true);
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        auto words = [](auto& comp) -> const ast::ValueSymbol& {
            return comp.getRoot()
                .topInstances.front()
                ->body.find("u")
                ->template as<ast::InstanceSymbol>()
                .body.find("words")
                ->template as<ast::ValueSymbol>();
        };
        const auto& selectedBody =
            current->getRoot().topInstances.front()->body.find("u")->as<ast::InstanceSymbol>().body;
        CHECK(current->isImportedInstanceBody(selectedBody) ==
              (std::ranges::find(preparedWidths, width) != preparedWidths.end()));
        const auto& nativeBody =
            native->getRoot().topInstances.front()->body.find("u")->as<ast::InstanceSymbol>().body;
        for (auto name : {"TABLE", "MASKS"}) {
            const auto& table = selectedBody.find(name)->as<ast::ParameterSymbol>();
            CHECK(table.getValue() == nativeBody.find(name)->as<ast::ParameterSymbol>().getValue());
            CHECK(table.getType().toString() ==
                  nativeBody.find(name)->as<ast::ParameterSymbol>().getType().toString());
        }
        const auto& at = words(*current).getType();
        REQUIRE(at.getCanonicalType().kind == ast::SymbolKind::FixedSizeUnpackedArrayType);
        REQUIRE(at.getArrayElementType()->getCanonicalType().kind ==
                ast::SymbolKind::FixedSizeUnpackedArrayType);
        CHECK(at.getArrayElementType()->getArrayElementType()->getBitWidth() == unsigned(width));
        analysis::AnalysisOptions opts{.flags = analysis::AnalysisFlags::CheckUnused |
                                                analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager am(opts), nm(opts);
        current->freeze();
        am.analyze(*current);
        current->unfreeze();
        native->freeze();
        nm.analyze(*native);
        native->unfreeze();
        CHECK(DiagnosticEngine::reportAll(driver->sm, am.getDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, nm.getDiagnostics()));
        CHECK(am.getDrivers(words(*current)).size() == nm.getDrivers(words(*native)).size());
        current->offerInstanceBodies(current);
        preparedWidths.push_back(width);
    }
    CHECK(cache->status().hits > 0);
    CHECK(cache->status().publications == 2);
}

TEST_CASE("Closed body extensions reject mutable function and array dependencies",
          "[scope-semantic][scope-body-closed-dependencies]") {
    const auto implementation = GENERATE(
        std::string("function automatic int f(input int a); return a+consumer_pkg::N; "
                    "endfunction always_comb y=f(a);"),
        std::string("function automatic int f(input int a); return consumer_pkg::f(a); "
                    "endfunction always_comb y=f(a);"),
        std::string("function automatic int f(input int a, output int q); q=a; return a; "
                    "endfunction int q; always_comb y=f(a,q);"),
        std::string("function automatic void f(input int x); y=x+consumer_pkg::N; "
                    "endfunction always_comb f(a);"),
        std::string("always_comb y=$urandom(a);"),
        std::string("initial $sformat($root.top.mutable_name,\"%0d\",a); assign y=a;"),
        std::string("int fd; initial fd=$fopen($root.top.mutable_name,\"w\"); assign y=a;"),
        std::string("localparam string NAME=\"fixed\"; initial $info(NAME); assign y=a;"),
        std::string("always_comb y={<<consumer_pkg::N{a}};"),
        std::string("localparam int TABLE[2]='{consumer_pkg::N,0}; "
                    "always_comb y=TABLE[a[0]];"),
        std::string("localparam int TABLE[consumer_pkg::N]='{default:1}; "
                    "always_comb y=TABLE[0];"),
        std::string("if(consumer_pkg::N==4) begin:g wire q=a[0]; end always_comb y=a;"),
        std::string("if(consumer_pkg::N==4) if(1) begin:g wire q=a[0]; end always_comb y=a;"),
        std::string("case(1) consumer_pkg::N: begin:g wire q=a[0]; end "
                    "default: begin:h wire q=a[1]; end endcase always_comb y=a;"),
        std::string("for(genvar i=0;i<consumer_pkg::N;i++) begin:g wire q=a[0]; end "
                    "always_comb y=a;"),
        std::string("int seed=1; initial $info(\"random %0d\",$urandom(seed)); always_comb y=a;"),
        std::string("logic words[consumer_pkg::N]; always_comb begin words[0]=a[0]; "
                    "y=32'(words[0]); end"));
    INFO(implementation);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv",
                "module stable(input int a, output int y); " + implementation + " endmodule\n");
    auto source = [](int n) {
        return fmt::format("package consumer_pkg; parameter int N={}; "
                           "function automatic int f(input int a); return a+N; endfunction "
                           "endpackage module top; string mutable_name=\"consumer\"; int a,y; "
                           "stable u(.*); endmodule\n",
                           n);
    };
    files.write(files.root / "consumer.sv", source(4));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    for (int n : {4, 8, 4}) {
        auto make = [&](bool native) {
            auto compilation = std::make_shared<ast::Compilation>(driver->options);
            REQUIRE(server::importTrustedUvm(*compilation, driver->sm, driver->options));
            if (native)
                compilation->setInstanceBodyReuseHooks({}, {});
            compilation->addSyntaxTree(syntax::SyntaxTree::fromText(
                source(n), driver->sm, "closed-dependency-user.sv", "", driver->options));
            const auto& diagnostics = compilation->getSemanticDiagnostics();
            INFO(DiagnosticEngine::reportAll(driver->sm, diagnostics));
            REQUIRE_FALSE(
                std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }));
            return compilation;
        };
        auto current = make(false), native = make(true);
        const auto& body =
            current->getRoot().topInstances.front()->body.find("u")->as<ast::InstanceSymbol>().body;
        CHECK_FALSE(current->isImportedInstanceBody(body));
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        analysis::AnalysisOptions opts{.flags = analysis::AnalysisFlags::CheckUnused |
                                                analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager am(opts), nm(opts);
        current->freeze();
        am.analyze(*current);
        current->unfreeze();
        native->freeze();
        nm.analyze(*native);
        native->unfreeze();
        CHECK(DiagnosticEngine::reportAll(driver->sm, am.getDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, nm.getDiagnostics()));
        current->offerInstanceBodies(current);
        CHECK(cache->status().publications == 0);
    }
}

TEST_CASE("Runtime random call reuse preserves native constant-context errors",
          "[scope-semantic][scope-body-runtime-constant-errors]") {
    const auto implementation = GENERATE(
        std::string("localparam int R=$urandom(); assign y=R;"),
        std::string("function automatic int f(); return $urandom(); endfunction "
                    "localparam int R=f(); assign y=R;"),
        std::string("if($urandom()) begin:g assign y=1; end else begin:h assign y=0; end"),
        std::string("localparam int FD=$fopen(\"scope-cache-must-not-open\",\"w\"); assign y=FD;"),
        std::string(
            "function automatic int f(); return $fopen(\"scope-cache-must-not-open\",\"w\"); "
            "endfunction localparam int FD=f(); assign y=FD;"));
    INFO(implementation);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv",
                "module stable(output wire [31:0] y); " + implementation + " endmodule\n");
    auto source = [](int edit) {
        return fmt::format("module top; wire [31:0] y; localparam int EDIT={}; "
                           "stable u(.*); endmodule\n",
                           edit);
    };
    files.write(files.root / "consumer.sv", source(0));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    for (int edit : {0, 1, 0}) {
        auto make = [&](bool native) {
            auto compilation = std::make_shared<ast::Compilation>(driver->options);
            REQUIRE(server::importTrustedUvm(*compilation, driver->sm, driver->options));
            if (native)
                compilation->setInstanceBodyReuseHooks({}, {});
            compilation->addSyntaxTree(syntax::SyntaxTree::fromText(
                source(edit), driver->sm, "random-user.sv", "", driver->options));
            const auto& diagnostics = compilation->getSemanticDiagnostics();
            CHECK(std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }));
            auto report = DiagnosticEngine::reportAll(driver->sm, diagnostics);
            CHECK((report.find("$urandom") != std::string::npos ||
                   report.find("$fopen") != std::string::npos));
            return compilation;
        };
        auto current = make(false), native = make(true);
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        const auto& body =
            current->getRoot().topInstances.front()->body.find("u")->as<ast::InstanceSymbol>().body;
        CHECK_FALSE(current->isImportedInstanceBody(body));
        current->offerInstanceBodies(current);
        CHECK(cache->status().publications == 0);
    }
}

TEST_CASE("Mutable caller ref and symbol arguments do not survive in retained body usage profiles",
          "[scope-semantic][scope-body-caller-ref-origin]") {
    const bool fullContext = GENERATE(false, true);
    INFO(fullContext);
    const bool constantCaller = GENERATE(false, true);
    INFO(constantCaller);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(),
         std::string("--single-unit --threads 1 --top top") + (fullContext ? " --top other" : "") +
             (constantCaller ? " --allow-hierarchical-const" : "")}};
    files.write(files.root / "fixed.sv", R"(
module stable(output wire [31:0] y);
 logic [31:0] q, spare;
 assign y=q;
 function automatic logic [31:0] query(); return 32'h456; endfunction
endmodule
)");
    auto source = [&](bool write) {
        return std::string("module top; wire [31:0] y; stable u(.*); "
                           "task automatic drive(ref logic [31:0] v); v=32'h123; endtask ") +
               (constantCaller && write ? "localparam logic [31:0] MAGIC=u.query(); " : "") +
               (write ? "initial begin logic [31:0] r; drive(u.q); $dumpvars(0,u.spare); "
                        "r=u.query(); end "
                      : "") +
               "endmodule module other; endmodule\n";
    };
    files.write(files.root / "consumer.sv", source(true));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    REQUIRE(cache->status().publications == 1);
    CHECK(cache->status().compactPublications == (fullContext ? 0 : 1));
    auto bodyOf = [](auto& comp) -> const ast::InstanceBodySymbol& {
        auto tops = comp.getRoot().topInstances;
        auto it = std::ranges::find(tops, "top", [](auto* top) { return top->name; });
        REQUIRE(it != tops.end());
        return (*it)->body.find("u")->template as<ast::InstanceSymbol>().body;
    };
    for (bool write : {true, false, true, false}) {
        INFO(write);
        auto make = [&](bool native) {
            auto comp = std::make_shared<ast::Compilation>(driver->options);
            REQUIRE(server::importTrustedUvm(*comp, driver->sm, driver->options));
            if (native)
                comp->setInstanceBodyReuseHooks({}, {});
            comp->addSyntaxTree(syntax::SyntaxTree::fromText(
                source(write), driver->sm, "ref-origin-user.sv", "", driver->options));
            const auto& diagnostics = comp->getSemanticDiagnostics();
            INFO(DiagnosticEngine::reportAll(driver->sm, diagnostics));
            REQUIRE_FALSE(
                std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }));
            return comp;
        };
        auto current = make(false), native = make(true);
        // Constructor-time constant lookups may expose native members before
        // import; removal must still exercise the retained full-context owner.
        if (!constantCaller || !write)
            REQUIRE(current->isImportedInstanceBody(bodyOf(*current)));
        const auto& cq = bodyOf(*current).find("q")->template as<ast::ValueSymbol>();
        const auto& nq = bodyOf(*native).find("q")->template as<ast::ValueSymbol>();
        CHECK(current->isReferenced(*cq.getSyntax()) == native->isReferenced(*nq.getSyntax()));
        CHECK(native->isReferenced(*nq.getSyntax()).second == write);
        const auto& cs = bodyOf(*current).find("spare")->template as<ast::ValueSymbol>();
        const auto& ns = bodyOf(*native).find("spare")->template as<ast::ValueSymbol>();
        CHECK(current->isReferenced(*cs.getSyntax()) == native->isReferenced(*ns.getSyntax()));
        CHECK(native->isReferenced(*ns.getSyntax()).first == write);
        const auto* cf = bodyOf(*current).find("query");
        const auto* nf = bodyOf(*native).find("query");
        CHECK(current->isReferenced(*cf->getSyntax()) == native->isReferenced(*nf->getSyntax()));
        CHECK(native->isReferenced(*nf->getSyntax()).first == write);
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        analysis::AnalysisOptions options{.flags = analysis::AnalysisFlags::CheckUnused |
                                                   analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager am(options), nm(options);
        current->freeze();
        am.analyze(*current);
        current->unfreeze();
        native->freeze();
        nm.analyze(*native);
        native->unfreeze();
        CHECK(DiagnosticEngine::reportAll(driver->sm, am.getDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, nm.getDiagnostics()));
        CHECK(am.getDrivers(cq).size() == nm.getDrivers(nq).size());
        current->offerInstanceBodies(current);
    }
}

TEST_CASE("Mutable roots introducing a fixed package name preserve native declaration order",
          "[scope-semantic][scope-package-namespace]") {
    const bool lateCollision = GENERATE(false, true);
    const bool macroCollision = GENERATE(false, true);
    const std::string collisionName = GENERATE(std::string("stable"), std::string("dependency"));
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.packages = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    const std::string original = "package unrelated; int x; endpackage\n";
    const auto declaration = fmt::format("package {}; localparam int N=77; endpackage",
                                         collisionName);
    const std::string collision = macroCollision ? "`define COLLIDING_PACKAGE " + declaration +
                                                       "\n`COLLIDING_PACKAGE\n"
                                                 : declaration + "\n";
    files.write(files.root / "mutable.sv", original);
    files.write(files.root / "fixed.sv",
                "package dependency; localparam int N=7; endpackage\n"
                "package stable; localparam int N=dependency::N; endpackage\n");
    files.write(files.root / "consumer.sv", "module top; localparam int V=stable::N; endmodule\n");
    files.write(files.root / "design.f", lateCollision ? "fixed.sv\nmutable.sv\nconsumer.sv\n"
                                                       : "mutable.sv\nfixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto cached = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(cached);
    REQUIRE(cached->options.get<server::TrustedUvmImport>());
    const auto library = cached->options.get<server::TrustedUvmImport>()->library;
    CHECK_FALSE(library->files.contains((files.root / "mutable.sv").generic_string()));
    auto nativeConfig = config;
    nativeConfig.experimentalScopeSemanticCache = false;
    Indexer nativeIndexer;
    ClientHarness nativeClient;
    auto native = server::ServerDriver::createFromFileLists(nativeIndexer, nativeClient,
                                                            nativeConfig, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(native);
    auto uri = URI::fromFile(files.root / "mutable.sv");
    cached->openDocument(uri, original, 0, true);
    native->openDocument(uri, original, 0, true);
    for (int version = 0; version < 3; ++version) {
        if (version) {
            lsp::DidChangeTextDocumentParams change{
                .textDocument = {.version = version, .uri = uri},
                .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                    .text = version == 1 ? collision : original}}};
            cached->onDocDidChange(change);
            native->onDocDidChange(change);
        }
        const auto& actual = cached->comp->getInstancesOfModule("top").front()->body;
        const auto& expected = native->comp->getInstancesOfModule("top").front()->body;
        auto value = [](const auto& body) {
            return body.find("V")
                ->template as<ast::ParameterSymbol>()
                .getValue()
                .integer()
                .template as<int>();
        };
        CHECK(value(expected) == (version == 1 && !lateCollision ? 77 : 7));
        CHECK((actual.getCompilation().getPackage("stable") ==
               library->compilation->getPackage("stable")) == (version != 1));
        CHECK(value(actual) == value(expected));
        CHECK(DiagnosticEngine::reportAll(cached->sm,
                                          actual.getCompilation().getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(native->sm,
                                          expected.getCompilation().getSemanticDiagnostics()));
        // The unchanged provider also needs a private native view during a
        // collision, then its shared owner must become usable again on repair.
        const auto fixedUri = URI::fromFile(files.root / "fixed.sv");
        auto sourceActual = cached->getDocument(fixedUri)->getAnalysis()->getCompilation();
        auto sourceExpected = native->getDocument(fixedUri)->getAnalysis()->getCompilation();
        auto constant = [](const auto& compilation) {
            return compilation->getPackage("stable")
                ->find("N")
                ->template as<ast::ParameterSymbol>()
                .getValue();
        };
        CHECK(constant(sourceActual) == constant(sourceExpected));
        CHECK((sourceActual->getPackage("stable") == library->compilation->getPackage("stable")) ==
              (version != 1));
        CHECK(DiagnosticEngine::reportAll(cached->sm, sourceActual->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(native->sm, sourceExpected->getSemanticDiagnostics()));
        CHECK(cached->options.get<server::TrustedUvmImport>()->library == library);
        CHECK((library->namespaceDeclines.load() > 0) == (version > 0));
    }
}

TEST_CASE("Mutable duplicate module declarations suspend and restore frozen body imports",
          "[scope-semantic][scope-module-namespace]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv",
                "module stable(input int a, output int y); assign y=a; endmodule\n");
    auto source = [](bool duplicate) {
        return std::string(
                   duplicate
                       ? "module stable(input int a, output int y); assign y=a+77; endmodule\n"
                       : "") +
               "module top; int a,y; stable u(.*); endmodule\n";
    };
    files.write(files.root / "consumer.sv", source(false));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto cached = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(cached);
    auto library = cached->options.get<server::TrustedUvmImport>()->library;
    REQUIRE(library);
    auto cache = cached->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    CHECK_FALSE(library->files.contains((files.root / "consumer.sv").generic_string()));
    auto nativeConfig = config;
    nativeConfig.experimentalScopeSemanticCache = false;
    Indexer nativeIndexer;
    ClientHarness nativeClient;
    auto native = server::ServerDriver::createFromFileLists(nativeIndexer, nativeClient,
                                                            nativeConfig, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(native);
    auto uri = URI::fromFile(files.root / "consumer.sv");
    cached->openDocument(uri, source(false), 0, true);
    native->openDocument(uri, source(false), 0, true);
    for (int version = 1; version <= 3; ++version) {
        const bool duplicate = version == 2;
        lsp::DidChangeTextDocumentParams change{
            .textDocument = {.version = version, .uri = uri},
            .contentChanges = {
                lsp::TextDocumentContentChangeWholeDocument{.text = source(duplicate)}}};
        cached->onDocDidChange(change);
        native->onDocDidChange(change);
        const auto beforeHits = cache->status().hits;
        const auto& actual = cached->comp->getInstancesOfModule("top").front()->body;
        const auto& expected = native->comp->getInstancesOfModule("top").front()->body;
        const auto& child = actual.find("u")->as<ast::InstanceSymbol>().body;
        CHECK(actual.getCompilation().isImportedInstanceBody(child) == !duplicate);
        CHECK((cache->status().hits > beforeHits) == !duplicate);
        CHECK(DiagnosticEngine::reportAll(cached->sm,
                                          actual.getCompilation().getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(native->sm,
                                          expected.getCompilation().getSemanticDiagnostics()));
        CHECK(cached->options.get<server::TrustedUvmImport>()->library == library);
    }
    CHECK(library->namespaceDeclines.load() > 0);
}

TEST_CASE("Exploration fixed-source views recover from mutable namespace ambiguity",
          "[scope-semantic][scope-namespace-explore]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.compilationFilelist = "design.f";
    config.frozenSources =
        Config::FrozenSourcesConfig{.packages = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1"}};
    const std::string original = "package unrelated; int x; endpackage\n";
    const std::string collision = "package dependency; localparam int N=77; endpackage\n";
    files.write(files.root / "mutable.sv", original);
    files.write(files.root / "fixed.sv",
                "package dependency; localparam int N=7; endpackage\n"
                "package stable; localparam int N=dependency::N; endpackage\n");
    files.write(files.root / "consumer.sv", "module top; localparam int V=stable::N; endmodule\n");
    files.write(files.root / "design.f", "mutable.sv\nfixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto cached = server::ServerDriver::createForExplore(indexer, client, config,
                                                         files.root.string(), nullptr, true);
    REQUIRE(cached);
    const auto library = cached->options.get<server::TrustedUvmImport>()->library;
    REQUIRE(library);
    auto nativeConfig = config;
    nativeConfig.experimentalScopeSemanticCache = false;
    Indexer nativeIndexer;
    ClientHarness nativeClient;
    auto native = server::ServerDriver::createForExplore(nativeIndexer, nativeClient, nativeConfig,
                                                         files.root.string(), nullptr, true);
    REQUIRE(native);
    auto uri = URI::fromFile(files.root / "mutable.sv");
    auto provider = URI::fromFile(files.root / "fixed.sv");
    cached->openDocument(uri, original, 0, true);
    native->openDocument(uri, original, 0, true);
    for (int version = 0; version < 3; ++version) {
        if (version) {
            lsp::DidChangeTextDocumentParams change{
                .textDocument = {.version = version, .uri = uri},
                .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                    .text = version == 1 ? collision : original}}};
            cached->onDocDidChange(change);
            native->onDocDidChange(change);
        }
        if (version == 1) {
            CHECK_THROWS_AS(cached->getDocument(provider)->getAnalysis(), lsp::RequestFailed);
            CHECK_THROWS_AS(native->getDocument(provider)->getAnalysis(), lsp::RequestFailed);
        }
        else {
            auto actual = cached->getDocument(provider)->getAnalysis()->getCompilation();
            auto expected = native->getDocument(provider)->getAnalysis()->getCompilation();
            CHECK(actual->getPackage("stable") == library->compilation->getPackage("stable"));
            CHECK(actual->getPackage("stable")->find("N")->as<ast::ParameterSymbol>().getValue() ==
                  expected->getPackage("stable")->find("N")->as<ast::ParameterSymbol>().getValue());
            CHECK_FALSE(cached->hasExplicitContextError(provider));
        }
    }
}

TEST_CASE("Oversized body admission remembers failures without retaining editing generations",
          "[scope-semantic][scope-admission-budget]") {
    const bool children = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv",
                "module leaf(input int a, output int y); assign y=a; endmodule\n"
                "module stable #(parameter int N=256)(input int a, output int y);\n"
                "wire [N-1:0] q;\n" +
                    std::string(
                        children ? "for(genvar i=0;i<N;i++) begin:g wire [31:0] v; leaf "
                                   "child(.a(a),.y(v)); assign q[i]=v[i%32]; end\n"
                                 : "for(genvar i=0;i<N;i++) begin:g assign q[i]=a[i%32]; end\n") +
                    "assign y=32'(q); endmodule\n");
    auto source = [](int n, int edit) {
        return fmt::format("module top(input int a, output int y); localparam int EDIT={}; "
                           "stable #(.N({})) u(.*); endmodule\n",
                           edit, n);
    };
    files.write(files.root / "consumer.sv", source(1, 0));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto library = driver->options.get<server::TrustedUvmImport>()->library;
    auto newCache = [&](size_t budget) {
        return std::make_shared<server::ScopeBodyCache>(library, budget, std::vector<std::string>{},
                                                        std::vector<std::string>{"top.u"});
    };
    auto make = [&](const auto& cache, int n, int edit, bool native = false) {
        auto options = driver->options;
        options.set(server::ScopeBodyImport{cache});
        auto compilation = std::make_shared<ast::Compilation>(options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, options));
        if (native)
            compilation->setInstanceBodyReuseHooks({}, {});
        compilation->addSyntaxTree(syntax::SyntaxTree::fromText(
            source(n, edit), driver->sm, "admission-consumer.sv", "", options));
        (void)compilation->getSemanticDiagnostics();
        return compilation;
    };
    auto probe = newCache(512 * 1024 * 1024);
    auto small = make(probe, 1, 0);
    small->offerInstanceBodies(small);
    REQUIRE(probe->status().publications == 1);
    const auto limit = library->semanticBytes + 2 * probe->status().ownerBytes;
    auto cache = newCache(limit);
    uint64_t firstBuilds = 0;
    for (int edit = 0; edit < 3; ++edit) {
        auto current = make(cache, 256, edit), native = make(cache, 256, edit, true);
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        std::weak_ptr<ast::Compilation> oldCompilation = current;
        std::weak_ptr<syntax::SyntaxTree> oldSyntax = current->getSyntaxTrees().front();
        current->offerInstanceBodies(current);
        if (edit == 0)
            firstBuilds = cache->status().ownerBuilds;
        else
            CHECK(cache->status().ownerBuilds == firstBuilds);
        if (children)
            CHECK(cache->status().publications > 0);
        else
            CHECK(cache->status().owners == 0);
        current.reset();
        native.reset();
        CHECK(oldCompilation.expired());
        CHECK(oldSyntax.expired());
        const auto status = cache->status();
        CHECK(status.baseBytes + status.ownerBytes + status.pinnedBytes + status.admissionBytes <=
              limit);
    }
    CHECK(cache->status().admissionHits > 0);
    const auto beforeChanged = cache->status().ownerBuilds;
    auto changed = make(cache, 1, 4);
    changed->offerInstanceBodies(changed);
    CHECK(cache->status().publications > 0);
    // Admission advice must allow a distinct smaller variant to be built.
    // Complete source accounting can still reject publication at this tight
    // budget when the changed consumer pins previously imported descendants.
    CHECK(cache->status().ownerBuilds > beforeChanged);
    changed.reset();
    // After the consumer releases those pins, the same registry must admit and
    // reuse the smaller variant without increasing its budget or clearing advice.
    auto retry = make(cache, 1, 5);
    retry->offerInstanceBodies(retry);
    retry.reset();
    auto repeated = make(cache, 1, 6);
    CHECK(repeated->hasImportedInstanceBodies());
    auto repeatedNative = make(cache, 1, 6, true);
    CHECK(DiagnosticEngine::reportAll(driver->sm, repeated->getSemanticDiagnostics()) ==
          DiagnosticEngine::reportAll(driver->sm, repeatedNative->getSemanticDiagnostics()));
    const auto finalStatus = cache->status();
    CHECK(finalStatus.baseBytes + finalStatus.ownerBytes + finalStatus.pinnedBytes +
              finalStatus.admissionBytes <=
          limit);
    if (!children) {
        // A new registry (budget change / explicit rebuild) does not inherit
        // admission advice from the previous one.
        auto rebuilt = newCache(512 * 1024 * 1024);
        CHECK(rebuilt->status().admissionRecords == 0);
        auto large = make(rebuilt, 256, 6);
        large->offerInstanceBodies(large);
        CHECK(rebuilt->status().publications == 1);
        CHECK(rebuilt->status().admissionRecords == 0);
        // Many genuine rejected parameter variants remain bounded and charged.
        auto bounded = newCache(library->semanticBytes + 1024 * 1024);
        for (int variant = 0; variant < 40; ++variant) {
            auto candidate = make(bounded, 8192 + variant, variant);
            candidate->offerInstanceBodies(candidate);
            const auto status = bounded->status();
            CHECK(status.owners == 0);
            CHECK(status.admissionRecords <= 32);
            CHECK(status.admissionBytes <= 64 * 1024);
            CHECK(status.baseBytes + status.ownerBytes + status.pinnedBytes +
                      status.admissionBytes <=
                  status.budgetBytes);
        }
        CHECK(bounded->status().admissionRecords == 32);
    }
}

TEST_CASE("Fixed module source completion keeps the native physical source import context",
          "[scope-semantic][scope-source-import-context]") {
    struct CompletionProbe : server::completions::MemberCompletionQuery {
        using MemberCompletionQuery::addCompletions;
    };
    const bool includedImport = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "package.sv", "package p; localparam int EarlierFile=7; endpackage\n");
    files.write(files.root / "imports.svh", "import p::*;\n");
    files.write(files.root / "imports.sv", "import p::*;\n");
    files.write(files.root / "fixed.sv",
                std::string(includedImport ? "`include \"imports.svh\"\n" : "") +
                    "module stable; logic marker; endmodule\n");
    files.write(files.root / "consumer.sv", "module top; stable u(); endmodule\n");
    files.write(files.root / "design.f", includedImport
                                             ? "package.sv\nfixed.sv\nconsumer.sv\n"
                                             : "package.sv\nimports.sv\nfixed.sv\nconsumer.sv\n");
    Indexer indexer;
    indexer.addDocuments({files.root / "package.sv"});
    ClientHarness client;
    auto cached = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(cached);
    REQUIRE(cached->options.get<server::TrustedUvmImport>());
    auto nativeConfig = config;
    nativeConfig.experimentalScopeSemanticCache = false;
    Indexer nativeIndexer;
    nativeIndexer.addDocuments({files.root / "package.sv"});
    ClientHarness nativeClient;
    auto native = server::ServerDriver::createFromFileLists(nativeIndexer, nativeClient,
                                                            nativeConfig, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(native);
    auto labels = [&](server::ServerDriver& driver) {
        const auto uri = URI::fromFile(files.root / "fixed.sv");
        auto compilation = driver.getDocument(uri)->getAnalysis()->getCompilation();
        const ast::InstanceBodySymbol* body = nullptr;
        for (auto instance : compilation->getRoot().topInstances) {
            if (instance->name == "stable")
                body = &instance->body;
            if (instance->name == "top")
                body = &instance->body.find("u")->as<ast::InstanceSymbol>().body;
        }
        REQUIRE(body);
        std::vector<lsp::CompletionItem> items;
        CompletionProbe::addCompletions(items, body, server::CompletionContextKind::Expression,
                                        body, uri.str());
        std::vector<std::string> result;
        for (const auto& item : items)
            result.push_back(item.label);
        return result;
    };
    const auto expected = labels(*native), actual = labels(*cached);
    CHECK(std::ranges::find(expected, "marker") != expected.end());
    CHECK((std::ranges::find(expected, "EarlierFile") != expected.end()) == includedImport);
    CHECK(actual == expected);
}

TEST_CASE("A newly closed parent promotes from borrowed children through a fresh native owner",
          "[scope-semantic][scope-parent-promotion]") {
    // The negative cases put an unsupported call or an outside reference after
    // the borrowed child. A mixed graph is never an independence certificate.
    const auto boundary = GENERATE(0, 1, 2);
    CAPTURE(boundary);
    const bool canPromote = boundary == 0;
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    std::string fixed = R"(
module leaf(input int a, output int y); assign y=a; endmodule
module stable #(parameter bit EXTERNAL=1)(input int a, output int y);
 wire [31:0] local_y;
 leaf child(.a(a),.y(local_y));
 if(EXTERNAL) assign y=local_y+$root.top.outside;
 else assign y=local_y;
endmodule
)";
    if (boundary == 1)
        fixed.insert(
            fixed.rfind("endmodule"),
            "if(!EXTERNAL) begin:g int seed=1; initial $display(\"%0d\",$urandom(seed)); end\n");
    if (boundary == 2)
        fixed.replace(fixed.find("else assign y=local_y;"),
                      std::string("else assign y=local_y;").size(),
                      "else assign y=local_y+$root.top.outside;");
    files.write(files.root / "fixed.sv", fixed);
    auto source = [](bool external) {
        return fmt::format("module top(input int a, output int y); int outside; "
                           "stable #(.EXTERNAL({})) u(.*); endmodule\n",
                           external ? 1 : 0);
    };
    files.write(files.root / "consumer.sv", source(true));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cache = driver->options.get<server::ScopeBodyImport>()->cache.lock();
    REQUIRE(cache);
    REQUIRE(cache->status().publications == 1);
    auto make = [&](bool external, bool native = false) {
        auto result = std::make_shared<ast::Compilation>(driver->options);
        REQUIRE(server::importTrustedUvm(*result, driver->sm, driver->options));
        if (native)
            result->setInstanceBodyReuseHooks({}, {});
        result->addSyntaxTree(syntax::SyntaxTree::fromText(
            source(external), driver->sm, "promotion-consumer.sv", "", driver->options));
        (void)result->getSemanticDiagnostics();
        return result;
    };
    auto root = [](auto& comp) -> const ast::InstanceBodySymbol& {
        return comp.getRoot()
            .topInstances.front()
            ->body.find("u")
            ->template as<ast::InstanceSymbol>()
            .body;
    };
    auto mixed = make(false);
    CHECK_FALSE(mixed->isImportedInstanceBody(root(*mixed)));
    CHECK(
        mixed->isImportedInstanceBody(root(*mixed).find("child")->as<ast::InstanceSymbol>().body));
    std::weak_ptr<ast::Compilation> old = mixed;
    std::weak_ptr<syntax::SyntaxTree> oldSyntax = mixed->getSyntaxTrees().front();
    mixed->offerInstanceBodies(mixed);
    CHECK(cache->status().publications == (canPromote ? 2 : 1));
    const auto buildsAfterAttempt = cache->status().ownerBuilds;
    if (boundary == 1)
        CHECK(cache->status().admissionRecords == 1);
    mixed.reset();
    CHECK(old.expired());
    CHECK(oldSyntax.expired());
    for (bool external : {false, true, false}) {
        auto current = make(external), native = make(external, true);
        CHECK(current->isImportedInstanceBody(root(*current)) == (!external && canPromote));
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        analysis::AnalysisOptions opts{.flags = analysis::AnalysisFlags::CheckUnused |
                                                analysis::AnalysisFlags::CheckShadow};
        analysis::AnalysisManager actualAnalysis(opts), nativeAnalysis(opts);
        current->freeze();
        actualAnalysis.analyze(*current);
        current->unfreeze();
        native->freeze();
        nativeAnalysis.analyze(*native);
        native->unfreeze();
        CHECK(DiagnosticEngine::reportAll(driver->sm, actualAnalysis.getDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, nativeAnalysis.getDiagnostics()));
        current->offerInstanceBodies(current);
        CHECK(cache->status().ownerBuilds == buildsAfterAttempt);
        const auto usage = cache->status();
        CHECK(usage.baseBytes + usage.ownerBytes + usage.pinnedBytes + usage.admissionBytes <=
              usage.budgetBytes);
    }
}

TEST_CASE("Mutable compilation-unit bindings suspend and restore fixed definition imports",
          "[scope-semantic][scope-unit-namespace]") {
    const auto kind = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    CAPTURE(kind);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources = Config::FrozenSourcesConfig{
        .instances = std::vector<std::string>{"top.u"}};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv",
                std::string("package fixed_pkg; localparam int Z=1; endpackage\nmodule "
                            "stable(output wire [31:0] y); ") +
                    (kind == 0 ? ""
                               : "function automatic int helper(int a);return a+7;endfunction ") +
                    "localparam int N=helper(5); assign y=N; endmodule\n");
    auto source = [=](int value) {
        std::string declaration;
        if (value) {
            switch (kind) {
                case 0:
                    declaration = fmt::format(
                        "function automatic int helper(int a);return a+{};endfunction\n", value);
                    break;
                case 1:
                    declaration = "task automatic user_task;endtask\n";
                    break;
                case 2:
                    declaration = "typedef int user_t;\n";
                    break;
                case 3:
                    declaration = "int user_value;\n";
                    break;
                case 4:
                    declaration = "class User;endclass\n";
                    break;
                case 5:
                    declaration = "import fixed_pkg::*;\n";
                    break;
                case 6:
                    declaration = "import \"DPI-C\" function int foreign_dpi(int x);\n";
                    break;
                case 7:
                    declaration = "import fixed_pkg::Z;\n";
                    break;
            }
        }
        return declaration + "module top; wire [31:0] y; stable u(.*); endmodule\n";
    };
    files.write(files.root / "consumer.sv", source(7));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto cached = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(cached);
    auto library = cached->options.get<server::TrustedUvmImport>()->library;
    REQUIRE(library);
    auto nativeConfig = config;
    nativeConfig.experimentalScopeSemanticCache = false;
    Indexer nativeIndexer;
    ClientHarness nativeClient;
    auto native = server::ServerDriver::createFromFileLists(nativeIndexer, nativeClient,
                                                            nativeConfig, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(native);
    auto uri = URI::fromFile(files.root / "consumer.sv");
    cached->openDocument(uri, source(7), 0, true);
    native->openDocument(uri, source(7), 0, true);
    for (int version = 0; version < 4; version++) {
        const int value = version == 1 ? 8 : (version == 2 ? 0 : 7);
        if (version) {
            lsp::DidChangeTextDocumentParams change{
                .textDocument = {.version = version, .uri = uri},
                .contentChanges = {
                    lsp::TextDocumentContentChangeWholeDocument{.text = source(value)}}};
            cached->onDocDidChange(change);
            native->onDocDidChange(change);
        }
        const auto& actual = cached->comp->getInstancesOfModule("top")
                                 .front()
                                 ->body.find("u")
                                 ->as<ast::InstanceSymbol>()
                                 .body;
        const auto& expected = native->comp->getInstancesOfModule("top")
                                   .front()
                                   ->body.find("u")
                                   ->as<ast::InstanceSymbol>()
                                   .body;
        auto n = [](const auto& body) {
            return body.find("N")->template as<ast::ParameterSymbol>().getValue();
        };
        CHECK(n(actual) == n(expected));
        if (kind != 0 || value)
            CHECK(n(expected).integer().template as<int>() == (kind == 0 ? 5 + value : 12));
        CHECK(DiagnosticEngine::reportAll(cached->sm,
                                          actual.getCompilation().getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(native->sm,
                                          expected.getCompilation().getSemanticDiagnostics()));
        const auto fixed = URI::fromFile(files.root / "fixed.sv");
        auto sourceActual = cached->getDocument(fixed)->getAnalysis()->getCompilation();
        auto sourceExpected = native->getDocument(fixed)->getAnalysis()->getCompilation();
        CHECK(DiagnosticEngine::reportAll(cached->sm, sourceActual->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(native->sm, sourceExpected->getSemanticDiagnostics()));
        CHECK(cached->hasCompatibleTrustedNamespace() == (!value || kind >= 5));
        CHECK(cached->options.get<server::TrustedUvmImport>()->library == library);
    }
    CHECK((library->namespaceDeclines.load() > 0) == (kind < 5));
}

TEST_CASE("Package generic methods keep current user types through fixed import aliases",
          "[scope-semantic][scope-generic-imports]") {
    const bool wildcard = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources = Config::FrozenSourcesConfig{
        .packages = std::vector<std::string>{"stable"}};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "leaf.sv",
                "package leaf; typedef enum logic[1:0] {Ready,Busy} state_t; function int plus(int "
                "x);return x+1;endfunction endpackage\n");
    files.write(
        files.root / "box.svh",
        "class Box#(type T); T value; state_t state=Ready; extern function int get_value(); "
        "endclass\nfunction int Box::get_value();return plus(value.field);endfunction\n");
    files.write(files.root / "stable.sv",
                std::string("package stable; ") +
                    (wildcard ? "import leaf::*;"
                              : "import leaf::state_t; import leaf::Ready; import leaf::plus;") +
                    "\n`include \"box.svh\"\nendpackage\n");
    auto source = [](int width, bool broken) {
        return fmt::format(
            "package user_pkg; class Packet; logic [{}:0] {}; endclass endpackage\n"
            "module top; stable::Box#(user_pkg::Packet) b; "
            "initial begin b=new(); b.value=new(); $display(b.get_value()); end endmodule\n",
            width - 1, broken ? "other" : "field");
    };
    files.write(files.root / "consumer.sv", source(8, false));
    files.write(files.root / "design.f", "leaf.sv\nstable.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto cached = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(cached);
    auto library = cached->options.get<server::TrustedUvmImport>()->library;
    REQUIRE(library);
    auto package = library->compilation->getPackage("stable");
    auto& generic = package->find("Box")->as<ast::GenericClassDefSymbol>();
    const auto specializations = generic.numSpecializations();
    const auto bytes = library->compilation->getTotalBytesAllocated();
    auto fixedType = &library->compilation->getPackage("leaf")
                          ->find("state_t")
                          ->as<ast::Type>()
                          .getCanonicalType();
    auto nativeConfig = config;
    nativeConfig.experimentalScopeSemanticCache = false;
    Indexer nativeIndexer;
    ClientHarness nativeClient;
    auto native = server::ServerDriver::createFromFileLists(nativeIndexer, nativeClient,
                                                            nativeConfig, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(native);
    const auto uri = URI::fromFile(files.root / "consumer.sv");
    cached->openDocument(uri, source(8, false), 0, true);
    native->openDocument(uri, source(8, false), 0, true);
    for (int version = 0; version < 24; version++) {
        const bool broken = version % 4 == 2;
        const int width = version % 4 == 1 ? 64 : (broken ? 16 : 8);
        lsp::DidChangeTextDocumentParams change{
            .textDocument = {.version = version + 1, .uri = uri},
            .contentChanges = {
                lsp::TextDocumentContentChangeWholeDocument{.text = source(width, broken)}}};
        cached->onDocDidChange(change);
        native->onDocDidChange(change);
        const auto& body = cached->comp->getInstancesOfModule("top").front()->body;
        auto& actual = body.getCompilation();
        auto& expected = native->comp->getInstancesOfModule("top").front()->body.getCompilation();
        const auto& diagnostics = actual.getSemanticDiagnostics();
        CHECK(std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }) ==
              broken);
        CHECK(DiagnosticEngine::reportAll(cached->sm, diagnostics) ==
              DiagnosticEngine::reportAll(native->sm, expected.getSemanticDiagnostics()));
        const auto& box = body.find("b")
                              ->as<ast::ValueSymbol>()
                              .getType()
                              .getCanonicalType()
                              .as<ast::ClassType>();
        CHECK(&box.getCompilation() == &actual);
        const auto& packet = box.find("value")
                                 ->as<ast::ValueSymbol>()
                                 .getType()
                                 .getCanonicalType()
                                 .as<ast::ClassType>();
        REQUIRE(packet.find(broken ? "other" : "field"));
        CHECK(packet.find(broken ? "other" : "field")
                  ->as<ast::ValueSymbol>()
                  .getType()
                  .getBitWidth() == unsigned(width));
        CHECK(&box.find("state")->as<ast::ValueSymbol>().getType().getCanonicalType() == fixedType);
        CHECK(actual.getPackage("stable") == package);
        CHECK(generic.numSpecializations() == specializations);
        CHECK(library->compilation->getTotalBytesAllocated() == bytes);
    }
}

TEST_CASE("Fixed compilation-unit namespaces retain class and helper identity across edits",
          "[scope-semantic][scope-unit-declarations]") {
    const bool byPackage = GENERATE(false, true);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources = Config::FrozenSourcesConfig{.maxMiB = 64};
    if (byPackage)
        config.frozenSources.value()->packages = std::vector<std::string>{"fixed_imports"};
    else
        config.frozenSources.value()->instances = std::vector<std::string>{"top.u"};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
package fixed_imports;
  typedef logic [3:0] imported_word_t;
  typedef int imported_int_t;
endpackage
import fixed_imports::*;
import fixed_imports::*;
import fixed_imports::imported_int_t;
typedef struct packed { logic [7:0] bits; } word_t;
localparam int OFFSET=3;
int fixed_global;
function automatic int bump(input int x); return x+OFFSET; endfunction
class Base;
  word_t value;
  function int read(); return bump(value.bits); endfunction
endclass
class Trace #(type T=int) extends Base;
  T item;
  extern function int get();
endclass
function int Trace::get(); return bump(item); endfunction
module leaf(input logic a, output wire y); assign y=a; endmodule
module stable(input logic a, output wire y);
  Base base; Trace #(int) trace;
  initial begin trace=new; base=trace; end
  leaf child(a,y);
endmodule
)");
    auto source = [](int value, bool error) {
        return fmt::format(
            "module top; logic a,y; stable u(a,y); imported_word_t w; imported_int_t v; Base base; "
            "Trace#(byte) trace; localparam int V=bump({}); "
            "initial begin trace=new; base=trace; {} end endmodule\n",
            value, error ? "missing_user_symbol=1;" : "fixed_global=V;");
    };
    files.write(files.root / "consumer.sv", source(0, false));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    INFO(rfl::json::write(driver->scopeLibraryStatus()));
    auto setting = driver->options.get<server::TrustedUvmImport>();
    REQUIRE(setting);
    auto library = setting->library;
    REQUIRE(library->hasUnitDeclarations);
    auto unit = library->compilation->getCompilationUnits().front();
    auto base = unit->find("Base");
    auto generic = unit->find("Trace");
    REQUIRE(base);
    REQUIRE(generic);
    const auto specializations = generic->as<ast::GenericClassDefSymbol>().numSpecializations();
    const auto ownerBytes = library->compilation->getTotalBytesAllocated();
    const auto uri = URI::fromFile(files.root / "consumer.sv");
    for (int version = 1; version <= 12; ++version) {
        const bool error = version == 5;
        if (version == 1)
            driver->openDocument(uri, source(version, error), version, true);
        else
            driver->onDocDidChange({.textDocument = {.version = version, .uri = uri},
                                    .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                                        .text = source(version, error)}}});
        auto compilation = driver->getDocument(uri)->getAnalysis()->getCompilation();
        const auto& diagnostics = compilation->getSemanticDiagnostics();
        INFO("version=" << version);
        INFO(DiagnosticEngine::reportAll(driver->sm, diagnostics));
        CHECK(std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); }) == error);
        bool found = false;
        for (auto currentUnit : compilation->getCompilationUnits()) {
            if (auto symbol = currentUnit->find("Base")) {
                CHECK(symbol == base);
                CHECK(currentUnit->find("Trace") == generic);
                CHECK(currentUnit->find("word_t") == unit->find("word_t"));
                CHECK(currentUnit->find("bump") == unit->find("bump"));
                found = true;
            }
        }
        CHECK(found);
        CHECK(generic->as<ast::GenericClassDefSymbol>().numSpecializations() == specializations);
        CHECK(library->compilation->getTotalBytesAllocated() == ownerBytes);
        CHECK(driver->options.get<server::TrustedUvmImport>()->library == library);
        auto nativeConfig = config;
        nativeConfig.experimentalScopeSemanticCache = false;
        Indexer nativeIndexer;
        ClientHarness nativeClient;
        auto native = server::ServerDriver::createFromFileLists(nativeIndexer, nativeClient,
                                                                nativeConfig, {"design.f"},
                                                                files.root.string(), nullptr, true);
        REQUIRE(native);
        native->openDocument(uri, source(version, error), version, true);
        auto& expectedCompilation =
            native->comp->getInstancesOfModule("top").front()->body.getCompilation();
        CHECK(
            DiagnosticEngine::reportAll(driver->sm, diagnostics) ==
            DiagnosticEngine::reportAll(native->sm, expectedCompilation.getSemanticDiagnostics()));
    }
}

TEST_CASE("Independent bodies retain inherited bind flags beside mutable errors",
          "[scope-semantic][scope-error-bind]") {
    const int scenario = GENERATE(0, 1, 2);
    const bool fixedError = scenario == 1;
    const bool insideBind = scenario == 2;
    CAPTURE(scenario);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(
        files.root / "fixed.sv",
        fixedError
            ? "module stable #(parameter int W=0); if(W) initial missing_fixed=1; endmodule\n"
            : "module stable(input logic a, output wire y); assign y=a; endmodule\n");
    std::string source = fixedError
                             ? "module monitor; endmodule bind top monitor observer(); module top; "
                               "stable #(.W(1)) u(); endmodule\n"
                             : "module monitor; endmodule bind top monitor observer(); module top; "
                               "logic a,y; stable u(a,y); initial missing_user=1; endmodule\n";
    if (insideBind)
        source.replace(source.find("bind top"), 8, "bind stable");
    files.write(files.root / "consumer.sv", source);
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto cacheSetting = driver->options.get<server::ScopeBodyImport>();
    if (insideBind) {
        // The selected dependency closure includes the bound monitor's mutable
        // provider file and declines the library before body admission.
        CHECK_FALSE(cacheSetting);
        CHECK_FALSE(driver->options.get<server::TrustedUvmImport>());
        REQUIRE(driver->comp->getInstancesOfModule("stable").front()->body.find("observer"));
        return;
    }
    REQUIRE(cacheSetting);
    auto cache = cacheSetting->cache.lock();
    REQUIRE(cache);
    auto make = [&](bool native) {
        auto compilation = std::make_shared<ast::Compilation>(driver->options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, driver->options));
        if (native)
            compilation->setInstanceBodyReuseHooks({}, {});
        compilation->addSyntaxTree(syntax::SyntaxTree::fromText(
            source, driver->sm, "error-consumer.sv", "", driver->options));
        (void)compilation->getSemanticDiagnostics();
        return compilation;
    };
    for (int edit = 0; edit < 4; ++edit) {
        auto current = make(false);
        auto native = make(true);
        REQUIRE(native->hasIssuedErrors());
        CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
        const auto& body =
            current->getRoot().topInstances.front()->body.find("u")->as<ast::InstanceSymbol>().body;
        CHECK(body.flags.has(ast::InstanceFlags::TargetedByBind));
        if (edit)
            CHECK(current->isImportedInstanceBody(body) == (!fixedError && !insideBind));
        current->offerInstanceBodies(current);
    }
    CHECK((cache->status().hits > 0) == (!fixedError && !insideBind));
}

TEST_CASE("Explicit unit lookups cannot be hidden by package-local names",
          "[scope-semantic][scope-unit-shadow]") {
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.modules = std::vector<std::string>{"stable"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", R"(
package api_pkg;
  function automatic int helper(int x); return x+1; endfunction
endpackage
import api_pkg::*;
package fixed_pkg;
  function automatic int helper(int x); return x+2; endfunction
  localparam int N=$unit::helper(3);
endpackage
module stable(output wire [31:0] y); assign y=fixed_pkg::N; endmodule
)");
    files.write(files.root / "consumer.sv", "import \"DPI-C\" function int helper(int x);\n"
                                            "module top; wire [31:0] y; stable u(y); endmodule\n");
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto setting = driver->options.get<server::TrustedUvmImport>();
    REQUIRE(setting);
    auto library = setting->library;
    REQUIRE(library->hasUnitDeclarations);
    auto fixedValue = library->compilation->getPackage("fixed_pkg")->find("N");
    REQUIRE(fixedValue);
    CHECK(fixedValue->as<ast::ParameterSymbol>().getValue().integer().as<int>() == 4);
    CHECK_FALSE(driver->hasCompatibleTrustedNamespace());
}

TEST_CASE("Closed ancestor type arguments retain nominal identity without old consumers",
          "[scope-semantic][scope-type-closure]") {
    const bool enumeration = GENERATE(false, true);
    INFO(enumeration);
    const bool trace = GENERATE(false, true);
    INFO(trace);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources =
        Config::FrozenSourcesConfig{.instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    auto provider = std::string(R"(
module stable #(parameter type T=logic)(input T a, output T y);
 localparam int SIZE=$bits(T);
 assign y=a;
)");
    if (trace) {
        provider = R"(
class frozen_packet #(parameter type P=logic, parameter P MASK=P'('0));
 P data;
 function new(P value); data=value; endfunction
 function P read(); return data; endfunction
endclass
)" + provider;
        provider += R"(
 T pending[$];
 task automatic record();
   frozen_packet #(.P(T)) item;
   pending.push_back(a);
   item=new(pending.pop_front());
   if(pending.size()) pending.delete();
   $display("%0t %h", $time, item.read());
 endtask
 initial record();
)";
    }
    files.write(files.root / "fixed.sv", provider + "endmodule\n");
    auto source = [&](int edit, int width = 8, bool alternate = false) {
        auto first =
            enumeration
                ? "typedef enum logic[7:0] {FIRST=1, SECOND=2} first_t;"
                : fmt::format(
                      "typedef struct packed {{logic [{}:0] payload; logic valid;}} first_t;",
                      width - 1);
        auto second =
            enumeration
                ? "typedef enum logic[7:0] {OTHER_FIRST=1, OTHER_SECOND=2} second_t;"
                : fmt::format(
                      "typedef struct packed {{logic [{}:0] payload; logic valid;}} second_t;",
                      width - 1);
        return fmt::format("module top; {} {} localparam int EDIT={}; {} a,y; "
                           "stable #(.T({})) u(.*); endmodule\n",
                           first, second, edit, alternate ? "second_t" : "first_t",
                           alternate ? "second_t" : "first_t");
    };
    files.write(files.root / "consumer.sv", source(0));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto library = driver->options.get<server::TrustedUvmImport>()->library;
    auto cache = std::make_shared<server::ScopeBodyCache>(library, 64 * 1024 * 1024,
                                                          std::vector<std::string>{},
                                                          std::vector<std::string>{"top.u"});
    std::string extraSource;
    bool shiftLine = false;
    bool shiftMember = false;
    auto make = [&](int edit, int width = 8, bool alternate = false, bool native = false) {
        auto options = driver->options;
        options.set(server::ScopeBodyImport{cache});
        auto result = std::make_shared<ast::Compilation>(options);
        REQUIRE(server::importTrustedUvm(*result, driver->sm, options));
        if (native)
            result->setInstanceBodyReuseHooks({}, {});
        auto oldBuffer = driver->sm.readSource(files.root / "consumer.sv").value();
        auto text = source(edit, width, alternate) + extraSource;
        if (shiftLine)
            text.replace(text.find("module top;"), 11, "module\ntop;");
        if (shiftMember)
            text.replace(text.find(" payload;"), 9, "\npayload;");
        SmallVector<char> bytes;
        bytes.insert(bytes.end(), text.begin(), text.end());
        bytes.push_back('\0');
        auto buffer = driver->sm.replaceBuffer(oldBuffer.id, std::move(bytes));
        result->addSyntaxTree(syntax::SyntaxTree::fromBuffer(buffer, driver->sm, options));
        (void)result->getSemanticDiagnostics();
        return result;
    };
    auto first = make(0);
    first->offerInstanceBodies(first);
    INFO(rfl::json::write(cache->status()));
    REQUIRE(cache->status().owners == 1);
    std::weak_ptr<ast::Compilation> oldCompilation = first;
    std::weak_ptr<syntax::SyntaxTree> oldSyntax = first->getSyntaxTrees().front();
    first.reset();
    CHECK(oldCompilation.expired());
    CHECK(oldSyntax.expired());
    auto repeated = make(1), native = make(1, 8, false, true);
    CHECK(repeated->hasImportedInstanceBodies());
    auto argument = [](ast::Compilation& compilation) -> const ast::Type& {
        const auto& top = compilation.getRoot().topInstances.front()->body;
        const auto& instance = top.find("u")->as<ast::InstanceSymbol>();
        return instance.body.find("T")->as<ast::TypeParameterSymbol>().targetType.getType();
    };
    for (unsigned flags = 0; flags < 16; ++flags) {
        ast::TypePrinter actual, expected;
        actual.options.printAKA = expected.options.printAKA = flags & 1;
        actual.options.skipTypeDefs = expected.options.skipTypeDefs = flags & 2;
        actual.options.skipScopedTypeNames = expected.options.skipScopedTypeNames = flags & 4;
        actual.options.anonymousTypeStyle = expected.options.anonymousTypeStyle =
            flags & 8 ? ast::TypePrintingOptions::FriendlyName
                      : ast::TypePrintingOptions::SystemName;
        actual.append(argument(*repeated));
        expected.append(argument(*native));
        CAPTURE(flags);
        CHECK(actual.toString() == expected.toString());
    }
    CHECK(DiagnosticEngine::reportAll(driver->sm, repeated->getSemanticDiagnostics()) ==
          DiagnosticEngine::reportAll(driver->sm, native->getSemanticDiagnostics()));
    auto sourcePosition = [&](const ast::Symbol& symbol) {
        auto location = driver->sm.getFullyOriginalLoc(symbol.location);
        return fmt::format("{}:{}:{}", driver->sm.getFullPath(location.buffer()).string(),
                           driver->sm.getLineNumber(location),
                           driver->sm.getColumnNumber(location));
    };
    const auto& actualType = argument(*repeated).getCanonicalType();
    const auto& expectedType = argument(*native).getCanonicalType();
    CHECK(sourcePosition(actualType) == sourcePosition(expectedType));
    if (!enumeration) {
        const auto& actual = actualType.as<ast::PackedStructType>();
        const auto& expected = expectedType.as<ast::PackedStructType>();
        REQUIRE(actual.find("payload"));
        REQUIRE(expected.find("payload"));
        CHECK(sourcePosition(*actual.find("payload")) == sourcePosition(*expected.find("payload")));
        REQUIRE(actual.find("payload")->getSyntax());
        CHECK(actual.find("payload")->getSyntax()->toString() ==
              expected.find("payload")->getSyntax()->toString());
    }
    analysis::AnalysisOptions analysisOptions{.flags = analysis::AnalysisFlags::CheckUnused |
                                                       analysis::AnalysisFlags::CheckShadow};
    analysis::AnalysisManager actualAnalysis(analysisOptions), expectedAnalysis(analysisOptions);
    repeated->freeze();
    actualAnalysis.analyze(*repeated);
    repeated->unfreeze();
    native->freeze();
    expectedAnalysis.analyze(*native);
    native->unfreeze();
    CHECK(DiagnosticEngine::reportAll(driver->sm, actualAnalysis.getDiagnostics()) ==
          DiagnosticEngine::reportAll(driver->sm, expectedAnalysis.getDiagnostics()));
    shiftLine = true;
    auto relocated = make(2), nativeRelocated = make(2, 8, false, true);
    CHECK_FALSE(relocated->hasImportedInstanceBodies());
    CHECK(DiagnosticEngine::reportAll(driver->sm, relocated->getSemanticDiagnostics()) ==
          DiagnosticEngine::reportAll(driver->sm, nativeRelocated->getSemanticDiagnostics()));
    shiftLine = false;
    CHECK(make(2)->hasImportedInstanceBodies());
    if (!enumeration) {
        shiftMember = true;
        CHECK_FALSE(make(2)->hasImportedInstanceBodies());
        shiftMember = false;
        CHECK(make(2)->hasImportedInstanceBodies());
    }
    for (const auto& bind : {"bind stable audit probe();", "bind top.u audit probe();"}) {
        extraSource = std::string("module audit; initial missing_audit=1; endmodule ") + bind;
        auto bound = make(2), expected = make(2, 8, false, true);
        CHECK_FALSE(bound->hasImportedInstanceBodies());
        CHECK(DiagnosticEngine::reportAll(driver->sm, bound->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, expected->getSemanticDiagnostics()));
        extraSource.clear();
        CHECK(make(2)->hasImportedInstanceBodies());
    }
    if (!enumeration) {
        auto resized = make(2, 9);
        CHECK_FALSE(resized->hasImportedInstanceBodies());
        auto expected = make(2, 9, false, true);
        CHECK(DiagnosticEngine::reportAll(driver->sm, resized->getSemanticDiagnostics()) ==
              DiagnosticEngine::reportAll(driver->sm, expected->getSemanticDiagnostics()));
    }
    auto changedOrigin = make(3, 8, true);
    CHECK_FALSE(changedOrigin->hasImportedInstanceBodies());
    auto expected = make(3, 8, true, true);
    CHECK(DiagnosticEngine::reportAll(driver->sm, changedOrigin->getSemanticDiagnostics()) ==
          DiagnosticEngine::reportAll(driver->sm, expected->getSemanticDiagnostics()));
}

TEST_CASE("Captured fields keep native hover and declaration targets across edits",
          "[scope-semantic][scope-type-closure-hover]") {
    const bool macro = GENERATE(false, true);
    const bool forwarded = GENERATE(false, true);
    const auto macroView = GENERATE(Config::HoverConfig::MacroView::source,
                                    Config::HoverConfig::MacroView::expanded,
                                    Config::HoverConfig::MacroView::both);
    CAPTURE(macro, forwarded, int(macroView));
    LibraryWorkspace files;
    files.write(files.root / "fixed.sv",
                "module stable #(parameter type T=logic)(input T a,output T y); "
                "T local_copy; assign local_copy=a; assign y=local_copy; endmodule\n");
    const std::string shape = "struct packed {logic [7:0] payload; logic valid;}";
    const std::string declaration = "typedef " + shape + " packet_t; ";
    const std::string prefix =
        forwarded ? (macro ? "`define SHAPE_DATA " + shape +
                                 "\nmodule top #(parameter type packet_t=`SHAPE_DATA); "
                           : "module top #(parameter type packet_t=" + shape + "); ")
                  : (macro ? "`define DECL_DATA " + declaration + "\nmodule top; `DECL_DATA\n"
                           : "module top; " + declaration);
    const std::string original =
        prefix +
        "packet_t a,y; stable #(.T(packet_t)) u(.*); wire [7:0] result=u.local_copy.payload; "
        "localparam int EDIT=0; endmodule\n";
    files.write(files.root / "consumer.sv", original);
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    struct Harness : ServerHarness {
        using server::SlangServer::m_driver;
        using ServerHarness::ServerHarness;
    };
    std::vector<std::string> native;
    for (bool enabled : {false, true}) {
        Config config;
        config.build = "design.f";
        config.flags = "--single-unit --threads 1 --top top";
        config.experimentalScopeSemanticCache = enabled;
        config.hovers.value().macroView = macroView;
        config.frozenSources = Config::FrozenSourcesConfig{
            .instances = std::vector<std::string>{"top.u"}, .maxMiB = 64};
        files.write(files.root / ".slang/server.json", rfl::json::write(config));
        Harness server(lsp::InitializeParams{
            .capabilities = {},
            .workspaceFolders = {{lsp::WorkspaceFolder{.uri = URI::fromFile(files.root),
                                                       .name = "closed-fields"}}}});
        REQUIRE(server.m_driver->comp);
        REQUIRE(server.m_driver->comp->setActiveInstance("top.u"));
        auto document = server.openFile("consumer.sv");
        auto fixedDocument = server.openFile("fixed.sv");
        for (int edit = 1; edit <= 3; ++edit) {
            auto text = original;
            text.replace(text.find("EDIT=0"), 6, fmt::format("EDIT={}", edit));
            document.replaceAll(text);
            document.publishChanges();
            auto cursor = document.before("payload; localparam");
            auto hover = document.getHoverAt(cursor.m_offset);
            REQUIRE(hover);
            auto completions = cursor.getResolvedCompletions();
            CHECK(completions.size() == 2);
            auto info = document.getDefinitionInfoAt(cursor.m_offset);
            REQUIRE(info);
            auto actual = rfl::json::write(*hover) +
                          rfl::json::write(info->getDefinitionLspLinks());
            CHECK(actual.find("__slang_scope_cache") == std::string::npos);
            if (enabled)
                CHECK(actual == native.at(edit - 1));
            else
                native.push_back(actual);
        }
        if (enabled) {
            auto cache = server.m_driver->options.get<server::ScopeBodyImport>()->cache.lock();
            REQUIRE(cache);
            CHECK(cache->status().hits >= 3);
            CHECK(cache->status().owners == 1);
            CHECK(cache->status().publications == 1);
            CHECK(cache->status().retainedRoots == std::vector<std::string>{"top.u"});
        }
    }
}

TEST_CASE("Independent nominal captures cannot overwrite a current declaration identity",
          "[scope-semantic][scope-type-closure-owners]") {
    const bool classScoped = GENERATE(false, true);
    CAPTURE(classScoped);
    LibraryWorkspace files;
    files.write(files.root / "fixed.sv",
                "module stable #(parameter type T=logic)(input T a,output T y); "
                "assign y=a; endmodule\n");
    auto source = [&](int edit) {
        auto declarations = classScoped
                                ? "class C #(parameter W=8); typedef struct packed {logic[W-1:0] "
                                  "data;} value_t; endclass typedef C#(8)::value_t packet_t;"
                                : "typedef struct packed {logic[7:0] data;} packet_t;";
        return fmt::format("module top; {} packet_t a,b,c; "
                           "stable #(.T(packet_t)) u(.a(a),.y(b)),v(.a(b),.y(c)); "
                           "localparam int EDIT={}; endmodule\n",
                           declarations, edit);
    };
    files.write(files.root / "consumer.sv", source(0));
    files.write(files.root / "design.f", "fixed.sv\nconsumer.sv\n");
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.frozenSources = Config::FrozenSourcesConfig{
        .instances = std::vector<std::string>{"top.u", "top.v"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            files.root.string(), nullptr, true);
    REQUIRE(driver);
    auto setting = driver->options.get<server::TrustedUvmImport>();
    REQUIRE(setting);
    auto library = setting->library;
    auto cache = std::make_shared<server::ScopeBodyCache>(
        library, 64 * 1024 * 1024, std::vector<std::string>{},
        std::vector<std::string>{"top.u", "top.v"});
    auto make = [&](int edit, bool native = false) {
        auto options = driver->options;
        options.set(server::ScopeBodyImport{cache});
        auto compilation = std::make_shared<ast::Compilation>(options);
        REQUIRE(server::importTrustedUvm(*compilation, driver->sm, options));
        if (native)
            compilation->setInstanceBodyReuseHooks({}, {});
        auto old = driver->sm.readSource(files.root / "consumer.sv").value();
        auto text = source(edit);
        SmallVector<char> bytes(text.begin(), text.end());
        bytes.push_back('\0');
        auto buffer = driver->sm.replaceBuffer(old.id, std::move(bytes));
        compilation->addSyntaxTree(syntax::SyntaxTree::fromBuffer(buffer, driver->sm, options));
        (void)compilation->getSemanticDiagnostics();
        return compilation;
    };
    auto first = make(0);
    REQUIRE_FALSE(first->hasIssuedErrors());
    first->offerInstanceBodies(first);
    CHECK(cache->status().owners == (classScoped ? 0 : 2));
    auto current = make(1), expected = make(1, true);
    CHECK_FALSE(current->hasIssuedErrors());
    CHECK(DiagnosticEngine::reportAll(driver->sm, current->getSemanticDiagnostics()) ==
          DiagnosticEngine::reportAll(driver->sm, expected->getSemanticDiagnostics()));
    CHECK(cache->status().hits == (classScoped ? 0 : 1));
    analysis::AnalysisOptions options{.flags = analysis::AnalysisFlags::CheckUnused |
                                               analysis::AnalysisFlags::CheckShadow};
    analysis::AnalysisManager actualAnalysis(options), expectedAnalysis(options);
    current->freeze();
    actualAnalysis.analyze(*current);
    current->unfreeze();
    expected->freeze();
    expectedAnalysis.analyze(*expected);
    expected->unfreeze();
    CHECK(DiagnosticEngine::reportAll(driver->sm, actualAnalysis.getDiagnostics()) ==
          DiagnosticEngine::reportAll(driver->sm, expectedAnalysis.getDiagnostics()));
}

TEST_CASE("Configured syntax replay has an independent budget and invalidates edited prefixes",
          "[scope-semantic][configured-syntax-prefix]") {
    const int mode = GENERATE(0, 1, 2, 3);
    const bool directRoot = GENERATE(false, true);
    INFO(mode);
    INFO(directRoot); // disabled, zero budget, reusable, over budget
    LibraryWorkspace files;
    Config config;
    CHECK_FALSE(config.experimentalSyntaxPrefixCache.value());
    CHECK(config.syntaxPrefixCacheMaxMiB.value() == 256);
    config.experimentalScopeSemanticCache = true;
    config.experimentalCooperativeAnalysis = true;
    config.experimentalSyntaxPrefixCache = mode != 0;
    config.syntaxPrefixCacheMaxMiB = mode == 1 ? 0u : mode == 3 ? 1u : 64u;
    config.frozenSources = Config::FrozenSourcesConfig{
        .packages = std::vector<std::string>{"fixed_pkg"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", "package fixed_pkg; class Base; endclass endpackage\n");
    auto sharedSource = [&](int macro) {
        std::string text = "`pragma protect begin\npackage shared_pkg; class C;\n";
        if (mode == 3)
            for (int i = 0; i < 12000; ++i)
                text += "int member_" + std::to_string(i) + ";\n";
        text += "endclass endpackage\n`pragma protect end\n`define SHARED_VALUE " +
                std::to_string(macro) + "\n";
        return text;
    };
    files.write(files.root / "shared.sv", sharedSource(7));
    auto text = [=](int value) {
        return std::string(directRoot ? "module top;" : "class C;") +
               " shared_pkg::C obj; localparam int V=`SHARED_VALUE+" +
               std::to_string(value) + "; int added_" + std::to_string(value) +
               (directRoot ? "; endmodule\n" : "; endclass\n");
    };
    files.write(files.root / "user.svh", text(0));
    files.write(files.root / "user.sv", directRoot ? text(0) :
                "package user_pkg;\n`include \"user.svh\"\n"
                "endpackage\nmodule top; user_pkg::C obj; endmodule\n");
    files.write(files.root / "design.f", "fixed.sv\nshared.sv\nuser.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(
        indexer, client, config, {"design.f"}, files.root.string(), nullptr, true);
    REQUIRE(driver);
    const auto uri = URI::fromFile(files.root / (directRoot ? "user.sv" : "user.svh"));
    driver->openDocument(uri, text(0), 1, true);
    auto verify = [&](int version, int value, int expected) {
        driver->onDocDidChange({.textDocument = {.version = version, .uri = uri},
            .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text(value)}}});
        auto compilation = driver->getDocument(uri)->getAnalysis()->getCompilation();
        const ast::Symbol* parameter = nullptr;
        if (directRoot) {
            REQUIRE(compilation->getRoot().topInstances.size() == 1);
            parameter = compilation->getRoot().topInstances.front()->body.find("V");
        }
        else {
            REQUIRE(compilation->getPackage("user_pkg"));
            auto cls = compilation->getPackage("user_pkg")->find("C");
            REQUIRE(cls);
            parameter = cls->as<ast::ClassType>().find("V");
        }
        REQUIRE(parameter);
        CHECK(parameter->as<ast::ParameterSymbol>().getValue().integer().as<int>() == expected);
        CHECK(compilation->getSemanticDiagnostics().empty());
    };
    verify(2, 1, 8);
    if (mode == 2) {
        driver->onDocDidChange({.textDocument = {.version = 3, .uri = uri},
            .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text(2)}}});
        lsp::RequestContext request("test/syntax-prefix", lsp::ID_t{1});
        bool reached = false;
        request.setCheckpointObserverForTesting([&](std::string_view where) {
            if (where == "during disposable syntax parsing") {
                reached = true;
                request.cancel();
            }
        });
        CHECK_THROWS_AS(driver->getDocument(uri)->getAnalysis(request), lsp::RequestCancelled);
        REQUIRE(reached);
        const auto cancelled = rfl::json::write(driver->scopeLibraryStatus());
        CHECK(cancelled.find("\"cachedRoots\":0") != std::string::npos);
        lsp::RequestContext late("test/syntax-prefix-late", lsp::ID_t{2});
        bool beforeCommit = false;
        late.setCheckpointObserverForTesting([&](std::string_view where) {
            if (where == "before syntax prefix cache commit") {
                beforeCommit = true;
                late.cancel();
            }
        });
        CHECK_THROWS_AS(driver->getDocument(uri)->getAnalysis(late), lsp::RequestCancelled);
        REQUIRE(beforeCommit);
        const auto afterLateCancel = rfl::json::write(driver->scopeLibraryStatus());
        CHECK(afterLateCancel.find("\"cachedRoots\":0") != std::string::npos);
        CHECK(afterLateCancel.find("\"pendingRoots\":2") != std::string::npos);
    }
    verify(4, 2, 9);
    auto status = rfl::json::write(driver->scopeLibraryStatus());
    INFO(status);
    if (mode == 2) {
        CHECK(status.find("\"cachedRoots\":2") != std::string::npos);
        CHECK(status.find("\"hits\":1") != std::string::npos);
        driver->prepareForContextRetirement(true);
        const auto retired = rfl::json::write(driver->scopeLibraryStatus());
        CHECK(retired.find("\"cachedRoots\":0") != std::string::npos);
        CHECK(retired.find("\"pendingRoots\":0") != std::string::npos);
        CHECK(retired.find("\"chargedBytes\":0") != std::string::npos);
        verify(5, 3, 10);
        verify(6, 4, 11);
        CHECK(rfl::json::write(driver->scopeLibraryStatus()).find("\"cachedRoots\":2") !=
              std::string::npos);
        const auto sharedUri = URI::fromFile(files.root / "shared.sv");
        driver->openDocument(sharedUri, sharedSource(13), 7, true);
        verify(8, 5, 18);
        auto changed = rfl::json::write(driver->scopeLibraryStatus());
        CHECK(changed.find("\"cachedRoots\":0") != std::string::npos);
    }
    else {
        CHECK(status.find("\"cachedRoots\":0") != std::string::npos);
        CHECK(status.find("\"pendingRoots\":0") != std::string::npos);
        if (mode == 3)
            CHECK(status.find("\"refusals\":1") != std::string::npos);
    }
}

TEST_CASE("Syntax prefix witness failures refuse stale native inputs and preserve editor recovery",
          "[scope-semantic][syntax-prefix-input-change]") {
    const int mode = GENERATE(0, 1, 2); // changed bytes, same-byte alias, new negative lookup
    INFO(mode);
    LibraryWorkspace files;
    Config config;
    config.experimentalScopeSemanticCache = true;
    config.experimentalCooperativeAnalysis = true;
    config.experimentalSyntaxPrefixCache = true;
    config.frozenSources = Config::FrozenSourcesConfig{
        .packages = std::vector<std::string>{"fixed_pkg"}, .maxMiB = 64};
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(files.root / "server.json").string(), "--single-unit --threads 1 --top top"}};
    files.write(files.root / "fixed.sv", "package fixed_pkg; endpackage\n");
    files.write(files.root / "first/value.svh", "localparam int N=7;\n");
    files.write(files.root / "second/value.svh", "localparam int N=7;\n");
    fs::create_directory_symlink(files.root / "first", files.root / "alias");
    files.write(files.root / "shared.sv", "package shared_pkg;\n`include \"alias/value.svh\"\nendpackage\n");
    auto text = [](int value) { return "module top; localparam int V=shared_pkg::N+" +
        std::to_string(value) + "; int added_" + std::to_string(value) + "; endmodule\n"; };
    files.write(files.root / "user.sv", text(0));
    files.write(files.root / "design.f", "fixed.sv\nshared.sv\nuser.sv\n");
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(
        indexer, client, config, {"design.f"}, files.root.string(), nullptr, true);
    REQUIRE(driver);
    const auto uri = URI::fromFile(files.root / "user.sv");
    driver->openDocument(uri, text(0), 1, true);
    if (mode == 2)
        REQUIRE_FALSE(driver->sm.readSource(files.root / "later.svh"));
    auto edit = [&](int value) {
        driver->onDocDidChange({.textDocument = {.version = value + 1, .uri = uri},
            .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text(value)}}});
    };
    edit(1);
    REQUIRE(driver->getDocument(uri)->getAnalysis());
    edit(2);
    REQUIRE(driver->getDocument(uri)->getAnalysis());
    REQUIRE(rfl::json::write(driver->scopeLibraryStatus()).find("\"cachedRoots\":2") !=
            std::string::npos);
    if (mode == 0)
        files.write(files.root / "first/value.svh", "localparam int N=11;\n");
    else if (mode == 1) {
        fs::remove(files.root / "alias");
        fs::create_directory_symlink(files.root / "second", files.root / "alias");
    }
    else
        files.write(files.root / "later.svh", "newly available\n");
    edit(3);
    CHECK_THROWS_AS(driver->getDocument(uri)->getAnalysis(), lsp::RequestFailed);
    const auto failed = rfl::json::write(driver->scopeLibraryStatus());
    INFO(failed);
    CHECK(failed.find("\"witnessFailures\":1") != std::string::npos);
    CHECK(failed.find("\"cachedRoots\":0") != std::string::npos);
    CHECK(failed.find("\"pendingRoots\":0") != std::string::npos);
    CHECK(driver->hasStaleSyntaxPrefixInputs());
    CHECK_THROWS_AS(driver->getDocument(uri)->getAnalysis(), lsp::RequestFailed);
    // The ordinary recovery path constructs a fresh manager and preserves the
    // unsaved editor document, rather than parsing through stale failed lookups.
    auto fresh = server::ServerDriver::createFromFileLists(
        indexer, client, config, {"design.f"}, files.root.string(), driver.get(), true);
    REQUIRE(fresh);
    CHECK(fresh->getDocument(uri)->getText().starts_with(text(3)));
    auto compilation = fresh->getDocument(uri)->getAnalysis()->getCompilation();
    REQUIRE(compilation->getRoot().topInstances.size() == 1);
    CHECK(compilation->getRoot().topInstances.front()->body.find("V")
              ->as<ast::ParameterSymbol>().getValue().integer().as<int>() == (mode == 0 ? 14 : 10));
}

TEST_CASE("The server reloads failed syntax inputs without a watcher event or lost editor text",
          "[scope-semantic][syntax-prefix-input-change][syntax-prefix-server-recovery]") {
    const int mode = GENERATE(0, 1, 2);
    INFO(mode);
    LibraryWorkspace files;
    files.write(files.root / "fixed.sv", "package fixed_pkg; endpackage\n");
    fs::create_directories(files.root / "first");
    if (mode != 2)
        files.write(files.root / "first/value.svh", "localparam int N=7;\n");
    files.write(files.root / "second/value.svh", "localparam int N=7;\n");
    fs::create_directory_symlink(files.root / "first", files.root / "alias");
    files.write(files.root / "shared.sv", mode == 2 ?
        "package shared_pkg;\n`include \"value.svh\"\nendpackage\n" :
        "package shared_pkg;\n`include \"alias/value.svh\"\nendpackage\n");
    auto text = [](int value) { return "module top; localparam int V=shared_pkg::N+" +
        std::to_string(value) + "; int added_" + std::to_string(value) + "; endmodule\n"; };
    files.write(files.root / "user.sv", text(0));
    files.write(files.root / "design.f", "fixed.sv\nshared.sv\nuser.sv\n");
    files.write(files.root / ".slang/server.json", R"({
        "indexingThreads":1,"build":"design.f",
        "flags":"--single-unit --threads 1 --top top +incdir+first +incdir+second",
        "experimentalScopeSemanticCache":true,"experimentalCooperativeAnalysis":true,
        "experimentalSyntaxPrefixCache":true,
        "frozenSources":{"packages":["fixed_pkg"],"maxMiB":64}})");
    ServerHarness harness(lsp::InitializeParams{.workspaceFolders = {{
        lsp::WorkspaceFolder{.uri = URI::fromFile(files.root), .name = "syntax-recovery"}}}});
    const auto uri = URI::fromFile(files.root / "user.sv");
    harness.onDocDidOpen({.textDocument = {.uri = uri, .languageId = "systemverilog",
                                          .version = 1, .text = text(0)}});
    auto edit = [&](int value) {
        harness.SlangServer::onDocDidChange(
            {.textDocument = {.version = value + 1, .uri = uri},
             .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text(value)}}});
    };
    const lsp::HoverParams hover{.textDocument = {.uri = uri},
        .position = {0, uint32_t(text(0).find(" V=") + 1)}};
    edit(1); REQUIRE(harness.getDocHover(hover));
    edit(2); REQUIRE(harness.getDocHover(hover));
    REQUIRE(rfl::json::write(harness.m_driver->scopeLibraryStatus()).find("\"cachedRoots\":2") !=
            std::string::npos);
    std::weak_ptr<syntax::SyntaxTree> previousTree =
        harness.m_driver->getDocument(uri)->getCachedSyntaxTree();
    REQUIRE_FALSE(previousTree.expired());
    if (mode == 1) {
        fs::remove(files.root / "alias");
        fs::create_directory_symlink(files.root / "second", files.root / "alias");
    }
    else
        files.write(files.root / "first/value.svh", "localparam int N=11;\n");
    edit(3);
    CHECK_THROWS_AS(harness.getDocHover(hover), lsp::RequestFailed);
    REQUIRE(harness.m_driver->hasStaleSyntaxPrefixInputs());
    const auto* previous = harness.m_driver.get();
    lsp::RequestContext cancelled("test/syntax-input-recovery", lsp::ID_t{7});
    bool candidateComplete = false;
    cancelled.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "before retirement commit") {
            candidateComplete = true;
            cancelled.cancel();
        }
    });
    CHECK_THROWS_AS(harness.getDocHover(hover, cancelled), lsp::RequestCancelled);
    REQUIRE(candidateComplete);
    CHECK(harness.m_driver.get() == previous);
    CHECK(harness.m_driver->hasStaleSyntaxPrefixInputs());
    CHECK(harness.m_driver->getDocument(uri)->getText().starts_with(text(3)));
    REQUIRE(harness.getDocHover(hover));
    CHECK(harness.m_driver.get() != previous);
    CHECK(previousTree.expired());
    CHECK_FALSE(harness.m_driver->hasStaleSyntaxPrefixInputs());
    CHECK(harness.m_driver->getDocument(uri)->getText().starts_with(text(3)));
    auto compilation = harness.m_driver->getDocument(uri)->getAnalysis()->getCompilation();
    REQUIRE(compilation->getRoot().topInstances.size() == 1);
    CHECK(compilation->getRoot().topInstances.front()->body.find("V")
              ->as<ast::ParameterSymbol>().getValue().integer().as<int>() == (mode == 1 ? 10 : 14));
}
