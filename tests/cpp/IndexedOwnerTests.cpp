// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "Indexer.h"
#include "utils/ServerHarness.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {
namespace fs = std::filesystem;

// Declare this BEFORE ServerHarness so every source survives the harness teardown.
// create_directory, rather than a shared fixed path, establishes exclusive ownership.
class OwnerWorkspace {
public:
    OwnerWorkspace() {
        static std::atomic_uint64_t serial{0};
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (;;) {
            auto candidate = fs::temp_directory_path() /
                             ("slang-indexed-owner-" + std::to_string(stamp) + "-" +
                              std::to_string(serial++));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                root = fs::canonical(candidate);
                break;
            }
            REQUIRE_FALSE(error);
        }
    }

    ~OwnerWorkspace() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    OwnerWorkspace(const OwnerWorkspace&) = delete;
    OwnerWorkspace& operator=(const OwnerWorkspace&) = delete;

    fs::path path(const fs::path& relative) const { return root / relative; }
    URI uri(const fs::path& relative) const { return URI::fromFile(path(relative)); }

    void write(const fs::path& relative, std::string_view text) const {
        fs::create_directories(path(relative).parent_path());
        std::ofstream output(path(relative), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << text;
        output.close();
        REQUIRE(output.good());
    }

    void remove(const fs::path& relative) const { REQUIRE(fs::remove(path(relative))); }

    lsp::InitializeParams params() const {
        return lsp::InitializeParams{
            .workspaceFolders = {
                {lsp::WorkspaceFolder{.uri = URI::fromFile(root), .name = "indexed-owner-test"}}}};
    }

    fs::path root;
};

const std::string itemText = "class item;\n"
                             "    value_t value;\n"
                             "    function value_t get();\n"
                             "        return value;\n"
                             "    endfunction\n"
                             "endclass\n";

std::string ownerText(std::string_view name = "p", std::string_view include = "item.svh",
                      std::string_view type = "int") {
    return "package " + std::string(name) + ";\n    typedef " + std::string(type) +
           " value_t;\n    `include \"" + std::string(include) + "\"\nendpackage\n";
}

lsp::Position positionOf(std::string_view text, std::string_view needle) {
    auto offset = text.find(needle);
    REQUIRE(offset != std::string_view::npos);
    lsp::Position position{};
    for (size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') {
            ++position.line;
            position.character = 0;
        }
        else {
            ++position.character;
        }
    }
    return position;
}

bool hasDiagnostic(DocumentHandle& document, std::string_view needle) {
    return std::ranges::any_of(document.getDiagnostics(), [&](const auto& diagnostic) {
        return diagnostic.message.find(needle) != std::string::npos;
    });
}

lsp::DidChangeWatchedFilesParams changed(const OwnerWorkspace& workspace, const fs::path& relative,
                                         lsp::FileChangeType type) {
    return {.changes = {{lsp::FileEvent{.uri = workspace.uri(relative), .type = type}}}};
}

void checkOwner(DocumentHandle& header, const OwnerWorkspace& workspace, const fs::path& owner,
                std::string_view ownerSource) {
    auto definitions = header.before("value_t value").getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == workspace.uri(owner));
    CHECK(definitions.front().targetSelectionRange.start == positionOf(ownerSource, "value_t;"));
}

std::string budgetWarning(const DocumentHandle& header) {
    return "Package owner discovery limit reached for " +
           std::string(header.doc->getURI().getPath()) +
           ". Open the owning package; semantic diagnostics are deferred.";
}

void indexScope(Indexer& indexer, const fs::path& path) {
    auto scope = path.string();
    indexer.startIndexing(std::vector<Config::IndexConfig>{}, scope);
}

void checkOneCandidate(const Indexer& indexer, const fs::path& header, const fs::path& owner) {
    auto candidates = indexer.getPackageOwnerCandidates(header);
    CHECK(candidates.revision == indexer.getRevision());
    REQUIRE_FALSE(candidates.limitExceeded);
    REQUIRE(candidates.paths.size() == 1);
    CHECK(candidates.paths.front() == owner);
}
} // namespace

TEST_CASE("Indexed owner supplies header-only semantics and unsaved positions",
          "[package-header][indexed-owner]") {
    OwnerWorkspace workspace;
    auto package = ownerText();
    workspace.write("owner.sv", package);
    workspace.write("item.svh", itemText);
    ServerHarness server(workspace.params());
    // Always use the two-argument overload: a header's standalone outline may be empty.
    auto header = server.openFile("item.svh", itemText);

    CHECK_FALSE(server.m_driver->isDocumentOpen(workspace.uri("owner.sv")));
    REQUIRE(server.m_driver->comp == nullptr);
    checkOwner(header, workspace, "owner.sv", package);
    CHECK(header.getDiagnostics().empty());
    CHECK(header.getHoverAt(header.before("value_t value").m_offset).has_value());

    auto trees = header.doc->getAnalysis()->getCompilation()->getSyntaxTrees();
    REQUIRE(trees.size() == 1);
    CHECK(server.sourceManager().getFullPath(trees.front()->getSourceBufferIds().front()) ==
          workspace.path("owner.sv"));

    auto attempts = server.m_driver->getOwnerDiscoveryAttemptCount();
    for (int i = 0; i < 8; ++i) {
        checkOwner(header, workspace, "owner.sv", package);
        (void)header.getHoverAt(header.before("value_t value").m_offset);
    }
    CHECK(server.m_driver->getOwnerDiscoveryAttemptCount() == attempts);
    header.replaceAll("\n\n// Unsaved prefix shifts all header-local locations.\n" + itemText);
    header.publishChanges();
    checkOwner(header, workspace, "owner.sv", package);
    auto member = header.after("return ").getDefinitions();
    REQUIRE(member.size() == 1);
    CHECK(member.front().targetUri == header.doc->getURI());
    CHECK(member.front().targetSelectionRange.start == header.before("value;").getPosition());
    auto completions = header.after("return ").getResolvedCompletions();
    CHECK(std::ranges::any_of(completions, [](const auto& item) { return item.label == "value"; }));

    auto broken = header.getText();
    broken.replace(broken.find("value_t value"), std::string("value_t").size(), "missing_t");
    header.replaceAll(broken);
    header.publishChanges();
    CHECK(hasDiagnostic(header, "missing_t"));
    header.replaceAll(itemText);
    header.publishChanges();
    CHECK(header.getDiagnostics().empty());
    checkOwner(header, workspace, "owner.sv", package);
    CHECK(server.m_driver->getOwnerDiscoveryAttemptCount() == attempts);
}

TEST_CASE("Indexed nested owner validates basename collisions by real parsing",
          "[package-header][indexed-owner]") {
    OwnerWorkspace workspace;
    auto package = ownerText("good_pkg", "nested/wrapper.svh");
    workspace.write("good.sv", package);
    workspace.write("nested/wrapper.svh", "`include \"item.svh\"\n");
    workspace.write("nested/item.svh", itemText);
    workspace.write("other/item.svh", itemText);
    workspace.write("bad.sv", ownerText("wrong_pkg", "other/item.svh", "bit"));
    ServerHarness server(workspace.params());

    // Basename lookup intentionally over-approximates: both roots must be considered.
    auto candidates = server.m_indexer.getPackageOwnerCandidates(workspace.path("nested/item.svh"));
    REQUIRE_FALSE(candidates.limitExceeded);
    REQUIRE(candidates.paths.size() == 2);
    auto header = server.openFile("nested/item.svh", itemText);
    checkOwner(header, workspace, "good.sv", package);
    CHECK_FALSE(server.m_driver->isPackageContextDeferred(header.doc->getURI()));
    CHECK(header.getDiagnostics().empty());
    CHECK_FALSE(server.m_driver->isDocumentOpen(workspace.uri("good.sv")));
    CHECK_FALSE(server.m_driver->isDocumentOpen(workspace.uri("bad.sv")));
}

TEST_CASE("Multiple indexed owners default deterministically and honor explicit selection",
          "[package-header][indexed-owner]") {
    OwnerWorkspace workspace;
    auto first = ownerText("first_pkg");
    auto second = ownerText("second_pkg", "item.svh", "bit");
    workspace.write("first.sv", first);
    workspace.write("second.sv", second);
    workspace.write("item.svh", itemText);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", itemText);
    CHECK_FALSE(server.m_driver->isPackageContextDeferred(header.doc->getURI()));
    checkOwner(header, workspace, "first.sv", first);
    CHECK(header.getDiagnostics().empty());
    auto attempts = server.m_driver->getOwnerDiscoveryAttemptCount();
    for (int i = 0; i < 3; ++i) {
        checkOwner(header, workspace, "first.sv", first);
        server.m_driver->refreshOpenIncludeContexts();
    }
    CHECK(server.m_driver->getOwnerDiscoveryAttemptCount() == attempts);
    SECTION("An explicit include selection changes the package") {
        REQUIRE(server.m_driver->setIncludeContext(
            {.uri = header.m_uri, .source = workspace.uri("second.sv")}));
        checkOwner(header, workspace, "second.sv", second);
        CHECK(header.getDiagnostics().empty());
    }
    SECTION("Changing the header into standalone RTL keeps diagnostics available") {
        header.replaceAll("module standalone;\n    missing_t value;\nendmodule\n");
        header.publishChanges();
        CHECK_FALSE(server.m_driver->isPackageContextDeferred(header.doc->getURI()));
        CHECK(hasDiagnostic(header, "missing_t"));
        auto trees = header.doc->getAnalysis()->getCompilation()->getSyntaxTrees();
        REQUIRE(trees.size() == 1);
        CHECK(trees.front() == header.doc->getSyntaxTree());
    }
}

TEST_CASE("Owner candidate budget avoids root parsing and recovers after index removal",
          "[package-header][indexed-owner]") {
    OwnerWorkspace workspace;
    workspace.write("item.svh", itemText);
    for (size_t i = 0; i <= Indexer::MaxOwnerCandidates; ++i) {
        workspace.write("owner_" + std::to_string(i) + ".sv", ownerText("p" + std::to_string(i)));
    }
    ServerHarness server(workspace.params());
    auto candidates = server.m_indexer.getPackageOwnerCandidates(workspace.path("item.svh"));
    REQUIRE(candidates.limitExceeded);
    auto header = server.openFile("item.svh", itemText);
    server.client.expectWarning(budgetWarning(header));
    REQUIRE(server.m_driver->isPackageContextDeferred(header.doc->getURI()));
    CHECK(header.getDiagnostics().empty());
    for (size_t i = 0; i <= Indexer::MaxOwnerCandidates; ++i) {
        auto file = "owner_" + std::to_string(i) + ".sv";
        CHECK_FALSE(server.m_driver->docs.contains(workspace.uri(file)));
        CHECK_FALSE(server.sourceManager().isCached(workspace.path(file)));
    }
    auto attempts = server.m_driver->getOwnerDiscoveryAttemptCount();
    CHECK(header.before("value_t value").getDefinitions().empty());
    server.m_driver->refreshOpenIncludeContexts();
    CHECK(server.m_driver->getOwnerDiscoveryAttemptCount() == attempts);

    // One batch means one new index revision and no transient repeated budget warnings.
    lsp::DidChangeWatchedFilesParams deleted;
    for (size_t i = 1; i <= Indexer::MaxOwnerCandidates; ++i) {
        auto file = "owner_" + std::to_string(i) + ".sv";
        workspace.remove(file);
        deleted.changes.push_back(
            {.uri = workspace.uri(file), .type = lsp::FileChangeType::Deleted});
    }
    auto revision = server.m_indexer.getRevision();
    server.onWorkspaceDidChangeWatchedFiles(deleted);
    header.doc = server.getDoc(header.m_uri);
    CHECK(server.m_indexer.getRevision() > revision);
    CHECK(server.m_driver->getOwnerDiscoveryAttemptCount() > attempts);
    CHECK_FALSE(server.m_driver->isPackageContextDeferred(header.doc->getURI()));
    checkOwner(header, workspace, "owner_0.sv", ownerText("p0"));
    CHECK(header.getDiagnostics().empty());
}

TEST_CASE("Closed indexed owner changes deletion and recreation refresh an open header",
          "[package-header][indexed-owner]") {
    OwnerWorkspace workspace;
    auto initial = ownerText();
    workspace.write("owner.sv", initial);
    workspace.write("item.svh", itemText);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", itemText);
    checkOwner(header, workspace, "owner.sv", initial);
    REQUIRE_FALSE(server.m_driver->isDocumentOpen(workspace.uri("owner.sv")));

    auto replacement = "// New disk contents, including a shifted typedef.\n\n" +
                       ownerText("p", "item.svh", "bit");
    workspace.write("owner.sv", replacement);
    server.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "owner.sv", lsp::FileChangeType::Changed));
    checkOwner(header, workspace, "owner.sv", replacement);
    CHECK(header.getDiagnostics().empty());

    header.replaceAll("// Unsaved text must survive a SourceManager replacement.\n" + itemText);
    header.publishChanges();
    workspace.remove("owner.sv");
    server.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "owner.sv", lsp::FileChangeType::Deleted));
    // Cached-source deletion can replace the driver and its SourceManager.
    header.doc = server.getDoc(header.m_uri);
    CHECK(header.doc->getText().starts_with("// Unsaved text must survive"));
    CHECK(header.before("value_t value").getDefinitions().empty());
    CHECK(hasDiagnostic(header, "value_t"));
    CHECK_FALSE(server.m_driver->isPackageContextDeferred(header.doc->getURI()));
    auto attempts = server.m_driver->getOwnerDiscoveryAttemptCount();
    (void)header.getHoverAt(header.before("value_t value").m_offset);
    server.m_driver->refreshOpenIncludeContexts();
    CHECK(server.m_driver->getOwnerDiscoveryAttemptCount() == attempts);

    workspace.write("owner.sv", initial);
    server.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "owner.sv", lsp::FileChangeType::Created));
    checkOwner(header, workspace, "owner.sv", initial);
    CHECK(header.getDiagnostics().empty());
    CHECK_FALSE(server.m_driver->isDocumentOpen(workspace.uri("owner.sv")));
}

TEST_CASE("Deleted sibling include cannot retain its cached typedef",
          "[package-header][indexed-owner]") {
    auto recreatedType = lsp::FileChangeType::Created;
    SECTION("Explicit creation event") {
    }
    SECTION("Client coalesces create and write into Changed") {
        recreatedType = lsp::FileChangeType::Changed;
    }
    OwnerWorkspace workspace;
    workspace.write("owner.sv",
                    "package p;\n`include \"types.svh\"\n`include \"item.svh\"\nendpackage\n");
    workspace.write("types.svh", "typedef int value_t;\n");
    workspace.write("item.svh", itemText);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", itemText);
    auto definitions = header.before("value_t value").getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == workspace.uri("types.svh"));
    CHECK(header.getDiagnostics().empty());
    REQUIRE(server.sourceManager().isCached(workspace.path("types.svh")));
    REQUIRE_FALSE(server.m_driver->isDocumentOpen(workspace.uri("types.svh")));

    workspace.remove("types.svh");
    server.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "types.svh", lsp::FileChangeType::Deleted));
    header.doc = server.getDoc(header.m_uri);
    CHECK(header.before("value_t value").getDefinitions().empty());
    CHECK(hasDiagnostic(header, "value_t"));

    workspace.write("types.svh", "// Replacement include\ntypedef bit value_t;\n");
    server.onWorkspaceDidChangeWatchedFiles(changed(workspace, "types.svh", recreatedType));
    header.doc = server.getDoc(header.m_uri);
    definitions = header.before("value_t value").getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == workspace.uri("types.svh"));
    CHECK(definitions.front().targetSelectionRange.start.line == 1);
    CHECK(header.getDiagnostics().empty());
}

TEST_CASE("Editor-owned standalone RTL republishes diagnostics beside package includes",
          "[package-header][indexed-owner]") {
    OwnerWorkspace workspace;
    const std::string clean = "module top;\nendmodule\n";
    const std::string broken = "module top;\n    missing_t value;\nendmodule\n";
    workspace.write("top.sv", clean);
    // Deliberately global: this must not confer package ownership.
    workspace.write("owner.sv",
                    "package p; typedef int value_t; endpackage\n`include \"top.sv\"\n");
    ServerHarness server(workspace.params());
    auto rtl = server.openFile("top.sv", clean);
    REQUIRE(rtl.getDiagnostics().empty());
    auto publications = server.client.diagnosticPublications.size();

    workspace.write("top.sv", broken);
    server.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "top.sv", lsp::FileChangeType::Changed));
    // Main keeps open editor text authoritative across disk notifications.
    CHECK(rtl.doc->getText() == clean + '\0');
    CHECK_FALSE(hasDiagnostic(rtl, "missing_t"));
    rtl.replaceAll(broken);
    rtl.publishChanges();
    CHECK(hasDiagnostic(rtl, "missing_t"));
    CHECK(server.client.diagnosticPublications.size() > publications);
    auto trees = rtl.doc->getAnalysis()->getCompilation()->getSyntaxTrees();
    REQUIRE(trees.size() == 1);
    CHECK(trees.front() == rtl.doc->getSyntaxTree());
    CHECK_FALSE(server.m_driver->isPackageContextDeferred(rtl.doc->getURI()));

    workspace.write("top.sv", clean);
    server.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "top.sv", lsp::FileChangeType::Changed));
    CHECK(rtl.doc->getText() == broken + '\0');
    rtl.replaceAll(clean);
    rtl.publishChanges();
    CHECK(rtl.getDiagnostics().empty());
}

TEST_CASE("Index replacement removes old owner edges and repeated updates stay unique",
          "[indexer][indexed-owner]") {
    OwnerWorkspace workspace;
    workspace.write("a.svh", "// First include\n");
    workspace.write("b.svh", "// Second include\n");
    workspace.write("owner.sv", ownerText("old_pkg", "a.svh"));
    Indexer indexer;
    indexer.setNumThreads(1);
    indexScope(indexer, workspace.root);
    checkOneCandidate(indexer, workspace.path("a.svh"), workspace.path("owner.sv"));

    auto revision = indexer.getRevision();
    for (int i = 0; i < 3; ++i) {
        indexer.addDocuments({workspace.path("owner.sv"), workspace.path("owner.sv")});
        REQUIRE(indexer.getFilesForSymbol("old_pkg").size() == 1);
        checkOneCandidate(indexer, workspace.path("a.svh"), workspace.path("owner.sv"));
    }
    CHECK(indexer.getRevision() > revision);

    workspace.write("owner.sv", ownerText("new_pkg", "b.svh"));
    indexer.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "owner.sv", lsp::FileChangeType::Changed));
    CHECK(indexer.getPackageOwnerCandidates(workspace.path("a.svh")).paths.empty());
    CHECK(indexer.getFilesForSymbol("old_pkg").empty());
    REQUIRE(indexer.getFilesForSymbol("new_pkg").size() == 1);
    checkOneCandidate(indexer, workspace.path("b.svh"), workspace.path("owner.sv"));

    // Exercise the saved-document path, not only the lightweight on-disk parser.
    slang::SourceManager sourceManager;
    auto parsed = slang::syntax::SyntaxTree::fromFile(workspace.path("owner.sv").string(),
                                                      sourceManager);
    REQUIRE(parsed);
    indexer.updateDocument(workspace.path("owner.sv"), **parsed);
    indexer.updateDocument(workspace.path("owner.sv"), **parsed);
    REQUIRE(indexer.getFilesForSymbol("new_pkg").size() == 1);
    checkOneCandidate(indexer, workspace.path("b.svh"), workspace.path("owner.sv"));

    // Saved syntax trees contain transitive includes. Only a file's own directives
    // may be indexed as edges, or deleting an intermediate header leaves a shortcut.
    workspace.write("wrapper.svh", "`include \"b.svh\"\n");
    workspace.write("owner.sv", ownerText("new_pkg", "wrapper.svh"));
    indexer.addDocuments({workspace.path("wrapper.svh")});
    slang::SourceManager nestedSourceManager;
    auto nested = slang::syntax::SyntaxTree::fromFile(workspace.path("owner.sv").string(),
                                                      nestedSourceManager);
    REQUIRE(nested);
    indexer.updateDocument(workspace.path("owner.sv"), **nested);
    checkOneCandidate(indexer, workspace.path("b.svh"), workspace.path("owner.sv"));
    workspace.remove("wrapper.svh");
    indexer.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "wrapper.svh", lsp::FileChangeType::Deleted));
    CHECK(indexer.getPackageOwnerCandidates(workspace.path("b.svh")).paths.empty());

    workspace.remove("owner.sv");
    indexer.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "owner.sv", lsp::FileChangeType::Deleted));
    CHECK(indexer.getPackageOwnerCandidates(workspace.path("b.svh")).paths.empty());
    CHECK(indexer.getFilesForSymbol("new_pkg").empty());
}

TEST_CASE("Reindexing a narrower scope purges stale owner candidates", "[indexer][indexed-owner]") {
    OwnerWorkspace workspace;
    workspace.write("old/owner.sv", ownerText("old_pkg", "item.svh"));
    workspace.write("old/item.svh", itemText);
    workspace.write("current/owner.sv", ownerText("current_pkg", "item.svh"));
    workspace.write("current/item.svh", itemText);
    Indexer indexer;
    indexer.setNumThreads(1);
    indexScope(indexer, workspace.root);
    REQUIRE(indexer.getPackageOwnerCandidates(workspace.path("current/item.svh")).paths.size() ==
            2);
    auto revision = indexer.getRevision();

    indexScope(indexer, workspace.path("current"));
    CHECK(indexer.getRevision() > revision);
    CHECK(indexer.getFilesForSymbol("old_pkg").empty());
    checkOneCandidate(indexer, workspace.path("current/item.svh"),
                      workspace.path("current/owner.sv"));

    indexer.startIndexing(std::vector<std::string>{}, {});
    CHECK(indexer.getPackageOwnerCandidates(workspace.path("current/item.svh")).paths.empty());
    CHECK(indexer.getSymbolCount() == 0);
}

TEST_CASE("Owner reverse graph handles cycles and enforces its file budget",
          "[indexer][indexed-owner]") {
    OwnerWorkspace workspace;
    Indexer indexer;
    indexer.setNumThreads(1);
    SECTION("Cycles terminate without duplicating roots") {
        workspace.write("leaf.svh", "// Target\n");
        workspace.write("a.svh", "`include \"b.svh\"\n");
        workspace.write("b.svh", "`include \"a.svh\"\n`include \"leaf.svh\"\n");
        workspace.write("owner.sv", ownerText("p", "a.svh"));
        indexScope(indexer, workspace.root);
        checkOneCandidate(indexer, workspace.path("leaf.svh"), workspace.path("owner.sv"));
        checkOneCandidate(indexer, workspace.path("a.svh"), workspace.path("owner.sv"));
    }
    SECTION("The initial header counts toward the graph budget") {
        workspace.write("leaf.svh", "// Target\n");
        std::string previous = "leaf.svh";
        for (size_t i = 0; i < Indexer::MaxOwnerGraphFiles - 1; ++i) {
            auto file = "wrapper_" + std::to_string(i) + ".svh";
            workspace.write(file, "`include \"" + previous + "\"\n");
            previous = file;
        }
        workspace.write("owner.sv", ownerText("p", previous));
        indexScope(indexer, workspace.root);
        checkOneCandidate(indexer, workspace.path("leaf.svh"), workspace.path("owner.sv"));

        workspace.write("one_more.svh", "`include \"" + previous + "\"\n");
        indexer.addDocuments({workspace.path("one_more.svh")});
        auto overBudget = indexer.getPackageOwnerCandidates(workspace.path("leaf.svh"));
        CHECK(overBudget.limitExceeded);
        CHECK(overBudget.paths.empty());
        CHECK(overBudget.revision == indexer.getRevision());

        workspace.remove("one_more.svh");
        indexer.onWorkspaceDidChangeWatchedFiles(
            changed(workspace, "one_more.svh", lsp::FileChangeType::Deleted));
        checkOneCandidate(indexer, workspace.path("leaf.svh"), workspace.path("owner.sv"));
    }
}

TEST_CASE("Unsaved sibling typedef edits republish header diagnostics",
          "[package-header][indexed-owner]") {
    OwnerWorkspace workspace;
    workspace.write("owner.sv", "package p;\n`include \"types.svh\"\n`include \"item.svh\"\n"
                                "`include \"deleted.svh\"\nendpackage\n");
    workspace.write("deleted.svh", "// Cached sibling\n");
    workspace.write("types.svh", "typedef int value_t;\n");
    workspace.write("item.svh", itemText);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", itemText);
    auto types = server.openFile("types.svh", "typedef int value_t;\n");
    REQUIRE(header.getDiagnostics().empty());
    auto publications = server.client.diagnosticPublications.size();
    types.replaceAll("typedef int renamed_t;\n");
    types.publishChanges();
    CHECK(hasDiagnostic(header, "value_t"));
    CHECK(std::find(server.client.diagnosticPublications.begin() + publications,
                    server.client.diagnosticPublications.end(),
                    header.m_uri) != server.client.diagnosticPublications.end());
    workspace.remove("deleted.svh");
    server.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "deleted.svh", lsp::FileChangeType::Deleted));
    header.doc = server.getDoc(header.m_uri);
    types.doc = server.getDoc(types.m_uri);
    CHECK(types.doc->getText().starts_with("typedef int renamed_t;"));
    CHECK(hasDiagnostic(header, "value_t"));
    types.replaceAll("typedef int value_t;\n");
    types.publishChanges();
    CHECK(header.getDiagnostics().empty());
}

TEST_CASE("Package class headers retain virtual interface dependencies",
          "[package-header][indexed-owner][uvm-dependencies]") {
    OwnerWorkspace workspace;
    workspace.write("bus.sv", "interface bus;\n  int value;\nendinterface\n");
    workspace.write("owner.sv", "package p;\n`include \"item.svh\"\nendpackage\n");
    const std::string text = "class item;\n  virtual bus vif;\n"
                             "  function int get(); return vif.value; endfunction\nendclass\n";
    workspace.write("item.svh", text);
    workspace.write("unrelated.sv", "module unrelated; missing_module bad(); endmodule\n");
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    CHECK_FALSE(hasDiagnostic(header, "unknown interface"));
    auto definitions = header.before("bus vif").getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == workspace.uri("bus.sv"));
    auto member = header.after("vif.").getDefinitions();
    REQUIRE(member.size() == 1);
    CHECK(member.front().targetUri == workspace.uri("bus.sv"));
    CHECK(member.front().targetSelectionRange.start.line == 1);
    auto completions = header.after("vif.").getResolvedCompletions();
    CHECK(std::ranges::any_of(completions, [](const auto& item) { return item.label == "value"; }));
    auto trees = header.doc->getAnalysis()->getCompilation()->getSyntaxTrees();
    REQUIRE(trees.size() == 2);
    CHECK(server.m_driver->comp == nullptr);
    for (const auto& tree : trees) {
        auto path = server.sourceManager().getFullPath(tree->getSourceBufferIds().front());
        CHECK((path == workspace.path("owner.sv") || path == workspace.path("bus.sv")));
    }
}

TEST_CASE("Virtual interface aliases retain parameters and modport restrictions",
          "[package-header][indexed-owner][uvm-dependencies]") {
    bool array = false;
    SECTION("Scalar receiver") {
    }
    SECTION("Array element receiver") {
        array = true;
    }
    OwnerWorkspace workspace;
    workspace.write("bus.sv", "interface bus #(parameter int WIDTH = 8);\n"
                              "  logic [WIDTH-1:0] value;\n  int hidden;\n"
                              "  modport consumer(input value);\nendinterface\n");
    workspace.write("owner.sv", "package p;\n`include \"item.svh\"\nendpackage\n");
    const std::string receiver = array ? "vifs[0]." : "vif.";
    const std::string text = "typedef virtual bus #(16).consumer bus_t;\nclass item;\n  bus_t " +
                             std::string(array ? "vifs[2]" : "vif") +
                             ";\n  function int get(); return " + receiver +
                             "value; endfunction\nendclass\n";
    workspace.write("item.svh", text);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    CHECK_FALSE(hasDiagnostic(header, "unknown interface"));
    auto completions = header.after(receiver).getResolvedCompletions();
    auto value = std::ranges::find_if(completions,
                                      [](const auto& item) { return item.label == "value"; });
    REQUIRE(value != completions.end());
    CHECK(value->documentation.has_value());
    CHECK_FALSE(
        std::ranges::any_of(completions, [](const auto& item) { return item.label == "hidden"; }));
    auto hover = header.getHoverAt(header.after(receiver).m_offset);
    REQUIRE(hover.has_value());
    CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find("16") != std::string::npos);
    auto definitions = header.after(receiver).getDefinitions();
    CHECK_FALSE(definitions.empty());
    for (const auto& definition : definitions)
        CHECK(definition.targetUri == workspace.uri("bus.sv"));
}

TEST_CASE("Included macro expansion stops before unrelated class and owner tokens",
          "[package-header][syntax-index]") {
    OwnerWorkspace workspace;
    std::string headerText = "`DECLARE(item)\n";
    std::string ownerTail = "`include \"sibling.svh\"\n";
    SECTION("Macro at the end of the included header") {
    }
    SECTION("Macro followed by an include and another header token") {
        headerText += "`include \"sibling.svh\"\nint after_include;\n";
        ownerTail.clear();
    }
    workspace.write("owner.sv", "`define FIELD(name) int name;\n"
                                "`define DECLARE(name) class name; `FIELD(kept) endclass\n"
                                "package p;\n`include \"item.svh\"\n" +
                                    ownerTail + "int owner_tail;\nendpackage\n");
    workspace.write("item.svh", headerText);
    workspace.write("sibling.svh", "class sibling_marker; endclass\n");
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", headerText);
    auto hover = header.getHoverAt(header.before("`DECLARE").m_offset);
    REQUIRE(hover.has_value());
    auto markdown = rfl::get<lsp::MarkupContent>(hover->contents).value;
    auto expansionStart = markdown.find("Expands to");
    REQUIRE(expansionStart != std::string::npos);
    auto expansion = markdown.substr(expansionStart);
    CHECK(expansion.find("kept") != std::string::npos);
    CHECK(expansion.find("item") != std::string::npos);
    CHECK(markdown.find("sibling_marker") == std::string::npos);
    CHECK(markdown.find("owner_tail") == std::string::npos);
    CHECK(markdown.find("endpackage") == std::string::npos);
}

TEST_CASE("Inactive included header keeps directives attached to the following class",
          "[package-header][syntax-index]") {
    OwnerWorkspace workspace;
    const std::string text = "`ifdef UNDEFINED_BRANCH\nclass hidden; endclass\n`endif\n";
    workspace.write("item.svh", text);
    workspace.write("sibling.svh", "class sibling; endclass\n");
    workspace.write("owner.sv", "package p;\n`include \"item.svh\"\n"
                                "`include \"sibling.svh\"\nendpackage\n");
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    auto regions = header.getInactiveRegions();
    REQUIRE(regions.size() == 1);
    CHECK(regions.front().start.line <= 1);
    CHECK(regions.front().end.line >= 1);
    CHECK(header.getSymbolTree().empty());
}

TEST_CASE("Unsaved virtual interface type edits replace required dependencies",
          "[package-header][uvm-dependencies]") {
    OwnerWorkspace workspace;
    workspace.write("first.sv", "interface first; int value; endinterface\n");
    workspace.write("second.sv", "interface second; int value; endinterface\n");
    workspace.write("owner.sv", "package p;\n`include \"item.svh\"\nendpackage\n");
    const std::string original = "class item; virtual first vif; endclass\n";
    workspace.write("item.svh", original);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", original);
    for (const auto& name : {"first", "second", "first"}) {
        header.replaceAll("class item; virtual " + std::string(name) + " vif; endclass\n");
        header.publishChanges();
        CHECK_FALSE(hasDiagnostic(header, "unknown interface"));
        auto definitions = header.before(std::string(name) + " vif").getDefinitions();
        REQUIRE(definitions.size() == 1);
        CHECK(definitions.front().targetUri == workspace.uri(std::string(name) + ".sv"));
        auto trees = header.doc->getAnalysis()->getCompilation()->getSyntaxTrees();
        REQUIRE(trees.size() == 2);
    }
}

TEST_CASE("Syntax indexing retains a class that encloses the edited include",
          "[package-header][syntax-index]") {
    OwnerWorkspace workspace;
    const std::string text = "int value;\nfunction int get(); return value; endfunction\n";
    workspace.write("item.svh", text);
    workspace.write("owner.sv", "package p;\nclass outer;\n`include \"item.svh\"\n"
                                "endclass\nclass unrelated; int other; endclass\nendpackage\n");
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    CHECK(header.getDiagnostics().empty());
    auto definitions = header.after("return ").getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == header.m_uri);
    CHECK(definitions.front().targetSelectionRange.start == header.before("value;").getPosition());
    auto hover = header.getHoverAt(header.after("return ").m_offset);
    REQUIRE(hover.has_value());
    CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find("p::outer") !=
          std::string::npos);
}

TEST_CASE("Cancelled owner rolls back diagnostics in every included header",
          "[indexed-owner][scheduling]") {
    OwnerWorkspace workspace;
    workspace.write("first.svh", "class first; int value; endclass\n");
    workspace.write("second.svh", "class second; int value; endclass\n");
    const std::string ownerText =
        "package p;\n`include \"first.svh\"\n`include \"second.svh\"\nendpackage\n";
    workspace.write("owner.sv", ownerText);
    ServerHarness server(workspace.params());
    auto owner = server.openFile(workspace.path("owner.sv").string(), ownerText);
    auto first = server.openFile(workspace.path("first.svh").string(),
                                 "class first; int value; endclass\n");
    auto second = server.openFile(workspace.path("second.svh").string(),
                                  "class second; int value; endclass\n");
    first.doc->onChange(
        {lsp::TextDocumentContentChangeWholeDocument{"class first; int value endclass\n"}});
    second.doc->onChange(
        {lsp::TextDocumentContentChangeWholeDocument{"class second; int value endclass\n"}});
    first.doc->setVersion(11);
    second.doc->setVersion(21);
    lsp::RequestContext ctx("textDocument/didChange", std::nullopt);
    class CancelAfterTwoReports : public slang::DiagnosticClient {
    public:
        explicit CancelAfterTwoReports(lsp::RequestContext ctx) : ctx(std::move(ctx)) {}
        void report(const slang::ReportedDiagnostic&) override {
            if (++count == 2)
                ctx.cancel();
        }
        lsp::RequestContext ctx;
        int count = 0;
    };
    auto cancel = std::make_shared<CancelAfterTwoReports>(ctx);
    server.m_driver->diagEngine.addClient(cancel);
    server.client.diagnosticPublications.clear();
    CHECK_THROWS_AS(
        server.onDocDidChange(
            lsp::DidChangeTextDocumentParams{
                .textDocument = {.version = 2, .uri = owner.doc->getURI()},
                .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{ownerText + "\n"}}},
            ctx),
        lsp::RequestCancelled);
    server.m_driver->diagEngine.removeClient(cancel);
    REQUIRE(cancel->count >= 2);
    CHECK(server.client.diagnosticPublications.empty());
    server.openFile(workspace.path("other.sv").string(), "module other; endmodule\n");
    for (const auto& uri : {first.doc->getURI(), second.doc->getURI(), owner.doc->getURI()}) {
        CHECK(std::ranges::find(server.client.diagnosticPublications, uri) ==
              server.client.diagnosticPublications.end());
    }
    server.onDocDidChange(lsp::DidChangeTextDocumentParams{
        .textDocument = {.version = 3, .uri = owner.doc->getURI()},
        .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{ownerText + "\n\n"}}});
    CHECK(!first.getDiagnostics().empty());
    CHECK(!second.getDiagnostics().empty());
    CHECK(server.client.diagnosticVersions.at(first.doc->getURI()) == 11);
    CHECK(server.client.diagnosticVersions.at(second.doc->getURI()) == 21);
}

TEST_CASE("Diagnostic versions survive driver recreation and reset on disk reload",
          "[indexed-owner][scheduling]") {
    OwnerWorkspace workspace;
    const std::string original = "class versioned; int value; endclass\n";
    workspace.write("versioned.sv", original);
    ServerHarness server(workspace.params());
    auto uri = workspace.uri("versioned.sv");
    server.onDocDidOpen(lsp::DidOpenTextDocumentParams{
        .textDocument = {
            .uri = uri, .languageId = "systemverilog", .version = 17, .text = "\n" + original}});
    REQUIRE(server.client.diagnosticVersions.at(uri) == 17);
    auto replacement = server::ServerDriver::createForExplore(server.m_indexer, server.client,
                                                              server.getConfig(),
                                                              workspace.root.string(),
                                                              server.m_driver.get());
    CHECK(replacement->getDocument(uri)->getVersion() == 17);
    CHECK(server.client.diagnosticVersions.at(uri) == 17);
    server.m_driver = std::move(replacement);
    server.m_driver->reloadDocument(uri);
    CHECK(!server.getDoc(uri)->getVersion());
    CHECK(!server.client.diagnosticVersions.at(uri));
}

TEST_CASE("Saving a shared owner republishes complete diagnostics with the editor version",
          "[indexed-owner][scheduling]") {
    OwnerWorkspace workspace;
    const std::string text = "class item; missing_t value endclass\n";
    workspace.write("item.svh", text);
    const std::string root = ownerText("p", "item.svh");
    workspace.write("owner.sv", root);
    ServerHarness server(workspace.params());
    auto owner = server.openFile(workspace.path("owner.sv").string(), root);
    auto header = server.openFile(workspace.path("item.svh").string(), text);
    auto uri = header.doc->getURI();
    auto hasSemanticError = [&] {
        return std::ranges::any_of(server.client.getDiagnostics(uri), [](const auto& diag) {
            return diag.message.find("missing_t") != std::string::npos;
        });
    };
    REQUIRE(hasSemanticError());
    REQUIRE(server.client.diagnosticVersions.at(uri).has_value());
    owner.save();
    CHECK(hasSemanticError());
    // Main reanalyzes every open view of the same shared tree after this save.
    CHECK(server.client.diagnosticVersions.at(uri) == header.doc->getVersion());
}

TEST_CASE("One edit shares completed package diagnostics across open siblings",
          "[indexed-owner][semantic-batch]") {
    for (size_t count : {1u, 2u, 4u, 8u}) {
        DYNAMIC_SECTION(count << " open headers") {
            OwnerWorkspace workspace;
            std::string owner = "package p; typedef int value_t; "
                                "class box #(type T=int); T data; endclass\n";
            std::vector<std::string> sources;
            for (size_t i = 0; i < count; ++i) {
                auto name = "item" + std::to_string(i);
                sources.push_back("class " + name + "; value_t value; missing_" +
                                  std::to_string(i) + " bad; " + name +
                                  " handle; box#(value_t) boxed; function value_t get(); "
                                  "value = handle; return value; endfunction endclass\n");
                workspace.write(name + ".svh", sources.back());
                owner += "`include \"" + name + ".svh\"\n";
            }
            workspace.write("owner.sv", owner + "endpackage\n");
            ServerHarness server(workspace.params());
            std::vector<DocumentHandle> headers;
            for (size_t i = 0; i < count; ++i)
                headers.push_back(server.openFile("item" + std::to_string(i) + ".svh", sources[i]));
            std::weak_ptr<server::ShallowAnalysis> previous;
            // Reverse the producer and require a fresh computation on the next edit.
            for (size_t changed : {size_t{0}, count - 1}) {
                auto before = server.m_driver->getSemanticDiagnosticBatchStatistics();
                headers[changed].replaceAll("\n" + headers[changed].getText());
                headers[changed].publishChanges();
                CHECK(previous.expired());
                auto after = server.m_driver->getSemanticDiagnosticBatchStatistics();
                CHECK(after.computations - before.computations == 1);
                CHECK(after.hits - before.hits == count - 1);
                auto producer = headers[changed].doc->getAnalysis();
                previous = producer;
                auto complete = slang::DiagnosticEngine::reportAll(
                    server.sourceManager(), producer->getSemanticDiagnostics());
                std::vector<std::string> expected;
                for (size_t i = 0; i < count; ++i) {
                    auto& header = headers[i];
                    auto analysis = header.doc->getAnalysis();
                    if (i != changed) {
                        // Main shares compilation state while retaining per-file indexes.
                        CHECK(analysis.get() != producer.get());
                        CHECK(analysis->getShallowCompilation() ==
                              producer->getShallowCompilation());
                        CHECK(analysis->getCompilation() == producer->getCompilation());
                    }
                    REQUIRE(!header.getDiagnostics().empty());
                    expected.push_back(rfl::json::write(header.getDiagnostics()));
                    auto definitions = header.before("value_t value").getDefinitions();
                    REQUIRE(definitions.size() == 1);
                    CHECK(definitions.front().targetUri == workspace.uri("owner.sv"));
                    CHECK(header.getHoverAt(header.before("value;").m_offset).has_value());
                    auto completions = header.after("return ").getResolvedCompletions();
                    CHECK(std::ranges::any_of(completions, [](const auto& item) {
                        return item.label == "value" && item.documentation.has_value();
                    }));
                    if (i != changed)
                        CHECK(analysis->getCompilation()->isElaborated() ==
                              producer->getCompilation()->isElaborated());
                    // Compare the complete semantic result, including diagnostics in
                    // other headers and formatted type arguments owned by the producer.
                    CHECK(slang::DiagnosticEngine::reportAll(server.sourceManager(),
                                                             analysis->getSemanticDiagnostics()) ==
                          complete);
                }
                // Independently recompute each full LSP publication (notes, severity, ranges).
                for (size_t i = 0; i < count; ++i) {
                    auto& header = headers[i];
                    auto uri = header.doc->getURI();
                    server.m_driver->diagClient->clear(uri);
                    header.doc->issueDiagnosticsTo(server.m_driver->diagEngine);
                    server.m_driver->diagClient->pushDiags(uri);
                    CHECK(rfl::json::write(header.getDiagnostics()) == expected[i]);
                }
            }
        }
    }
}

TEST_CASE("Semantic diagnostic batch rejects incomplete and non-package contexts",
          "[semantic-batch]") {
    slang::SourceManager sm;
    slang::Bag options;
    std::string text = "package p; class item; missing_a a; missing_b b; endclass endpackage";
    bool expectedHit = true;
    SECTION("complete erroneous package") {
    }
    SECTION("fatal error limit") {
        slang::ast::CompilationOptions compilationOptions;
        compilationOptions.errorLimit = 1;
        options.set(compilationOptions);
        expectedHit = false;
    }
    SECTION("owner has a module") {
        text += " module top; endmodule";
        expectedHit = false;
    }
    SECTION("standalone class") {
        text = "class item; missing_a a; endclass";
        expectedHit = false;
    }
    auto tree = slang::syntax::SyntaxTree::fromText(text, sm);
    auto makeAnalysis = [&] {
        return std::make_shared<server::ShallowAnalysis>(
            sm, tree->getSourceBufferIds().front(), tree, options,
            std::vector<std::shared_ptr<slang::syntax::SyntaxTree>>{tree});
    };
    auto first = makeAnalysis();
    auto second = makeAnalysis();
    REQUIRE(first->getCompilation().get() != second->getCompilation().get());
    server::SemanticDiagnosticBatch::Statistics statistics;
    std::weak_ptr<server::ShallowAnalysis> producer = first;
    {
        server::SemanticDiagnosticBatch batch(statistics);
        auto original = batch.collect(first);
        auto repeated = batch.collect(second);
        CHECK(repeated.reused == expectedHit);
        CHECK(statistics.computations == (expectedHit ? 1 : 2));
        CHECK(statistics.hits == (expectedHit ? 1 : 0));
        if (expectedHit) {
            CHECK(&original.diagnostics == &repeated.diagnostics);
            CHECK_FALSE(second->getCompilation()->isElaborated());
            first.reset();
            CHECK_FALSE(producer.expired());
        }
    }
    first.reset();
    CHECK(producer.expired());
    // No snapshot lives in the recipient; ordinary lazy semantic work remains possible.
    CHECK(!second->getSemanticDiagnostics().empty());
}

TEST_CASE("Semantic diagnostic batch requires exact ordered dependency trees", "[semantic-batch]") {
    slang::SourceManager sm;
    auto owner = slang::syntax::SyntaxTree::fromText("package p; endpackage", sm);
    auto left = slang::syntax::SyntaxTree::fromText("package left; endpackage", sm);
    auto right = slang::syntax::SyntaxTree::fromText("package right; endpackage", sm);
    auto otherOwner = slang::syntax::SyntaxTree::fromText("package p; endpackage", sm);
    auto makeAnalysis = [&](auto tree, auto trees) {
        return std::make_shared<server::ShallowAnalysis>(sm, tree->getSourceBufferIds().front(),
                                                         tree, slang::Bag{}, trees);
    };
    using Trees = std::vector<std::shared_ptr<slang::syntax::SyntaxTree>>;
    server::SemanticDiagnosticBatch::Statistics statistics;
    server::SemanticDiagnosticBatch batch(statistics);
    CHECK_FALSE(batch.collect(makeAnalysis(owner, Trees{owner, left, right})).reused);
    CHECK_FALSE(batch.collect(makeAnalysis(owner, Trees{owner, right, left})).reused);
    CHECK_FALSE(batch.collect(makeAnalysis(owner, Trees{owner, left})).reused);
    CHECK_FALSE(batch.collect(makeAnalysis(otherOwner, Trees{otherOwner, left, right})).reused);
    CHECK(batch.collect(makeAnalysis(owner, Trees{owner, left, right})).reused);
    CHECK(statistics.computations == 4);
    CHECK(statistics.hits == 1);
}

TEST_CASE("Cancellation during reused sibling diagnostics rolls back and refreshes later",
          "[indexed-owner][semantic-batch][scheduling]") {
    OwnerWorkspace workspace;
    const std::string firstText = "class first; int value; endclass\n";
    const std::string secondText = "class second; first obj; function int get(); "
                                   "return obj.value; endfunction endclass\n";
    workspace.write("first.svh", firstText);
    workspace.write("second.svh", secondText);
    workspace.write("owner.sv", "package p;\n`include \"first.svh\"\n"
                                "`include \"second.svh\"\nendpackage\n");
    ServerHarness server(workspace.params());
    auto first = server.openFile("first.svh", firstText);
    auto second = server.openFile("second.svh", secondText);
    REQUIRE(second.getDiagnostics().empty());
    lsp::RequestContext context("textDocument/didChange", std::nullopt);
    class CancelOnReport : public slang::DiagnosticClient {
    public:
        explicit CancelOnReport(lsp::RequestContext context) : context(std::move(context)) {}
        void report(const slang::ReportedDiagnostic&) override {
            ++reports;
            context.cancel();
        }
        lsp::RequestContext context;
        size_t reports = 0;
    };
    auto cancellation = std::make_shared<CancelOnReport>(context);
    server.m_driver->diagEngine.addClient(cancellation);
    auto before = server.m_driver->getSemanticDiagnosticBatchStatistics();
    server.client.diagnosticPublications.clear();
    const std::string replacement = "class first; int changed; endclass\n";
    CHECK_THROWS_AS(
        server.onDocDidChange(
            lsp::DidChangeTextDocumentParams{
                .textDocument = {.version = 2, .uri = first.doc->getURI()},
                .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{replacement}}},
            context),
        lsp::RequestCancelled);
    server.m_driver->diagEngine.removeClient(cancellation);
    REQUIRE(cancellation->reports > 0);
    CHECK(server.m_driver->getSemanticDiagnosticBatchStatistics().hits - before.hits == 1);
    CHECK(std::ranges::find(server.client.diagnosticPublications, second.doc->getURI()) ==
          server.client.diagnosticPublications.end());
    server.openFile("unrelated.sv", "module unrelated; endmodule\n");
    CHECK(second.getDiagnostics().empty());
    server.onDocDidChange(lsp::DidChangeTextDocumentParams{
        .textDocument = {.version = 3, .uri = first.doc->getURI()},
        .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{replacement + "\n"}}});
    CHECK(hasDiagnostic(second, "value"));
    CHECK(server.client.diagnosticVersions.at(second.doc->getURI()) == second.doc->getVersion());
    server.onDocDidChange(lsp::DidChangeTextDocumentParams{
        .textDocument = {.version = 4, .uri = first.doc->getURI()},
        .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{firstText}}});
    CHECK(second.getDiagnostics().empty());
}

TEST_CASE("Exploration preincludes preserve ordered macro context and document identity",
          "[indexed-owner][preinclude]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", R"({
        "flags": "-DREMOVED=1",
        "explorePreincludeFiles": ["common.svh", "late.svh"]
    })");
    workspace.write("common.svh", "`ifndef COMMON_GUARD\n`define COMMON_GUARD\n"
                                  "`include \"nested.svh\"\n`endif\n");
    workspace.write("nested.svh", "`define FIELD_TYPE int\n");
    workspace.write("late.svh", "`undef FIELD_TYPE\n`define FIELD_TYPE byte\n`undef REMOVED\n");
    workspace.write("owner.sv", "package p;\n`ifdef REMOVED\nmissing_t bad;\n`endif\n"
                                "`include \"item.svh\"\nendpackage\n");
    const std::string text = "class item; `FIELD_TYPE value; endclass\n";
    workspace.write("item.svh", text);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", text);
    CHECK(header.getDiagnostics().empty());
    auto analysis = header.doc->getAnalysis();
    CHECK(server.sourceManager().getFullPath(
              analysis->getSyntaxTree()->getSourceBufferIds().front()) ==
          workspace.path("owner.sv"));
    CHECK(server.sourceManager().getFullPath(analysis->getBuffer()) == workspace.path("item.svh"));
    auto hover = header.getHoverAt(header.before("value;").m_offset);
    REQUIRE(hover);
    CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find("byte") != std::string::npos);
    auto definitions = header.after("`").getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == workspace.uri("late.svh"));
    auto outline = header.getSymbolTree();
    REQUIRE(outline.size() == 1);
    CHECK(outline.front().name == "item");
}

TEST_CASE("Unsaved nested preinclude edits refresh package and standalone consumers",
          "[indexed-owner][preinclude]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", R"({"explorePreincludeFiles":["common.svh"]})");
    workspace.write("common.svh", "`include \"nested.svh\"\n");
    const std::string macroText = "`ifndef NESTED_GUARD\n`define NESTED_GUARD\n"
                                  "`define FIELD_TYPE int\n`endif\n";
    workspace.write("nested.svh", macroText);
    workspace.write("owner.sv", "package p;\n`include \"item.svh\"\nendpackage\n");
    const std::string itemText = "class item; `FIELD_TYPE value; endclass\n";
    const std::string interfaceText = "interface bus; `FIELD_TYPE value; endinterface\n";
    workspace.write("item.svh", itemText);
    workspace.write("bus.sv", interfaceText);
    ServerHarness server(workspace.params());
    auto header = server.openFile("item.svh", itemText);
    auto bus = server.openFile("bus.sv", interfaceText);
    auto macro = server.openFile("nested.svh", macroText);
    // An opened nested macro file must not disappear behind its own include guard.
    auto macroAnalysis = macro.doc->getAnalysis();
    auto definition = macroAnalysis->macros.at("FIELD_TYPE");
    CHECK(definition->name.location().buffer() == macro.doc->getBuffer());
    auto oldAnalysis = header.doc->getAnalysis();
    server.client.diagnosticPublications.clear();
    auto replacement = macroText;
    replacement.replace(replacement.find("TYPE int"), 8, "TYPE byte");
    macro.replaceAll("\n" + replacement);
    macro.publishChanges();
    CHECK_FALSE(oldAnalysis->hasValidBuffers());
    for (auto* document : {&header, &bus}) {
        CHECK(document->getDiagnostics().empty());
        CHECK(std::ranges::find(server.client.diagnosticPublications, document->doc->getURI()) !=
              server.client.diagnosticPublications.end());
        auto hover = document->getHoverAt(document->before("value;").m_offset);
        REQUIRE(hover);
        CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find("byte") !=
              std::string::npos);
        auto defs = document->after("`").getDefinitions();
        REQUIRE(defs.size() == 1);
        CHECK(defs.front().targetUri == macro.doc->getURI());
        CHECK(defs.front().targetSelectionRange.start.line == 3);
    }
    macro.replaceAll(macroText);
    macro.publishChanges();
    auto hover = bus.getHoverAt(bus.before("value;").m_offset);
    REQUIRE(hover);
    CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find("int") != std::string::npos);
}

TEST_CASE("Deleted preinclude dependencies recover on an ordinary Changed notification",
          "[indexed-owner][preinclude]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", R"({"explorePreincludeFiles":["common.svh"]})");
    workspace.write("common.svh", "`include \"nested.svh\"\n");
    workspace.write("nested.svh", "`define FIELD_TYPE int\n");
    const std::string text = "interface bus; `FIELD_TYPE value; endinterface\n";
    workspace.write("bus.sv", text);
    ServerHarness server(workspace.params());
    auto bus = server.openFile("bus.sv", text);
    REQUIRE(bus.getDiagnostics().empty());
    workspace.remove("nested.svh");
    server.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "nested.svh", lsp::FileChangeType::Deleted));
    CHECK(!server.client.getDiagnostics(workspace.uri("bus.sv")).empty());
    workspace.write("nested.svh", "`define FIELD_TYPE byte\n");
    server.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "nested.svh", lsp::FileChangeType::Changed));
    auto current = server.getDoc(workspace.uri("bus.sv"));
    CHECK(server.client.getDiagnostics(workspace.uri("bus.sv")).empty());
    auto hover = server.m_driver->getDocHover(current->getURI(), positionOf(text, "value;"));
    REQUIRE(hover);
    CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find("byte") != std::string::npos);
}

TEST_CASE("Opening an unsaved preinclude refreshes consumers and survives driver recreation",
          "[indexed-owner][preinclude]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", R"({"explorePreincludeFiles":["common.svh"]})");
    workspace.write("common.svh", "`define PRESENT\n");
    const std::string text = "interface bus;\n`ifdef BAD_FIELD\nmissing_t value;\n"
                             "`else\nint value;\n`endif\nendinterface\n";
    workspace.write("bus.sv", text);
    ServerHarness server(workspace.params());
    auto bus = server.openFile("bus.sv", text);
    REQUIRE(bus.getDiagnostics().empty());
    auto macro = server.openFile("common.svh", "`define BAD_FIELD\n");
    CHECK(hasDiagnostic(bus, "missing_t"));
    server.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, ".slang/server.json", lsp::FileChangeType::Changed));
    auto diagnostics = server.client.getDiagnostics(workspace.uri("bus.sv"));
    CHECK(std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.message.find("missing_t") != std::string::npos;
    }));
    CHECK(server.getDoc(workspace.uri("common.svh"))->getText().find("BAD_FIELD") !=
          std::string::npos);
}

TEST_CASE("Closed preinclude parse diagnostics neither duplicate nor survive repair",
          "[indexed-owner][preinclude]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", R"({"explorePreincludeFiles":["common.svh"]})");
    workspace.write("common.svh", "`unknown_preinclude_directive\n");
    workspace.write("bus.sv", "interface bus; int value; endinterface\n");
    workspace.write("other.sv", "interface other; int value; endinterface\n");
    ServerHarness server(workspace.params());
    auto bus = server.openFile("bus.sv", "interface bus; int value; endinterface\n");
    auto original = server.client.getDiagnostics(workspace.uri("common.svh"));
    REQUIRE(!original.empty());
    auto other = server.openFile("other.sv", "interface other; int value; endinterface\n");
    CHECK(rfl::json::write(server.client.getDiagnostics(workspace.uri("common.svh"))) ==
          rfl::json::write(original));
    bus.replaceAll("\ninterface bus; int value; endinterface\n");
    bus.publishChanges();
    CHECK(rfl::json::write(server.client.getDiagnostics(workspace.uri("common.svh"))) ==
          rfl::json::write(original));
    workspace.write("common.svh", "`define REPAIRED\n");
    server.onWorkspaceDidChangeWatchedFiles(
        changed(workspace, "common.svh", lsp::FileChangeType::Changed));
    CHECK(server.client.getDiagnostics(workspace.uri("common.svh")).empty());
}

TEST_CASE("Preinclude macro expansions retain consumer syntax and semantic diagnostics",
          "[indexed-owner][preinclude]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", R"({"explorePreincludeFiles":["common.svh"]})");
    workspace.write("common.svh", "`define FIELD_TYPE missing_t\n`define BAD_EXPR (1 + )\n");
    workspace.write("owner.sv", "package p; `include \"item.svh\"\nendpackage\n");
    const std::string itemText = "class item; `FIELD_TYPE value; endclass\n";
    const std::string busText =
        "interface bus; `FIELD_TYPE value; localparam X = `BAD_EXPR; endinterface\n";
    workspace.write("item.svh", itemText);
    workspace.write("bus.sv", busText);
    ServerHarness server(workspace.params());
    auto item = server.openFile("item.svh", itemText);
    auto bus = server.openFile("bus.sv", busText);
    CHECK(hasDiagnostic(item, "missing_t"));
    CHECK(hasDiagnostic(bus, "missing_t"));
    CHECK(hasDiagnostic(bus, "expected expression"));
    CHECK(server.client.getDiagnostics(workspace.uri("common.svh")).empty());
}

TEST_CASE("Closing a preinclude permits its diagnostics to be republished",
          "[indexed-owner][preinclude]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", R"({"explorePreincludeFiles":["common.svh"]})");
    const std::string macroText = "`unknown_preinclude_directive\n";
    workspace.write("common.svh", macroText);
    workspace.write("bus.sv", "interface bus; endinterface\n");
    ServerHarness server(workspace.params());
    auto macro = server.openFile("common.svh", macroText);
    REQUIRE(!macro.getDiagnostics().empty());
    server.onDocDidClose(
        lsp::DidCloseTextDocumentParams{.textDocument = {.uri = workspace.uri("common.svh")}});
    auto bus = server.openFile("bus.sv", "interface bus; endinterface\n");
    CHECK(!server.client.getDiagnostics(workspace.uri("common.svh")).empty());
}

TEST_CASE("An unsaved preinclude can replace a cached missing file without a watcher event",
          "[indexed-owner][preinclude]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", R"({"explorePreincludeFiles":["common.svh"]})");
    workspace.write("common.svh", "`include \"nested.svh\"\n");
    const std::string text = "interface bus; `FIELD_TYPE value; endinterface\n";
    workspace.write("bus.sv", text);
    ServerHarness server(workspace.params());
    auto bus = server.openFile("bus.sv", text);
    REQUIRE(!bus.getDiagnostics().empty());
    auto macro = server.openFile("nested.svh", "`define FIELD_TYPE byte\n");
    CHECK_FALSE(fs::exists(workspace.path("nested.svh")));
    CHECK(bus.getDiagnostics().empty());
    auto hover = bus.getHoverAt(bus.before("value;").m_offset);
    REQUIRE(hover);
    CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find("byte") != std::string::npos);
}

TEST_CASE("Cancelled preinclude diagnostics are retried before publication",
          "[indexed-owner][preinclude][scheduling]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", R"({"explorePreincludeFiles":["common.svh"]})");
    const std::string good = "`define FIELD_TYPE int\n";
    workspace.write("common.svh", good);
    workspace.write("bus.sv", "interface bus; `FIELD_TYPE value; endinterface\n");
    ServerHarness server(workspace.params());
    auto bus = server.openFile("bus.sv", "interface bus; `FIELD_TYPE value; endinterface\n");
    auto macro = server.openFile("common.svh", good);
    lsp::RequestContext context("textDocument/didChange", std::nullopt);
    class CancelOnReport : public slang::DiagnosticClient {
    public:
        explicit CancelOnReport(lsp::RequestContext context) : context(std::move(context)) {}
        void report(const slang::ReportedDiagnostic&) override {
            ++reports;
            context.cancel();
        }
        lsp::RequestContext context;
        size_t reports = 0;
    };
    auto cancellation = std::make_shared<CancelOnReport>(context);
    server.m_driver->diagEngine.addClient(cancellation);
    const std::string bad = good + "`unknown_preinclude_directive\n";
    CHECK_THROWS_AS(server.onDocDidChange(
                        lsp::DidChangeTextDocumentParams{
                            .textDocument = {.version = 2, .uri = macro.doc->getURI()},
                            .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{bad}}},
                        context),
                    lsp::RequestCancelled);
    server.m_driver->diagEngine.removeClient(cancellation);
    REQUIRE(cancellation->reports > 0);
    CHECK(macro.getDiagnostics().empty());
    bus.replaceAll("\ninterface bus; `FIELD_TYPE value; endinterface\n");
    bus.publishChanges();
    CHECK(hasDiagnostic(macro, "unknown_preinclude_directive"));
    macro.replaceAll(good);
    macro.publishChanges();
    CHECK(macro.getDiagnostics().empty());
}

TEST_CASE("Opening a changed preinclude invalidates a cached absent package owner",
          "[indexed-owner][preinclude]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", R"({"explorePreincludeFiles":["common.svh"]})");
    workspace.write("common.svh", "`define HIDE_ITEM\n");
    workspace.write("owner.sv", "package p; typedef int value_t;\n`ifndef HIDE_ITEM\n"
                                "`include \"item.svh\"\n`endif\nendpackage\n");
    const std::string itemText = "class item; value_t value; endclass\n";
    workspace.write("item.svh", itemText);
    ServerHarness server(workspace.params());
    auto candidates = server.m_indexer.getPackageOwnerCandidates(workspace.path("item.svh"));
    REQUIRE(std::ranges::find(candidates.paths, workspace.path("owner.sv")) !=
            candidates.paths.end());
    auto revision = server.m_indexer.getRevision();
    auto item = server.openFile("item.svh", itemText);
    CHECK(server.m_driver->sm.getFullPath(
              item.doc->getAnalysis()->getSyntaxTree()->getSourceBufferIds().front()) ==
          workspace.path("item.svh"));
    auto macro = server.openFile("common.svh", "`undef HIDE_ITEM\n");
    CHECK(server.m_indexer.getRevision() == revision);
    CHECK(item.getDiagnostics().empty());
    CHECK(server.m_driver->sm.getFullPath(
              item.doc->getAnalysis()->getSyntaxTree()->getSourceBufferIds().front()) ==
          workspace.path("owner.sv"));
    auto hover = item.getHoverAt(item.before("value;").m_offset);
    REQUIRE(hover);
    CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find("int") != std::string::npos);
}

TEST_CASE("Closing an unsaved preinclude restores disk authority and preserves other edits",
          "[indexed-owner][preinclude]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", R"({"explorePreincludeFiles":["common.svh"]})");
    workspace.write("common.svh", "`include \"nested.svh\"\n");
    workspace.write("nested.svh", "`define FIELD_TYPE int\n");
    const std::string disk = "interface bus; `FIELD_TYPE value; endinterface\n";
    const std::string unsaved = "\ninterface bus; `FIELD_TYPE edited; endinterface\n";
    workspace.write("bus.sv", disk);
    ServerHarness server(workspace.params());
    auto bus = server.openFile("bus.sv", unsaved);
    auto macro = server.openFile("nested.svh", "`define FIELD_TYPE missing_t\n");
    REQUIRE(hasDiagnostic(bus, "missing_t"));
    auto version = bus.doc->getVersion();
    server.onDocDidClose(
        lsp::DidCloseTextDocumentParams{.textDocument = {.uri = workspace.uri("nested.svh")}});
    auto current = server.getDoc(workspace.uri("bus.sv"));
    CHECK(current->getText().find("edited") != std::string::npos);
    CHECK(current->getVersion() == version);
    CHECK(server.client.getDiagnostics(workspace.uri("bus.sv")).empty());
    auto hover = server.m_driver->getDocHover(current->getURI(), positionOf(unsaved, "edited;"));
    REQUIRE(hover);
    CHECK(rfl::get<lsp::MarkupContent>(hover->contents).value.find("int") != std::string::npos);
}

TEST_CASE("Closing a never-saved preinclude removes its in-memory definitions",
          "[indexed-owner][preinclude]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", R"({"explorePreincludeFiles":["common.svh"]})");
    workspace.write("common.svh", "`include \"nested.svh\"\n");
    const std::string text = "interface bus; `FIELD_TYPE value; endinterface\n";
    workspace.write("bus.sv", text);
    ServerHarness server(workspace.params());
    auto bus = server.openFile("bus.sv", text);
    auto macro = server.openFile("nested.svh", "`define FIELD_TYPE int\n");
    REQUIRE(bus.getDiagnostics().empty());
    REQUIRE_FALSE(fs::exists(workspace.path("nested.svh")));
    server.onDocDidClose(
        lsp::DidCloseTextDocumentParams{.textDocument = {.uri = workspace.uri("nested.svh")}});
    auto diagnostics = server.client.getDiagnostics(workspace.uri("bus.sv"));
    CHECK(std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.message.find("FIELD_TYPE") != std::string::npos;
    }));
    CHECK(!server.client.getDiagnostics(workspace.uri("common.svh")).empty());
}

TEST_CASE("Foreground edits and queries defer diagnostics and reuse current analysis",
          "[indexed-owner][scheduling][background]") {
    OwnerWorkspace workspace;
    const std::string original = "class item; int value; endclass\n";
    workspace.write("item.svh", original);
    workspace.write("owner.sv", "package p; `include \"item.svh\"\nendpackage\n");
    ServerHarness server(workspace.params());
    auto item = server.openFile("item.svh", original);
    auto before = server.m_driver->getSemanticDiagnosticBatchStatistics();
    server.client.diagnosticPublications.clear();
    const std::string changed =
        "class item; int latest; function int f(); return latest; endfunction endclass\n";
    server.SlangServer::onDocDidChange(lsp::DidChangeTextDocumentParams{
        .textDocument = {.version = 2, .uri = item.doc->getURI()},
        .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{changed}}});
    CHECK(server.m_driver->hasPendingDiagnostics());
    CHECK(server.client.diagnosticPublications.empty());
    CHECK(server.m_driver->getSemanticDiagnosticBatchStatistics().computations ==
          before.computations);
    auto analysis = item.doc->getAnalysis();
    server.client.capabilities.definitionLinksSupported = true;
    auto definitionResult = server.getDocDefinition(lsp::DefinitionParams{
        .textDocument = {.uri = item.doc->getURI()},
        .position = {.line = 0, .character = static_cast<lsp::uint>(changed.rfind("latest"))}});
    REQUIRE(rfl::holds_alternative<std::vector<lsp::DefinitionLink>>(definitionResult));
    auto definitions = rfl::get<std::vector<lsp::DefinitionLink>>(definitionResult);
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetSelectionRange.start.character == changed.find("latest"));
    CHECK(server.m_driver->getSemanticDiagnosticBatchStatistics().computations ==
          before.computations);
    CHECK(server.client.diagnosticPublications.empty());
    server.m_driver->runPendingDiagnostics();
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
    CHECK(item.doc->getAnalysis() == analysis);
    CHECK(server.client.diagnosticVersions.at(item.doc->getURI()) == 2);
    CHECK(item.getDiagnostics().empty());
}

TEST_CASE("Deferred diagnostics coalesce generations and recover after cancellation",
          "[indexed-owner][scheduling][background]") {
    OwnerWorkspace workspace;
    const std::string original = "class item; int value; endclass\n";
    workspace.write("item.svh", original);
    workspace.write("sibling.svh", "class sibling; item obj; endclass\n");
    workspace.write("owner.sv",
                    "package p;\n`include \"item.svh\"\n`include \"sibling.svh\"\nendpackage\n");
    ServerHarness server(workspace.params());
    auto item = server.openFile("item.svh", original);
    auto sibling = server.openFile("sibling.svh");
    auto before = server.m_driver->getSemanticDiagnosticBatchStatistics();
    server.client.diagnosticPublications.clear();
    for (int version : {2, 3}) {
        server.SlangServer::onDocDidChange(lsp::DidChangeTextDocumentParams{
            .textDocument = {.version = version, .uri = item.doc->getURI()},
            .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                version == 2 ? "class item; missing_type value; endclass\n" : original}}});
    }
    CHECK(server.client.diagnosticPublications.empty());
    lsp::RequestContext cancelled("background/diagnostics", std::nullopt);
    cancelled.cancel();
    CHECK_THROWS_AS(server.m_driver->runPendingDiagnostics(cancelled), lsp::RequestCancelled);
    CHECK(server.m_driver->hasPendingDiagnostics());
    CHECK(server.m_driver->getSemanticDiagnosticBatchStatistics().computations ==
          before.computations);
    server.m_driver->runPendingDiagnostics();
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
    CHECK(server.client.diagnosticVersions.at(item.doc->getURI()) == 3);
    CHECK(item.getDiagnostics().empty());
    CHECK(sibling.getDiagnostics().empty());
    CHECK(std::ranges::find(server.client.diagnosticPublications, sibling.doc->getURI()) !=
          server.client.diagnosticPublications.end());
}

TEST_CASE("Query analysis refreshes when a saved provider changes the dependency index",
          "[indexed-owner][background][lifecycle]") {
    OwnerWorkspace workspace;
    const std::string consumerText = "class item; late_pkg::word_t value; endclass\n";
    workspace.write("consumer.sv", consumerText);
    ServerHarness server(workspace.params());
    auto consumer = server.openFile("consumer.sv", consumerText);
    auto oldAnalysis = consumer.doc->getAnalysis();
    REQUIRE_FALSE(consumer.getDiagnostics().empty());
    const std::string providerText = "package late_pkg; typedef int word_t; endpackage\n";
    workspace.write("provider.sv", providerText);
    auto provider = server.openFile("provider.sv", providerText);
    server.onDocDidSave(lsp::DidSaveTextDocumentParams{.textDocument = {provider.doc->getURI()}});
    CHECK(consumer.doc->getAnalysis() != oldAnalysis);
    auto definitions = consumer.before("word_t").getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == provider.doc->getURI());
    server.onDocDidSave(lsp::DidSaveTextDocumentParams{.textDocument = {consumer.doc->getURI()}});
    CHECK(consumer.getDiagnostics().empty());
}

TEST_CASE("Closing an edited package preserves pending discovery for open headers",
          "[indexed-owner][background][lifecycle]") {
    OwnerWorkspace workspace;
    const std::string header = "class item; value_t value; endclass\n";
    const std::string original = "package p; typedef int value_t; endpackage\n";
    workspace.write("item.svh", header);
    workspace.write("owner.sv", original);
    ServerHarness server(workspace.params());
    auto item = server.openFile("item.svh", header);
    auto owner = server.openFile("owner.sv", original);
    REQUIRE_FALSE(item.getDiagnostics().empty());
    server.SlangServer::onDocDidChange(lsp::DidChangeTextDocumentParams{
        .textDocument = {.version = 2, .uri = owner.doc->getURI()},
        .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
            "package p; typedef int value_t;\n`include \"item.svh\"\nendpackage\n"}}});
    server.onDocDidClose(lsp::DidCloseTextDocumentParams{.textDocument = {owner.doc->getURI()}});
    CHECK(server.m_driver->hasPendingDiagnostics());
    server.m_driver->runPendingDiagnostics();
    CHECK_FALSE(server.m_driver->hasPendingDiagnostics());
    CHECK(item.getDiagnostics().empty());
    CHECK(server.client.getDiagnostics(owner.doc->getURI()).empty());
}

TEST_CASE("Known owner reparsing preserves current edits across cancellation",
          "[owner-cancellation]") {
    OwnerWorkspace workspace;
    workspace.write(".slang/server.json", "{\"experimentalCooperativeAnalysis\":true}\n");
    workspace.write("item.svh", itemText);
    auto owner = ownerText();
    const auto end = owner.find("endpackage");
    std::string padding;
    for (int i = 0; i < 5000; ++i)
        padding += "int padding_" + std::to_string(i) + ";\n";
    owner.insert(end, padding);
    workspace.write("owner.sv", owner);
    ServerHarness server(workspace.params());
    server.m_driver->openDocument(workspace.uri("item.svh"), itemText, 1, true);
    auto header = server.m_driver->getDocument(workspace.uri("item.svh"));
    auto ownerDoc = server.m_driver->getDocument(workspace.uri("owner.sv"));
    auto originalTree = ownerDoc->getSyntaxTree();
    auto current = itemText;
    current.replace(current.find("return value;"), 13, "return missing_value;");
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = 2, .uri = workspace.uri("item.svh")},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = current}}});
    const auto publications = server.client.diagnosticPublications.size();
    bool ownerEntered = false, interrupted = false;
    lsp::RequestContext ctx("known owner", lsp::ID_t{901});
    ctx.setCheckpointObserverForTesting([&](std::string_view where) {
        if (where == "before owner syntax")
            ownerEntered = true;
        if (ownerEntered && where == "during disposable syntax parsing") {
            interrupted = true;
            ctx.cancel();
        }
    });
    CHECK_THROWS_AS(header->getAnalysis(ctx), lsp::RequestCancelled);
    REQUIRE(interrupted);
    CHECK(header->textMatches(current));
    CHECK(ownerDoc->getCachedSyntaxTree() == originalTree);
    CHECK(server.client.diagnosticPublications.size() == publications);
    server.m_driver->runPendingDiagnostics({});
    auto hasMissing = [&] {
        return std::ranges::any_of(server.client.getDiagnostics(workspace.uri("item.svh")),
                                   [](const auto& diagnostic) {
                                       return diagnostic.message.find("missing_value") !=
                                              std::string::npos;
                                   });
    };
    CHECK(hasMissing());
    CHECK(server.m_driver->sm.getFullPath(
              header->getAnalysis()->getSyntaxTree()->getSourceBufferIds().front()) ==
          workspace.path("owner.sv"));
    server.SlangServer::onDocDidChange(
        {.textDocument = {.version = 3, .uri = workspace.uri("item.svh")},
         .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = itemText}}});
    server.m_driver->runPendingDiagnostics({});
    CHECK_FALSE(hasMissing());
}
