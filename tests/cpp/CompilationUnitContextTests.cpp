// SPDX-License-Identifier: MIT
#include "Indexer.h"
#include "ServerDriver.h"
#include "catch2/matchers/catch_matchers.hpp"
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
               std::vector<std::string> preincludes = {}, bool fastCwd = false) {
        config.flagsByFile = std::vector<Config::FlagSource>{
            {workspace.path("server.json").string(), std::move(flags)}};
        config.explorePreincludeFiles = std::move(preincludes);
        config.fastCwdSnapshotChecks = fastCwd;
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
    CHECK_THROWS_WITH(
        doc->getSyntaxTree(),
        "Native compilation unit requires SourceLoader replay to preserve preprocessing context");

    auto source = server::SlangDoc::fromText(driver, workspace.uri("source.sv"),
                                             "`define FIELD_TYPE byte\n");
    REQUIRE(driver.driver.parseAllSources());
    auto consumer = std::find_if(
        driver.driver.syntaxTrees.begin(), driver.driver.syntaxTrees.end(), [&](const auto& tree) {
            return driver.sm.getFullPath(tree->getSourceBufferIds().front()) ==
                   workspace.path("consumer.sv");
        });
    REQUIRE(consumer != driver.driver.syntaxTrees.end());
    auto replayed = server::SlangDoc::fromTree(driver, *consumer, true);
    CHECK(aliasWidth(replayed->getSyntaxTree(), "p", "changed_t") == 8);
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
    CHECK_THROWS_WITH(
        doc->getSyntaxTree(),
        "Native compilation unit requires SourceLoader replay to preserve preprocessing context");
}

TEST_CASE("Fast cwd preserves native filelist single-unit macro order after unsaved edits", "[fast-cwd]") {
    UnitWorkspace workspace;
    workspace.write("preamble.sv", "`define FIELD_TYPE byte\n");
    workspace.write("owner.sv", "package p; typedef `FIELD_TYPE field_t; endpackage\n");
    workspace.write("poison.svh", "`define FIELD_TYPE bit\n");
    workspace.write("nested/native.f", "../preamble.sv\n../owner.sv\n");
    workspace.write("native.f", "-F nested/native.f\n");
    for (bool fast : {false, true}) {
        UnitDriver fixture(workspace,
            "--threads 4 --single-unit -F \"" + workspace.path("native.f").string() + "\"",
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
