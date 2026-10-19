// SPDX-License-Identifier: MIT
#include "ServerDriver.h"
#include "completions/CompletionContext.h"
#include "utils/ClientHarness.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/util/OS.h"
#include "slang/util/ScopeGuard.h"
using namespace slang;
namespace {
struct Access : server::CompletionQuery {
    using CompletionQuery::getIndexedSyntaxTree;
};
int value(const std::shared_ptr<syntax::SyntaxTree>& tree) {
    ast::Compilation compilation;
    compilation.addSyntaxTree(tree);
    auto* pkg = compilation.getPackage("uvm_pkg");
    REQUIRE(pkg);
    auto* symbol = pkg->find("X");
    REQUIRE(symbol);
    return *symbol->as<ast::ParameterSymbol>().getValue().integer().as<int>();
}
} // namespace
TEST_CASE("Shared prefix resolver proves options cwd callbacks and observable directives",
          "[parsed-library-guards]") {
    namespace fs = std::filesystem;
    auto originalCwd = fs::current_path();
    auto root = fs::temp_directory_path() /
                std::string("slang-uvm-prefix-" + std::to_string(slang::OS::getpid()));
    fs::create_directories(root / "other");
    root = fs::canonical(root);
    auto restoreCwd = ScopeGuard([&] {
        fs::current_path(originalCwd);
        fs::remove_all(root);
    });
    fs::current_path(root);
    auto write = [&](std::string_view name, std::string_view text) {
        std::ofstream f(root / name);
        REQUIRE(f.good());
        f << text;
    };
    const std::string text = "package uvm_pkg; localparam int X=`CONTEXT_VALUE; endpackage\n";
    write("uvm_pkg.sv", text);
    write("consumer.sv", "module consumer; endmodule\n");
    write("design.f", "uvm_pkg.sv\nconsumer.sv\n");
    Config cfg;
    cfg.experimentalUvmPrefixCache = true;
    cfg.flagsByFile = std::vector<Config::FlagSource>{
        {(root / "server.json").string(), "--single-unit --threads 1 -DCONTEXT_VALUE=1"}};
    cfg.compilationFilelist = "design.f";
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, cfg, root.string(),
                                                         nullptr);
    REQUIRE(driver);
    auto file = root / "uvm_pkg.sv";
    std::shared_ptr<syntax::SyntaxTree> prefix;
    for (auto& [uri, doc] : driver->docs)
        if (auto p = doc->getPreparedLibraryPrefix())
            prefix = p;
    REQUIRE(prefix);
    CHECK(prefix->getPrefixParseCwd() == root.string());
    CHECK(prefix->getRetainedSyntaxBytes() > prefix->allocator().getTotalBytesAllocated());
    CHECK(prefix->getRetainedSourceBytes() >
          driver->sm.getSourceText(prefix->getSourceBufferIds()[0]).size());
    auto initial = Access::getIndexedSyntaxTree(driver->completions, file.string());
    CHECK(initial == prefix);
    CHECK(value(initial) == 1);
    auto originalPP = driver->options.getOrDefault<parsing::PreprocessorOptions>();
    auto changed = originalPP;
    std::erase_if(changed.predefines, [](auto& s) { return s.starts_with("CONTEXT_VALUE="); });
    changed.predefines.push_back("CONTEXT_VALUE=2");
    driver->options.set(changed);
    auto different = Access::getIndexedSyntaxTree(driver->completions, file.string());
    CHECK(different != prefix);
    CHECK(value(different) == 2);
    driver->options.set(originalPP);
    auto restored = Access::getIndexedSyntaxTree(driver->completions, file.string());
    CHECK(restored == prefix);
    CHECK(value(restored) == 1);
    fs::current_path(root / "other");
    auto moved = Access::getIndexedSyntaxTree(driver->completions, file.string());
    CHECK(moved != prefix);
    CHECK(value(moved) == 1);
    fs::current_path(root);
    int calls = 0;
    auto callback = [&](BufferID, bool, bool) { ++calls; };
    auto hooked = originalPP;
    hooked.bufferChangeCB = callback;
    driver->options.set(hooked);
    auto callbackTree = Access::getIndexedSyntaxTree(driver->completions, file.string());
    CHECK(callbackTree != prefix);
    CHECK(calls > 0);
    CHECK(value(callbackTree) == 1);
    driver->options.set(originalPP);
    for (auto tail : {"`line 100 \"virtual.sv\" 0\n", "`pragma once\n"}) {
        write("uvm_pkg.sv", text + tail);
        driver->onWorkspaceDidChangeWatchedFiles(
            {.changes = {{.uri = URI::fromFile(file), .type = lsp::FileChangeType::Changed}}});
        auto first = Access::getIndexedSyntaxTree(driver->completions, file.string());
        auto second = Access::getIndexedSyntaxTree(driver->completions, file.string());
        CHECK(first != second);
        CHECK(first->getPrefixSourceEffects() == true);
        CHECK(value(first) == 1);
        CHECK(value(second) == 1);
    }
    // Cache and view owners must not form a cycle across a context retirement.
    std::weak_ptr<syntax::SyntaxTree> released = prefix;
    prefix.reset();
    initial.reset();
    different.reset();
    restored.reset();
    moved.reset();
    callbackTree.reset();
    driver.reset();
    CHECK(released.expired());
}

TEST_CASE("Published UVM prefix views preserve ancestry during concurrent extraction",
          "[parsed-library-guards]") {
    SourceManager sm;
    parsing::PreprocessorOptions pp;
    pp.captureUvmPrefix = true;
    pp.predefines = {"CONTEXT_VALUE=7"};
    Bag options;
    options.set(pp);
    const auto root = std::filesystem::temp_directory_path() / "slang-prefix-concurrent";
    auto library = sm.assignText((root / "uvm_pkg.sv").string(), R"(
package uvm_pkg;
class base #(int N = 7);
    typedef enum { FIRST, SECOND } choice;
endclass
localparam int X = `CONTEXT_VALUE;
endpackage
`define TAIL(V) V
)");
    auto user = sm.assignText((root / "consumer.sv").string(),
                              "module consumer; localparam int Y = `TAIL(8); endmodule\n");
    std::array buffers{library, user};
    auto native = syntax::SyntaxTree::fromBuffers(buffers, sm, options);
    auto prefix = syntax::SyntaxTree::preparedUvmPrefix(native);
    REQUIRE(prefix);
    CHECK(prefix->getPrefixSourceEffects() == false);
    CHECK(prefix->isPrefixResumeSafe() == true);
    CHECK(value(prefix) == 7);
    auto* package = native->root().as<syntax::CompilationUnitSyntax>().members[0];
    auto fingerprint = [](const syntax::SyntaxNode& rootNode) {
        size_t result = 0;
        auto mix = [&](const void* pointer) {
            result ^= reinterpret_cast<uintptr_t>(pointer) + 0x9e3779b9 + (result << 6) +
                      (result >> 2);
        };
        auto walk = [&](auto&& self, const syntax::SyntaxNode& node) -> void {
            mix(&node);
            mix(node.parent.get());
            mix(node.previewNode());
            auto trivia = [&](auto&& visit, std::span<const parsing::Trivia> entries) -> void {
                for (const auto& entry : entries) {
                    if (entry.kind == parsing::TriviaKind::Directive)
                        self(self, *entry.syntax());
                    else if (entry.kind == parsing::TriviaKind::SkippedTokens)
                        for (auto token : entry.getSkippedTokens())
                            visit(visit, token.trivia());
                }
            };
            for (size_t i = 0; i < node.getChildCount(); ++i) {
                if (auto child = node.childNode(i))
                    self(self, *child);
                else if (auto token = node.childToken(i))
                    trivia(trivia, token.trivia());
            }
        };
        walk(walk, rootNode);
        return result;
    };
    const auto before = fingerprint(*package);
    std::atomic<size_t> failed = 0;
    std::vector<std::thread> threads;
    for (size_t i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            for (size_t iteration = 0; iteration < 5; ++iteration) {
                try {
                    auto view = syntax::SyntaxTree::preparedUvmPrefix(native);
                    if (!view || view->getPrefixSourceEffects() != false) {
                        ++failed;
                        continue;
                    }
                    ast::Compilation compilation;
                    compilation.addSyntaxTree(view);
                    auto* pkg = compilation.getPackage("uvm_pkg");
                    auto* symbol = pkg ? pkg->find("X") : nullptr;
                    if (!symbol ||
                        symbol->as<ast::ParameterSymbol>().getValue().integer().as<int>() !=
                            std::optional<int>(7) ||
                        compilation.getCompilationUnits().size() != 1)
                        ++failed;
                }
                catch (...) {
                    ++failed;
                }
            }
        });
    }
    for (auto& thread : threads)
        thread.join();
    CHECK(failed.load() == 0);
    CHECK(fingerprint(*package) == before);

    pp.predefines.clear();
    options.set(pp);
    const std::string nextText = "module consumer_next; localparam int Y = `TAIL(9); endmodule\n";
    SmallVector<char> nextBytes;
    nextBytes.append_range(nextText);
    nextBytes.push_back('\0');
    auto nextUser = sm.replaceBuffer(user.id, std::move(nextBytes));
    CHECK_FALSE(syntax::SyntaxTree::preparedUvmPrefix(native));
    auto suffix = syntax::SyntaxTree::afterCachedPrefix(prefix, std::span(&nextUser, 1), options);
    CHECK(suffix->diagnostics().empty());
    auto composed = syntax::SyntaxTree::withCachedPrefix(prefix, suffix);
    CHECK(value(composed) == 7);
    ast::Compilation rebound;
    rebound.addSyntaxTree(composed);
    CHECK(rebound.getAllDiagnostics().empty());
    const auto& reboundRoot = rebound.getRoot();
    REQUIRE(reboundRoot.topInstances.size() == 1);
    auto* y = reboundRoot.topInstances[0]->body.find("Y");
    REQUIRE(y);
    CHECK(y->as<ast::ParameterSymbol>().getValue().integer().as<int>() == std::optional<int>(9));
    CHECK(fingerprint(*package) == before);
    auto unprepared = syntax::SyntaxTree::fromText("package plain; endpackage", sm);
    CHECK_THROWS_AS(syntax::SyntaxTree::withCachedPrefix(unprepared, suffix),
                    std::invalid_argument);
    CHECK_THROWS_AS(syntax::SyntaxTree::afterCachedPrefix(unprepared, std::span(&nextUser, 1),
                                                          options),
                    std::invalid_argument);
    SmallVector<char> replacement;
    const std::string edited = "module changed; endmodule\n";
    replacement.append_range(edited);
    replacement.push_back('\0');
    auto changed = sm.replaceBuffer(nextUser.id, std::move(replacement));
    CHECK(changed.id != nextUser.id);
    CHECK_FALSE(sm.isLatestData(nextUser.id));
    // Native replacement invalidates the manager lookup, while our composed
    // view must keep the raw token storage alive independently of that lookup.
    CHECK(nextUser.data.starts_with("module consumer_next"));
    CHECK(value(composed) == 7);
}

TEST_CASE("Library resolve shares only transparent same-directory UVM wrappers",
          "[parsed-library-guards]") {
    struct Scenario {
        std::string wrapper;
        bool shared;
        bool warning = false;
    };
    const std::array scenarios{
        Scenario{"// a plain wrapper\n`include \"uvm_pkg.sv\"\n", true},
        Scenario{"`undef CONTEXT_VALUE\n`define CONTEXT_VALUE 2\n`include \"uvm_pkg.sv\"\n", false},
        Scenario{"`ifdef CONTEXT_VALUE\n`include \"uvm_pkg.sv\"\n`endif\n", false},
        Scenario{"// slang lint_off width-expand\n`include \"uvm_pkg.sv\"\n", false},
        Scenario{"`include \"uvm_pkg.sv\"\n`define EXTRA_STATE 1\n", false},
        Scenario{"`include \"uvm_pkg.sv\"\n", true, true}};
    auto previous = std::filesystem::current_path();
    auto root = std::filesystem::temp_directory_path() /
                ("slang-wrapper-" + std::to_string(slang::OS::getpid()));
    std::filesystem::create_directories(root);
    auto cleanup = ScopeGuard([&] {
        std::filesystem::current_path(previous);
        std::filesystem::remove_all(root);
    });
    std::filesystem::current_path(root);
    auto write = [&](std::string_view name, std::string_view text) {
        std::ofstream file(root / name);
        file << text;
        REQUIRE(file.good());
    };
    write("uvm_pkg.sv", "package uvm_pkg; localparam int X=`CONTEXT_VALUE; endpackage\n");
    write("consumer.sv", "module consumer; endmodule\n");
    write("design.f", "uvm.sv\nconsumer.sv\n");
    Config config;
    config.experimentalUvmPrefixCache = true;
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(root / "server.json").string(), "--single-unit --threads 1 -DCONTEXT_VALUE=1"}};
    config.compilationFilelist = "design.f";
    for (const auto& scenario : scenarios) {
        INFO(scenario.wrapper);
        write("uvm.sv", scenario.wrapper);
        if (scenario.warning)
            write("uvm_pkg.sv", "`ifndef EXPECTED_GUARD\n`define ACTUAL_GUARD\n"
                                "package uvm_pkg; localparam int X=`CONTEXT_VALUE; endpackage\n"
                                "`endif\n");
        Indexer indexer;
        ClientHarness client;
        auto driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                             root.string());
        REQUIRE(driver);
        std::shared_ptr<syntax::SyntaxTree> prefix;
        for (auto& [uri, doc] : driver->docs)
            if (auto tree = doc->getPreparedLibraryPrefix())
                prefix = std::move(tree);
        REQUIRE(prefix);
        CHECK(prefix->diagnostics().empty() == !scenario.warning);
        CHECK(value(prefix) == (scenario.wrapper.starts_with("`undef") ? 2 : 1));
        size_t before = 0;
        driver->sm.visitDiagnosticDirectives(
            [&](BufferID, const auto& directives) { before += directives.size(); });
        auto resolved = Access::getIndexedSyntaxTree(driver->completions,
                                                     (root / "uvm_pkg.sv").string());
        REQUIRE(resolved);
        CHECK((resolved == prefix) == scenario.shared);
        CHECK(value(resolved) == 1);
        size_t after = 0;
        driver->sm.visitDiagnosticDirectives(
            [&](BufferID, const auto& directives) { after += directives.size(); });
        CHECK(after == before);
    }
    // Removing a wrapper reduces include depth. A truncated wrapper parse must
    // never replace the successful independently parsed package.
    write("uvm.sv", "`include \"uvm_pkg.sv\"\n");
    write("uvm_pkg.sv", "package uvm_pkg; localparam int X=`CONTEXT_VALUE;\n"
                        "`include \"depth1.svh\"\nendpackage\n");
    write("depth1.svh", "`include \"depth2.svh\"\n");
    write("depth2.svh", "typedef int depth_marker;\n");
    config.flagsByFile = std::vector<Config::FlagSource>{
        {(root / "server.json").string(),
         "--single-unit --threads 1 -DCONTEXT_VALUE=1 --max-include-depth 2"}};
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createForExplore(indexer, client, config, root.string());
    REQUIRE(driver);
    std::shared_ptr<syntax::SyntaxTree> truncated;
    for (auto& [uri, doc] : driver->docs)
        if (auto tree = doc->getPreparedLibraryPrefix())
            truncated = std::move(tree);
    REQUIRE(truncated);
    CHECK_FALSE(truncated->diagnostics().empty());
    auto independent = Access::getIndexedSyntaxTree(driver->completions,
                                                    (root / "uvm_pkg.sv").string());
    REQUIRE(independent);
    CHECK(independent != truncated);
    CHECK(independent->diagnostics().empty());
    CHECK(value(independent) == 1);
}
