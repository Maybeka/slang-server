//------------------------------------------------------------------------------
// SlangServer.cpp
// Main implementation of the Slang LSP server.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

// main.cpp
#include "SlangServer.h"

#include "Config.h"
#include "ast/WcpClient.h"
#include "completions/CompletionContext.h"
#include "completions/CompletionDispatch.h"
#include "lsp/LspTypeExtensions.h"
#include "lsp/LspTypes.h"
#include "lsp/URI.h"
#include "util/Converters.h"
#include "util/Logging.h"
#include "util/UvmPhaseTrace.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/ranges.h>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <rfl/DefaultIfMissing.hpp>
#include <rfl/Variant.hpp>
#include <rfl/from_generic.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxVisitor.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"
#include "slang/util/OS.h"
#include "slang/util/ScopeGuard.h"
#include "slang/util/TimeTrace.h"
#include "slang/util/VersionInfo.h"

#if defined(__APPLE__)
extern "C" uint64_t slang_server_parser_units() __attribute__((weak_import));
#elif defined(__GNUC__)
extern "C" uint64_t slang_server_parser_units() __attribute__((weak));
#endif

namespace fs = std::filesystem;

namespace server {

SlangServer::SlangServer(SlangLspClient& client) : m_client(client), m_config(Config()) {

    /// Keep this short to get server started quickly
    registerInitialize();
    registerShutdown();
}

// InitializeResult initialize(InitializeParams)
lsp::InitializeResult SlangServer::getInitialize(const lsp::InitializeParams& params) {
    // TODO: may want to use raw strings here bc all these types make compile time longer

    // File Lifecycle
    registerDocDidOpen();
    registerDocDidChange();
    registerDocDidSave();
    registerDocDidClose();

    // Doc Features
    registerDocDefinition();
    registerDocHover();
    registerDocDocumentSymbol();
    registerDocDocumentLink();
    registerDocCompletion();
    registerCompletionItemResolve();
    registerDocDocumentHighlight();

    registerDocCodeLens();
    registerDocInlayHint();
    registerDocReferences();
    registerDocRename();
    registerDocCodeAction();

    // Cone tracing (drivers/loads)
    registerDocPrepareCallHierarchy();
    registerCallHierarchyIncomingCalls();
    registerCallHierarchyOutgoingCalls();

    // Workspace Features
    registerWorkspaceExecuteCommand();
    registerWorkspaceSymbol();
    registerWorkspaceDidChangeWatchedFiles();

    // LSP Lifecycle
    registerInitialized();
    registerCancelRequest();

    INFO("Server started with pid: {}", OS::getpid());

    // Top level setting- these are internal commands, the main command should be in the client
    registerCommand<std::string, std::monostate, &SlangServer::setTopLevel>("slang.setTopLevel");
    registerCommand<std::string, std::monostate, &SlangServer::setBuildFile>("slang.setBuildFile");

    // WCP Commands
    registerCommand<lsp::TextDocumentPositionParams, std::vector<std::string>,
                    &SlangServer::getInstances>("slang.getInstances");

    registerCommand<std::string, std::vector<std::string>, &SlangServer::getModulesInFile>(
        "slang.getModulesInFile");
    registerCommand<waves::ItemToWaveform, std::monostate, &SlangServer::addToWaveform>(
        "slang.addToWaveform");
    registerCommand<std::string, std::monostate, &SlangServer::openWaveform>("slang.openWaveform");

    registerDesignCommand<std::string, std::vector<server::ConeEntry>>(
        "slang.getDriversWithLocation", [](ServerCompilation& comp, const std::string& hierPath) {
            return comp.getDriverCone(hierPath).getLocations();
        });
    registerDesignCommand<std::string, std::vector<server::ConeEntry>>(
        "slang.getLoadsWithLocation", [](ServerCompilation& comp, const std::string& hierPath) {
            return comp.getLoadCone(hierPath).getLocations();
        });

    // Hierarchy View (sidebar)
    registerCommand<std::string, std::vector<hier::HierItem_t>, &SlangServer::getScope>(
        "slang.getScope");
    registerCommand<std::string, std::vector<hier::ScopeStep>, &SlangServer::getScopes>(
        "slang.getScopes");
    registerDesignCommand<std::string, hier::HierarchySearchResult>(
        "slang.searchHierarchy", [](ServerCompilation& comp, const std::string& query) {
            return comp.searchHierarchy(query);
        });
    registerCommand<ShowHierLocationArgs, std::monostate, &SlangServer::showHierLocation>(
        "slang.showHierLocation");
    registerCommand<std::string, std::monostate, &SlangServer::openModuleDefinition>(
        "slang.openModuleDefinition");

    // Terminal Links
    registerCommand<std::string, std::vector<std::string>, &SlangServer::getFilesContainingModule>(
        "slang.getFilesContainingModule");

    // Hierarchy and active instance selection
    registerCommand<std::monostate, std::vector<hier::InstanceSet>,
                    &SlangServer::getScopesByModule>("slang.getScopesByModule");
    registerCommand<std::string, std::vector<hier::QualifiedInstance>,
                    &SlangServer::getInstancesOfModule>("slang.getInstancesOfModule");
    registerCommand<std::string, bool, &SlangServer::setActiveInstance>("slang.setActiveInstance");
    registerCommand<SlangLspClient::ActivateInstanceParams, bool, &SlangServer::activateInstance>(
        "slang.activateInstance");
    registerCommand<std::string, std::optional<hier::QualifiedInstance>,
                    &SlangServer::getActiveInstance>("slang.getActiveInstance");
    // File features
    registerCommand<ExpandMacroArgs, bool, &SlangServer::expandMacros>("slang.expandMacros");
    registerCommand<lsp::Location, std::monostate, &SlangServer::showLocation>(
        "slang.showLocation");

    if (std::getenv("SLANG_SERVER_TESTS")) {
        registerCommand<std::string, rfl::Generic>(
            "slang.methodScopeStatus", [this](const std::string&) {
                ensureMethodScopeDriver();
                auto state = m_methodScope ? m_methodScope->statistics()
                                           : MethodScopeOverlay::Statistics{};
                struct DocumentState {
                    std::string uri;
                    uint32_t buffer;
                    int version;
                };
                std::vector<DocumentState> documents;
                for (const auto& [uri, document] : m_driver->docs)
                    if (document->getVersion())
                        documents.push_back(
                            {uri.str(), document->getBuffer().getId(), *document->getVersion()});
                struct SourceState {
                    uint32_t buffer;
                    std::string uri;
                    bool latest;
                    std::string bytes;
                };
                std::vector<SourceState> sources;
                for (auto buffer : m_driver->sm.getAllBuffers()) {
                    const auto& path = m_driver->sm.getFullPath(buffer);
                    if (path.empty())
                        continue;
                    auto uri = URI::fromFile(path);
                    sources.push_back({buffer.getId(), uri.str(), m_driver->sm.isLatestData(buffer),
                                       std::string(m_driver->sm.getSourceText(buffer))});
                }
                struct State {
                    bool active;
                    uint64_t accepted;
                    int version;
                    std::string decline;
                    std::optional<uint64_t> parserUnits;
                    std::vector<DocumentState> documents;
                    std::vector<SourceState> sources;
                };
                std::optional<uint64_t> count;
#if defined(__GNUC__)
                if (slang_server_parser_units)
                    count = slang_server_parser_units();
#endif
                return rfl::to_generic(State{state.active, state.accepted, state.version,
                                             m_methodScopeDecline, count, documents, sources});
            });
    }

    registerCommand<ServerDriver::IncludeContextSelection, bool>(
        "slang.setIncludeContext", [this](const ServerDriver::IncludeContextSelection& selection) {
            return m_driver->setIncludeContext(selection);
        });

    // Config modification
    registerCommand<std::string, std::monostate, &SlangServer::addDefine>("slang.addDefine");
    registerCommand<std::nullopt_t, bool>("slang.autoConfigure",
                                          [this](std::monostate) { return autoConfigure({}); });

    if (params.workspaceFolders.has_value() && !params.workspaceFolders->empty()) {
        auto folders = params.workspaceFolders.value();
        if (folders.size() > 1) {

            m_client.showWarning(
                "Slang only supports a single workspace folder at the moment; using the first one");
        }
        m_workspaceFolder = params.workspaceFolders->at(0);
    }
    else if (params.rootUri.has_value()) {
        m_workspaceFolder = lsp::WorkspaceFolder{
            .uri = params.rootUri.value(),
            .name = "root",
        };
    }
    else if (params.rootPath.has_value()) {
        m_workspaceFolder = lsp::WorkspaceFolder{
            .uri = URI::fromFile(params.rootPath.value()),
            .name = "root",
        };
    }

    if (m_workspaceFolder) {
        INFO("Using workspace folder: {}", m_workspaceFolder->uri.getPath());
    }
    else {
        WARN("No workspace folder or root provided");
    }

    m_client.capabilities = SlangLspClient::Capabilities(params.capabilities);

    loadConfig();
    m_driver->completions.resolveEdits = m_client.capabilities.completionEditResolveSupported;

    if (params.capabilities.experimental) {
        auto exp = rfl::from_generic<lsp::ExperimentalClientCapabilities>(
            *params.capabilities.experimental);

        if (exp) {
            if (exp->slangClient) {
                const auto& client = *exp->slangClient;
                const auto clientName = client.name.value_or("slang client");
                INFO("Using {} v{}", clientName, client.version.value_or("unknown"));
                auto parseVersion = [](std::string_view version) {
                    std::optional<std::pair<int, int>> result;
                    if (version.starts_with('v'))
                        version.remove_prefix(1);

                    int major = 0;
                    auto [majorEnd, majorError] =
                        std::from_chars(version.data(), version.data() + version.size(), major);
                    if (majorError != std::errc() || majorEnd == version.data() + version.size() ||
                        *majorEnd != '.') {
                        return result;
                    }

                    int minor = 0;
                    auto [minorEnd, minorError] =
                        std::from_chars(majorEnd + 1, version.data() + version.size(), minor);
                    if (minorError != std::errc() ||
                        (minorEnd != version.data() + version.size() && *minorEnd != '.')) {
                        return result;
                    }
                    return std::optional(std::pair(major, minor));
                };

                const auto parsed = client.version ? parseVersion(*client.version) : std::nullopt;
                const auto required = std::pair(VersionInfo::getMajor(), VersionInfo::getMinor());
                if (!parsed) {
                    m_client.showWarning(
                        fmt::format("Could not determine the {} version. Please update {}.",
                                    clientName, clientName));
                }
                else if (*parsed < required) {
                    m_client.showWarning(fmt::format(
                        "{} v{} is older than the server requirement {}.{}.x. Please update {}.",
                        clientName, *client.version, required.first, required.second, clientName));
                }
            }
        }
    }

    auto result = lsp::InitializeResult{
        .capabilities =
            lsp::ServerCapabilities{
                .positionEncoding = m_client.capabilities.utf8Positions
                                        ? std::optional<lsp::PositionEncodingKind>("utf-8")
                                        : std::nullopt,
                .textDocumentSync =
                    lsp::TextDocumentSyncOptions{
                        .openClose = true,
                        .change = lsp::TextDocumentSyncKind::Incremental,
                        .save =
                            lsp::SaveOptions{
                                .includeText = true,
                            },
                    },
                .completionProvider =
                    lsp::CompletionOptions{.triggerCharacters =
                                               completions::completionTriggerCharacters(),
                                           .resolveProvider = true,
                                           .completionItem =
                                               lsp::ServerCompletionItemOptions{
                                                   .labelDetailsSupport = true,
                                               }},
                .hoverProvider = true,
                .definitionProvider = true,
                .referencesProvider = true,
                .documentHighlightProvider = true,
                .documentSymbolProvider = true,
                .codeActionProvider = true,
                .codeLensProvider =
                    lsp::CodeLensOptions{
                        .resolveProvider = false,
                    },
                .documentLinkProvider =
                    lsp::DocumentLinkOptions{
                        .resolveProvider = false,
                        .workDoneProgress = false,
                    },
                .workspaceSymbolProvider = true,
                .renameProvider = true,
                .executeCommandProvider =
                    lsp::ExecuteCommandOptions{
                        .commands = getCommandList(),
                    },
                .callHierarchyProvider = true,
                .inlayHintProvider =
                    lsp::InlayHintOptions{
                        .resolveProvider = false,
                    },

            },
        .serverInfo =
            lsp::ServerInfo{.name = "slang-server",
                            .version = fmt::format("{}.{}.{}+{}\n", VersionInfo::getMajor(),
                                                   VersionInfo::getMinor(), VersionInfo::getPatch(),
                                                   VersionInfo::getHash())},
    };

    INFO("Initialize result: {} ", rfl::json::write(result));

    return result;
}

void SlangServer::onInitialized(const lsp::InitializedParams&) {
    INFO("Server initialized at {}", m_workspaceFolder ? m_workspaceFolder->uri.getPath() : "none");
    m_client.setConfig(m_config);
    m_initialized = true;

    if (m_workspaceFolder) {
        auto options = lsp::DidChangeWatchedFilesRegistrationOptions{
            .watchers{
                // SystemVerilog/Verilog source files
                lsp::FileSystemWatcher{
                    .globPattern = lsp::RelativePattern{.baseUri = m_workspaceFolder->uri,
                                                        .pattern = "**/*.{sv,svh,v,vh}"}},
                // Config files
                lsp::FileSystemWatcher{
                    .globPattern = lsp::RelativePattern{.baseUri = m_workspaceFolder->uri,
                                                        .pattern = ".slang/**/*.json"}},
                // Build/flag files
                lsp::FileSystemWatcher{
                    .globPattern = lsp::RelativePattern{.baseUri = m_workspaceFolder->uri,
                                                        .pattern = "**/*.f"}},
            },
        };

        m_client.getClientRegisterCapability(
            lsp::RegistrationParams{.registrations{lsp::Registration{
                .id = "slang-server-file-watcher",
                .method = "workspace/didChangeWatchedFiles",
                .registerOptions = rfl::to_generic<rfl::UnderlyingEnums>(options),
            }}});
    }
    refreshContextFileWatches();
}

namespace {
fs::path normalizeWatchedPath(const fs::path& path) {
    std::error_code error;
    auto normalized = fs::weakly_canonical(path, error);
    return error ? path.lexically_normal() : normalized;
}

std::string escapeWatchPath(std::string_view relativePath) {
    std::string pattern;
    for (char c : relativePath) {
        // LSP glob escaping uses single-character ranges. Backslash escapes are
        // not portable (VS Code treats backslashes as path separators).
        if (std::string_view("*?[]{}").find(c) != std::string_view::npos) {
            pattern += '[';
            pattern += c;
            pattern += ']';
        }
        else {
            pattern += c;
        }
    }
    return pattern;
}
} // namespace

void SlangServer::refreshContextFileWatches() {
    if (!m_initialized || !m_driver)
        return;

    const auto revision = m_driver->getContextInputRevision();
    const auto documentCount = m_driver->docs.size();
    const bool sameDriver = !m_contextWatchesDirty && m_contextWatchDriver == m_driver.get();
    if (sameDriver && m_contextWatchRevision == revision &&
        m_contextWatchDocumentCount == documentCount)
        return;

    auto paths = m_driver->getContextWatchPaths();
    for (auto& path : paths)
        path = path.lexically_normal();
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    m_contextWatchesDirty = false;
    m_contextWatchDriver = m_driver.get();
    m_contextWatchRevision = revision;
    m_contextWatchDocumentCount = documentCount;
    // A replacement driver may observe a deleted parent directory even when
    // the target paths are unchanged. Recompute its registration anchors.
    if (sameDriver && paths == m_contextWatchPaths)
        return;

    if (!m_contextWatchRegistrationId.empty()) {
        m_client.getClientUnregisterCapability(lsp::UnregistrationParams{.unregisterations{
            {.id = m_contextWatchRegistrationId, .method = "workspace/didChangeWatchedFiles"}}});
        m_contextWatchRegistrationId.clear();
    }
    m_contextWatchPaths = std::move(paths);
    if (m_contextWatchPaths.empty())
        return;

    lsp::DidChangeWatchedFilesRegistrationOptions options;
    options.watchers.reserve(m_contextWatchPaths.size());
    for (const auto& path : m_contextWatchPaths) {
        // Watch an existing directory so a client can observe a target whose
        // intermediate directories have not been created yet.
        auto base = path.parent_path();
        std::error_code error;
        while (!fs::is_directory(base, error)) {
            auto parent = base.parent_path();
            if (parent.empty() || parent == base)
                break;
            base = std::move(parent);
        }
        // The base URI is literal. Escape each relative path segment while
        // retaining '/' separators. Omitted kind observes create, change, and delete.
        options.watchers.push_back(lsp::FileSystemWatcher{
            .globPattern = lsp::RelativePattern{
                .baseUri = URI::fromFile(base),
                .pattern = escapeWatchPath(path.lexically_relative(base).generic_string())}});
    }
    m_contextWatchRegistrationId = "slang-server-context-file-watcher-" +
                                   std::to_string(++m_contextWatchRegistrationSerial);
    m_client.getClientRegisterCapability(lsp::RegistrationParams{
        .registrations{{.id = m_contextWatchRegistrationId,
                        .method = "workspace/didChangeWatchedFiles",
                        .registerOptions = rfl::to_generic<rfl::UnderlyingEnums>(options)}}});
}

void SlangServer::setExplore() {
    // Clear any existing diagnostics and set mode to explore
    m_buildfile = std::nullopt;
    m_topFile = std::nullopt;

    // Move data into the Server Driver
    const auto workspacePath = m_workspaceFolder ? std::optional<std::string_view>(
                                                       m_workspaceFolder->uri.getPath())
                                                 : std::nullopt;
    m_driver = ServerDriver::createForExplore(m_indexer, m_client, m_config, workspacePath,
                                              m_driver.get());
    m_driver->diagClient->pushDiags();
    m_contextWatchesDirty = true;
    refreshContextFileWatches();
}

std::monostate SlangServer::setTopLevel(const std::string& path) {
    if (path.empty()) {
        setExplore();
        return std::monostate{};
    }

    const auto workspacePath = m_workspaceFolder ? std::optional<std::string_view>(
                                                       m_workspaceFolder->uri.getPath())
                                                 : std::nullopt;

    // Resolve relative paths against the workspace folder. The driver keys its documents by
    // absolute-path URI, so an unresolved relative path matches nothing and the top level
    // silently fails to load.
    auto topPath = fs::path(path);
    if (topPath.is_relative()) {
        if (!workspacePath) {
            m_client.showError(fmt::format(
                "Cannot set top level to relative path {}: no workspace folder to resolve it "
                "against. Use an absolute path.",
                path));
            return std::monostate{};
        }
        topPath = (fs::path(*workspacePath) / topPath).lexically_normal();
    }

    INFO("Setting top level to {}", topPath.string());
    m_buildfile = std::nullopt;
    m_topFile = topPath.string();

    m_driver = ServerDriver::createFromTop(m_indexer, m_client, m_config, URI::fromFile(topPath),
                                           workspacePath, m_driver.get());
    m_contextWatchesDirty = true;
    refreshContextFileWatches();

    return std::monostate{};
}

std::monostate SlangServer::setBuildFile(const std::string& path) {
    if (path.empty()) {
        setExplore();
        return std::monostate{};
    }
    m_topFile = std::nullopt;
    m_buildfile = path;

    const auto workspacePath = m_workspaceFolder ? std::optional<std::string_view>(
                                                       m_workspaceFolder->uri.getPath())
                                                 : std::nullopt;
    m_driver = ServerDriver::createFromFileLists(m_indexer, m_client, m_config,
                                                 std::vector<std::string>{path}, workspacePath,
                                                 m_driver.get());
    m_contextWatchesDirty = true;
    refreshContextFileWatches();
    return std::monostate{};
}

std::vector<hier::InstanceSet> SlangServer::getScopesByModule(const std::monostate&) {
    if (!m_driver->comp) {
        ERROR("No compilation available, cannot get scopes by module");
        return {};
    }
    return m_driver->comp->getScopesByModule();
}

std::vector<hier::QualifiedInstance> SlangServer::getInstancesOfModule(
    const std::string moduleName) {
    if (!m_driver->comp) {
        ERROR("No compilation available, cannot get instances of module {}", moduleName);
        return {};
    }
    auto instances = m_driver->comp->getInstancesOfModule(moduleName);
    if (instances.empty()) {
        m_client.showError(fmt::format("Module {} not found", moduleName));
        return {};
    }
    std::vector<hier::QualifiedInstance> result;
    result.reserve(instances.size());
    for (const auto* inst : instances) {
        result.push_back(hier::toQualifiedInstance(*inst, m_driver->sm));
    }
    return result;
}

bool SlangServer::expandMacros(ExpandMacroArgs args) {
    auto doc = m_driver->getDocument(URI::fromFile(args.src));

    if (!doc) {
        return false;
    }

    auto analysis = doc->getAnalysis();
    const auto& expansions = analysis->syntaxes.macroExpansions;
    std::vector<const syntax::SyntaxNode*> usages;
    for (const auto& [usage, _] : expansions)
        usages.push_back(usage);
    std::ranges::sort(usages, [](const auto* left, const auto* right) {
        auto a = left->sourceRange();
        auto b = right->sourceRange();
        return a.start() != b.start() ? a.start() < b.start() : a.end() > b.end();
    });

    auto text = doc->getText();
    text.remove_suffix(1);
    std::string expanded;
    size_t offset = 0;
    for (const auto* usage : usages) {
        auto range = usage->sourceRange();
        // An outer expansion already contains the expansions of its macro arguments.
        if (range.start().offset() < offset)
            continue;
        expanded.append(text.substr(offset, range.start().offset() - offset));
        expanded.append(expansions.at(usage).getText());
        offset = range.end().offset();
    }
    expanded.append(text.substr(offset));
    // Preserve source line endings without Windows text-mode translation.
    std::ofstream file(args.dst, std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.write(expanded.data(), static_cast<std::streamsize>(expanded.size()));
    file.flush();
    return true;
}

std::vector<std::string> SlangServer::getFilesContainingModule(const std::string moduleName) {
    // lookup in index
    auto paths = m_indexer.getFilesForSymbol(moduleName);
    auto transformed = paths | std::ranges::views::transform(
                                   [](const auto& path) { return path.string(); });
    return std::vector<std::string>(transformed.begin(), transformed.end());
}

std::vector<hier::HierItem_t> SlangServer::getScope(const std::string& hierPath) {
    if (!m_driver->comp) {
        ERROR("No compilation available, cannot get scope for {}", hierPath);
        return {};
    }
    return m_driver->comp->getScope(hierPath);
}

// Returns scopes up to the last resolvable segment of the provided hierarchical path.
std::vector<hier::ScopeStep> SlangServer::getScopes(const std::string& hierPath,
                                                    const lsp::RequestContext& ctx) {
    if (!m_driver->comp) {
        ctx.error("No compilation available, cannot get scopes for {}", hierPath);
        return {};
    }
    auto scopes = m_driver->comp->getScopes(hierPath, ctx);
    ctx.info("Resolved {} hierarchy scopes for {}", scopes.size(), hierPath);
    return scopes;
}

bool SlangServer::setActiveInstance(const std::string& hierPath) {
    if (!m_driver || !m_driver->setActiveInstance(hierPath)) {
        m_client.showError(fmt::format("Failed to set active instance {}", hierPath));
        return false;
    }
    return true;
}

bool SlangServer::activateInstance(const SlangLspClient::ActivateInstanceParams& params,
                                   const lsp::RequestContext& ctx) {
    ctx.throwIfCancelled("before activating instance");
    if (!setActiveInstance(params.hierPath))
        return false;

    m_client.onActiveInstanceChanged(params);
    if (params.interactionSource == SlangLspClient::InteractionSource::codeLensGotoInstantiation) {
        showHierLocation({.hierPath = params.hierPath, .takeFocus = true});
    }
    ctx.info("Activated instance {}", params.hierPath);
    return true;
}

std::optional<hier::QualifiedInstance> SlangServer::getActiveInstance(
    const std::string& moduleName) {
    if (!m_driver->comp) {
        ERROR("No compilation available, cannot get active instance for {}", moduleName);
        return std::nullopt;
    }
    return m_driver->comp->getActiveInstance(moduleName);
}

std::optional<lsp::Location> SlangServer::getHierLocation(const std::string& hierPath) {
    if (!m_driver->comp) {
        ERROR("No compilation available, cannot resolve location for {}", hierPath);
        return std::nullopt;
    }
    auto location = m_driver->comp->getHierLocation(hierPath);
    if (location) {
        return location;
    }

    // Packages aren't always part of the active build; fall back to any open doc.
    auto separator = hierPath.rfind("::");
    if (separator == std::string::npos) {
        WARN("getHierLocation: failed to resolve {}", hierPath);
        return std::nullopt;
    }
    auto packageName = std::string_view(hierPath).substr(0, separator);
    auto memberName = std::string_view(hierPath).substr(separator + 2);
    for (const auto& [_, doc] : m_driver->docs) {
        auto analysis = doc->getAnalysis();
        auto* pkg = analysis->getCompilation()->getPackage(packageName);
        auto* member = pkg ? pkg->lookupName(memberName) : nullptr;
        if (!member) {
            continue;
        }
        if (member->location.valid()) {
            auto memberLoc = toLocation(member->location, m_driver->sm);
            if (!memberLoc.uri.getPath().empty()) {
                return memberLoc;
            }
        }
        if (auto* syntax = member->getSyntax()) {
            auto memberLoc = toLocation(syntax->sourceRange(), m_driver->sm);
            if (!memberLoc.uri.getPath().empty()) {
                return memberLoc;
            }
        }
    }
    WARN("getHierLocation: failed to resolve {}", hierPath);
    return std::nullopt;
}

std::monostate SlangServer::showHierLocation(const ShowHierLocationArgs& args) {
    auto location = getHierLocation(args.hierPath);
    if (!location) {
        return {};
    }
    m_client.onShowDocument(lsp::ShowDocumentParams{
        .uri = location->uri,
        .takeFocus = args.takeFocus,
        .selection = location->range,
    });
    return {};
}

std::monostate SlangServer::showLocation(const lsp::Location& location) {
    m_client.onShowDocument(lsp::ShowDocumentParams{
        .uri = location.uri,
        .takeFocus = true,
        .selection = location.range,
    });
    return {};
}

std::monostate SlangServer::openModuleDefinition(const std::string& moduleName) {
    if (!m_driver->comp) {
        ERROR("No compilation available, cannot open module definition for {}", moduleName);
        return {};
    }
    auto instances = m_driver->comp->getInstancesOfModule(moduleName);
    if (instances.empty()) {
        WARN("openModuleDefinition: module {} has no instances", moduleName);
        return {};
    }
    auto declLoc = toLocation(instances[0]->getDefinition().location, m_driver->sm);
    if (declLoc.uri.getPath().empty()) {
        WARN("openModuleDefinition: module {} declaration has no source location", moduleName);
        return {};
    }
    if (m_driver->isDocumentOpen(declLoc.uri)) {
        return {};
    }
    m_client.onShowDocument(lsp::ShowDocumentParams{
        .uri = declLoc.uri,
        .takeFocus = false,
        .selection = declLoc.range,
    });
    return {};
}

// TODO -- Underlying InstanceVisitor implementation is slow for larger designs -- fix
// Currently InstanceVisitor walks the entire design searching for symbols which match
// the provided location.  Instead we should use the ShallowAnalysis to find all the
// containing module instances.  This assumes that hierarchical references don't leave
// a given module.  We should make sure slang can warn for this so that people can lint
// their designs accordingly, fail gracefully when this happens and if really needed
// provided a slower / more thorough fallback when this rule is violated (last part is debatable).
// We won't be able to fully depend on the ShallowAnalysis to give us all instances because
// it won't actually know things such as how generate loops were evaluated.  Either we can
// crawl the AST limited just to the modules in question or we can attempt to use the
// provided hierarchical path to deduce where such points may be and then use the remainder
// of the provided path.
std::vector<std::string> SlangServer::getInstances(const lsp::TextDocumentPositionParams& params) {
    if (!m_driver->comp) {
        ERROR("No compilation available, cannot get instances");
        return {};
    }
    return m_driver->comp->getInstances(params);
}

std::vector<std::string> SlangServer::getModulesInFile(const std::string path) {
    // just use the shallow compilation
    return m_driver->getModulesInFile(path);
}

std::monostate SlangServer::addToWaveform(const waves::ItemToWaveform& params) {
    if (!m_wcpClient.has_value() || !m_wcpClient->running()) {
        ERROR("No WCP session available, cannot add items");
        return std::monostate{};
    }

    m_wcpClient->addItem(params);

    return std::monostate{};
}

std::monostate SlangServer::openWaveform(const std::string& path) {
    std::string wcpCommand("surfer --wcp-initiate {}");
    if (m_config.wcpCommand.value()) {
        wcpCommand = *m_config.wcpCommand.value();
    }
    if (m_wcpClient.has_value() && m_wcpClient->running()) {
        m_client.showInfo(fmt::format("Opening waveform from {} (reusing WCP)", path));
    }
    else {
        m_client.showInfo(fmt::format("Opening waveform from {} (creating WCP)", path));
        m_wcpClient.emplace(this, wcpCommand);
    }
    m_wcpClient->loadWaveform(path);

    return std::monostate{};
}

void SlangServer::onGotoDeclaration(const std::string& path) {
    if (m_driver->comp) {
        auto showDocParams = m_driver->comp->getHierDocParams(path);
        if (showDocParams) {
            m_client.onShowDocument(*showDocParams);
        }
    }
}

void SlangServer::onWaveformLoaded(const std::string& path) {
    if (m_config.buildPattern.value().has_value()) {
        fs::path waveform{path};
        std::string waveStem{waveform.stem().string()};
        std::string buildFile{
            fmt::vformat(*m_config.buildPattern.value(), fmt::make_format_args(waveStem))};
        if (fs::exists(buildFile)) {
            setBuildFile(buildFile);
        }
    }
}

std::optional<std::vector<lsp::CallHierarchyItem>> SlangServer::getDocPrepareCallHierarchy(
    const lsp::CallHierarchyPrepareParams& params) {
    if (!m_driver->comp) {
        ERROR("No compilation available, cannot trace cones");
        return std::nullopt;
    }

    return m_driver->comp->getDocPrepareCallHierarchy(params);
}

// TODO: Use custom client code to render drivers/loads
std::optional<std::vector<lsp::CallHierarchyIncomingCall>> SlangServer::
    getCallHierarchyIncomingCalls(const lsp::CallHierarchyIncomingCallsParams& params) {
    if (!m_driver->comp) {
        ERROR("No compilation available, cannot trace cones");
        return std::nullopt;
    }
    // Drivers are presented as "incoming calls" in the call hierarchy view.
    std::vector<lsp::CallHierarchyIncomingCall> result;
    for (const auto& entry : m_driver->comp->getDriverCone(params.item.name).getLocations()) {
        result.push_back({
            .from =
                {
                    .name = entry.path,
                    .uri = entry.location.uri,
                    .range = entry.location.range,
                    .selectionRange = entry.location.range,
                },
            // This is intended to show the 'edge', which would be where this driver
            // connects to the current item. This could be multiple locations in a
            // process. For the default neovim UI, these need a location.
            .fromRanges = {entry.location.range},
        });
    }
    return result;
}

std::optional<std::vector<lsp::CallHierarchyOutgoingCall>> SlangServer::
    getCallHierarchyOutgoingCalls(const lsp::CallHierarchyOutgoingCallsParams&) {
    m_client.showWarning("Load tracing through the call hierarchy is not currently supported");

    // This was removed since showing loads through this route can't work accross files - it doesn't
    // fit this lsp method.

    // In the future we can add:
    // - Actual call hierarchies, although these won't be very deep/useful in hdl, but perhaps dv
    // - Module Hierarchies
    return std::nullopt;
}

std::vector<std::string> SlangServer::getDrivers(const std::string& path) {
    if (!m_driver->comp) {
        ERROR("No compilation available, cannot trace cones");
        return {};
    }
    return m_driver->comp->getDriverCone(path).getPaths();
}

std::vector<std::string> SlangServer::getLoads(const std::string& path) {
    if (!m_driver->comp) {
        ERROR("No compilation available, cannot trace cones");
        return {};
    }
    return m_driver->comp->getLoadCone(path).getPaths();
}

bool SlangServer::loadConfig(const Config& config, bool forceIndexing, bool requireValidConfig,
                             bool deferOpenDiagnostics) {
    const auto workspacePath = m_workspaceFolder ? std::optional<std::string_view>(
                                                       m_workspaceFolder->uri.getPath())
                                                 : std::nullopt;
    auto buildfile = requireValidConfig && m_buildfile ? m_buildfile : config.build.value();
    std::unique_ptr<ServerDriver> newDriver;
    if (buildfile) {
        newDriver = ServerDriver::createFromFileLists(m_indexer, m_client, config, {*buildfile},
                                                      workspacePath, m_driver.get(),
                                                      requireValidConfig);
    }
    else {
        newDriver = ServerDriver::createForExplore(m_indexer, m_client, config, workspacePath,
                                                   m_driver.get(), requireValidConfig,
                                                   deferOpenDiagnostics);
    }
    if (!newDriver)
        return false;

    auto old_config = m_config;
    m_config = config;
    m_driver = std::move(newDriver);
    m_buildfile = std::move(buildfile);
    m_topFile.reset();
    m_configReloadPending = false;
    m_driver->diagClient->pushDiags();

    if (!m_config.indexGlobs.get().empty() || !m_config.excludeDirs.get().empty()) {
        // Deprecated config globs
        WARN("Using legacy indexGlobs or excludeDirs from config, please migrate to 'index' "
             "field");
        auto indexGlobs = m_config.getIndexGlobs();
        if (forceIndexing || (old_config.getIndexGlobs() != indexGlobs ||
                              (old_config.excludeDirs.value() != m_config.excludeDirs.value()))) {
            INFO("Updating index globs");
            if (!m_workspaceFolder) {
                // Filter to abs path globs if there's no workspace folder
                std::vector<std::string> filtered;
                std::copy_if(indexGlobs.begin(), indexGlobs.end(), std::back_inserter(filtered),
                             [](const auto& glob) {
                                 return std::filesystem::path(glob).is_absolute();
                             });
                indexGlobs = std::move(filtered);
            }
            INFO("Indexing with globs: {}", fmt::join(indexGlobs, ", "));
            m_indexer.setNumThreads(m_config.indexingThreads.value());
            m_indexer.startIndexing(indexGlobs, m_config.excludeDirs.value());
        }
    }
    else if (forceIndexing) {
        auto maybePath = m_workspaceFolder.has_value()
                             ? std::optional<std::string_view>(m_workspaceFolder->uri.getPath())
                             : std::nullopt;

        m_indexer.setNumThreads(m_config.indexingThreads.value());
        m_indexer.startIndexing(m_config.index.value(), maybePath);
    }

    // The replacement driver was already built above. Recreating it here would
    // report configuration errors twice and discard the validated reload state.
    m_contextWatchesDirty = true;
    // Retirement copies every open document into the pending queue. Analyze only
    // after the incoming edit, instead of binding the soon-obsolete text first.
    if (!deferOpenDiagnostics)
        m_driver->refreshOpenIncludeContexts();

    refreshContextFileWatches();

    // A successful reload starts a fresh CWD context. Report a later failure
    // again even when synchronous analysis never commits an isolated job.
    if (m_diagnosticFallbackReason == slang::SourceManager::FastCwdContextError().what())
        m_diagnosticFallbackReason.clear();

    // Send config to editor client if it needs to parse general configs
    m_client.setConfig(m_config);
    return true;
}

bool SlangServer::loadConfig(bool requireValidConfig) {
    std::optional<std::string> workspaceConf, userConf, localConf;

    if (m_workspaceFolder) {
        auto fsPath = fs::path(m_workspaceFolder->uri.getPath());
        workspaceConf = (fsPath / ".slang" / "server.json").string();
        localConf = (fsPath / ".slang" / "local" / "server.json").string();
    }
    else {
        WARN("No workspace folder provided, skipping workspace config");
    }

    auto home = std::getenv("HOME");
    if (home && !std::getenv("SLANG_SERVER_TESTS"))
        userConf = (fs::path(home) / ".slang" / "server.json").string();

    auto config = Config::fromFiles(workspaceConf, userConf, localConf);
    if (!config) {
        if (requireValidConfig) {
            WARN("Config reload failed: {}", config.error().what());
            return false;
        }
        m_client.showError(config.error().what());
        return m_driver ? false : loadConfig(Config{}, true);
    }
    return loadConfig(*config, true, requireValidConfig);
}

std::monostate SlangServer::addDefine(const std::string& macroName) {
    if (!m_workspaceFolder) {
        m_client.showError("Cannot add define: no workspace folder");
        return {};
    }

    auto localConfigPath = fs::path(m_workspaceFolder->uri.getPath()) / ".slang" / "local" /
                           "server.json";

    // Read existing config or start fresh
    rfl::Generic::Object obj;
    if (fs::exists(localConfigPath)) {
        auto file = std::ifstream(localConfigPath);
        auto jsonstr = std::string(std::istreambuf_iterator<char>(file),
                                   std::istreambuf_iterator<char>());
        auto generic = rfl::json::read<rfl::Generic>(
            jsonstr, YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS);
        if (generic) {
            auto maybeObj = generic->to_object();
            if (maybeObj)
                obj = *maybeObj;
        }
    }

    // Append -DMACRO to flags
    std::string newFlag = "-D" + macroName;
    std::string flags;
    if (auto existing = obj["flags"].to_string()) {
        flags = *existing;
        if (!flags.empty())
            flags += " ";
    }
    flags += newFlag;
    obj["flags"] = rfl::Generic(flags);

    // Ensure directory exists and write
    fs::create_directories(localConfigPath.parent_path());
    {
        auto out = std::ofstream(localConfigPath);
        out << rfl::json::write(rfl::Generic(obj), YYJSON_WRITE_PRETTY_TWO_SPACES);
    }

    INFO("Added {} to {}", newFlag, localConfigPath.string());

    // Reload config, preserving build file state
    auto savedBuildfile = m_buildfile;
    loadConfig();
    if (savedBuildfile)
        setBuildFile(*savedBuildfile);

    return {};
}

bool SlangServer::autoConfigure(const std::monostate&) {
    if (!m_workspaceFolder) {
        m_client.showError("Cannot auto-configure: no workspace folder");
        return false;
    }

    const auto root = fs::path(m_workspaceFolder->uri.getPath());
    const auto configPath = root / ".slang/server.json";
    try {
        rfl::Generic::Object obj;
        std::vector<std::string> incdirs;
        if (fs::exists(configPath)) {
            auto file = std::ifstream(configPath);
            if (!file)
                throw std::runtime_error("cannot read " + configPath.string());
            auto text = std::string(std::istreambuf_iterator<char>(file), {});
            auto json = rfl::json::read<rfl::Generic>(text, YYJSON_READ_ALLOW_COMMENTS |
                                                                YYJSON_READ_ALLOW_TRAILING_COMMAS);
            if (!json || !json->to_object())
                throw std::runtime_error("invalid JSON object in " + configPath.string());
            obj = *json->to_object();
            if (obj.count("incdirs")) {
                auto parsed = rfl::from_generic<std::vector<std::string>>(obj["incdirs"]);
                if (!parsed)
                    throw std::runtime_error("incdirs must be a list of strings in " +
                                             configPath.string());
                incdirs = *parsed;
            }
        }

        std::vector<Config::IndexConfig> workspaceIndex;
        if (obj.count("index")) {
            auto parsed =
                rfl::from_generic<std::vector<Config::IndexConfig>, rfl::DefaultIfMissing>(
                    obj["index"]);
            if (!parsed)
                throw std::runtime_error("invalid index configuration in " + configPath.string());
            workspaceIndex = *parsed;
        }
        const bool legacyIndex = !m_config.indexGlobs.value().empty() ||
                                 !m_config.excludeDirs.value().empty();
        bool exclusionsChanged = false;
        if (!legacyIndex) {
            for (auto& entry : workspaceIndex) {
                if (auto& exclusions = entry.excludeDirs.value(); exclusions) {
                    exclusionsChanged |= std::erase_if(*exclusions, [](const auto& path) {
                                             return (path.starts_with("./") ||
                                                     fs::path(path).is_absolute()) &&
                                                    path.find_first_of("*?") != std::string::npos;
                                         }) != 0;
                }
            }
        }
        m_indexer.setNumThreads(m_config.indexingThreads.value());
        if (legacyIndex)
            m_indexer.startIndexing(m_config.getIndexGlobs(), m_config.excludeDirs.value(), true);
        else
            m_indexer.startIndexing(m_config.index.value(), root.string(), true);

        auto ignored = m_indexer.getGitIgnoreMatches(root, workspaceIndex);
        auto within = [](const fs::path& path, const fs::path& directory) {
            auto relative = path.lexically_normal().lexically_relative(
                directory.lexically_normal());
            return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
        };
        const auto workspaceEntries = workspaceIndex.size();
        if (!legacyIndex && Indexer::usesDefaultWorkspaceIndex(m_config.index.value(), root))
            workspaceIndex.push_back({.dirs = std::vector<std::string>{"."}});
        auto indexesDirectory = [&](const Config::IndexConfig& entry, const fs::path& directory) {
            return std::ranges::any_of(entry.dirs.value(), [&](const auto& indexRoot) {
                auto path = root / indexRoot;
                return within(directory, path) || within(path, directory);
            });
        };
        struct InferredExclusion {
            fs::path directory;
            std::string path;
            std::string rule;
            bool useName;
        };
        std::vector<InferredExclusion> inferredExclusions;
        std::vector<fs::path> excludedDirectories;
        bool refreshExclusions = false;
        for (const auto& match : ignored) {
            if (!match.files.empty())
                INFO("Auto-configure: {} indexed files match Git ignore rule {} (example: {})",
                     match.files.size(), match.rule, match.files.front().generic_string());
            if (legacyIndex)
                continue;
            for (const auto& directory : match.directories) {
                const auto absolute = (root / directory).lexically_normal();
                const auto name = directory.filename().string();
                const bool useName = std::ranges::find(match.names, name) != match.names.end();
                const auto path = useName ? name : "./" + directory.generic_string();
                bool inherited = false;
                for (size_t i = workspaceEntries; i < m_config.index.value().size(); ++i)
                    inherited |= indexesDirectory(m_config.index.value()[i], absolute);
                if (inherited)
                    continue;
                bool coveredExclusion = false;
                for (const auto& entry : workspaceIndex) {
                    if (!indexesDirectory(entry, absolute))
                        continue;
                    const auto& exclusions = entry.excludeDirs.value();
                    refreshExclusions |= !exclusions ||
                                         std::ranges::find(*exclusions, path) == exclusions->end();
                    coveredExclusion = true;
                }
                if (coveredExclusion) {
                    excludedDirectories.push_back(absolute);
                    inferredExclusions.push_back({absolute, path, match.rule, useName});
                }
            }
        }
        // Ignored generated sources must not widen the suggested roots. Persist exclusions only
        // after those roots are known, so unrelated folders from the initial crawl are omitted.
        if (refreshExclusions)
            m_indexer.excludeDirectories(excludedDirectories);
        size_t ignoredFiles = 0;
        std::vector<std::string> ignoredExamples;
        for (const auto& match : ignored) {
            for (const auto& file : match.files) {
                if (std::ranges::any_of(excludedDirectories, [&](const auto& directory) {
                        return within(root / file, directory);
                    }))
                    continue;
                ++ignoredFiles;
                if (ignoredExamples.size() < 5)
                    ignoredExamples.push_back(file.generic_string());
            }
        }
        if (ignoredFiles) {
            m_client.showWarning(fmt::format(
                "Auto-configure found {} indexed {} matched by Git ignore rules that could not "
                "be excluded as whole directories, including {}. See the server log for matching "
                "rules.",
                ignoredFiles, ignoredFiles == 1 ? "file" : "files",
                fmt::join(ignoredExamples, ", ")));
        }
        auto isWorkspaceRoot = [&](const std::string& directory) {
            return (root / directory).lexically_normal().lexically_relative(root) == ".";
        };
        bool inferIndex = !legacyIndex &&
                          std::ranges::any_of(workspaceIndex, [&](const auto& entry) {
                              return std::ranges::any_of(entry.dirs.value(), isWorkspaceRoot);
                          });
        for (const auto* field : {"indexGlobs", "excludeDirs"}) {
            if (obj.count(field)) {
                auto entries = obj[field].to_array();
                inferIndex &= entries && entries->empty();
            }
        }
        std::vector<std::string> indexDirectories;
        if (inferIndex) {
            for (const auto& directory : m_indexer.getSuggestedIndexDirectories(root))
                indexDirectories.push_back(directory.generic_string());
        }
        if (!indexDirectories.empty()) {
            for (auto& entry : workspaceIndex) {
                std::vector<std::string> directories;
                for (const auto& directory : entry.dirs.value()) {
                    if (isWorkspaceRoot(directory))
                        directories.insert(directories.end(), indexDirectories.begin(),
                                           indexDirectories.end());
                    else
                        directories.push_back(directory);
                }
                entry.dirs.value() = std::move(directories);
            }
        }
        for (const auto& inferred : inferredExclusions) {
            bool changed = false;
            for (auto& entry : workspaceIndex) {
                if (!indexesDirectory(entry, inferred.directory))
                    continue;
                auto& exclusions = entry.excludeDirs.value();
                if (!exclusions)
                    exclusions.emplace();
                if (inferred.useName) {
                    changed |= std::erase_if(*exclusions, [&](const auto& existing) {
                                   return existing != inferred.path &&
                                          (existing.starts_with("./") ||
                                           fs::path(existing).is_absolute()) &&
                                          existing.find_first_of("*?") == std::string::npos &&
                                          within(root / existing, inferred.directory);
                               }) != 0;
                }
                if (std::ranges::find(*exclusions, inferred.path) == exclusions->end()) {
                    exclusions->push_back(inferred.path);
                    changed = true;
                }
            }
            exclusionsChanged |= changed;
            if (changed)
                INFO("Auto-configure: excluding {} from index ({})", inferred.path, inferred.rule);
        }

        std::unordered_set<fs::path> configured;
        for (const auto& path : m_config.incdirs.value())
            configured.insert(fs::weakly_canonical(root / path));
        for (const auto& path : incdirs)
            configured.insert(fs::weakly_canonical(root / path));

        const std::vector<fs::path> configuredDirectories(configured.begin(), configured.end());
        const auto conflicts = m_indexer.getConflictingIncludeDirectories(configuredDirectories);
        std::vector<std::string> skipped;
        size_t added = 0;
        for (const auto& path : m_indexer.getIncludeDirectories()) {
            if (configured.contains(path))
                continue;
            if (std::ranges::find(conflicts, path) != conflicts.end()) {
                skipped.push_back(path.lexically_relative(root).generic_string());
                continue;
            }
            auto relative = path.lexically_relative(root);
            incdirs.push_back((relative.empty() ? path : relative).generic_string());
            configured.insert(path);
            added++;
        }
        if (!skipped.empty()) {
            m_client.showWarning(fmt::format(
                "Auto-configure skipped include directories with conflicting headers: {}. "
                "Set their order in incdirs manually if needed.",
                fmt::join(skipped, ", ")));
        }
        if (added == 0 && indexDirectories.empty() && !exclusionsChanged) {
            m_client.showInfo("Auto-configure: no configuration changes found");
            return true;
        }

        if (added)
            obj["incdirs"] = rfl::to_generic(incdirs);
        if (!indexDirectories.empty() || exclusionsChanged) {
            auto entries = obj.count("index") ? *obj["index"].to_array() : rfl::Generic::Array{};
            for (size_t i = 0; i < workspaceIndex.size(); ++i) {
                auto entry = i < workspaceEntries ? *entries[i].to_object()
                                                  : *rfl::to_generic(workspaceIndex[i]).to_object();
                if (!indexDirectories.empty())
                    entry["dirs"] = rfl::to_generic(workspaceIndex[i].dirs.value());
                if (exclusionsChanged && workspaceIndex[i].excludeDirs.value())
                    entry["excludeDirs"] = rfl::to_generic(*workspaceIndex[i].excludeDirs.value());
                if (i < workspaceEntries)
                    entries[i] = entry;
                else
                    entries.push_back(entry);
            }
            obj["index"] = entries;
        }
        fs::create_directories(configPath.parent_path());
        const auto outputPath = fs::exists(configPath) ? fs::canonical(configPath) : configPath;
        const auto temporaryPath = outputPath.string() +
                                   fmt::format(".auto-configure-{}.tmp", slang::OS::getpid());
        slang::ScopeGuard cleanup([&] {
            std::error_code ec;
            fs::remove(temporaryPath, ec);
        });
        {
            auto file = std::ofstream(temporaryPath);
            file << rfl::json::write(rfl::Generic(obj), YYJSON_WRITE_PRETTY_TWO_SPACES) << '\n';
            file.close();
            if (!file)
                throw std::runtime_error("cannot write " + configPath.string());
        }
        fs::rename(temporaryPath, outputPath);

        auto savedBuild = m_buildfile;
        auto savedTop = m_topFile;
        loadConfig();
        if (savedBuild)
            setBuildFile(*savedBuild);
        if (savedTop)
            setTopLevel(*savedTop);
        std::vector<std::string> changes;
        if (added)
            changes.push_back(fmt::format("added {} include directories", added));
        if (exclusionsChanged)
            changes.push_back("updated index exclusions");
        if (!indexDirectories.empty())
            changes.push_back(
                fmt::format("set index directories to {}", fmt::join(indexDirectories, ", ")));
        m_client.showInfo(fmt::format("Auto-configure {} in {}", fmt::join(changes, " and "),
                                      configPath.string()));
        return true;
    }
    catch (const std::exception& error) {
        m_client.showError(fmt::format("Cannot auto-configure: {}", error.what()));
        return false;
    }
}

rfl::Variant<lsp::Definition, std::vector<lsp::DefinitionLink>, std::monostate> SlangServer::
    getDocDefinition(const lsp::DefinitionParams& params, lsp::RequestContext ctx) {
    ensureMethodScopeDriver();
    if (m_methodScope) {
        if (auto links = m_methodScope->define(params, m_config, ctx)) {
            if (m_client.capabilities.definitionLinksSupported)
                return *links;
            std::vector<lsp::Location> out;
            for (auto& link : *links)
                out.push_back({link.targetUri, link.targetRange});
            return lsp::Definition(out);
        }
        m_methodScope.reset();
    }
    if (auto instance = m_driver->getDesignInstancePathAt(params.textDocument.uri, params.position,
                                                          ctx)) {
        activateInstance(
            {
                .hierPath = std::move(*instance),
                .interactionSource = SlangLspClient::InteractionSource::editor,
            },
            ctx);
    }

    auto info = m_driver->getDefinitionInfoAt(params.textDocument.uri, params.position, ctx);
    ctx.throwIfCancelled("after definition lookup");
    if (m_client.capabilities.definitionLinksSupported)
        return info ? info->getDefinitionLspLinks() : std::vector<lsp::DefinitionLink>{};

    return lsp::Definition(info ? info->getDefinitionLspLocs() : std::vector<lsp::Location>{});
}

std::optional<lsp::Hover> SlangServer::getDocHover(const lsp::HoverParams& params,
                                                   lsp::RequestContext ctx) {
    return m_driver->getDocHover(params.textDocument.uri, params.position, ctx);
}

std::optional<std::vector<rfl::Variant<lsp::Command, lsp::CodeAction>>> SlangServer::
    getDocCodeAction(const lsp::CodeActionParams& params) {
    auto results = m_driver->codeActions.getCodeActions(params);
    if (results.empty())
        return std::nullopt;
    return results;
}

std::optional<std::vector<lsp::DocumentLink>> SlangServer::getDocDocumentLink(
    const lsp::DocumentLinkParams& params) {
    auto doc = m_driver->getDocument(params.textDocument.uri);
    if (!doc) {
        return std::nullopt;
    }
    return doc->getDocLinks();
}

rfl::Variant<std::vector<lsp::SymbolInformation>, std::vector<lsp::DocumentSymbol>, std::monostate>
SlangServer::getDocDocumentSymbol(const lsp::DocumentSymbolParams& params) {
    auto doc = m_driver->getDocument(params.textDocument.uri);
    if (doc) {
        return doc->getSymbols();
    }
    ERROR("Document {} not found", params.textDocument.uri.getPath());
    return std::vector<lsp::SymbolInformation>{};
}

std::optional<std::vector<lsp::DocumentHighlight>> SlangServer::getDocDocumentHighlight(
    const lsp::DocumentHighlightParams& params) {
    return m_driver->getDocDocumentHighlight(params.textDocument.uri, params.position);
}

std::monostate SlangServer::onShutdown(const std::nullopt_t&) {
    m_shutdownRequested = true;
    INFO("Server shutting down");
    return std::monostate{};
}

void SlangServer::onProtocolStopped() {
    if (m_diagnosticProcess) {
        m_diagnosticProcess->requestStop();
        m_diagnosticProcess->join();
    }
    m_activeDiagnosticSnapshot.reset();
}

void SlangServer::onForegroundComplete() {
    if (m_shutdownRequested) {
        if (m_diagnosticProcess)
            m_diagnosticProcess->requestStop();
        m_activeDiagnosticSnapshot.reset();
        return;
    }
    refreshContextFileWatches();
    if (m_activeDiagnosticSnapshot &&
        (!m_driver || !m_driver->isDiagnosticSnapshotCurrent(*m_activeDiagnosticSnapshot))) {
        m_diagnosticProcess->cancel();
        m_activeDiagnosticSnapshot.reset();
    }
}

bool SlangServer::hasBackgroundWork() const {
    return !m_shutdownRequested && m_driver &&
           (m_diagnosticWake->ready.load() ||
            (!m_activeDiagnosticSnapshot && m_driver->hasPendingDiagnostics()));
}

void SlangServer::reportFastCwdFailure(const slang::SourceManager::FastCwdContextError& error) {
    if (m_activeDiagnosticSnapshot && m_diagnosticProcess)
        m_diagnosticProcess->cancel();
    m_activeDiagnosticSnapshot.reset();
    if (m_diagnosticFallbackReason != error.what()) {
        m_diagnosticFallbackReason = error.what();
        m_client.showError(error.what());
    }
}

void SlangServer::onMessageException(const std::exception& error) {
    if (const auto* cwdError = dynamic_cast<const slang::SourceManager::FastCwdContextError*>(
            &error))
        reportFastCwdFailure(*cwdError);
}

void SlangServer::runBackgroundWork(const lsp::RequestContext& ctx) {
    if (m_shutdownRequested || !m_driver)
        return;
    auto reportFallback = [&](std::string_view reason) {
        if (((!m_driver->comp && m_driver->hasConfiguredCompilationInputs()) ||
             m_driver->hasSourceContextHints()) &&
            m_diagnosticFallbackReason != reason) {
            m_diagnosticFallbackReason = reason;
            m_client.showWarning("Diagnostics are running sequentially: " + std::string(reason));
        }
    };
    auto runPending = [&] {
        try {
            m_driver->runPendingDiagnostics(ctx);
        }
        catch (const slang::SourceManager::FastCwdContextError& error) {
            reportFastCwdFailure(error);
            throw;
        }
    };
    if (m_diagnosticProcess) {
        m_diagnosticWake->ready.exchange(false);
        if (auto result = m_diagnosticProcess->takeResult()) {
            if (m_activeDiagnosticSnapshot && result->jobId == m_diagnosticJobId &&
                result->generation == m_activeDiagnosticSnapshot->diagnosticGeneration &&
                m_driver->isDiagnosticSnapshotCurrent(*m_activeDiagnosticSnapshot)) {
                if (result->error.empty()) {
                    auto output = rfl::json::read<DiagnosticJobOutput, rfl::UnderlyingEnums>(
                        result->output);
                    if (output) {
                        // Retain the snapshot until commit succeeds; cancellation
                        // cannot lose the only pending complete result.
                        try {
                            m_driver->commitDiagnosticSnapshot(*m_activeDiagnosticSnapshot,
                                                               std::move(output).value(), ctx);
                            m_activeDiagnosticSnapshot.reset();
                            if (!m_diagnosticFallbackReason.empty()) {
                                m_diagnosticFallbackReason.clear();
                                m_client.showInfo("Package diagnostics are isolated again");
                            }
                            return;
                        }
                        catch (const slang::SourceManager::FastCwdContextError& error) {
                            reportFastCwdFailure(error);
                            throw;
                        }
                        catch (const std::exception& error) {
                            result->error = error.what();
                        }
                    }
                    else
                        result->error = output.error().what();
                }
                ctx.error("Isolated diagnostics unavailable for this generation: {}. Using "
                          "sequential diagnostics.",
                          result->error);
                reportFallback(result->error);
                m_activeDiagnosticSnapshot.reset();
                runPending();
                return;
            }
            m_activeDiagnosticSnapshot.reset();
        }
    }
    if (m_activeDiagnosticSnapshot || !m_driver->hasPendingDiagnostics())
        return;
    if (!m_diagnosticExecutable.empty()) {
        try {
            if (!m_diagnosticProcess) {
                auto state = m_diagnosticWake;
                auto wake = backgroundWakeCallback();
                m_diagnosticProcess = std::make_unique<isolated_diagnostics::DiagnosticProcess>(
                    m_diagnosticExecutable, isolated_diagnostics::Limits{}, [state, wake] {
                        state->ready.store(true);
                        wake();
                    });
            }
            ensureMethodScopeDriver();
            auto snapshot = m_methodScope ? m_methodScope->diagnostics(m_config, ctx)
                                          : std::optional<ServerDriver::DiagnosticSnapshot>{};
            if (!snapshot) {
                m_methodScope.reset();
                snapshot = m_driver->captureDiagnosticSnapshot(ctx);
            }
            if (snapshot) {
                auto metadata = snapshot->metadata;
                auto input = snapshot->input;
                isolated_diagnostics::Job job;
                job.generation = snapshot->diagnosticGeneration;
                job.jobId = ++m_diagnosticJobId;
                if (m_config.experimentalUvmPrefixCache.value())
                    job.sessionKey = m_driver->completions.getCompletionEpoch();
                job.encode = [metadata = std::move(metadata), input] {
                    return encodeDiagnosticJob(metadata, *input);
                };
                if (!m_diagnosticProcess->submit(std::move(job)))
                    throw std::runtime_error("diagnostic helper is stopped");
                m_activeDiagnosticSnapshot = std::move(snapshot);
                ctx.info("Submitted isolated diagnostic snapshot job {}", m_diagnosticJobId);
                return;
            }
        }
        catch (const lsp::RequestCancelled&) {
            throw;
        }
        catch (const slang::SourceManager::FastCwdContextError& error) {
            reportFastCwdFailure(error);
            throw;
        }
        catch (const std::exception& error) {
            ctx.info("Isolated diagnostics unsupported for this context: {}. Using sequential "
                     "diagnostics.",
                     error.what());
            reportFallback(error.what());
        }
    }
    runPending();
}

void SlangServer::onDocDidOpen(const lsp::DidOpenTextDocumentParams& params) {
    m_methodScope.reset();
    m_driver->openDocument(params.textDocument.uri, params.textDocument.text,
                           params.textDocument.version, !m_diagnosticExecutable.empty());
}

void SlangServer::maybeRetireParsedContext() {
    if (!m_config.experimentalUvmPrefixCache.value() ||
        std::getenv("SLANG_UVM_GENERATION_DISABLE") || !m_driver || m_driver->comp || m_buildfile ||
        m_topFile)
        return;
    auto epoch = m_driver->completions.getCompletionEpoch();
    auto table = m_driver->sm.getSourceLocationTableBytes();
    auto bytes = table + m_driver->sm.getMemoryUsage();
    if (m_contextEpoch != epoch) {
        m_contextEpoch = epoch;
        m_contextBudget = std::max<size_t>(16 * 1024 * 1024, bytes * 2);
        m_contextEdits = 0;
    }
    uint64_t forced = 0;
    if (auto* value = std::getenv("SLANG_UVM_GENERATION_EDIT_LIMIT"))
        forced = std::strtoull(value, nullptr, 10);
    if (bytes <= m_contextBudget && (!forced || m_contextEdits < forced)) {
        ++m_contextEdits;
        return;
    }
    auto oldEntries = m_driver->sm.getSourceLocationEntryCount();
    UvmPhaseTrace phases;
    auto trace = [&](const char* result, uint64_t newEpoch, size_t newBytes) {
        if (auto* path = std::getenv("SLANG_UVM_GENERATION_TRACE"))
            if (auto* file = std::fopen(path, "a")) {
                std::fprintf(file, "%ld\t%s\t%llu\t%llu\t%zu\t%zu\t%zu\n",
                             long(slang::OS::getpid()), result, (unsigned long long)epoch,
                             (unsigned long long)newEpoch, oldEntries, bytes, newBytes);
                std::fclose(file);
            }
    };
    // Foreground and background model access is serialized by serverStateMutex.
    // Diagnostic children own copied snapshots; cancel and discard their old owner before swapping.
    m_methodScope.reset();
    if (m_diagnosticProcess)
        m_diagnosticProcess->cancel();
    m_activeDiagnosticSnapshot.reset();
    m_driver->prepareForContextRetirement();
    phases.mark("retirement-release");
    try {
        if (!loadConfig(m_config, false, true, true)) {
            ++m_contextEdits;
            trace("replacement-failed", epoch, bytes);
            return;
        }
        phases.mark("retirement-load-publish");
        m_contextRetired = true;
        m_driver->completions.requireCompletionEpoch();
        m_contextEpoch = m_driver->completions.getCompletionEpoch();
        auto fresh = m_driver->sm.getSourceLocationTableBytes() + m_driver->sm.getMemoryUsage();
        m_contextBudget = std::max<size_t>(16 * 1024 * 1024, fresh * 2);
        m_contextEdits = 0;
        trace("retired", m_contextEpoch, fresh);
    }
    catch (const std::exception&) {
        // A failed construction keeps the old owner. If a later publication step threw after
        // the swap, fence that new owner too; the incoming text change is still applied once.
        auto actual = m_driver->completions.getCompletionEpoch();
        if (actual != epoch) {
            m_contextRetired = true;
            m_driver->completions.requireCompletionEpoch();
            m_contextEpoch = actual;
            auto fresh = m_driver->sm.getSourceLocationTableBytes() + m_driver->sm.getMemoryUsage();
            m_contextBudget = std::max<size_t>(16 * 1024 * 1024, fresh * 2);
            m_contextEdits = 0;
        }
        else
            ++m_contextEdits;
        trace("replacement-exception", actual,
              m_driver->sm.getSourceLocationTableBytes() + m_driver->sm.getMemoryUsage());
    }
}

void SlangServer::onDocDidChange(const lsp::DidChangeTextDocumentParams& params,
                                 lsp::RequestContext ctx) {
    if (m_config.experimentalMethodScopeOverlay.value() && !m_driver->comp) {
        bool synced = false;
        try {
            ensureMethodScopeDriver();
            if (!m_methodScope) {
                if (params.contentChanges.size() != 1)
                    throw std::runtime_error("one method change required");
                auto doc = m_driver->getDocument(params.textDocument.uri);
                size_t offset = 0;
                if (auto* whole = rfl::get_if<lsp::TextDocumentContentChangeWholeDocument>(
                        &params.contentChanges[0])) {
                    auto old = doc->getText();
                    if (!old.empty() && old.back() == '\0')
                        old.remove_suffix(1);
                    while (offset < old.size() && offset < whole->text.size() &&
                           old[offset] == whole->text[offset])
                        ++offset;
                }
                else {
                    auto& edit = rfl::get<lsp::TextDocumentContentChangePartial>(
                        params.contentChanges[0]);
                    auto loc = toSourceLocation(doc->getBuffer(), edit.range.start, m_driver->sm);
                    if (!loc)
                        throw std::runtime_error("invalid edit location");
                    offset = loc->offset();
                }
                m_methodScope = std::make_unique<MethodScopeOverlay>(*m_driver, m_config,
                                                                     params.textDocument.uri,
                                                                     offset, ctx);
            }
            if (m_methodScope->apply(params, m_config, ctx, synced)) {
                m_methodScopeDecline.clear();
                maybeRetireParsedContext();
                return;
            }
        }
        catch (const lsp::RequestCancelled&) {
            m_methodScope.reset();
            if (!synced)
                m_driver->onDocDidChange(params,
                                         {}); // Cancellation never drops text synchronization.
            throw;
        }
        catch (const std::exception& error) {
            m_methodScopeDecline = error.what();
            m_methodScope.reset();
            if (synced)
                throw; // Text may already be committed: never replay an incremental range.
        }
    }
    m_driver->onDocDidChange(params, ctx);
    // Seed a replacement with the latest unsaved text. Rebuilding before this
    // notification would parse the previous version and immediately invalidate it.
    maybeRetireParsedContext();
}

void SlangServer::onDocDidSave(const lsp::DidSaveTextDocumentParams& params) {
    m_methodScope.reset();
    // Only process documents that have been explicitly opened by the client
    if (!m_driver->isDocumentOpen(params.textDocument.uri)) {
        return;
    }

    auto doc = m_driver->getDocument(params.textDocument.uri);
    if (!doc) {
        throw std::runtime_error(
            fmt::format("Document {} not found", params.textDocument.uri.getPath()));
    }

    // Validate that our view of the document is accurate
    if (params.text.has_value()) {
        std::string_view text = params.text.value();

        if (!doc->textMatches(text)) {
            // Recover by overwriting the buffer with the saved text
            INFO("Document text does not match on save, overwriting");
            m_driver->openDocument(params.textDocument.uri, text);
            doc = m_driver->getDocument(params.textDocument.uri);
        }
    }

    m_driver->onDocDidSave(*doc);
}

void SlangServer::onDocDidClose(const lsp::DidCloseTextDocumentParams& params) {
    m_methodScope.reset();
    // TODO: Add method in ServerDriver to check that the rc of the document is 1 before
    // removing (non-compilation mode)
    bool restorePreinclude = !m_driver->comp && !m_buildfile && !m_topFile &&
                             m_driver->isDocumentOpen(params.textDocument.uri) &&
                             (m_driver->isExplorePreinclude(params.textDocument.uri) ||
                              m_driver->hasConfiguredCompilationInputs() ||
                              m_driver->hasSourceContextHints());
    m_driver->closeDocument(params.textDocument.uri);
    if (restorePreinclude) {
        // didClose returns content authority to disk, including discarded edits.
        // A fresh cache also handles an unsaved header that never existed on disk.
        // copyOpenDocumentsFrom retains every other open unsaved buffer.
        setExplore();
        m_driver->refreshOpenIncludeContexts();
    }
}

void SlangServer::onWorkspaceDidChangeWatchedFiles(const lsp::DidChangeWatchedFilesParams& params) {
    m_methodScope.reset();
    auto openOverlay = [&](const URI& uri, const fs::path& path) {
        if (m_driver->isDocumentOpen(uri))
            return true;
        for (const auto& [openUri, document] : m_driver->docs) {
            if (m_driver->isDocumentOpen(openUri) &&
                (normalizeWatchedPath(fs::path(openUri.getPath())) == path ||
                 normalizeWatchedPath(m_driver->sm.getFullPath(document->getBuffer())) == path))
                return true;
        }
        return false;
    };
    // Check if any config or active build files changed
    bool needsReload = m_configReloadPending;
    for (const auto& change : params.changes) {
        const auto changedPath = normalizeWatchedPath(fs::path(change.uri.getPath()));
        auto isConfigPath = [&](const fs::path& root) {
            return changedPath == normalizeWatchedPath(root / ".slang" / "server.json") ||
                   changedPath == normalizeWatchedPath(root / ".slang" / "local" / "server.json");
        };
        if (m_workspaceFolder && isConfigPath(fs::path(m_workspaceFolder->uri.getPath())))
            needsReload = true;
        if (const char* home = std::getenv("HOME"); home && !std::getenv("SLANG_SERVER_TESTS"))
            needsReload |= changedPath ==
                           normalizeWatchedPath(fs::path(home) / ".slang" / "server.json");
        if (m_driver->isUvmConfigurationInput(changedPath) && !openOverlay(change.uri, changedPath))
            needsReload = true;
        if (needsReload)
            break;
        for (const auto& [commandPath, _] : m_driver->driver.getCommandFileMetadata()) {
            if (normalizeWatchedPath(commandPath) == changedPath) {
                needsReload = true;
                break;
            }
        }
        if (needsReload)
            break;
    }

    if (needsReload) {
        INFO("Config or build file changed, reloading");
        auto savedTop = m_topFile;
        if (loadConfig(/* requireValidConfig */ true)) {
            if (savedTop)
                setTopLevel(*savedTop);
            return;
        }
        m_configReloadPending = true;
        WARN("Keeping the current configuration; retrying on the next file change");
        return;
    }

    // Disk events never supersede an open editor buffer. Filter before deciding
    // to rebuild the SourceManager, and use canonical paths as clients can report
    // another URI spelling for the same file (including deletion notifications).
    lsp::DidChangeWatchedFilesParams sourceChanges;
    for (const auto& change : params.changes) {
        const auto changedPath = normalizeWatchedPath(fs::path(change.uri.getPath()));
        if (!openOverlay(change.uri, changedPath))
            sourceChanges.changes.push_back(change);
    }
    if (sourceChanges.changes.empty())
        return;

    // SourceManager cannot evict one cached file. In package-first / explore mode,
    // rebuild its cache after deletion or a cached failed read. Some clients coalesce
    // create+write into Changed, so both Created and Changed must retry negative entries.
    // Preserve every other open unsaved buffer. This reloads only open documents and their bounded
    // owners/dependencies, not a design.
    bool rebuildExplore = false;
    if (!m_driver->comp && !m_buildfile && !m_topFile) {
        for (const auto& change : sourceChanges.changes) {
            auto path = change.uri.getPath();
            if (m_driver->hasConfiguredCompilationInputs() &&
                m_driver->configuredInputLoadFailed() &&
                (change.type == lsp::FileChangeType::Created ||
                 change.type == lsp::FileChangeType::Changed)) {
                const auto changedPath = normalizeWatchedPath(fs::path(path));
                for (const auto& failedPath :
                     m_driver->driver.sourceLoader.getFailedLiteralPaths()) {
                    if (normalizeWatchedPath(failedPath) == changedPath) {
                        rebuildExplore = true;
                        break;
                    }
                }
            }
            if (!m_driver->sm.isCached(path))
                continue;
            if (change.type == lsp::FileChangeType::Deleted ||
                ((change.type == lsp::FileChangeType::Created ||
                  change.type == lsp::FileChangeType::Changed) &&
                 !m_driver->sm.readSource(path, nullptr)))
                rebuildExplore = true;
        }
    }
    m_indexer.onWorkspaceDidChangeWatchedFiles(sourceChanges);
    if (rebuildExplore) {
        setExplore();
        m_driver->refreshOpenIncludeContexts();
    }
    else {
        m_driver->onWorkspaceDidChangeWatchedFiles(sourceChanges);
    }
}

static bool fuzzyMatch(std::string_view query, std::string_view candidate) {
    auto qi = query.begin();
    for (auto ci = candidate.begin(); qi != query.end() && ci != candidate.end(); ++ci) {
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(*qi))) ==
            static_cast<char>(std::tolower(static_cast<unsigned char>(*ci)))) {
            ++qi;
        }
    }
    return qi == query.end();
}

rfl::Variant<std::vector<lsp::SymbolInformation>, std::vector<lsp::WorkspaceSymbol>, std::monostate>
SlangServer::getWorkspaceSymbol(const lsp::WorkspaceSymbolParams& params) {
    slang::TimeTraceScope _timeScope("getWorkspaceSymbol", "");

    std::vector<lsp::WorkspaceSymbol> result;

    m_indexer.forEachSymbol([&](const std::string& name, const Indexer::GlobalSymbolLoc& entry) {
        if (!params.query.empty() && !fuzzyMatch(params.query, name))
            return;
        result.emplace_back(
            lsp::WorkspaceSymbol{.location = lsp::LocationUriOnly{URI::fromFile(*entry.uri)},
                                 .name = name,
                                 .kind = toSymbolKind(entry.kind)});
    });

    return result;
}

rfl::Variant<std::vector<lsp::CompletionItem>, lsp::CompletionList, std::monostate> SlangServer::
    getDocCompletion(const lsp::CompletionParams& params, lsp::RequestContext requestCtx) {
    ensureMethodScopeDriver();
    if (m_methodScope) {
        if (auto result = m_methodScope->complete(params, m_config, requestCtx))
            return *result;
        m_methodScope.reset();
    }
    std::vector<lsp::CompletionItem> results;

    auto doc = m_driver->getDocument(params.textDocument.uri);
    if (!doc) {
        m_client.showError(fmt::format("Document {} not found", params.textDocument.uri.getPath()));
        return std::monostate{};
    }
    auto maybeLoc = doc->getLocation(params.position, requestCtx);
    m_driver->requireExplicitContext(params.textDocument.uri);
    if (!maybeLoc) {
        WARN("No location found for position {},{}", params.position.line,
             params.position.character);
        return std::monostate{};
    }
    auto loc = maybeLoc.value();

    // Clients only send context when they advertise contextSupport. Treat an omitted
    // context as an ordinary invocation instead of dereferencing an empty optional.
    auto lspContext = params.context.value_or(
        lsp::CompletionContext{.triggerKind = lsp::CompletionTriggerKind::Invoked});
    auto ctx = CompletionContext::fromLocation(*doc, loc, std::move(lspContext), requestCtx);

    INFO("Completion: kind={} trigger='{}' query={}", toString(ctx.lspContext.triggerKind),
         ctx.lspContext.triggerCharacter.value_or(""), toString(ctx.query->kind()));

    requestCtx.throwIfCancelled("before completion lookup");
    m_driver->completions.getCompletions(results, doc, ctx);
    requestCtx.throwIfCancelled("after completion lookup");

    // TODO: rank results using order- the lsp is pretty stupid with this.
    // We need to hack around with client side middileware like in clangd-
    // https://github.com/clangd/vscode-clangd/blob/master/src/clangd-context.ts

    if (ctx.query->isIncomplete()) {
        // Macro results are server-filtered and bounded. Mark the list incomplete so clients ask
        // again as the user types instead of treating omitted candidates as unavailable.
        return lsp::CompletionList{.isIncomplete = true, .items = std::move(results)};
    }

    return results;
}

lsp::CompletionItem SlangServer::getCompletionItemResolve(const lsp::CompletionItem& item,
                                                          lsp::RequestContext ctx) {
    if (item.documentation.has_value()) {
        // Already resolved
        return item;
    }

    ensureMethodScopeDriver();
    if (m_methodScope) {
        if (auto result = m_methodScope->resolve(item, m_config, ctx))
            return *result;
        // Native resolution of an enclosing declaration can need current full
        // syntax. Retain the value certificate while its inputs remain valid so
        // other items from this same completion response still resolve safely.
        if (!m_methodScope->valid(m_config, ctx))
            m_methodScope.reset();
    }
    lsp::CompletionItem ret = item;
    if (m_contextRetired && m_config.experimentalUvmPrefixCache.value())
        m_driver->completions.requireCompletionEpoch();
    m_driver->completions.getCompletionItemResolve(ret, ctx);
    return ret;
}

std::optional<std::vector<lsp::InlayHint>> SlangServer::getDocInlayHint(
    const lsp::InlayHintParams& params) {
    auto doc = m_driver->getDocument(params.textDocument.uri);
    if (!doc) {
        return {};
    }
    auto hints = doc->getAnalysis()->getInlayHints(params.range, m_config.inlayHints.get());
    INFO("Providing {} inlay hints for {}", hints.size(), doc->getWsRelativePath());
    return hints;
}

std::optional<std::vector<lsp::CodeLens>> SlangServer::getDocCodeLens(
    const lsp::CodeLensParams& params) {
    auto doc = m_driver->getDocument(params.textDocument.uri);
    if (!doc) {
        return std::nullopt;
    }

    std::vector<lsp::CodeLens> lenses;
    auto tree = doc->getSyntaxTree();
    auto path = m_driver->sm.getFullPath(doc->getBuffer());
    auto contexts = m_driver->getIncludeContexts(path);
    if (!contexts.empty()) {
        const auto& active = contexts.front();
        const auto& location = active.location;
        lenses.push_back(lsp::CodeLens{
            .range = {{0, 0}, {0, 0}},
            .command =
                lsp::Command{
                    .title = fmt::format("Included by {}:{}",
                                         fs::path(location.uri.getPath()).filename().string(),
                                         location.range.start.line + 1),
                    .command = "slang.showLocation",
                    .arguments = std::vector<lsp::LSPAny>{rfl::to_generic(location)},
                },
        });
        if (contexts.size() > 1) {
            std::vector<SlangLspClient::QuickPickItem> items;
            for (const auto& context : contexts) {
                auto source = context.source->getWsRelativePath();
                auto include = context.location.uri.getPath();
                include.remove_prefix(m_driver->getWsRelativePathOffset(include));
                auto description = std::string(source);
                if (&context == &active)
                    description += " (current)";
                items.push_back({
                    .label = fmt::format("{}:{}", include, context.location.range.start.line + 1),
                    .description = std::move(description),
                    .value = rfl::to_generic(ServerDriver::IncludeContextSelection{
                        .uri = params.textDocument.uri,
                        .source = context.source->getURI(),
                        .occurrence = context.occurrence,
                    }),
                });
            }
            lenses.push_back(lsp::CodeLens{
                .range = {{0, 0}, {0, 0}},
                .command =
                    lsp::Command{
                        .title = fmt::format("And {} other{}", contexts.size() - 1,
                                             contexts.size() == 2 ? "" : "s"),
                        .tooltip = "Select include context",
                        .command = "slang.quickPick",
                        .arguments = std::vector<lsp::LSPAny>{rfl::to_generic(
                            SlangLspClient::QuickPickParams{
                                .placeholder = "Select include context",
                                .items = std::move(items),
                                .onSelectCommand = "slang.setIncludeContext",
                            })},
                    },
            });
        }
    }
    if (!m_driver->comp)
        return lenses.empty() ? std::nullopt : std::optional(std::move(lenses));

    auto& meta = tree->getMetadata();
    for (const auto& [decl, _] : meta.nodeMeta) {
        if (!decl || !decl->header) {
            continue;
        }

        const auto& nameToken = decl->header->name;
        if (nameToken.location().buffer() != doc->getBuffer())
            continue;
        auto moduleName = std::string(nameToken.valueText());
        if (moduleName.empty()) {
            continue;
        }

        const auto& instances = m_driver->comp->getInstancesOfModule(moduleName);
        if (instances.empty()) {
            continue;
        }

        // Module isn't reachable from the active build.
        auto* activeInstance = m_driver->comp->getActiveInstanceSymbol(moduleName);
        if (!activeInstance) {
            continue;
        }

        std::vector<std::string> instancePaths;
        instancePaths.reserve(instances.size());
        for (auto* instance : instances)
            instancePaths.push_back(instance->getHierarchicalPath());

        auto range = toRange(nameToken.location(), m_driver->sm, nameToken.rawText().size());
        auto title = fmt::format("{} ({})", activeInstance->getHierarchicalPath(),
                                 instances.size());
        auto selectCommand = instances.size() == 1
                                 ? lsp::Command{
                                       .title = title,
                                       .tooltip = "Show active instance in hierarchy",
                                       .command = "slang.showInHierarchy",
                                       .arguments = std::vector<lsp::LSPAny>{
                                           rfl::to_generic<rfl::UnderlyingEnums>(
                                               SlangLspClient::ActivateInstanceParams{
                                                   .hierPath = activeInstance->getHierarchicalPath(),
                                                   .interactionSource = SlangLspClient::
                                                       InteractionSource::codeLensSelect,
                                               })},
                                   }
                                 : m_client.makeQuickPickCommand(
                                       title, fmt::format("Select active instance for {}", moduleName),
                                       fmt::format("Select active instance for {}", moduleName),
                                       activeInstance->getHierarchicalPath(), instancePaths,
                                       "slang.activateInstance",
                                       SlangLspClient::InteractionSource::codeLensSelect);
        lenses.push_back(lsp::CodeLens{
            .range = range,
            .command = std::move(selectCommand),
        });
        if (!activeInstance->isTopLevel()) {
            lenses.push_back(lsp::CodeLens{
                .range = range,
                .command = m_client.makeActivateInstanceCommand(
                    "Go to Instantiation", "", activeInstance->getHierarchicalPath(),
                    SlangLspClient::InteractionSource::codeLensGotoInstantiation),
            });
        }

        auto generateVisitor = syntax::makeSyntaxVisitor(
            [&](auto& visitor, const syntax::LoopGenerateSyntax& loop) {
                if (auto activeLoop = m_driver->comp->getActiveGenerateLoop(moduleName, loop)) {
                    auto range = toRange(loop.keyword.location(), m_driver->sm,
                                         loop.keyword.rawText().size());
                    lenses.push_back(lsp::CodeLens{
                        .range = range,
                        .command = m_client.makeQuickPickCommand(
                            activeLoop->activePath, "Select active generate iteration",
                            "Select active generate iteration", activeLoop->activePath,
                            activeLoop->iterationPaths, "slang.activateInstance",
                            SlangLspClient::InteractionSource::codeLensSelect),
                    });
                }
                visitor.visitDefault(loop);
            },
            [&](auto& visitor, const syntax::ModuleDeclarationSyntax& module) {
                if (&module == decl)
                    visitor.visitDefault(module);
            });
        generateVisitor.visit(*decl);
    }

    return lenses;
}

std::optional<std::vector<lsp::Location>> SlangServer::getDocReferences(
    const lsp::ReferenceParams& params, lsp::RequestContext ctx) {
    auto references = m_driver->getDocReferences(params.textDocument.uri, params.position,
                                                 params.context.includeDeclaration, ctx);
    auto doc = m_driver->getDocument(params.textDocument.uri);
    ctx.info("Found {} references for {}", references ? references->size() : 0,
             doc ? doc->getWsRelativePath() : params.textDocument.uri.getPath());
    return references;
}

std::optional<lsp::WorkspaceEdit> SlangServer::getDocRename(const lsp::RenameParams& params) {
    return m_driver->getDocRename(params.textDocument.uri, params.position, params.newName);
}

SourceManager& SlangServer::sourceManager() {
    return m_driver->sm;
}
} // namespace server
