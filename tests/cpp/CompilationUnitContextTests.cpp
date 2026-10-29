// SPDX-License-Identifier: MIT
#include "Indexer.h"
#include "ServerDriver.h"
#include "catch2/matchers/catch_matchers.hpp"
#include "catch2/generators/catch_generators.hpp"
#include "document/ShallowAnalysis.h"
#include "document/SlangDoc.h"
#include "utils/ClientHarness.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/parsing/Preprocessor.h"

namespace {
namespace fs = std::filesystem;

class UnitWorkspace {
public:
    UnitWorkspace() {
        static std::atomic_uint64_t serial{0};
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (;;) {
            auto candidate = fs::temp_directory_path() /
                             ("slang-unit-context-" + std::to_string(stamp) + "-" +
                              std::to_string(serial++));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                root = fs::canonical(candidate);
                break;
            }
            REQUIRE_FALSE(error);
        }
    }
    ~UnitWorkspace() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path path(std::string_view name) const { return root / name; }
    URI uri(std::string_view name) const { return URI::fromFile(path(name)); }
    void write(std::string_view name, std::string_view text) const {
        fs::create_directories(path(name).parent_path());
        std::ofstream output(path(name), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << text;
        output.close();
        REQUIRE(output.good());
    }
    fs::path root;
};

struct UnitDriver {
    Config config;
    Indexer indexer;
    ClientHarness client;
    std::unique_ptr<server::ServerDriver> driver;

    UnitDriver(const UnitWorkspace& workspace, std::string flags,
               std::vector<std::string> preincludes = {}, bool fastCwd = false,
               bool cooperative = false) {
        config.flagsByFile = std::vector<Config::FlagSource>{
            {workspace.path("server.json").string(), std::move(flags)}};
        config.explorePreincludeFiles = std::move(preincludes);
        config.fastCwdSnapshotChecks = fastCwd;
        config.experimentalCooperativeAnalysis = cooperative;
        driver = server::ServerDriver::createForExplore(indexer, client, config,
                                                        workspace.root.string(), nullptr);
    }
};

uint64_t aliasWidth(const std::shared_ptr<slang::syntax::SyntaxTree>& tree,
                    std::string_view packageName, std::string_view aliasName) {
    REQUIRE(tree->diagnostics().empty());
    slang::ast::Compilation compilation;
    compilation.addSyntaxTree(tree);
    auto* package = compilation.getPackage(packageName);
    REQUIRE(package);
    auto* alias = package->find(aliasName);
    REQUIRE(alias);
    return alias->as<slang::ast::TypeAliasType>().getCanonicalType().getBitWidth();
}

void replaceText(server::SlangDoc& doc, std::string text) {
    doc.onChange({lsp::TextDocumentContentChangeWholeDocument{.text = std::move(text)}});
}
} // namespace

TEST_CASE("Document syntax keeps replaced source bytes until its last strong owner",
          "[source-lifetime][document-syntax-lease]") {
    const auto mode = GENERATE("standalone", "preinclude-root", "preinclude-input");
    const bool cancelled = GENERATE(false, true);
    CAPTURE(mode, cancelled);
    const bool preinclude = std::string_view(mode) != "standalone";
    const bool editPrelude = std::string_view(mode) == "preinclude-input";
    UnitWorkspace workspace;
    workspace.write("preamble.sv", "`define FIELD_TYPE byte\n");
    const std::string original = "// retained documentation and declaration spelling\n"
                                 "package p; typedef " +
                                 std::string(preinclude ? "`FIELD_TYPE" : "byte") +
                                 " field_t; endpackage\n";
    workspace.write("owner.sv", original);
    UnitDriver fixture(workspace, "--single-unit --threads 1",
                       preinclude ? std::vector{workspace.path("preamble.sv").string()}
                                  : std::vector<std::string>{});
    auto& driver = *fixture.driver;
    auto doc = driver.getDocument(workspace.uri("owner.sv"));
    if (cancelled) {
        lsp::RequestContext ctx("completed document parse", lsp::ID_t{971});
        ctx.setCheckpointObserverForTesting([&](std::string_view where) {
            if (where == "after syntax parsing")
                ctx.cancel();
        });
        CHECK_THROWS_AS(doc->getSyntaxTree(ctx), lsp::RequestCancelled);
    }
    auto old = cancelled ? doc->getCachedSyntaxTree() : doc->getSyntaxTree();
    REQUIRE(old);
    const auto rendered = old->root().toString();
    std::weak_ptr<slang::syntax::SyntaxTree> weakTree = old;
    const auto editedPath = workspace.path(editPrelude ? "preamble.sv" : "owner.sv");
    std::weak_ptr<void> oldStorage;
    for (auto id : old->getSourceBufferIds()) {
        if (driver.sm.getFullPath(id) == editedPath) {
            auto pins = driver.sm.retainBuffers(std::vector{id});
            REQUIRE(pins.size() == 1);
            oldStorage = pins.front();
            break;
        }
    }
    REQUIRE_FALSE(oldStorage.expired());
    if (editPrelude)
        (void)server::SlangDoc::fromText(driver, workspace.uri("preamble.sv"),
                                       "`define FIELD_TYPE shortint\n");
    else {
        auto next = original;
        next.replace(next.find(preinclude ? "`FIELD_TYPE" : "byte"),
                     preinclude ? 11 : 4, "shortint");
        replaceText(*doc, std::move(next));
    }
    REQUIRE_FALSE(oldStorage.expired());
    CHECK(old->root().toString() == rendered);
    CHECK(aliasWidth(old, "p", "field_t") == 8);
    CHECK(aliasWidth(doc->getSyntaxTree(), "p", "field_t") == 16);
    // An expired weak syntax cache must not retain the old source allocation.
    old.reset();
    CHECK(weakTree.expired());
    if (!editPrelude)
        CHECK(oldStorage.expired());
    // Repeated preinclude reads can leave source-manager aliases of its old
    // FileData. They are separate live owners, not ownership by the weak tree.
    doc.reset();
    fixture.driver.reset();
    CHECK(oldStorage.expired());
}

TEST_CASE("Native single-unit recipes retain root order and unsaved macro inputs",
          "[native-unit]") {
    UnitWorkspace workspace;
    workspace.write("preamble.sv", "`define FIELD_TYPE byte\n");
    workspace.write("owner.sv", "package p; typedef `FIELD_TYPE field_t; endpackage\n");
    workspace.write("poison.svh", "`define FIELD_TYPE bit\n");
    workspace.write("native.f", "preamble.sv\nowner.sv\n");
    UnitDriver fixture(workspace,
                       "--single-unit -F \"" + workspace.path("native.f").string() + "\"",
                       {workspace.path("poison.svh").string()});
    auto& driver = *fixture.driver;
    auto doc = driver.docs.at(workspace.uri("owner.sv"));
    REQUIRE(doc->isNativeCompilationUnit());
    CHECK_FALSE(doc->requiresNativeReplay());
    CHECK(std::vector<fs::path>(doc->getRootPaths().begin(), doc->getRootPaths().end()) ==
          std::vector<fs::path>{workspace.path("preamble.sv"), workspace.path("owner.sv")});
    auto original = doc->getSyntaxTree();
    CHECK(aliasWidth(original, "p", "field_t") == 8);

    auto preamble = server::SlangDoc::fromText(driver, workspace.uri("preamble.sv"),
                                               "`define FIELD_TYPE shortint\n");
    auto changed = doc->getSyntaxTree();
    CHECK(changed != original);
    CHECK(aliasWidth(changed, "p", "field_t") == 16);
    CHECK(doc->getSyntaxTree() == changed);

    replaceText(*doc, "package p; typedef `FIELD_TYPE edited_t; endpackage\n");
    CHECK(aliasWidth(doc->getSyntaxTree(), "p", "edited_t") == 16);
    CHECK(preamble->textMatches("`define FIELD_TYPE shortint\n"));
}

TEST_CASE("Native separate-unit recipes retain per-unit defines include paths and libraries",
          "[native-unit]") {
    UnitWorkspace workspace;
    workspace.write("includes/member.svh", "typedef `FIELD_TYPE field_t;\n");
    workspace.write("owner.sv", "package p;\n`include \"member.svh\"\nendpackage\n");
    workspace.write("unit.f", "+define+FIELD_TYPE=longint\n+incdir+includes\n"
                              "--library unit_library\nowner.sv\n");
    UnitDriver fixture(workspace, "-C \"" + workspace.path("unit.f").string() + "\"");
    auto& driver = *fixture.driver;
    auto doc = driver.docs.at(workspace.uri("owner.sv"));
    REQUIRE(doc->isNativeCompilationUnit());
    REQUIRE_FALSE(doc->requiresNativeReplay());
    REQUIRE(doc->getRootPaths().size() == 1);
    CHECK(doc->getRootPaths().front() == workspace.path("owner.sv"));
    auto original = doc->getSyntaxTree();
    REQUIRE(original->isLibraryUnit);
    REQUIRE(original->getSourceLibrary());
    CHECK(aliasWidth(original, "p", "field_t") == 64);
    const auto& options = original->options().getOrDefault<slang::parsing::PreprocessorOptions>();

    auto member = server::SlangDoc::fromText(driver, workspace.uri("includes/member.svh"),
                                             "typedef `FIELD_TYPE updated_t;\n");
    auto changed = doc->getSyntaxTree();
    CHECK(changed != original);
    CHECK(changed->isLibraryUnit);
    CHECK(changed->getSourceLibrary() == original->getSourceLibrary());
    const auto& changedOptions =
        changed->options().getOrDefault<slang::parsing::PreprocessorOptions>();
    CHECK(changedOptions.predefines == options.predefines);
    CHECK(changedOptions.additionalIncludePaths == options.additionalIncludePaths);
    CHECK(aliasWidth(changed, "p", "updated_t") == 64);
    CHECK(member->textMatches("typedef `FIELD_TYPE updated_t;\n"));

    doc->invalidateSyntax();
    CHECK(aliasWidth(doc->getSyntaxTree(), "p", "updated_t") == 64);
}

TEST_CASE("Native unit recipe survives editor replacement of its primary document",
          "[native-unit]") {
    UnitWorkspace workspace;
    workspace.write("preamble.sv", "`define FIELD_TYPE int\n");
    workspace.write("owner.sv", "package p; typedef `FIELD_TYPE field_t; endpackage\n");
    workspace.write("native.f", "preamble.sv\nowner.sv\n");
    UnitDriver fixture(workspace,
                       "--single-unit -F \"" + workspace.path("native.f").string() + "\"");
    auto& driver = *fixture.driver;
    auto recipe = driver.docs.at(workspace.uri("owner.sv"));
    auto editor = server::SlangDoc::fromText(
        driver, workspace.uri("owner.sv"), "package p; typedef `FIELD_TYPE editor_t; endpackage\n");
    driver.docs[workspace.uri("owner.sv")] = editor;
    CHECK_FALSE(editor->isNativeCompilationUnit());
    CHECK(recipe->isNativeCompilationUnit());
    CHECK(aliasWidth(recipe->getSyntaxTree(), "p", "editor_t") == 32);
    CHECK(recipe->getText() == editor->getText());
}

TEST_CASE("Native inherited macro consumers require SourceLoader replay after invalidation",
          "[native-unit]") {
    UnitWorkspace workspace;
    workspace.write("source.sv", "`define FIELD_TYPE shortint\n");
    workspace.write("consumer.sv", "package p; typedef `FIELD_TYPE field_t; endpackage\n");
    workspace.write("consumer.f", "consumer.sv\n");
    workspace.write("native.f", "source.sv\n-C consumer.f\n");
    UnitDriver fixture(workspace,
                       "--single-unit -F \"" + workspace.path("native.f").string() + "\"");
    auto& driver = *fixture.driver;
    auto doc = driver.docs.at(workspace.uri("consumer.sv"));
    REQUIRE(doc->requiresNativeReplay());
    CHECK(aliasWidth(doc->getSyntaxTree(), "p", "field_t") == 16);
    replaceText(*doc, "package p; typedef `FIELD_TYPE changed_t; endpackage\n");
    CHECK(aliasWidth(doc->getSyntaxTree(), "p", "changed_t") == 16);

    auto source = server::SlangDoc::fromText(driver, workspace.uri("source.sv"),
                                             "`define FIELD_TYPE byte\n");
    CHECK(aliasWidth(doc->getSyntaxTree(), "p", "changed_t") == 8);
    CHECK(driver.docs.at(workspace.uri("consumer.sv")) == doc);
    CHECK(source->textMatches("`define FIELD_TYPE byte\n"));
}

TEST_CASE("Native replay requirement survives when inherited macros leave no provenance",
          "[native-unit]") {
    UnitWorkspace workspace;
    workspace.write("source.sv", "`define UNUSED_INPUT\n");
    workspace.write("consumer.sv", "`undefineall\npackage p; typedef int field_t; endpackage\n");
    workspace.write("consumer.f", "consumer.sv\n");
    workspace.write("native.f", "source.sv\n-C consumer.f\n");
    UnitDriver fixture(workspace,
                       "--single-unit -F \"" + workspace.path("native.f").string() + "\"");
    auto& driver = *fixture.driver;
    auto original = driver.docs.at(workspace.uri("consumer.sv"))->getSyntaxTree();
    auto doc = server::SlangDoc::fromTree(driver, original, true);
    REQUIRE(doc->requiresNativeReplay());
    CHECK(aliasWidth(doc->getSyntaxTree(), "p", "field_t") == 32);
    doc->invalidateSyntax();
    CHECK(aliasWidth(doc->getSyntaxTree(), "p", "field_t") == 32);
    CHECK(doc->requiresNativeReplay());
}

TEST_CASE("Native inherited replay preserves unit defines includes and named library",
          "[native-unit][native-replay]") {
    UnitWorkspace workspace;
    workspace.write("producer.sv", "`define FIELD_TYPE shortint\n");
    workspace.write("include/member.svh", "typedef `FIELD_TYPE field_t;\n");
    workspace.write("consumer.sv", "package p;\n`include \"member.svh\"\n"
                                  "typedef bit[`LOCAL_WIDTH-1:0] local_t; endpackage\n");
    workspace.write("consumer.f", "+define+LOCAL_WIDTH=7\n+incdir+include\n"
                                  "--library inherited_lib\nconsumer.sv\n");
    workspace.write("native.f", "producer.sv\n-C consumer.f\n");
    UnitDriver fixture(workspace, "--single-unit --libraries-inherit-macros -F \"" +
                                     workspace.path("native.f").string() + "\"");
    auto& driver = *fixture.driver;
    auto document = driver.docs.at(workspace.uri("consumer.sv"));
    REQUIRE(document->requiresNativeReplay());
    auto original = document->getSyntaxTree();
    REQUIRE(original->getSourceLibrary());
    CHECK(aliasWidth(original, "p", "field_t") == 16);
    CHECK(aliasWidth(original, "p", "local_t") == 7);
    auto member = server::SlangDoc::fromText(driver, workspace.uri("include/member.svh"),
                                             "typedef `FIELD_TYPE changed_t;\n");
    auto changed = document->getSyntaxTree();
    CHECK(changed->getSourceLibrary() == original->getSourceLibrary());
    CHECK(changed->isLibraryUnit == original->isLibraryUnit);
    CHECK(aliasWidth(changed, "p", "changed_t") == 16);
    CHECK(aliasWidth(changed, "p", "local_t") == 7);
    auto producer = server::SlangDoc::fromText(driver, workspace.uri("producer.sv"),
                                               "`define FIELD_TYPE byte\n");
    auto latest = document->getSyntaxTree();
    CHECK(aliasWidth(latest, "p", "changed_t") == 8);
    CHECK(aliasWidth(latest, "p", "local_t") == 7);
    CHECK(latest->getSourceLibrary() == original->getSourceLibrary());
    CHECK(driver.docs.at(workspace.uri("consumer.sv")) == document);
    CHECK(member->textMatches("typedef `FIELD_TYPE changed_t;\n"));
    CHECK(producer->textMatches("`define FIELD_TYPE byte\n"));
}

TEST_CASE("Cancelled Native replay keeps published recipes and can retry",
          "[native-unit][native-replay][cancellation]") {
    const std::string boundary = GENERATE("before configured syntax parsing",
                                          "during disposable syntax parsing",
                                          "after configured syntax parsing");
    CAPTURE(boundary);
    UnitWorkspace workspace;
    workspace.write("producer.sv", "`define FIELD_TYPE shortint\n");
    std::string text = "package p; typedef `FIELD_TYPE field_t;\n";
    for (int i = 0; i < 1500; ++i)
        text += "int padding_" + std::to_string(i) + ";\n";
    text += "endpackage\n";
    workspace.write("consumer.sv", text);
    workspace.write("consumer.f", "consumer.sv\n");
    workspace.write("native.f", "producer.sv\n-C consumer.f\n");
    UnitDriver fixture(workspace, "--single-unit --threads 1 -F \"" +
                                     workspace.path("native.f").string() + "\"", {}, false, true);
    auto& driver = *fixture.driver;
    auto document = driver.docs.at(workspace.uri("consumer.sv"));
    const auto original = document->getSyntaxTree();
    const auto previousTrees = driver.driver.syntaxTrees;
    text.replace(text.find("field_t"), 7, "changed_t");
    replaceText(*document, text);
    bool cancelled = false;
    lsp::RequestContext ctx("native replay", lsp::ID_t{970});
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == boundary) {
            cancelled = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(document->getSyntaxTree(ctx), lsp::RequestCancelled);
    REQUIRE(cancelled);
    CHECK(driver.driver.syntaxTrees == previousTrees);
    CHECK(document->getCachedSyntaxTree() == nullptr);
    CHECK(driver.docs.at(workspace.uri("consumer.sv")) == document);
    // Exercise old borrowed tokens under allocation churn after cancellation;
    // the published tree must keep its characters independently of the document.
    std::vector<slang::SmallVector<char>> churn;
    for (int i = 0; i < 32; ++i) {
        slang::SmallVector<char> bytes;
        bytes.resize(text.size() - 1); // original spelling is two characters shorter
        std::fill(bytes.begin(), bytes.end(), 'x');
        churn.push_back(std::move(bytes));
    }
    CHECK(aliasWidth(original, "p", "field_t") == 16);
    CHECK(aliasWidth(document->getSyntaxTree(), "p", "changed_t") == 16);
    CHECK(document->textMatches(text));
}

TEST_CASE("Fast cwd preserves native filelist single-unit macro order after unsaved edits",
          "[fast-cwd]") {
    UnitWorkspace workspace;
    workspace.write("preamble.sv", "`define FIELD_TYPE byte\n");
    workspace.write("owner.sv", "package p; typedef `FIELD_TYPE field_t; endpackage\n");
    workspace.write("poison.svh", "`define FIELD_TYPE bit\n");
    workspace.write("nested/native.f", "../preamble.sv\n../owner.sv\n");
    workspace.write("native.f", "-F nested/native.f\n");
    for (bool fast : {false, true}) {
        UnitDriver fixture(workspace,
                           "--threads 4 --single-unit -F \"" + workspace.path("native.f").string() +
                               "\"",
                           {workspace.path("poison.svh").string()}, fast);
        auto& driver = *fixture.driver;
        auto doc = driver.docs.at(workspace.uri("owner.sv"));
        REQUIRE(doc->isNativeCompilationUnit());
        CHECK(aliasWidth(doc->getSyntaxTree(), "p", "field_t") == 8);
        auto preamble = server::SlangDoc::fromText(driver, workspace.uri("preamble.sv"),
                                                   "`define FIELD_TYPE shortint\n");
        CHECK(aliasWidth(doc->getSyntaxTree(), "p", "field_t") == 16);
        replaceText(*doc, "package p; typedef `FIELD_TYPE edited_t; endpackage\n");
        CHECK(aliasWidth(doc->getSyntaxTree(), "p", "edited_t") == 16);
        CHECK(preamble->textMatches("`define FIELD_TYPE shortint\n"));
    }
}

TEST_CASE("Configured package includes retain the filelist provider across edits",
          "[native-unit][configured-package-provider]") {
    UnitWorkspace workspace;
    workspace.write("vendor/base_pkg.sv",
                    "package base_pkg; class Base; int inherited; endclass endpackage\n");
    workspace.write("env/base_pkg.sv", "package base_pkg; int wrong; endpackage\n");
    workspace.write("env/env_pkg.sv", "package env_pkg; import base_pkg::*;\n"
                                      "`include \"item.svh\"\nendpackage\n");
    workspace.write("env/item.svh", "class Item extends Base; int own; endclass\n");
    workspace.write("top.sv", "module top; env_pkg::Item item; endmodule\n");
    workspace.write("design.f", "vendor/base_pkg.sv\nenv/env_pkg.sv\ntop.sv\n");
    Config config;
    config.flagsByFile = std::vector<Config::FlagSource>{
        {workspace.path("server.json").string(), "--single-unit --threads 1 --top top"}};
    Indexer indexer;
    ClientHarness client;
    auto driver = server::ServerDriver::createFromFileLists(indexer, client, config, {"design.f"},
                                                            workspace.root.string(), nullptr, true);
    REQUIRE(driver);
    driver->getDocument(workspace.uri("env/base_pkg.sv"))->getSyntaxTree();
    for (int version = 1; version <= 6; ++version) {
        auto text = "class Item extends Base; int own" + std::to_string(version) + "; endclass\n";
        if (version == 1)
            driver->openDocument(workspace.uri("env/item.svh"), text, version, true);
        else
            driver->onDocDidChange(
                {.textDocument = {.version = version, .uri = workspace.uri("env/item.svh")},
                 .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}});
        auto compilation =
            driver->getDocument(workspace.uri("env/item.svh"))->getAnalysis()->getCompilation();
        auto package = compilation->getPackage("env_pkg");
        REQUIRE(package);
        auto item = package->find("Item");
        REQUIRE(item);
        auto base = item->as<slang::ast::ClassType>().getBaseClass();
        REQUIRE(base);
        REQUIRE(base->isClass());
        CHECK(base->as<slang::ast::ClassType>().find("inherited"));
        CHECK(item->as<slang::ast::ClassType>().find("own" + std::to_string(version)));
        CHECK(compilation->getPackage("base_pkg")->find("wrong") == nullptr);
    }
}
