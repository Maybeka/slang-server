// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "SlangServer.h"
#include "utils/ClientHarness.h"
#include <algorithm>
#include <atomic>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <rfl/from_generic.hpp>
#include <system_error>

namespace {
namespace fs = std::filesystem;

struct WatchWorkspace {
    fs::path root;

    WatchWorkspace() {
        static std::atomic_uint64_t serial{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (;;) {
            auto candidate = fs::temp_directory_path() /
                             ("slang-context-watch-" + std::to_string(stamp) + "-" +
                              std::to_string(serial++));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                root = fs::canonical(candidate);
                break;
            }
            REQUIRE_FALSE(error);
        }
        fs::create_directories(root / "project");
    }

    ~WatchWorkspace() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path path(std::string_view name) const { return root / name; }
    URI uri(std::string_view name) const { return URI::fromFile(path(name)); }

    void write(std::string_view name, std::string_view text) const {
        fs::create_directories(path(name).parent_path());
        std::ofstream file(path(name), std::ios::binary | std::ios::trunc);
        REQUIRE(file.good());
        file << text;
        file.close();
        REQUIRE(file.good());
    }

    lsp::InitializeParams params() const {
        return {.workspaceFolders = {
                    {lsp::WorkspaceFolder{.uri = uri("project"), .name = "context-watch-test"}}}};
    }

    void configure() const {
        write("project/.slang/server.json",
              "{\"indexingThreads\":1,\"compilationFilelist\":\"lists/top.arguments\"}\n");
        write("project/lists/top.arguments", "-F nested/sources.manifest\n");
        write("project/lists/nested/sources.manifest", "../../../external/owner.sv\n");
        write("external/owner.sv", "package owner; typedef int value_t; endpackage\n");
    }
};

struct WatchClient : ClientHarness {
    std::vector<lsp::Registration> registrations;
    std::vector<lsp::Unregistration> unregistrations;
    std::vector<std::string> requests;

    std::monostate getClientRegisterCapability(const lsp::RegistrationParams& params) override {
        for (const auto& registration : params.registrations) {
            registrations.push_back(registration);
            requests.push_back("register:" + registration.id);
        }
        return {};
    }

    std::monostate getClientUnregisterCapability(const lsp::UnregistrationParams& params) override {
        for (const auto& unregistration : params.unregisterations) {
            unregistrations.push_back(unregistration);
            requests.push_back("unregister:" + unregistration.id);
        }
        return {};
    }

    std::vector<lsp::FileSystemWatcher> contextWatchers() const {
        for (auto it = registrations.rbegin(); it != registrations.rend(); ++it) {
            if (!it->id.starts_with("slang-server-context-file-watcher-"))
                continue;
            REQUIRE(it->registerOptions);
            auto options = rfl::from_generic<lsp::DidChangeWatchedFilesRegistrationOptions,
                                             rfl::UnderlyingEnums>(*it->registerOptions);
            REQUIRE(options);
            return options->watchers;
        }
        return {};
    }

    bool watches(const fs::path& path, std::string_view escapedFilename = {}) const {
        for (const auto& watcher : contextWatchers()) {
            REQUIRE(rfl::holds_alternative<lsp::RelativePattern>(watcher.globPattern));
            const auto& pattern = rfl::get<lsp::RelativePattern>(watcher.globPattern);
            REQUIRE(rfl::holds_alternative<URI>(pattern.baseUri));
            if (rfl::get<URI>(pattern.baseUri) == URI::fromFile(path.parent_path()) &&
                pattern.pattern ==
                    (escapedFilename.empty() ? path.filename().string() : escapedFilename)) {
                CHECK_FALSE(watcher.kind); // All three file lifecycle events.
                return true;
            }
        }
        return false;
    }
};

class WatchServer : public server::SlangServer {
public:
    explicit WatchServer(WatchClient& client) : SlangServer(client) {}
    using SlangServer::m_driver;
    using SlangServer::onForegroundComplete;

    void forceWatchRefresh(bool equivalentSpelling = false) {
        if (equivalentSpelling) {
            REQUIRE_FALSE(m_contextWatchPaths.empty());
            auto& value = m_contextWatchPaths.front();
            auto spelling = value.native();
            const auto at = spelling.find_last_of(fs::path::preferred_separator);
            REQUIRE(at != decltype(spelling)::npos);
            spelling.insert(at, 1, fs::path::preferred_separator);
            const fs::path alias(spelling);
            REQUIRE(alias.native() != value.native());
            REQUIRE(alias == value);
            value = alias;
        }
        ++m_contextWatchRevision;
    }

    void open(const URI& uri, std::string text, int version = 7) {
        onDocDidOpen({.textDocument = {.uri = uri,
                                       .languageId = "systemverilog",
                                       .version = version,
                                       .text = std::move(text)}});
        onForegroundComplete();
    }
};
} // namespace

TEST_CASE("Context watches register exact nested and external paths after initialized",
          "[context-watches]") {
    WatchWorkspace workspace;
    workspace.configure();
    WatchClient client;
    WatchServer server(client);
    server.getInitialize(workspace.params());
    CHECK(client.registrations.empty());
    server.onForegroundComplete();
    CHECK(client.registrations.empty());
    server.onInitialized({});

    REQUIRE(client.registrations.size() == 2);
    CHECK(client.registrations.front().id == "slang-server-file-watcher");
    CHECK(client.watches(workspace.path("project/lists/top.arguments")));
    CHECK(client.watches(workspace.path("project/lists/nested/sources.manifest")));
    CHECK(client.watches(workspace.path("external/owner.sv")));
    for (const auto& watcher : client.contextWatchers()) {
        const auto& pattern = rfl::get<lsp::RelativePattern>(watcher.globPattern);
        CHECK(pattern.pattern.find("**") == std::string::npos);
    }

    const auto requestCount = client.requests.size();
    server.onForegroundComplete();
    server.onForegroundComplete();
    CHECK(client.requests.size() == requestCount);
}

TEST_CASE("Context watch replacement unregisters before registering and clears stale paths",
          "[context-watches]") {
    WatchWorkspace workspace;
    workspace.configure();
    WatchClient client;
    WatchServer server(client);
    server.getInitialize(workspace.params());
    server.onInitialized({});
    const auto previousId = client.registrations.back().id;
    const auto requestCount = client.requests.size();

    workspace.write("external/replacement.sv", "package replacement; endpackage\n");
    workspace.write("project/lists/nested/sources.manifest", "../../../external/replacement.sv\n");
    server.onWorkspaceDidChangeWatchedFiles(
        {.changes = {{.uri = workspace.uri("project/lists/nested/sources.manifest"),
                      .type = lsp::FileChangeType::Changed}}});
    server.onForegroundComplete();

    REQUIRE(client.requests.size() >= requestCount + 2);
    CHECK(client.requests[requestCount] == "unregister:" + previousId);
    CHECK(client.requests[requestCount + 1].starts_with("register:"));
    CHECK(client.registrations.back().id != previousId);
    CHECK(client.watches(workspace.path("external/replacement.sv")));
    CHECK_FALSE(client.watches(workspace.path("external/owner.sv")));

    Config empty;
    empty.indexingThreads = 1;
    const auto activeId = client.registrations.back().id;
    server.loadConfig(empty);
    REQUIRE_FALSE(client.unregistrations.empty());
    CHECK(client.unregistrations.back().id == activeId);
    CHECK(client.requests.back() == "unregister:" + activeId);
}

TEST_CASE("Context watches escape literal glob characters in hinted external filenames",
          "[context-watches]") {
    WatchWorkspace workspace;
    workspace.write("project/.slang/server.json", "{\"indexingThreads\":1}\n");
    workspace.write("external[dir]/owner[1]{x}.sv",
                    "package p;\n`include \"../project/item.svh\"\nendpackage\n");
    const std::string header = "// slang-server: owner \"../external[dir]/owner[1]{x}.sv\"\n"
                               "class item; int value; endclass\n";
    workspace.write("project/item.svh", header);
    WatchClient client;
    WatchServer server(client);
    server.getInitialize(workspace.params());
    server.onInitialized({});
    server.open(workspace.uri("project/item.svh"), header);

    CHECK(client.watches(workspace.path("external[dir]/owner[1]{x}.sv"), "owner[[]1[]][{]x[}].sv"));
    CHECK_FALSE(server.m_driver->hasExplicitContextError(workspace.uri("project/item.svh")));
}

TEST_CASE("Explicit-source watcher events preserve open overlays and versions through URI aliases",
          "[context-watches]") {
    WatchWorkspace workspace;
    workspace.write("project/.slang/server.json",
                    "{\"indexingThreads\":1,\"compilationFilelist\":\"sources.f\"}\n");
    workspace.write("project/sources.f", "open.sv\nopen.json\n");
    workspace.write("project/open.sv", "module disk; endmodule\n");
    workspace.write("project/open.json", "module disk_json; endmodule\n");
    fs::create_directory(workspace.path("project/alias"));
    WatchClient client;
    WatchServer server(client);
    server.getInitialize(workspace.params());
    server.onInitialized({});
    const std::string overlay = "module editor; int unsaved; endmodule\n";
    server.open(workspace.uri("project/open.sv"), overlay);
    const std::string jsonOverlay = "module editor_json; int unsaved; endmodule\n";
    server.open(workspace.uri("project/open.json"), jsonOverlay, 11);
    const auto originalDriver = server.m_driver.get();
    auto originalDoc = server.m_driver->getDocument(workspace.uri("project/open.sv"));
    fs::remove(workspace.path("project/open.sv"));

    for (auto type : {lsp::FileChangeType::Changed, lsp::FileChangeType::Created,
                      lsp::FileChangeType::Deleted}) {
        server.onWorkspaceDidChangeWatchedFiles(
            {.changes = {{.uri = workspace.uri("project/alias/../open.sv"), .type = type},
                         {.uri = workspace.uri("project/open.json"), .type = type}}});
        CHECK(server.m_driver.get() == originalDriver);
        auto doc = server.m_driver->getDocument(workspace.uri("project/open.sv"));
        CHECK(doc == originalDoc);
        CHECK(server.m_driver->isDocumentOpen(workspace.uri("project/open.sv")));
        CHECK(doc->textMatches(overlay));
        CHECK(doc->getVersion() == 7);
        auto jsonDoc = server.m_driver->getDocument(workspace.uri("project/open.json"));
        CHECK(jsonDoc->textMatches(jsonOverlay));
        CHECK(jsonDoc->getVersion() == 11);
    }
}

TEST_CASE("Unopened file deletion rebuilds context while retaining every open overlay",
          "[context-watches]") {
    WatchWorkspace workspace;
    workspace.write("project/.slang/server.json", "{\"indexingThreads\":1}\n");
    workspace.write("project/open.sv", "module disk; endmodule\n");
    workspace.write("project/closed.sv", "module closed; endmodule\n");
    WatchClient client;
    WatchServer server(client);
    server.getInitialize(workspace.params());
    server.onInitialized({});
    const std::string overlay = "module editor; int unsaved; endmodule\n";
    server.open(workspace.uri("project/open.sv"), overlay, 19);
    REQUIRE(server.m_driver->getDocument(workspace.uri("project/closed.sv")));
    fs::remove(workspace.path("project/open.sv"));
    fs::remove(workspace.path("project/closed.sv"));
    server.onWorkspaceDidChangeWatchedFiles(
        {.changes = {
             {.uri = workspace.uri("project/open.sv"), .type = lsp::FileChangeType::Deleted},
             {.uri = workspace.uri("project/closed.sv"), .type = lsp::FileChangeType::Deleted}}});

    CHECK(server.m_driver->isDocumentOpen(workspace.uri("project/open.sv")));
    auto doc = server.m_driver->getDocument(workspace.uri("project/open.sv"));
    REQUIRE(doc);
    CHECK(doc->textMatches(overlay));
    CHECK(doc->getVersion() == 19);
}

TEST_CASE("Deleted nested command files remain watched and reload when recreated",
          "[context-watches]") {
    WatchWorkspace workspace;
    workspace.configure();
    WatchClient client;
    WatchServer server(client);
    server.getInitialize(workspace.params());
    server.onInitialized({});
    const auto manifest = workspace.path("project/lists/nested/sources.manifest");
    auto* originalDriver = server.m_driver.get();
    fs::remove(manifest);
    server.onWorkspaceDidChangeWatchedFiles(
        {.changes = {{.uri = URI::fromFile(manifest), .type = lsp::FileChangeType::Deleted}}});
    // Main rejects an invalid reload transaction and keeps its last valid driver.
    CHECK(server.m_driver.get() == originalDriver);
    CHECK_FALSE(server.m_driver->configuredInputLoadFailed());
    server.onForegroundComplete();
    CHECK(client.watches(manifest));

    workspace.write("external/replacement.sv", "package replacement; endpackage\n");
    workspace.write("project/lists/nested/sources.manifest", "../../../external/replacement.sv\n");
    server.onWorkspaceDidChangeWatchedFiles(
        {.changes = {{.uri = URI::fromFile(manifest), .type = lsp::FileChangeType::Created}}});
    server.onForegroundComplete();
    CHECK(server.m_driver.get() != originalDriver);
    CHECK(client.watches(workspace.path("external/replacement.sv")));
    CHECK(server.m_driver->driver.getCommandFileMetadata().contains(manifest));
}

TEST_CASE("Missing external literal sources remain watched and recover on creation",
          "[context-watches][native-unit]") {
    WatchWorkspace workspace;
    workspace.configure();
    auto event = lsp::FileChangeType::Created;
    std::string missingSource = "../../../external/generated.sv";
    std::string manifest = "../../../external/owner.sv\n../../../external/generated.sv\n";
    SECTION("direct literal and Created event") {
    }
    SECTION("directory-prefix candidate and Changed event") {
        manifest = "--dir-prefix ../../../external\n../../../external/owner.sv\ngenerated.sv\n";
        event = lsp::FileChangeType::Changed;
        missingSource = "generated.sv";
    }
    workspace.write("project/lists/nested/sources.manifest", manifest);
    const std::string ownerText = "package owner; import generated::*; value_t value; endpackage\n";
    workspace.write("external/owner.sv", ownerText);
    WatchClient client;
    WatchServer server(client);
    server.getInitialize(workspace.params());
    const auto missingSourceError =
        "'" + missingSource +
        "': " + std::make_error_code(std::errc::no_such_file_or_directory).message();
    client.expectError(missingSourceError);
    client.expectError("Failed to parse configuration or compilation filelist");
    // Native option processing and the initial source parse both report this load error.
    client.expectError(missingSourceError);
    server.onInitialized({});
    REQUIRE(server.m_driver->configuredInputLoadFailed());
    REQUIRE_FALSE(server.m_driver->sm.isCached(workspace.path("external/generated.sv")));
    CHECK(client.watches(workspace.path("external/generated.sv")));
    CHECK(server.m_driver->driver.sourceLoader.getFailedLiteralPaths().contains(
        workspace.path("external/generated.sv")));

    auto* originalDriver = server.m_driver.get();
    workspace.write("external/unrelated.sv", "package unrelated; endpackage\n");
    server.onWorkspaceDidChangeWatchedFiles(
        {.changes = {{.uri = workspace.uri("external/unrelated.sv"),
                      .type = lsp::FileChangeType::Created}}});
    CHECK(server.m_driver.get() == originalDriver);
    CHECK(server.m_driver->configuredInputLoadFailed());

    const std::string overlay = "// retain this unsaved edit\n" + ownerText;
    server.open(workspace.uri("external/owner.sv"), overlay, 11);
    workspace.write("external/generated.sv",
                    "package generated; typedef byte value_t; endpackage\n");
    server.onWorkspaceDidChangeWatchedFiles(
        {.changes = {{.uri = workspace.uri("external/generated.sv"), .type = event}}});
    server.onForegroundComplete();
    CHECK(server.m_driver.get() != originalDriver);
    CHECK_FALSE(server.m_driver->configuredInputLoadFailed());
    CHECK(server.m_driver->driver.sourceLoader.getFailedLiteralPaths().empty());
    auto owner = server.m_driver->getDocument(workspace.uri("external/owner.sv"));
    REQUIRE(owner);
    CHECK(owner->textMatches(overlay));
    CHECK(owner->getVersion() == 11);
    CHECK_FALSE(server.m_driver->hasExplicitContextError(owner->getURI()));
    CHECK(client.getDiagnostics(owner->getURI()).empty());
    CHECK(owner->getAnalysis()->getCompilation()->getSyntaxTrees().size() == 2);
    CHECK(client.watches(workspace.path("external/generated.sv")));
}

TEST_CASE("SourceLoader records only unresolved literal candidates",
          "[context-watches][native-unit]") {
    WatchWorkspace workspace;
    slang::SourceManager sm;
    slang::driver::SourceLoader loader(sm);
    loader.addFiles(workspace.path("external/*.sv").string());
    CHECK(loader.getFailedLiteralPaths().empty());
    CHECK(loader.getErrors().empty());

    loader.addFiles(workspace.path("external/missing.sv").string());
    CHECK(loader.getFilePaths().empty());
    CHECK(loader.getFailedLiteralPaths().contains(workspace.path("external/missing.sv")));
    CHECK_FALSE(loader.getErrors().empty());

    workspace.write("external/present.sv", "package present; endpackage\n");
    slang::driver::SourceLoader recovered(sm);
    recovered.addDirPrefix(workspace.path("external").string());
    recovered.addFiles("present.sv");
    CHECK(recovered.getFilePaths() == std::vector<fs::path>{workspace.path("external/present.sv")});
    CHECK(recovered.getFailedLiteralPaths().empty());
    CHECK(recovered.getErrors().empty());
}

TEST_CASE("Missing nested hint targets watch an existing ancestor with an exact escaped path",
          "[context-watches]") {
    WatchWorkspace workspace;
    workspace.write("project/.slang/server.json", "{\"indexingThreads\":1}\n");
    const std::string header =
        "// slang-server: owner \"../existing[base]/external[dir]/nested{level}/owner[1].sv\"\n"
        "class item; int value; endclass\n";
    workspace.write("project/item.svh", header);
    const auto ancestor = workspace.path("existing[base]");
    fs::create_directory(ancestor);
    REQUIRE_FALSE(fs::exists(ancestor / "external[dir]"));
    WatchClient client;
    WatchServer server(client);
    server.getInitialize(workspace.params());
    server.onInitialized({});
    server.open(workspace.uri("project/item.svh"), header);
    CHECK(server.m_driver->hasExplicitContextError(workspace.uri("project/item.svh")));

    bool found = false;
    for (const auto& watcher : client.contextWatchers()) {
        REQUIRE(rfl::holds_alternative<lsp::RelativePattern>(watcher.globPattern));
        const auto& pattern = rfl::get<lsp::RelativePattern>(watcher.globPattern);
        if (pattern.pattern != "external[[]dir[]]/nested[{]level[}]/owner[[]1[]].sv")
            continue;
        REQUIRE(rfl::holds_alternative<URI>(pattern.baseUri));
        CHECK(rfl::get<URI>(pattern.baseUri) == URI::fromFile(ancestor));
        CHECK(fs::is_directory(fs::path(rfl::get<URI>(pattern.baseUri).getPath())));
        CHECK_FALSE(watcher.kind); // Created, Changed, and Deleted remain enabled.
        found = true;
    }
    CHECK(found);
}

TEST_CASE("Recreated drivers move identical target watches away from deleted parent directories",
          "[context-watches]") {
    const bool memoize = GENERATE(false, true);
    WatchWorkspace workspace;
    workspace.write("project/.slang/server.json",
                    memoize ? "{\"indexingThreads\":1,\"experimentalMethodBodyQueries\":true,"
                              "\"experimentalMethodBodyCertification\":true}\n"
                            : "{\"indexingThreads\":1}\n");
    const std::string header =
        "// slang-server: owner \"../external[dir]/nested{level}/owner[1].sv\"\n"
        "class item; int value; endclass\n";
    workspace.write("project/item.svh", header);
    fs::create_directories(workspace.path("external[dir]/nested{level}"));
    WatchClient client;
    WatchServer server(client);
    server.getInitialize(workspace.params());
    server.onInitialized({});
    server.open(workspace.uri("project/item.svh"), header);
    REQUIRE(client.watches(workspace.path("external[dir]/nested{level}/owner[1].sv"),
                           "owner[[]1[]].sv"));
    const auto previousPaths = server.m_driver->getContextWatchPaths();
    const auto previousId = client.registrations.back().id;
    const auto requestCount = client.requests.size();

    fs::remove_all(workspace.path("external[dir]"));
    server.setExplore();
    server.onForegroundComplete();
    CHECK(server.m_driver->getContextWatchPaths() == previousPaths);
    REQUIRE(client.requests.size() == requestCount + 2);
    CHECK(client.requests[requestCount] == "unregister:" + previousId);
    CHECK(client.requests[requestCount + 1].starts_with("register:"));
    CHECK(client.registrations.back().id != previousId);
    bool found = false;
    for (const auto& watcher : client.contextWatchers()) {
        const auto& pattern = rfl::get<lsp::RelativePattern>(watcher.globPattern);
        if (pattern.pattern != "external[[]dir[]]/nested[{]level[}]/owner[[]1[]].sv")
            continue;
        CHECK(rfl::get<URI>(pattern.baseUri) == URI::fromFile(workspace.root));
        CHECK_FALSE(watcher.kind);
        found = true;
    }
    CHECK(found);
}

TEST_CASE("Unchanged watch paths retain registration for exact and equivalent spellings",
          "[context-watches][watch-path-compare]") {
    const bool memoize = GENERATE(false, true);
    WatchWorkspace workspace;
    workspace.configure();
    if (memoize)
        workspace.write("project/.slang/server.json",
                        "{\"indexingThreads\":1,\"compilationFilelist\":\"lists/top.arguments\","
                        "\"experimentalMethodBodyQueries\":true,"
                        "\"experimentalMethodBodyCertification\":true}\n");
    WatchClient client;
    WatchServer server(client);
    server.getInitialize(workspace.params());
    server.onInitialized({});
    const auto requests = client.requests.size();
    server.forceWatchRefresh();
    server.onForegroundComplete();
    CHECK(client.requests.size() == requests);
    server.forceWatchRefresh(true);
    server.onForegroundComplete();
    CHECK(client.requests.size() == requests);
    CHECK(client.watches(workspace.path("external/owner.sv")));
}

TEST_CASE("Independent catalogue reads cannot hide newly watched document paths",
          "[context-watches][watch-catalogue-cache]") {
    WatchWorkspace workspace;
    workspace.configure();
    workspace.write(
        "project/.slang/server.json",
        "{\"indexingThreads\":1,\"compilationFilelist\":\"lists/top.arguments\","
        "\"experimentalMethodBodyQueries\":true,\"experimentalMethodBodyCertification\":true}\n");
    WatchClient client;
    WatchServer server(client);
    server.getInitialize(workspace.params());
    server.onInitialized({});
    const auto before = server.m_driver->getContextWatchCatalogue();
    CHECK(*before == server.m_driver->getContextWatchPaths());
    const auto requests = client.requests.size();
    const auto added = workspace.path("external/new_leaf.sv");
    const std::string source = "package new_leaf; int value; endpackage\n";
    workspace.write("external/new_leaf.sv", source);
    // Another reader populates the driver's memo before the server consumes it.
    // The server must compare against its own last catalogue, not a global hit.
    server.m_driver->openDocument(URI::fromFile(added), source, 8, true);
    const auto current = server.m_driver->getContextWatchCatalogue();
    CHECK(*current == server.m_driver->getContextWatchPaths());
    CHECK(std::find(current->begin(), current->end(), added) != current->end());
    CHECK(std::find(before->begin(), before->end(), added) == before->end());
    server.onForegroundComplete();
    CHECK(client.watches(added));
    CHECK(client.requests.size() > requests);
    const auto stable = client.requests.size();
    server.forceWatchRefresh();
    server.onForegroundComplete();
    CHECK(client.requests.size() == stable);
}

TEST_CASE("Watch catalogue observes detached buffers and preserves earlier reader values",
          "[context-watches][watch-catalogue-cache]") {
    WatchWorkspace workspace;
    workspace.configure();
    workspace.write(
        "project/.slang/server.json",
        "{\"indexingThreads\":1,\"compilationFilelist\":\"lists/top.arguments\","
        "\"experimentalMethodBodyQueries\":true,\"experimentalMethodBodyCertification\":true}\n");
    WatchClient client;
    WatchServer server(client);
    server.getInitialize(workspace.params());
    auto& driver = *server.m_driver;
    auto original = driver.getContextWatchCatalogue();
    CHECK(*original == driver.getContextWatchPaths());
    CHECK(driver.getContextWatchCatalogue() == original);

    const auto added = workspace.uri("external/temporary.sv");
    const std::string text = "package temporary; int value; endpackage\n";
    workspace.write("external/temporary.sv", text);
    driver.openDocument(added, text, 9, true);
    const auto extended = driver.getContextWatchCatalogue();
    REQUIRE(*extended == driver.getContextWatchPaths());
    CHECK(extended != original);
    CHECK(std::find(extended->begin(), extended->end(), added.getPath()) != extended->end());
    CHECK(std::find(original->begin(), original->end(), added.getPath()) == original->end());

    auto buffer = driver.getDocument(added)->getBuffer();
    slang::SmallVector<char> replacement;
    replacement.append(text.begin(), text.end());
    replacement.push_back('\0');
    auto changed = driver.sm.replaceBuffer(buffer, std::move(replacement));
    REQUIRE(driver.sm.getFullPath(buffer).empty());
    CHECK(driver.sm.getFullPath(changed.id) == added.getPath());
    const auto detached = driver.getContextWatchCatalogue();
    REQUIRE(*detached == driver.getContextWatchPaths());
    CHECK(std::find(detached->begin(), detached->end(), added.getPath()) == detached->end());
    CHECK(std::find(extended->begin(), extended->end(), added.getPath()) != extended->end());
    CHECK(driver.getContextWatchCatalogue() == detached);

    driver.docs.erase(added);
    const auto removed = driver.getContextWatchCatalogue();
    CHECK(*removed == driver.getContextWatchPaths());
    CHECK(*removed == *original);
    CHECK(driver.getContextWatchCatalogue() == removed);
}
