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
#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/ranges.h>
#include <memory>
#include <optional>
#include <ranges>
#include <rfl/Variant.hpp>
#include <rfl/from_generic.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxPrinter.h"
#include "slang/syntax/SyntaxVisitor.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"
#include "slang/util/OS.h"
#include "slang/util/TimeTrace.h"
#include "slang/util/VersionInfo.h"

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

    // Config modification
    registerCommand<std::string, std::monostate, &SlangServer::addDefine>("slang.addDefine");

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
                lsp::FileSystemWatcher{.globPattern =
                                           lsp::RelativePattern{.baseUri = m_workspaceFolder->uri,
                                                                .pattern = ".slang/**/*.json"},
                                       .kind = lsp::WatchKind::Change},
                // Build/flag files
                lsp::FileSystemWatcher{.globPattern =
                                           lsp::RelativePattern{.baseUri = m_workspaceFolder->uri,
                                                                .pattern = "**/*.f"},
                                       .kind = lsp::WatchKind::Change},
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

    syntax::SyntaxPrinter printer(doc->getSyntaxTree()->sourceManager());
    printer.setSquashNewlines(false);
    printer.setIncludeDirectives(true);
    printer.setExpandMacros(true);
    OS::writeFile(args.dst, printer.print(*doc->getSyntaxTree()).str());
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

void SlangServer::loadConfig(const Config& config, bool forceIndexing) {
    auto old_config = m_config;
    m_config = Config(config);

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

    // Rebuild open documents only after the new index scope is available, so an
    // old graph cannot briefly supply stale ownership or selection warnings.
    if (m_config.build.value().has_value()) {
        m_client.showInfo("Using build file: " + *m_config.build.value());
        setBuildFile(*m_config.build.value());
    }
    else {
        setExplore();
    }
    m_driver->refreshOpenIncludeContexts();

    refreshContextFileWatches();

    // A successful reload starts a fresh CWD context. Report a later failure
    // again even when synchronous analysis never commits an isolated job.
    if (m_diagnosticFallbackReason == slang::SourceManager::FastCwdContextError().what())
        m_diagnosticFallbackReason.clear();

    // Send config to editor client if it needs to parse general configs
    m_client.setConfig(m_config);
}

void SlangServer::loadConfig() {
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

    loadConfig(Config::fromFiles(workspaceConf, userConf, localConf, m_client), true);
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

rfl::Variant<lsp::Definition, std::vector<lsp::DefinitionLink>, std::monostate> SlangServer::
    getDocDefinition(const lsp::DefinitionParams& params, lsp::RequestContext ctx) {
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

rfl::Variant<std::vector<lsp::SymbolInformation>, std::vector<lsp::DocumentSymbol>, std ::monostate>
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
    if (const auto* cwdError = dynamic_cast<const slang::SourceManager::FastCwdContextError*>(&error))
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
        try { m_driver->runPendingDiagnostics(ctx); }
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
            auto snapshot = m_driver->captureDiagnosticSnapshot(ctx);
            if (snapshot) {
                auto metadata = snapshot->metadata;
                auto input = snapshot->input;
                isolated_diagnostics::Job job;
                job.generation = snapshot->diagnosticGeneration;
                job.jobId = ++m_diagnosticJobId;
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
    m_driver->openDocument(params.textDocument.uri, params.textDocument.text,
                           params.textDocument.version, !m_diagnosticExecutable.empty());
}

void SlangServer::onDocDidChange(const lsp::DidChangeTextDocumentParams& params,
                                 lsp::RequestContext ctx) {
    m_driver->onDocDidChange(params, ctx);
}

void SlangServer::onDocDidSave(const lsp::DidSaveTextDocumentParams& params) {
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
    // Check if any config or active build files changed
    bool needsReload = false;
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
        auto savedBuildfile = m_buildfile;
        loadConfig();
        if (savedBuildfile)
            setBuildFile(*savedBuildfile);
        return;
    }

    // Disk events never supersede an open editor buffer. Filter before deciding
    // to rebuild the SourceManager, and use canonical paths as clients can report
    // another URI spelling for the same file (including deletion notifications).
    lsp::DidChangeWatchedFilesParams sourceChanges;
    for (const auto& change : params.changes) {
        const auto changedPath = normalizeWatchedPath(fs::path(change.uri.getPath()));
        bool openOverlay = m_driver->isDocumentOpen(change.uri);
        if (!openOverlay) {
            for (const auto& [uri, document] : m_driver->docs) {
                if (m_driver->isDocumentOpen(uri) &&
                    (normalizeWatchedPath(fs::path(uri.getPath())) == changedPath ||
                     normalizeWatchedPath(m_driver->sm.getFullPath(document->getBuffer())) ==
                         changedPath)) {
                    openOverlay = true;
                    break;
                }
            }
        }
        const bool protectOverlay = (!m_driver->comp &&
                                     m_driver->hasConfiguredCompilationInputs()) ||
                                    m_driver->hasSourceContextHints();
        if (!openOverlay || !protectOverlay)
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

    lsp::CompletionItem ret = item;
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
    if (!m_driver->comp) {
        return std::nullopt;
    }
    auto doc = m_driver->getDocument(params.textDocument.uri);
    if (!doc) {
        return std::nullopt;
    }

    std::vector<lsp::CodeLens> lenses;
    auto& meta = doc->getSyntaxTree()->getMetadata();
    for (const auto& [decl, _] : meta.nodeMeta) {
        if (!decl || !decl->header) {
            continue;
        }

        const auto& nameToken = decl->header->name;
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
