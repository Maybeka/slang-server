//------------------------------------------------------------------------------
// ServerDriver.cpp
// Implementation of server driver class for processing syntax trees
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "ServerDriver.h"

#include "Indexer.h"
#include "ServerDiagClient.h"
#include "SystemTaskDocs.h"
#include "ast/ServerCompilation.h"
#include "ast/TrustedUvmLibrary.h"
#include "completions/CompletionDispatch.h"
#include "document/SlangDoc.h"
#include "lsp/LspTypes.h"
#include "lsp/RequestContext.h"
#include "lsp/URI.h"
#include "util/Converters.h"
#include "util/Formatting.h"
#include "util/Logging.h"
#include "util/Markdown.h"
#include "util/SlangExtensions.h"
#include "util/UvmPhaseTrace.h"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <queue>
#include <set>
#include <string_view>
#include <tuple>

#include "slang/ast/Compilation.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/SystemSubroutine.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/TextDiagnosticClient.h"
#include "slang/driver/Driver.h"
#include "slang/driver/SourceLoader.h"
#include "slang/parsing/ParserMetadata.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxVisitor.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"

namespace server {
using namespace slang;

bool ServerDriver::s_debugHoversEnabled =
#ifdef SLANG_DEBUG
    true;
#else
    false;
#endif

ServerDriver::ServerDriver(Indexer& indexer, SlangLspClient& client, const Config& config,
                           std::vector<std::string> buildfiles,
                           std::optional<std::string_view> workspaceFolder, bool requireValidConfig,
                           const ServerDriver* overlays) :
    m_driverOwner(std::make_shared<driver::Driver>()), sm(m_driverOwner->sourceManager),
    diagEngine(m_driverOwner->diagEngine), client(client), driver(*m_driverOwner),
    diagClient(std::make_shared<ServerDiagClient>(
        sm, client, config.experimentalDiagnosticDeltaPublishing.value())),
    completions(*this, indexer, sm, options), codeActions(*this, sm, indexer), m_indexer(indexer),
    m_config(config), m_workspacePathPrefix(workspaceFolder.value_or("")) {
    if (overlays && overlays->m_preserveTrustedUvm && overlays->m_trustedScopes &&
        config.experimentalScopeSemanticCache.value()) {
        m_trustedScopes = overlays->m_trustedScopes;
        m_scopeBodies = overlays->m_scopeBodies;
        m_scopeRoots = overlays->m_scopeRoots;
        m_scopeRootCount = overlays->m_scopeRootCount;
        m_scopeInstanceSelections = overlays->m_scopeInstanceSelections;
        sm.inheritImmutablePrefix(m_trustedScopes->sources);
        if (overlays->m_scopePrefix)
            m_scopePrefix = syntax::SyntaxTree::sharedPrefixView(overlays->m_scopePrefix, sm);
    }
    if (!m_trustedScopes && overlays && overlays->m_preserveTrustedUvm && overlays->m_trustedUvm &&
        config.experimentalUvmSemanticCache.value()) {
        m_trustedUvm = overlays->m_trustedUvm;
        sm.inheritImmutablePrefix(m_trustedUvm->sources);
    }
    if (isFrozenSourcesEnabled() || config.experimentalScopeSemanticCache.value())
        completions.requireCompletionEpoch();
    diagClient->setUriResolver([this](const URI& physical) {
        // Exact open URIs win; otherwise bind the known physical source to its
        // client's spelling. Never canonicalize arbitrary publication URIs.
        if (m_openDocs.contains(physical))
            return physical;
        for (const auto& uri : m_openDocs) {
            auto found = docs.find(uri);
            if (found != docs.end() &&
                sm.getFullPath(found->second->getBuffer()).generic_string() == physical.getPath())
                return uri;
        }
        return physical;
    });
    if (!m_workspacePathPrefix.empty() && m_workspacePathPrefix.back() != '/' &&
        m_workspacePathPrefix.back() != '\\') {
        m_workspacePathPrefix.push_back(std::filesystem::path::preferred_separator);
    }
    if (overlays) {
        for (const auto& uri : overlays->m_openDocs) {
            auto found = overlays->docs.find(uri);
            if (found == overlays->docs.end())
                continue;
            auto text = found->second->getText();
            if (!text.empty() && text.back() == '\0')
                text.remove_suffix(1);
            auto path = overlays->sm.getFullPath(found->second->getBuffer());
            if (isFrozenSourcesEnabled()) {
                std::error_code error;
                auto current = std::filesystem::weakly_canonical(uri.getPath(), error);
                if (!error)
                    path = current;
                driver.sourceLoader.addInputOverlay(path);
            }
            sm.assignText(path.string(), text);
        }
    }

    m_configLoaded = parseAndLoadSources(buildfiles, requireValidConfig);
}

bool ServerDriver::parseAndLoadSources(const std::vector<std::string>& buildfiles,
                                       bool requireValidConfig) {
    // Resolve configuration paths in the workspace, then restore the caller's
    // cwd before recording immutable source lookups for diagnostics.
    const auto previousDirectory = std::filesystem::current_path();
    auto restoreDirectory = ScopeGuard([&] {
        std::error_code error;
        std::filesystem::current_path(previousDirectory, error);
    });
    if (!m_workspacePathPrefix.empty())
        std::filesystem::current_path(m_workspacePathPrefix);
    driver.addStandardArgs();
    diagEngine.removeClient(driver.textDiagClient);
    if (requireValidConfig) {
        driver.textDiagClient = std::make_shared<TextDiagnosticClient>();
        diagEngine.addClient(driver.textDiagClient);
    }
    else {
        diagEngine.addClient(diagClient);
    }

    slang::CommandLine::ParseOptions parseOpts;
    parseOpts.expandEnvVars = true;
    parseOpts.ignoreProgramName = true;
    parseOpts.supportComments = true;
    parseOpts.ignoreDuplicates = true;

    // Parse each config file's flags separately so -D defines are attributed correctly
    bool ok = true;

    for (auto& src : m_config.flagsByFile.value()) {
        auto guard = driver.setCurrentCommandFile(src.filePath);
        ok &= driver.parseCommandLine(src.flags, parseOpts);
    }

    if (m_config.compilationFilelist.value()) {
        auto path = std::filesystem::path(*m_config.compilationFilelist.value());
        if (path.is_relative())
            path = std::filesystem::path(m_workspacePathPrefix) / path;
        ok &= !m_config.compilationFilelist.value()->empty() &&
              driver.processCommandFiles(path.string(), true, false);
    }

    for (auto& buildfile : buildfiles) {
        bool loaded = driver.processCommandFiles(buildfile, m_config.buildRelativePaths.value(),
                                                 false);
        ok &= loaded;
        if (loaded) {
            INFO("Processed build file: {}", buildfile);
        }
        else if (!requireValidConfig) {
            client.showError(fmt::format("Failed to process build file: {}", buildfile));
        }
    }

    driver.options.errorLimit = 0;
    ok &= driver.processOptions(false);
    if (!ok && !requireValidConfig)
        client.showError(m_config.compilationFilelist.value()
                             ? "Failed to parse configuration or compilation filelist"
                             : "Failed to parse config flags");
    for (const auto& directory : m_config.incdirs.value()) {
        auto path = std::filesystem::path(directory);
        if (path.is_relative() && !m_workspacePathPrefix.empty())
            path = std::filesystem::path(m_workspacePathPrefix) / path;
        if (auto ec = sm.addUserDirectories(path.string())) {
            ok = false;
            auto message = fmt::format("Cannot use include directory {}: {}", path.string(),
                                       ec.message());
            if (requireValidConfig) {
                WARN("Config reload failed: {}", message);
            }
            else {
                client.showError(message);
            }
        }
    }

    ok &= configureUvmLibrary(m_workspacePathPrefix.empty()
                                  ? previousDirectory
                                  : std::filesystem::path(m_workspacePathPrefix),
                              requireValidConfig);
    if (m_config.uvm.value() && m_config.uvm.value()->useBundled.value() &&
        !driver.precompiledUvm) {
        options = driver.createOptionBag();
        auto pp = options.getOrDefault<parsing::PreprocessorOptions>();
        pp.precompiledSourceRoot = m_uvmDirectory;
        options.set(pp);
        m_configurationInputsFailed = m_nativeInputsFailed = true;
        return false;
    }
    if (m_config.experimentalUvmSemanticCache.value() &&
        (m_config.experimentalFrozenSources.value() ||
         m_config.experimentalMethodScopeOverlay.value()))
        ok &= uvmConfigurationError(
            "experimentalUvmSemanticCache cannot be combined with experimentalFrozenSources "
            "or experimentalMethodScopeOverlay",
            requireValidConfig);

    if (!ok && requireValidConfig) {
        WARN("Config reload failed: {}", driver.textDiagClient->getString());
        return false;
    }

    // Build macro name -> source file map from the driver's per-file define lists
    for (auto& [path, meta] : driver.getCommandFileMetadata()) {
        for (auto& define : meta.defines) {
            auto eqPos = define.find('=');
            auto macroName = eqPos != std::string::npos ? define.substr(0, eqPos) : define;
            m_defineSources[macroName] = path;
        }
    }

    // Configure diagnostic engine. The LSP server reports warnings as editor
    // diagnostics by default; user-provided -Wno-* mappings still suppress
    // specific warnings via the severity table populated by processOptions().
    diagEngine.setIgnoreAllWarnings(false);
    diagEngine.setIgnoreAllNotes(false);

    const auto frozen = m_config.frozenSources.value().value_or(Config::FrozenSourcesConfig{});
    m_captureFrozenNativeSeed = isFrozenSourcesEnabled() && frozen.maxMiB.value() &&
                                frozen.maxMiB.value() <= 512 &&
                                (!frozen.packages.value().empty() ||
                                 !frozen.modules.value().empty() ||
                                 !frozen.instances.value().empty()) &&
                                !std::getenv("SLANG_FROZEN_NATIVE_SEED_DISABLE");
    driver.options.captureUvmPrefix = isUvmPrefixCacheEnabled() || m_captureFrozenNativeSeed;
    options = driver.createOptionBag();
    options.set(driver.getAnalysisOptions());
    // Native command files can change cwd and install include paths. Only the
    // subsequent source parse belongs to the immutable diagnostic replay graph.
    // Existing editor overlays remain in the cache and their actual reads are recorded.
    std::filesystem::current_path(previousDirectory);
    restoreDirectory.release();
    sm.setFastCwdSnapshotChecks(m_config.fastCwdSnapshotChecks.value());
    sm.enableSnapshotRecording(true);
    m_configurationInputsFailed = !ok;
    ok &= driver.parseAllSources();
    if (!m_uvmEntryPath.empty() && !completeUvmLibraryInputs(requireValidConfig)) {
        ok = false;
        m_configurationInputsFailed = true;
    }
    diagEngine.setMappingsFromPragmas();

    if (requireValidConfig) {
        if (!ok) {
            WARN("Source reload failed: {}", driver.textDiagClient->getString());
            return false;
        }
        if (!driver.textDiagClient->getString().empty())
            WARN("Config reload diagnostics: {}", driver.textDiagClient->getString());
        diagEngine.removeClient(driver.textDiagClient);
        diagEngine.addClient(diagClient);
    }
    m_nativeInputsFailed = !ok;
    // Initial native capture is separate from independent completion-file parsing.
    // Keep those query parses under the explicitly enabled older UVM contract only.
    auto queryPP = options.getOrDefault<parsing::PreprocessorOptions>();
    queryPP.captureUvmPrefix = isUvmPrefixCacheEnabled();
    options.set(std::move(queryPP));
    if (ok && m_config.experimentalScopeSemanticCache.value())
        prepareScopeLibrary();
    if (ok && !m_trustedScopes && m_config.experimentalUvmSemanticCache.value())
        prepareTrustedUvmLibrary();
    installConfiguredUnits(driver.syntaxTrees);
    return ok;
}

std::vector<SourceBuffer> ServerDriver::getExplorePreincludeBuffers(BufferID target) {
    std::vector<SourceBuffer> buffers;
    if (comp || m_config.explorePreincludeFiles.value().empty())
        return buffers;

    auto targetPath = sm.getFullPath(target);
    bool targetIsRoot = false;
    flat_hash_set<URI> roots;
    for (const auto& configured : m_config.explorePreincludeFiles.value()) {
        auto path = std::filesystem::path(configured);
        if (path.is_relative())
            path = std::filesystem::path(m_workspacePathPrefix) / path;
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(path, ec);
        if (!ec)
            path = canonical;
        roots.emplace(URI::fromFile(path));
        m_explorePreincludeUris.emplace(URI::fromFile(path));
        targetIsRoot |= path == targetPath;
        auto buffer = sm.readSource(path.string(), nullptr);
        if (!buffer) {
            if (m_reportedPreincludeFailures.emplace(path.string()).second)
                client.showError(fmt::format("Failed to read exploration preinclude {}: {}",
                                             path.string(), buffer.error().message()));
            continue;
        }
        m_reportedPreincludeFailures.erase(path.string());
        buffers.push_back(*buffer);
    }
    // Never force a macro header to include itself: include guards would hide its
    // edited text and break diagnostics/navigation in the macro document itself.
    if (buffers.empty()) {
        if (m_explorePreincludeTree) {
            m_explorePreincludeTree.reset();
            ++m_explorePreincludeRevision;
        }
        return {};
    }

    if (!m_explorePreincludeTree || !hasValidBuffers(sm, m_explorePreincludeTree)) {
        m_explorePreincludeTree = syntax::SyntaxTree::fromBuffers(buffers, sm, options);
        ++m_explorePreincludeRevision;
        m_explorePreincludeUris = std::move(roots);
        for (auto buffer : m_explorePreincludeTree->getSourceBufferIds()) {
            auto path = sm.getFullPath(buffer);
            if (!path.empty())
                m_explorePreincludeUris.emplace(URI::fromFile(path));
        }
    }
    if (targetIsRoot || m_explorePreincludeUris.contains(URI::fromFile(targetPath)))
        return {};
    return buffers;
}

bool ServerDriver::isExplorePreinclude(const URI& uri) const {
    if (comp || m_config.explorePreincludeFiles.value().empty())
        return false;
    if (m_explorePreincludeUris.contains(uri))
        return true;
    auto found = docs.find(uri);
    if (found != docs.end())
        return m_explorePreincludeUris.contains(
            URI::fromFile(sm.getFullPath(found->second->getBuffer())));
    std::error_code ec;
    auto path = std::filesystem::weakly_canonical(std::filesystem::path(uri.getPath()), ec);
    return m_explorePreincludeUris.contains(ec ? uri : URI::fromFile(path));
}

void ServerDriver::issueExplorePreincludeDiagnostics(BufferID target) {
    if (comp || m_config.explorePreincludeFiles.value().empty())
        return;
    auto targetUri = URI::fromFile(sm.getFullPath(target));
    bool changed = m_publishedPreincludeRevision != m_explorePreincludeRevision;
    if (changed) {
        // Clear previous members too: a header may have left the preinclude graph.
        for (const auto& uri : m_publishedPreincludeUris)
            diagClient->clear(uri);
        for (const auto& uri : m_explorePreincludeUris)
            diagClient->clear(uri);
    }
    else if (m_explorePreincludeUris.contains(targetUri)) {
        // analyzeDocument cleared this macro document before entering here.
        diagClient->clear(targetUri);
    }
    else {
        return;
    }
    if (!m_explorePreincludeTree)
        return;
    for (auto buffer : m_explorePreincludeTree->getSourceBufferIds())
        diagEngine.setMappingsFromPragmas(buffer);
    for (const auto& diagnostic : m_explorePreincludeTree->diagnostics()) {
        auto location = sm.getFullyOriginalLoc(diagnostic.location);
        if (changed ||
            (location.valid() && URI::fromFile(sm.getFullPath(location.buffer())) == targetUri))
            diagEngine.issue(diagnostic);
    }
}

void ServerDriver::analyzeDocument(SlangDoc& doc, const lsp::RequestContext& ctx,
                                   SemanticDiagnosticBatch* batch) {
    ctx.throwIfCancelled("before diagnostics");

    if (comp) {
        auto previousUnits = m_configuredUnits;
        refreshConfiguredUnits();
        if (m_designUsesConfiguredUnits && previousUnits != m_configuredUnits)
            comp->refreshDocuments(m_configuredUnits);
        auto design = comp->createActiveDesignContext({});
        auto path = sm.getFullPath(doc.getBuffer());
        bool covered = false;
        for (const auto& tree : design.getAnalysis().compilation.getSyntaxTrees())
            for (auto id : tree->getSourceBufferIds())
                covered |= sm.getFullPath(id) == path;
        if (covered) {
            // A shallow view cannot validate bind targets and cross-hierarchy
            // references. Publish from the current full design for its sources.
            publishCompilationDiagnostics(&doc.getURI(), &ctx);
            m_fullDiagnosticGeneration = m_diagnosticGeneration;
            publishInactiveRegions(doc, ctx);
            return;
        }
    }

    auto cwdPhase = sm.beginFastCwdPhase();
    // Clear and re-issue diagnostics for this document
    auto update = diagClient->beginUpdate();
    diagClient->clear(doc.getURI());

    doc.issueDiagnosticsTo(diagEngine, ctx, batch);
    issueExplicitContextDiagnostics(doc.getURI());
    ctx.throwIfCancelled("before publishing diagnostics");
    cwdPhase.finish();
    update.commit();
    if (!comp && !m_config.explorePreincludeFiles.value().empty()) {
        // Publication bookkeeping commits with the diagnostic transaction. A cancelled
        // attempt must retry the shared header diagnostics on the next analysis.
        m_publishedPreincludeRevision = m_explorePreincludeRevision;
        m_publishedPreincludeUris = m_explorePreincludeUris;
    }
    diagClient->setVersion(doc.getURI(), doc.getVersion());
    diagClient->pushDiags(doc.getURI());

    publishInactiveRegions(doc, ctx);
}

void ServerDriver::onDocDidSave(SlangDoc& doc) {
    ++m_snapshotRevision;
    if (hasConfiguredCompilationInputs() || (!comp && hasSourceContextHints())) {
        auto inputs = doc.getAnalysisInputs();
        m_indexer.updateDocument(doc.getURI().getPath(), *inputs.tree, inputs.buffer);
    }
    else
        m_indexer.updateDocument(doc.getURI().getPath(), *doc.getSyntaxTree(), doc.getBuffer());
    m_indexer.updateIncludes(*doc.getSyntaxTree());

    if (!comp) {
        analyzeDocument(doc);
        // Reanalyze open documents whose dependency buffers were invalidated
        for (const auto& uri : m_openDocs) {
            if (uri == doc.getURI())
                continue;

            auto it = docs.find(uri);
            if (it == docs.end()) {
                ERROR("Open Doc {} not found", uri.getPath());
                continue;
            }
            if (it->second->hasAnalysis() && it->second->getSyntaxTree() != doc.getSyntaxTree())
                continue;

            analyzeDocument(*it->second);
        }
    }
    else {
        if (m_designUsesConfiguredUnits)
            comp->refreshDocuments(m_configuredUnits);
        else
            comp->refresh();
        invalidateAnalysesAndRefreshClient();
        publishCompilationDiagnostics(&doc.getURI());
        // Pending jobs covered by this full pass still finish their inactive-region
        // work, but must not overwrite full-design diagnostics with shallow results.
        m_fullDiagnosticGeneration = m_diagnosticGeneration;
        publishInactiveRegions(doc);
    }
}

void ServerDriver::publishCompilationDiagnostics(const URI* priorityUri,
                                                 const lsp::RequestContext* ctx) {
    auto cwdPhase = sm.beginFastCwdPhase();
    auto update = diagClient->beginUpdate();
    auto design = comp->createActiveDesignContext({});
    const auto trees = design.getAnalysis().compilation.getSyntaxTrees();
    flat_hash_set<URI> currentSources;
    for (const auto& tree : trees)
        for (auto id : tree->getSourceBufferIds())
            if (auto path = sm.getFullPath(id); !path.empty())
                currentSources.emplace(URI::fromFile(path));
    auto clearSources = m_buildSourceUris;
    clearSources.insert(currentSources.begin(), currentSources.end());
    for (const auto& uri : clearSources)
        diagClient->clear(uri);
    for (const auto& [uri, doc] : docs)
        if (clearSources.contains(URI::fromFile(sm.getFullPath(doc->getBuffer()))))
            diagClient->clear(uri);
    diagEngine.setMappingsFromPragmas();
    for (const auto& tree : trees)
        for (const auto& diagnostic : tree->diagnostics())
            diagEngine.issue(diagnostic);
    comp->issueDiagnosticsTo(diagEngine);
    for (const auto& [uri, doc] : docs) {
        if (currentSources.contains(URI::fromFile(sm.getFullPath(doc->getBuffer())))) {
            issueExplicitContextDiagnostics(uri);
            diagClient->setVersion(uri, doc->getVersion());
        }
    }
    if (ctx)
        ctx->throwIfCancelled("before publishing full-design diagnostics");
    cwdPhase.finish();
    update.commit();
    m_buildSourceUris = std::move(currentSources);
    if (priorityUri)
        diagClient->pushDiags(*priorityUri);
    else
        diagClient->pushDiags();
    // Generation zero is reserved for an unpublished full-design result.
    if (!m_diagnosticGeneration)
        ++m_diagnosticGeneration;
    m_fullDiagnosticGeneration = m_diagnosticGeneration;
}

void ServerDriver::prepareForContextRetirement() {
    if (m_frozenSources)
        m_frozenSources->clear("context retirement");
    completions.invalidateParsedLibraryCache();
    for (auto& [uri, doc] : docs)
        doc->releaseSemanticCaches();
    for (auto& doc : m_configuredUnits)
        doc->releaseSemanticCaches();
}

void ServerDriver::copyOpenDocumentsFrom(const ServerDriver* oldDriver,
                                         bool deferConfiguredDiagnostics) {
    if (!oldDriver)
        return;

    m_includeSelections = oldDriver->m_includeSelections;
    completions.resolveEdits = oldDriver->completions.resolveEdits;
    const bool handoff = oldDriver->m_preserveTrustedUvm &&
                         diagClient->inheritPublications(*oldDriver->diagClient);
    if (!handoff)
        oldDriver->diagClient->clearAndPush();
    for (const auto& uri : oldDriver->m_openDocs) {
        auto docIt = oldDriver->docs.find(uri);
        if (docIt == oldDriver->docs.end()) {
            ERROR("Open Doc {} not found in old driver", uri.getPath());
            continue;
        }

        // openDocument expects no \0
        auto text = docIt->second->getText();
        text.remove_suffix(1);
        openDocument(uri, text, docIt->second->getVersion(), deferConfiguredDiagnostics);
    }
    if (handoff)
        diagClient->completePublicationHandoff(*oldDriver->diagClient);
}

std::unique_ptr<ServerDriver> ServerDriver::createForExplore(
    Indexer& indexer, SlangLspClient& client, const Config& config,
    std::optional<std::string_view> workspaceFolder, const ServerDriver* oldDriver,
    bool requireValidConfig, bool deferOpenDiagnostics) {
    UvmPhaseTrace phases(deferOpenDiagnostics && config.experimentalUvmPrefixCache.value());
    auto newDriver = std::make_unique<ServerDriver>(indexer, client, config,
                                                    std::vector<std::string>{}, workspaceFolder,
                                                    requireValidConfig, oldDriver);
    phases.mark("retirement-construct");
    if (requireValidConfig && !newDriver->m_configLoaded)
        return nullptr;
    newDriver->copyOpenDocumentsFrom(oldDriver, deferOpenDiagnostics);
    phases.mark("retirement-copy-open");
    return newDriver;
}

std::unique_ptr<ServerDriver> ServerDriver::createFromFileLists(
    Indexer& indexer, SlangLspClient& client, const Config& config,
    std::vector<std::string> buildfiles, std::optional<std::string_view> workspaceFolder,
    const ServerDriver* oldDriver, bool requireValidConfig) {
    auto newDriver = std::make_unique<ServerDriver>(indexer, client, config, std::move(buildfiles),
                                                    workspaceFolder, requireValidConfig, oldDriver);
    if (requireValidConfig && !newDriver->m_configLoaded)
        return nullptr;

    std::vector<std::shared_ptr<SlangDoc>> buildDocuments;
    buildDocuments.reserve(newDriver->docs.size());
    for (const auto& entry : newDriver->docs) {
        buildDocuments.push_back(entry.second);
    }

    if (buildDocuments.empty()) {
        if (requireValidConfig)
            return nullptr;
        newDriver->copyOpenDocumentsFrom(oldDriver);
        ERROR("No documents available for compilation");
        return newDriver;
    }

    newDriver->m_designUsesConfiguredUnits = true;
    newDriver->comp = std::make_unique<ServerCompilation>(
        std::move(buildDocuments), newDriver->options, newDriver->sm, newDriver->client,
        std::nullopt,
        (newDriver->isFrozenSourcesEnabled() || newDriver->isUvmPrefixCacheEnabled()) && oldDriver
            ? oldDriver->comp.get()
            : nullptr);

    newDriver->configureFrozenInstances();
    newDriver->diagClient->clear();
    newDriver->copyOpenDocumentsFrom(oldDriver);
    newDriver->publishCompilationDiagnostics();
    return newDriver;
}

void ServerDriver::openDocument(const URI& uri, const std::string_view text,
                                std::optional<int> version, bool deferConfiguredDiagnostics) {
    completions.invalidateParsedLibraryCache();
    ++m_snapshotRevision;
    // An explicitly opened package takes precedence over automatic candidate discovery.
    if (m_automaticOwnerRoots.erase(uri))
        ++m_ownerEpoch;
    auto docIter = docs.find(uri);
    // A missing nested preinclude has no BufferID to invalidate. Conservatively
    // rebuild the explicit prelude when an editor supplies any cached failure.
    bool recoveredMissingSource = !comp &&
                                  (!m_config.explorePreincludeFiles.value().empty() ||
                                   hasConfiguredCompilationInputs() || hasSourceContextHints()) &&
                                  sm.isCached(uri.getPath()) && !sm.readSource(uri.getPath());
    bool preincludeReplaced = (isExplorePreinclude(uri) || recoveredMissingSource) &&
                              (docIter == docs.end() || !docIter->second->textMatches(text));
    std::shared_ptr<SlangDoc> doc;
    bool matchesBuildSource = false;
    if (docIter != docs.end() && docIter->second->textMatches(text)) {
        doc = docIter->second;
        matchesBuildSource = m_buildSourceUris.contains(uri);
    }
    else if (docIter == docs.end() && m_buildSourceUris.contains(uri)) {
        doc = SlangDoc::open(*this, uri);
        if (doc->textMatches(text)) {
            matchesBuildSource = true;
            docs[uri] = doc;
        }
        else {
            doc.reset();
        }
    }

    if (!doc) {
        if (docIter != docs.end())
            WARN("Document {} text does not match, updating", uri.getPath());
        doc = SlangDoc::fromText(*this, uri, text);
        docs[uri] = doc;
    }

    if (preincludeReplaced) {
        ++m_ownerEpoch;
        if (recoveredMissingSource)
            m_explorePreincludeTree.reset();
        for (auto& [_, document] : docs)
            document->invalidateSyntax();
    }
    if (recoveredMissingSource)
        for (const auto& unit : m_configuredUnits)
            unit->invalidateSyntax();
    doc->setVersion(version);
    m_openDocs.insert(uri);
    auto physicalUri = URI::fromFile(sm.getFullPath(doc->getBuffer()));
    if (comp && !matchesBuildSource && m_buildSourceUris.contains(physicalUri)) {
        ++m_diagnosticGeneration;
        comp->invalidate();
        m_fullDiagnosticGeneration = 0;
        invalidateAnalysesAndRefreshClient();
    }
    if (!m_openDocs.contains(physicalUri))
        diagClient->adoptDocumentUri(physicalUri, uri);
    // Protocol callers with an idle diagnostic loop can postpone complete
    // filelist diagnostics. Direct callers retain synchronous publication.
    const bool deferred = deferConfiguredDiagnostics && !comp && hasConfiguredCompilationInputs() &&
                          !preincludeReplaced;
    if (deferred) {
        m_pendingIncludedDiagnostics.insert_or_assign(uri, ++m_diagnosticGeneration);
    }
    else if (comp && matchesBuildSource) {
        if (m_fullDiagnosticGeneration && m_fullDiagnosticGeneration == m_diagnosticGeneration &&
            comp->containsCurrentSource(sm.getFullPath(doc->getBuffer()))) {
            // Reuse the complete current publication, with this editor version,
            // including an explicit empty list when the source has no diagnostics.
            auto found = diagClient->renderedDiagnostics().find(uri);
            auto rendered = found == diagClient->renderedDiagnostics().end()
                                ? std::vector<lsp::Diagnostic>{}
                                : found->second;
            diagClient->replaceRendered(uri, std::move(rendered));
            diagClient->setVersion(uri, doc->getVersion());
            diagClient->pushDiags(uri);
            publishInactiveRegions(*doc);
        }
        else
            analyzeDocument(*doc);
    }
    else {
        analyzeDocument(*doc);
    }

    // Opening a package can supply context for headers that were already open.
    // Refresh only headers whose selected context actually changes to this root.
    auto tree = doc->getCachedSyntaxTree();
    bool hasPackage = false;
    if (tree) {
        for (const auto& [node, _] : tree->getMetadata().nodeMeta) {
            if (node->kind == syntax::SyntaxKind::PackageDeclaration) {
                hasPackage = true;
                break;
            }
        }
    }
    if (hasPackage) {
        for (const auto& openUri : m_openDocs) {
            if (openUri == uri)
                continue;
            auto openDoc = docs.find(openUri);
            if (openDoc == docs.end())
                continue;
            auto previous = openDoc->second->getCachedAnalysisTree();
            auto context = getIncludeContext(openUri, previous.get());
            if (context && context->tree != previous &&
                sm.getFullPath(context->tree->getSourceBufferIds().front()) ==
                    sm.getFullPath(doc->getBuffer())) {
                if (deferred)
                    m_pendingIncludedDiagnostics.insert_or_assign(openUri, m_diagnosticGeneration);
                else
                    analyzeDocument(*openDoc->second);
            }
        }
    }

    // Track this as an open document
    if (preincludeReplaced)
        refreshOpenIncludeContexts();
}

std::shared_ptr<SlangDoc> ServerDriver::getDocument(const URI& uri) {
    auto it = docs.find(uri);
    if (it != docs.end())
        return it->second;

    auto doc = SlangDoc::open(*this, uri);
    if (doc) {
        docs[uri] = doc;
    }
    return doc;
}

bool ServerDriver::isDocumentOpen(const URI& uri) {
    return m_openDocs.find(uri) != m_openDocs.end();
}

void ServerDriver::onDocDidChange(const lsp::DidChangeTextDocumentParams& params,
                                  const lsp::RequestContext& ctx) {
    ++m_snapshotRevision;
    auto doc = getDocument(params.textDocument.uri);
    if (!doc) {
        ctx.error("Document {} not found", params.textDocument.uri.getPath());
        return;
    }

    ++m_diagnosticGeneration;
    auto changedPath = sm.getFullPath(doc->getBuffer());
    // Remember old ownership before replacing the owner's tree. In particular, a
    // removed include must clear diagnostics in its still-open header.
    for (const auto& uri : m_openDocs) {
        if (uri == doc->getURI())
            continue;
        auto openDoc = docs.find(uri);
        if (openDoc == docs.end())
            continue;
        auto tree = openDoc->second->getCachedAnalysisTree();
        if (tree && sm.getFullPath(tree->getSourceBufferIds().front()) == changedPath)
            m_pendingIncludedDiagnostics.insert_or_assign(uri, m_diagnosticGeneration);
    }

    ++m_ownerEpoch;
    doc->onChange(params.contentChanges);
    doc->setVersion(params.textDocument.version);
    if (comp) {
        comp->invalidate();
        // A query can rebuild semantic state without publishing a full diagnostic
        // pass. That newer compilation must not authenticate an older saved pass.
        m_fullDiagnosticGeneration = 0;
        invalidateAnalysesAndRefreshClient();
    }
    bool preincludeChanged = isExplorePreinclude(doc->getURI());
    if (preincludeChanged) {
        // A nested file can enter or leave the preinclude graph. Invalidate even
        // exempt macro documents, whose old trees might not contain the changed file.
        for (auto& [_, document] : docs)
            document->invalidateSyntax();
        for (const auto& uri : m_openDocs)
            if (uri != doc->getURI())
                m_pendingIncludedDiagnostics.insert_or_assign(uri, m_diagnosticGeneration);
    }
    // A changed sibling include can invalidate another open header's package
    // analysis even though neither document is the package root. Keep standalone
    // RTL diagnostic publication on its existing save-triggered path. Semantic
    // queries use the lazily refreshed full-design context, including unsaved text.
    for (const auto& uri : m_openDocs) {
        auto openDoc = docs.find(uri);
        if (uri == doc->getURI() || openDoc == docs.end() || openDoc->second->hasAnalysis())
            continue;
        auto previous = openDoc->second->getCachedAnalysisTree();
        if (previous && (preincludeChanged || hasConfiguredCompilationInputs() ||
                         m_explicitContexts.contains(uri) ||
                         sm.getFullPath(previous->getSourceBufferIds().front()) !=
                             sm.getFullPath(openDoc->second->getBuffer())))
            m_pendingIncludedDiagnostics.insert_or_assign(uri, m_diagnosticGeneration);
    }
    // Give the directly edited document the newest work generation. It leads the
    // eventual batch, preserving producer-before-sibling diagnostic reuse.
    m_pendingIncludedDiagnostics.insert_or_assign(doc->getURI(), ++m_diagnosticGeneration);
}

void ServerDriver::runPendingDiagnostics(const lsp::RequestContext& ctx) {
    // This batch is scoped to one uninterrupted idle pass; it never crosses edits.
    SemanticDiagnosticBatch batch(m_semanticDiagnosticStatistics);
    auto* diagnosticBatch = comp ? nullptr : &batch;
    while (!m_pendingIncludedDiagnostics.empty()) {
        ctx.throwIfCancelled("before analysis");
        auto newest = std::max_element(
            m_pendingIncludedDiagnostics.begin(), m_pendingIncludedDiagnostics.end(),
            [](const auto& left, const auto& right) { return left.second < right.second; });
        auto [uri, generation] = *newest;
        auto found = docs.find(uri);
        if (found == docs.end()) {
            m_pendingIncludedDiagnostics.erase(uri);
            continue;
        }
        auto doc = found->second;
        auto changedPath = sm.getFullPath(doc->getBuffer());
        if (m_openDocs.contains(uri)) {
            if (comp && generation <= m_fullDiagnosticGeneration &&
                comp->containsCurrentSource(changedPath))
                publishInactiveRegions(*doc, ctx);
            else
                analyzeDocument(*doc, ctx, diagnosticBatch);
        }
        else {
            // Closing an edited owner cannot lose discovery of newly included open
            // headers. Parse its retained context, but never publish closed-file diagnostics.
            doc->getSyntaxTree();
        }
        ctx.throwIfCancelled("before discovering included header contexts");

        auto tree = doc->getCachedSyntaxTree();
        bool hasPackage = false;
        if (tree) {
            for (const auto& [node, _] : tree->getMetadata().nodeMeta) {
                if (node->kind == syntax::SyntaxKind::PackageDeclaration) {
                    hasPackage = true;
                    break;
                }
            }
        }
        if (hasPackage) {
            for (const auto& openUri : m_openDocs) {
                if (openUri == uri)
                    continue;
                auto openDoc = docs.find(openUri);
                if (openDoc == docs.end())
                    continue;
                auto previous = openDoc->second->getCachedAnalysisTree();
                auto context = getIncludeContext(openUri, previous.get());
                if (context &&
                    sm.getFullPath(context->tree->getSourceBufferIds().front()) == changedPath) {
                    auto [pending, inserted] = m_pendingIncludedDiagnostics.try_emplace(openUri,
                                                                                        generation);
                    if (!inserted)
                        pending->second = std::max(pending->second, generation);
                }
            }
        }
        ctx.throwIfCancelled("before completing diagnostic generation");
        auto pending = m_pendingIncludedDiagnostics.find(uri);
        if (pending != m_pendingIncludedDiagnostics.end() && pending->second == generation)
            m_pendingIncludedDiagnostics.erase(pending);
    }
}

void ServerDriver::closeDocument(const URI& uri) {
    completions.invalidateParsedLibraryCache();
    ++m_snapshotRevision;
    if (isExplorePreinclude(uri))
        m_publishedPreincludeRevision.reset();
    // Remove from open docs set
    m_openDocs.erase(uri);
    // Retain pending owner discovery; idle work skips publication for closed documents.
    if (auto it = docs.find(uri); it != docs.end())
        it->second->setVersion(std::nullopt);
    diagClient->setVersion(uri, std::nullopt);
    if (!comp) {
        diagClient->clear(uri);
    }
}

void ServerDriver::reloadDocument(const URI& uri) {
    ++m_snapshotRevision;
    // Only reload if this is an open document
    if (m_openDocs.find(uri) == m_openDocs.end()) {
        return;
    }

    auto doc = getDocument(uri);
    if (!doc) {
        WARN("Document {} not found for reload", uri.getPath());
        return;
    }

    if (!doc->reloadBuffer()) {
        return;
    }

    INFO("Reloaded document {} from disk", uri.getPath());

    // Update the document (reparse and issue diagnostics)
    analyzeDocument(*doc);
}

void ServerDriver::onWorkspaceDidChangeWatchedFiles(
    const lsp::DidChangeWatchedFilesParams& params) {
    completions.invalidateParsedLibraryCache();
    ++m_snapshotRevision;
    bool preincludeChanged = false;
    std::vector<std::shared_ptr<SlangDoc>> updatedDocs;
    for (const auto& change : params.changes) {
        preincludeChanged |= isExplorePreinclude(change.uri);
        bool openOverlay = false;
        std::error_code canonicalError;
        auto changedPath = std::filesystem::weakly_canonical(change.uri.getPath(), canonicalError);
        if (isTrustedScopeSource(canonicalError ? std::filesystem::path(change.uri.getPath())
                                                : changedPath) ||
            isTrustedUvmSource(canonicalError ? std::filesystem::path(change.uri.getPath())
                                              : changedPath))
            continue;
        for (const auto& openUri : m_openDocs) {
            auto open = docs.find(openUri);
            if (open != docs.end() && sm.getFullPath(open->second->getBuffer()) == changedPath)
                openOverlay = true;
        }
        if (openOverlay)
            continue;
        if (change.type == lsp::FileChangeType::Changed ||
            change.type == lsp::FileChangeType::Created) {
            auto found = docs.find(change.uri);
            if (found != docs.end()) {
                if (found->second->reloadBuffer())
                    updatedDocs.push_back(found->second);
            }
            else if (sm.isCached(change.uri.getPath())) {
                // Nested include buffers may have no SlangDoc of their own.
                auto buffer = sm.readSource(change.uri.getPath(), nullptr);
                if (buffer) {
                    auto reloaded = sm.reloadBuffer(buffer->id);
                    if (!reloaded)
                        WARN("Unable to reload cached include {}: {}", change.uri.getPath(),
                             reloaded.error().message());
                }
            }
        }
        else if (change.type == lsp::FileChangeType::Deleted) {
            closeDocument(change.uri);
            docs.erase(change.uri);
            m_automaticOwnerRoots.erase(change.uri);
        }
    }
    if (preincludeChanged)
        for (auto& [_, document] : docs)
            document->invalidateSyntax();
    ++m_ownerEpoch;
    // Retain main's closed-document publication behavior, but only after the
    // complete disk batch has been loaded so no analysis sees half an update.
    for (const auto& document : updatedDocs)
        analyzeDocument(*document);
    refreshOpenIncludeContexts();
}

void ServerDriver::invalidateAnalysesAndRefreshClient() {
    ++m_snapshotRevision;
    for (auto& [_, doc] : docs) {
        doc->invalidateAnalysis();
    }
    client.onWorkspaceCodeLensRefresh(std::monostate{});
    client.onWorkspaceInlayHintRefresh(std::monostate{});
}

bool ServerDriver::setActiveInstance(std::string_view hierPath) {
    if (!comp) {
        return false;
    }

    if (!comp->setActiveInstance(std::string(hierPath))) {
        return false;
    }

    invalidateAnalysesAndRefreshClient();
    return true;
}

std::optional<ServerDriver::PackageIncludeContext> ServerDriver::getIncludeContext(
    const URI& uri, const syntax::SyntaxTree* previous) {
    auto target = docs.find(uri);
    if (target == docs.end())
        return std::nullopt;
    const auto targetDoc = target->second;
    const auto targetPath = sm.getFullPath(targetDoc->getBuffer());
    bool explicitContext = false;
    auto explicitSelection = getExplicitIncludeContext(uri, explicitContext);
    if (explicitContext)
        return explicitSelection;
    auto findBuffer = [&](const syntax::SyntaxTree& tree) {
        return findPackageInclude(tree, targetPath);
    };

    struct Candidate {
        std::shared_ptr<SlangDoc> doc;
        bool previous;
    };
    std::vector<Candidate> candidates;
    for (const auto& [candidateUri, doc] : docs) {
        if (m_automaticOwnerRoots.contains(candidateUri))
            continue;
        if (sm.getFullPath(doc->getBuffer()) == targetPath)
            continue;
        auto tree = doc->getCachedSyntaxTree();
        if (!tree || !findBuffer(*tree))
            continue;
        bool wasPrevious = previous && sm.getFullPath(previous->getSourceBufferIds().front()) ==
                                           sm.getFullPath(doc->getBuffer());
        candidates.push_back({doc, wasPrevious});
    }
    std::ranges::sort(candidates, [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.previous != rhs.previous)
            return lhs.previous > rhs.previous;
        return lhs.doc->getPath() < rhs.doc->getPath();
    });
    for (const auto& candidate : candidates) {
        // Only reparse a known owner. Its cached includes may have become stale after an edit.
        auto tree = candidate.doc->getSyntaxTree();
        if (auto buffer = findBuffer(*tree)) {
            m_ownerDiscovery.insert_or_assign(
                uri, OwnerDiscoveryCache{.indexRevision = m_indexer.getRevision(),
                                         .ownerEpoch = m_ownerEpoch,
                                         .state = OwnerDiscoveryState::Unique,
                                         .root = candidate.doc->getURI()});
            return PackageIncludeContext{std::move(tree), *buffer};
        }
    }

    // Explicit full-design mode retains its configured/build contexts.
    if (comp)
        return std::nullopt;

    auto hasStandaloneUnit = [&] {
        auto localTree = targetDoc->getCachedSyntaxTree();
        if (!localTree || !hasValidBuffers(sm, localTree))
            localTree = syntax::SyntaxTree::fromBuffer(
                {.data = targetDoc->getText(), .id = targetDoc->getBuffer()}, sm, options);
        // Inspect only physical declarations in this buffer. Calling the shared
        // include resolver here would parse owners before checking the budget.
        for (auto* member : localTree->root().as<syntax::CompilationUnitSyntax>().members)
            if (syntax::ModuleDeclarationSyntax::isKind(member->kind) &&
                sm.getFullPath(
                    sm.getFullyExpandedLoc(
                          member->as<syntax::ModuleDeclarationSyntax>().header->name.location())
                        .buffer()) == targetPath)
                return true;
        return false;
    };
    auto revision = m_indexer.getRevision();
    auto cached = m_ownerDiscovery.find(uri);
    auto previousDiscovery = cached == m_ownerDiscovery.end() ? std::optional<OwnerDiscoveryCache>{}
                                                              : std::optional{cached->second};
    if (cached != m_ownerDiscovery.end() && cached->second.indexRevision == revision &&
        cached->second.ownerEpoch != m_ownerEpoch && cached->second.oneCandidate &&
        cached->second.state == OwnerDiscoveryState::Unique && !hasStandaloneUnit()) {
        // A single indexed candidate can be revalidated directly after text changes,
        // without walking the reverse graph again on every keystroke.
        auto root = docs.find(*cached->second.root);
        if (root != docs.end()) {
            auto tree = root->second->getSyntaxTree();
            if (auto buffer = findBuffer(*tree)) {
                cached->second.ownerEpoch = m_ownerEpoch;
                return PackageIncludeContext{std::move(tree), *buffer};
            }
        }
    }
    if (cached != m_ownerDiscovery.end() && cached->second.indexRevision == revision &&
        cached->second.ownerEpoch == m_ownerEpoch) {
        if (cached->second.root) {
            auto root = docs.find(*cached->second.root);
            if (root != docs.end()) {
                auto tree = root->second->getSyntaxTree();
                if (auto buffer = findBuffer(*tree))
                    return PackageIncludeContext{std::move(tree), *buffer};
            }
        }
        return std::nullopt;
    }

    OwnerDiscoveryCache discovery{.indexRevision = revision, .ownerEpoch = m_ownerEpoch};
    // Standalone design units already provide their own scope. Never reinterpret RTL
    // or package definitions through an indexed include candidate.
    if (hasStandaloneUnit()) {
        m_ownerDiscovery[uri] = discovery;
        return std::nullopt;
    }

    ++m_ownerDiscoveryAttempts;
    auto indexed = m_indexer.getPackageOwnerCandidates(targetPath);
    discovery.indexRevision = indexed.revision;
    if (indexed.limitExceeded) {
        discovery.state = OwnerDiscoveryState::LimitExceeded;
        m_ownerDiscovery[uri] = discovery;
        if (!previousDiscovery || previousDiscovery->indexRevision != indexed.revision ||
            previousDiscovery->state != discovery.state)
            client.showWarning(fmt::format("Package owner discovery limit reached for {}. Open the "
                                           "owning package; semantic diagnostics are deferred.",
                                           uri.getPath()));
        return std::nullopt;
    }

    std::optional<PackageIncludeContext> selected;
    std::optional<URI> selectedUri;
    std::set<std::filesystem::path> validatedRoots;
    for (const auto& path : indexed.paths) {
        auto rootUri = URI::fromFile(path);
        auto found = docs.find(rootUri);
        bool wasKnown = found != docs.end() && !m_automaticOwnerRoots.contains(rootUri);
        std::error_code ec;
        if (found == docs.end() && !std::filesystem::is_regular_file(path, ec))
            continue;
        std::shared_ptr<SlangDoc> root;
        try {
            root = getDocument(rootUri);
        }
        catch (const std::exception& error) {
            WARN("Unable to load package candidate {}: {}", path.string(), error.what());
            continue;
        }
        if (sm.getFullPath(root->getBuffer()) == targetPath)
            continue;
        if (!wasKnown && !m_openDocs.contains(rootUri) && !m_buildSourceUris.contains(rootUri))
            m_automaticOwnerRoots.insert(rootUri);
        auto tree = root->getSyntaxTree();
        if (auto buffer = findBuffer(*tree)) {
            if (wasKnown) {
                m_ownerDiscovery.insert_or_assign(
                    uri, OwnerDiscoveryCache{.indexRevision = revision,
                                             .ownerEpoch = m_ownerEpoch,
                                             .state = OwnerDiscoveryState::Unique,
                                             .root = rootUri});
                return PackageIncludeContext{std::move(tree), *buffer};
            }
            auto rootPath = sm.getFullPath(tree->getSourceBufferIds().front());
            if (validatedRoots.insert(rootPath).second && !selected) {
                selected = PackageIncludeContext{std::move(tree), *buffer};
                selectedUri = rootUri;
            }
        }
    }
    // Preserve main's deterministic default and its explicit include picker.
    // Authoritative filelists and source hints still reject ambiguous contexts.
    if (selected) {
        discovery.state = OwnerDiscoveryState::Unique;
        discovery.oneCandidate = indexed.paths.size() == 1;
        discovery.root = selectedUri;
    }
    m_ownerDiscovery[uri] = discovery;
    return discovery.state == OwnerDiscoveryState::Unique ? selected : std::nullopt;
}

bool ServerDriver::isPackageContextDeferred(const URI& uri) const {
    if (hasExplicitContextError(uri))
        return true;
    auto found = m_ownerDiscovery.find(uri);
    return found != m_ownerDiscovery.end() &&
           (found->second.state == OwnerDiscoveryState::Ambiguous ||
            found->second.state == OwnerDiscoveryState::LimitExceeded);
}

void ServerDriver::refreshOpenIncludeContexts() {
    // File/index changes can affect an automatically loaded, closed package owner.
    // Refresh only open documents whose analysis or selected context actually changed.
    for (const auto& uri : m_openDocs) {
        auto found = docs.find(uri);
        if (found == docs.end())
            continue;
        auto doc = found->second;
        auto previous = doc->getCachedAnalysisTree();
        bool wasDeferred = isPackageContextDeferred(uri);
        auto context = getIncludeContext(uri, previous.get());
        bool changed = context
                           ? context->tree != previous
                           : previous && sm.getFullPath(previous->getSourceBufferIds().front()) !=
                                             sm.getFullPath(doc->getBuffer());
        if (changed || !doc->hasAnalysis() || wasDeferred != isPackageContextDeferred(uri))
            analyzeDocument(*doc);
    }
}

std::vector<ServerDriver::IncludeContext> ServerDriver::getIncludeContexts(
    const std::filesystem::path& path, bool selectedOnly) {
    const auto canonical = std::filesystem::weakly_canonical(path);
    if (!m_indexer.hasPotentialIncluders(canonical))
        return {};
    std::error_code ec;
    if ((!sm.isCached(canonical) && !std::filesystem::is_regular_file(canonical, ec)) ||
        getDocument(URI::fromFile(canonical))->isMacroOnly())
        return {};
    std::vector<std::filesystem::path> pending{canonical};
    std::unordered_set<std::filesystem::path> visited;
    std::vector<std::filesystem::path> sources;
    std::optional<IncludeContextSelection> preferred;
    for (size_t i = 0; i < pending.size(); ++i) {
        const auto current = pending[i];
        if (!visited.insert(current).second)
            continue;
        if (!preferred) {
            if (auto it = m_includeSelections.find(current); it != m_includeSelections.end()) {
                preferred = it->second;
                if (current != canonical)
                    preferred->occurrence = 0;
            }
        }
        auto parents = m_indexer.getFilesIncluding(current);
        std::ranges::sort(parents);
        if (parents.empty() && current != canonical)
            sources.push_back(current);
        pending.insert(pending.end(), parents.begin(), parents.end());
    }
    if (!preferred) {
        auto owner = m_ownerDiscovery.find(URI::fromFile(canonical));
        if (owner != m_ownerDiscovery.end() && owner->second.state == OwnerDiscoveryState::Unique &&
            owner->second.root)
            preferred = IncludeContextSelection{.uri = URI::fromFile(canonical),
                                                .source = *owner->second.root};
    }
    std::ranges::sort(sources);
    if (sources.empty())
        return {};
    if (preferred) {
        auto it = std::ranges::find(sources, std::filesystem::path(preferred->source.getPath()));
        if (it != sources.end())
            std::rotate(sources.begin(), it, it + 1);
    }

    std::vector<IncludeContext> contexts;
    for (const auto& source : sources) {
        std::error_code ec;
        if (!sm.isCached(source) && !std::filesystem::is_regular_file(source, ec))
            continue;
        auto doc = getDocument(URI::fromFile(source));
        if (!doc)
            continue;
        auto tree = doc->getSyntaxTree();
        size_t occurrence = 0;
        for (const auto& include : tree->getIncludeDirectives()) {
            if (!include.buffer || sm.getFullPath(include.buffer.id) != canonical)
                continue;
            contexts.push_back({.source = doc,
                                .buffer = include.buffer.id,
                                .location = toLocation(include.syntax->sourceRange(), sm),
                                .occurrence = occurrence++});
        }
        if (selectedOnly && !contexts.empty())
            break;
    }
    if (preferred) {
        auto it = std::ranges::find_if(contexts, [&](const auto& context) {
            return context.source->getURI() == preferred->source &&
                   context.occurrence == preferred->occurrence;
        });
        if (it != contexts.end())
            std::rotate(contexts.begin(), it, it + 1);
    }
    if (selectedOnly && contexts.size() > 1)
        contexts.erase(contexts.begin() + 1, contexts.end());
    return contexts;
}

bool ServerDriver::setIncludeContext(const IncludeContextSelection& selection) {
    auto canonical = std::filesystem::weakly_canonical(selection.uri.getPath());
    auto source = URI::fromFile(std::filesystem::weakly_canonical(selection.source.getPath()));
    auto contexts = getIncludeContexts(canonical);
    if (std::ranges::none_of(contexts, [&](const auto& context) {
            return context.source->getURI() == source && context.occurrence == selection.occurrence;
        }))
        return false;
    m_includeSelections.insert_or_assign(
        canonical, IncludeContextSelection{.uri = URI::fromFile(canonical),
                                           .source = source,
                                           .occurrence = selection.occurrence});
    invalidateAnalysesAndRefreshClient();
    for (const auto& uri : m_openDocs)
        analyzeDocument(*getDocument(uri));
    return true;
}

std::shared_ptr<syntax::SyntaxTree> ServerDriver::parseShallowTree(SourceBuffer buffer,
                                                                   Bag& documentOptions) {
    documentOptions = options;
    auto tree = syntax::SyntaxTree::fromBuffer(buffer, sm, documentOptions);
    if (!m_buildSourceUris.contains(URI::fromFile(sm.getFullPath(buffer.id)))) {
        const auto configured = options.getOrDefault<parsing::PreprocessorOptions>();
        auto findTarget = [&](const parsing::IncludeMetadata& include) {
            auto location = sm.getFullyExpandedLoc(include.syntax->sourceRange().start());
            auto normal = sm.readHeader(include.path, location, sm.getLibraryFor(location.buffer()),
                                        false, configured.additionalIncludePaths);
            auto target = normal ? std::optional(sm.getFullPath(normal->id))
                                 : m_indexer.getNearestFileForInclude(
                                       include.path, sm.getFullPath(location.buffer()));
            return std::pair{std::move(target), bool(normal)};
        };
        std::vector<std::filesystem::path> inferred;
        while (true) {
            auto ppOptions = configured;
            auto& paths = ppOptions.additionalIncludePaths;
            auto proposed = inferred;
            for (const auto& include : tree->getIncludeDirectives()) {
                if (include.isSystem)
                    continue;
                auto [target, normal] = findTarget(include);
                if (!target)
                    continue;
                auto suffix = std::filesystem::path(include.path).lexically_normal();
                if (suffix.is_absolute() ||
                    std::ranges::any_of(suffix, [](const auto& part) { return part == ".."; }))
                    continue;
                auto directory = *target;
                while (!suffix.empty() && directory.filename() == suffix.filename()) {
                    directory = directory.parent_path();
                    suffix = suffix.parent_path();
                }
                if (!suffix.empty())
                    continue;
                // Additional paths precede global -I paths, so keep known resolutions first.
                auto& destinations = normal ? paths : proposed;
                if (std::ranges::find(destinations, directory) == destinations.end())
                    destinations.push_back(std::move(directory));
            }
            if (proposed == inferred)
                break;
            for (const auto& directory : proposed) {
                if (std::ranges::find(paths, directory) == paths.end())
                    paths.push_back(directory);
            }
            auto proposedOptions = options;
            proposedOptions.set(std::move(ppOptions));
            auto candidate = syntax::SyntaxTree::fromBuffer(buffer, sm, proposedOptions);
            bool compatible = true;
            for (const auto& include : candidate->getIncludeDirectives()) {
                if (include.isSystem || !include.buffer)
                    continue;
                auto expected = findTarget(include).first;
                if (!expected || std::filesystem::weakly_canonical(*expected) !=
                                     sm.getFullPath(include.buffer.id)) {
                    compatible = false;
                    break;
                }
            }
            // One tree has one search order; conflicting choices need explicit configuration.
            if (!compatible)
                break;
            inferred = std::move(proposed);
            documentOptions = std::move(proposedOptions);
            tree = std::move(candidate);
        }
    }
    m_indexer.updateIncludes(*tree);
    return tree;
}

std::vector<std::shared_ptr<syntax::SyntaxTree>> ServerDriver::getDependentTrees(
    std::shared_ptr<syntax::SyntaxTree> tree, const URI* target) {
    const bool hasPhysicalHints = std::ranges::any_of(tree->getSourceBufferIds(), [&](auto id) {
        return sm.getSourceText(id).find("slang-server:") != std::string_view::npos;
    });
    if (!hasConfiguredCompilationInputs() && !hasSourceContextHints() && !hasPhysicalHints &&
        getExplorePreincludeBuffers(tree->getSourceBufferIds().front()).empty())
        return getDependentTrees(std::move(tree), false);
    if (target) {
        bool handled = false;
        auto configured = getExplicitDependencies(tree, *target, handled);
        if (handled)
            return configured;
    }
    std::erase_if(m_virtualInterfaceReferences,
                  [](const auto& entry) { return entry.first.expired(); });
    std::vector<std::shared_ptr<syntax::SyntaxTree>> result;
    std::queue<std::shared_ptr<syntax::SyntaxTree>> treesToProcess;
    flat_hash_set<std::string_view> knownNames;
    flat_hash_map<std::string, std::shared_ptr<syntax::SyntaxTree>> knownProviders;
    flat_hash_set<std::string> processedFiles;

    treesToProcess.push(tree);

    while (!treesToProcess.empty()) {
        auto currentTree = treesToProcess.front();
        treesToProcess.pop();

        auto& meta = currentTree->getMetadata();

        // Collect declared symbols from current tree
        meta.visitDeclaredSymbols([&](std::string_view name) {
            knownNames.emplace(name);
            knownProviders.try_emplace(std::string(name), currentTree);
        });

        auto loadDependency = [&](std::string_view name) {
            std::shared_ptr<syntax::SyntaxTree> dependencyTree;
            bool hinted = false;
            if (target)
                dependencyTree = getHintedPackageTree(currentTree, name, *target, hinted);
            if (hinted && !dependencyTree)
                return;
            if (knownNames.contains(name)) {
                if (hinted) {
                    auto provider = knownProviders.find(std::string(name));
                    if (provider != knownProviders.end() && provider->second != dependencyTree)
                        addExplicitContextError(
                            *target,
                            "Package source directive conflicts with the selected provider: " +
                                std::string(name));
                }
                return;
            }
            knownNames.emplace(name);
            std::string filePath;
            if (dependencyTree)
                filePath = sm.getFullPath(dependencyTree->getSourceBufferIds().front()).string();
            else {
                auto symbolLoc = m_indexer.getNearestSymbolLoc(
                    name, sm.getFullPath(currentTree->getSourceBufferIds().front()));
                if (!symbolLoc)
                    return;
                filePath = symbolLoc->uri->string();
            }
            if (!processedFiles.insert(filePath).second)
                return;
            auto newdoc = dependencyTree ? nullptr : getDocument(URI::fromFile(filePath));
            if (newdoc || dependencyTree) {
                if (!dependencyTree)
                    dependencyTree = newdoc->getSyntaxTree();
                knownProviders.try_emplace(std::string(name), dependencyTree);
                result.push_back(dependencyTree);

                // Recurse into packages and interfaces, since they may contain types from other
                // packages that are referenced by the analyzed module.
                for (auto& [decl, _] : dependencyTree->getMetadata().nodeMeta) {
                    if (decl->kind == syntax::SyntaxKind::PackageDeclaration ||
                        decl->kind == syntax::SyntaxKind::InterfaceDeclaration) {
                        treesToProcess.push(dependencyTree);
                        break;
                    }
                }
            }
            else {
                ERROR("No doc found for {}", filePath);
            }
        };

        meta.visitReferencedSymbols(loadDependency);
        // Parser metadata records interface ports but not virtual interface types
        // used by classes or typedefs. Those interfaces are required dependencies
        // of verification packages even when no RTL module instantiates them.
        auto [references, inserted] = m_virtualInterfaceReferences.try_emplace(currentTree);
        if (inserted) {
            flat_hash_set<std::string_view> seen;
            auto virtualInterfaces = syntax::makeSyntaxVisitor(
                [&](auto& visitor, const syntax::VirtualInterfaceTypeSyntax& type) {
                    auto name = type.name.valueText();
                    if (!name.empty() && seen.insert(name).second)
                        references->second.emplace_back(name);
                    visitor.visitDefault(type);
                });
            currentTree->root().visit(virtualInterfaces);
        }
        for (const auto& name : references->second)
            loadDependency(name);
    }

    return result;
}

std::vector<std::shared_ptr<syntax::SyntaxTree>> ServerDriver::getDependentTrees(
    std::shared_ptr<syntax::SyntaxTree> tree, bool fullHierarchy) {

    std::vector<std::shared_ptr<syntax::SyntaxTree>> result;
    std::queue<std::shared_ptr<syntax::SyntaxTree>> treesToProcess;
    flat_hash_set<std::string_view> knownNames;
    flat_hash_set<const syntax::SyntaxTree*> processedTrees{tree.get()};
    std::unordered_set<std::filesystem::path> processedFiles;
    for (auto buffer : tree->getSourceBufferIds())
        processedFiles.insert(sm.getFullPath(buffer));

    treesToProcess.push(tree);
    tree->getMetadata().visitDeclaredSymbols(
        [&](std::string_view name) { knownNames.emplace(name); });

    while (!treesToProcess.empty()) {
        auto currentTree = treesToProcess.front();
        treesToProcess.pop();

        auto& meta = currentTree->getMetadata();

        auto loadDependency = [&](parsing::Token token) {
            auto name = token.valueText();
            if (name.empty() || knownNames.contains(name))
                return;

            const auto& source = sm.getFullPath(sm.getFullyExpandedLoc(token.location()).buffer());
            auto symbolLoc = m_indexer.getNearestSymbolLoc(name, source);
            if (!symbolLoc)
                return;
            knownNames.emplace(name);
            // An included file may declare this name only under different preprocessor flags.
            // Reloading it would discard that context and can re-enter this compilation's analysis.
            if (processedFiles.contains(std::filesystem::weakly_canonical(*symbolLoc->uri)))
                return;
            auto newdoc = getDocument(URI::fromFile(*symbolLoc->uri));
            if (newdoc) {
                auto dependencyTree = newdoc->getSyntaxTree();
                if (!processedTrees.insert(dependencyTree.get()).second)
                    return;
                for (auto buffer : dependencyTree->getSourceBufferIds())
                    processedFiles.insert(sm.getFullPath(buffer));
                result.push_back(dependencyTree);
                dependencyTree->getMetadata().visitDeclaredSymbols(
                    [&](std::string_view declared) { knownNames.emplace(declared); });

                if (fullHierarchy ||
                    std::ranges::any_of(
                        dependencyTree->getMetadata().nodeMeta, [](const auto& entry) {
                            return entry.first->kind == syntax::SyntaxKind::PackageDeclaration ||
                                   entry.first->kind == syntax::SyntaxKind::InterfaceDeclaration;
                        }))
                    treesToProcess.push(dependencyTree);
            }
            else {
                ERROR("No doc found for {}", symbolLoc->uri->string());
            }
        };

        // Keep each reference's location so dependencies in included files use their own directory.
        for (auto instance : meta.globalInstances)
            loadDependency(instance->type);
        for (auto name : meta.classPackageNames)
            loadDependency(name->identifier);
        for (auto import : meta.packageImports) {
            for (auto item : import->items)
                loadDependency(item->package);
        }
        for (auto type : meta.virtualInterfaceTypes)
            loadDependency(type->name);
        if (!fullHierarchy) {
            for (auto port : meta.interfacePorts)
                loadDependency(port->nameOrKeyword);
            for (auto type : meta.namedPortTypes)
                loadDependency(type->identifier);
        }
    }

    return result;
}

std::vector<std::string> ServerDriver::getModulesInFile(const std::string& path) {
    // Find the document
    auto uri = URI::fromFile(path);
    auto it = docs.find(uri);
    if (it == docs.end()) {
        WARN("Document {} not found", path);
        return {};
    }

    auto& doc = it->second;

    // Get the module-like things from the document and collect into a vector
    std::vector<std::string> moduleNames;
    for (auto& name : doc->getSyntaxTree()->getMetadata().getDeclaredSymbols()) {
        moduleNames.push_back(std::string{name});
    }
    if (moduleNames.empty()) {
        WARN("No modules found in file {}", path);
    }
    INFO("Found {} modules in file {}", moduleNames.size(), path);
    return moduleNames;
}

std::unique_ptr<ServerDriver> ServerDriver::createFromTop(
    Indexer& indexer, SlangLspClient& client, const Config& config, const URI& topUri,
    std::optional<std::string_view> workspaceFolder, const ServerDriver* oldDriver,
    bool requireValidConfig) {
    auto newDriver = createForExplore(indexer, client, config, workspaceFolder, oldDriver,
                                      requireValidConfig);
    if (!newDriver)
        return nullptr;
    auto doc = newDriver->getDocument(topUri);
    if (!doc) {
        if (requireValidConfig)
            return nullptr;
        client.showError("Document not found: " + std::string(topUri.getPath()));
        return newDriver;
    }

    auto topTree = doc->getSyntaxTree();
    std::string topName;
    const auto& metadata = topTree->getMetadata().nodeMeta;
    if (metadata.size() == 1 &&
        (metadata[0].first->kind == syntax::SyntaxKind::ModuleDeclaration ||
         metadata[0].first->kind == syntax::SyntaxKind::InterfaceDeclaration ||
         metadata[0].first->kind == syntax::SyntaxKind::ProgramDeclaration)) {
        topName = topTree->getMetadata().nodeMeta[0].first->header->name.valueText();
    }
    else {
        ast::Compilation shallowCompilation;
        shallowCompilation.addSyntaxTree(topTree);
        auto& topInstances = shallowCompilation.getRoot().topInstances;
        if (topInstances.empty()) {
            if (requireValidConfig)
                return nullptr;
            client.showError("No top modules found in: " + std::string(topUri.getPath()));
            return newDriver;
        }
        for (auto& top : topInstances.subspan(1)) {
            WARN("Extra top module: {}", top->name);
        }
        topName = topInstances[0]->name;
    }

    auto syntaxTrees = newDriver->getDependentTrees(topTree, /* fullHierarchy */ true);
    for (auto& dependency : syntaxTrees)
        dependency->isLibraryUnit = true;
    syntaxTrees.insert(syntaxTrees.begin(), topTree);

    std::vector<std::shared_ptr<SlangDoc>> documents;
    documents.reserve(syntaxTrees.size());
    for (const auto& tree : syntaxTrees) {
        documents.push_back(SlangDoc::fromTree(*newDriver, tree));
    }
    for (const auto& dependency : documents) {
        if (auto previous = newDriver->docs.find(dependency->getURI());
            previous != newDriver->docs.end())
            dependency->setVersion(previous->second->getVersion());
        newDriver->docs[dependency->getURI()] = dependency;
    }

    newDriver->comp = std::make_unique<ServerCompilation>(
        documents, newDriver->options, newDriver->sm, newDriver->client, std::move(topName),
        (newDriver->isFrozenSourcesEnabled() || newDriver->isUvmPrefixCacheEnabled()) && oldDriver
            ? oldDriver->comp.get()
            : nullptr);

    newDriver->configureFrozenInstances();
    newDriver->publishCompilationDiagnostics();
    return newDriver;
}

std::optional<DefinitionInfo> ServerDriver::getMacroDefinitionInfo(
    const ShallowAnalysis& analysis, const parsing::Token& token,
    const syntax::SyntaxNode& referenceSyntax, const lsp::RequestContext& ctx) {
    std::vector<const syntax::DefineDirectiveSyntax*> macroDefs;
    auto addMacroDef = [&](const syntax::DefineDirectiveSyntax* definition) {
        if (definition && std::ranges::find(macroDefs, definition) == macroDefs.end())
            macroDefs.push_back(definition);
    };
    if (referenceSyntax.kind == syntax::SyntaxKind::MacroUsage ||
        referenceSyntax.kind == syntax::SyntaxKind::UndefDirective) {
        auto it = analysis.macroUsageDefinitions.find(&referenceSyntax);
        if (it != analysis.macroUsageDefinitions.end())
            addMacroDef(it->second);
    }

    // A macro usage in a define body has no direct usage mapping. The workspace index also
    // supplies definitions that are not part of the current shallow compilation.
    if (macroDefs.empty()) {
        auto macroName = token.kind == parsing::TokenKind::Directive ? token.rawText().substr(1)
                                                                     : token.valueText();
        auto macro = analysis.macros.find(macroName);
        if (macro != analysis.macros.end()) {
            addMacroDef(macro->second);
        }
        else {
            std::vector<std::filesystem::path> visited;
            auto paths = m_indexer.getFilesForMacro(macroName);
            if (paths.size() > 1)
                std::ranges::sort(paths);
            for (const auto& path : paths) {
                if (std::ranges::find(visited, path) != visited.end())
                    continue;
                visited.push_back(path);

                auto macroDoc = getDocument(URI::fromFile(path));
                if (!macroDoc)
                    continue;
                auto macroAnalysis = macroDoc->getAnalysis(ctx);
                auto indexedMacro = macroAnalysis->macros.find(macroName);
                if (indexedMacro != macroAnalysis->macros.end())
                    addMacroDef(indexedMacro->second);
            }
        }
        if (macroDefs.empty())
            return {};
    }

    std::vector<DefinitionInfo::Target> targets;
    for (auto* macroDef : macroDefs) {
        auto nameToken = macroDef->name;
        const bool isMacroGenerated = sm.isMacroLoc(nameToken.location());
        auto syntaxTarget = DefinitionInfo::SyntaxTarget::fromNode(macroDef, nameToken, sm);
        if (isMacroGenerated && syntaxTarget.macroUsageRange == SourceRange::NoLocation)
            ERROR("Couldn't get original range for macro {}", nameToken.valueText());
        DefinitionInfo::MacroTarget::Definition macroDefinition = syntaxTarget;

        const auto defPath = sm.getFullPath(nameToken.location().buffer());
        const auto defPathStr = defPath.filename().string();
        if (!isMacroGenerated && (defPathStr.empty() || defPathStr[0] == '<')) {
            std::string defineSourceFile;
            const auto srcIt = m_defineSources.find(std::string(nameToken.valueText()));
            if (srcIt != m_defineSources.end())
                defineSourceFile = srcIt->second.string();
            macroDefinition = DefinitionInfo::CommandLineDefineTarget{nameToken, defineSourceFile};
        }

        targets.emplace_back(
            DefinitionInfo::MacroTarget{std::move(macroDefinition), referenceSyntax, analysis});
    }
    return DefinitionInfo{sm, std::move(targets)};
}

std::optional<DefinitionInfo> ServerDriver::getDefinitionInfoAt(const URI& uri,
                                                                const lsp::Position& position,
                                                                const lsp::RequestContext& ctx) {
    auto doc = getDocument(uri);
    if (!doc) {
        return {};
    }
    auto analysis = doc->getAnalysis(ctx);

    // Get location, token, and syntax node at position
    auto loc = toSourceLocation(analysis->getBuffer(), position, sm);
    if (!loc) {
        return {};
    }
    const parsing::Token* declTok = analysis->syntaxes.getWordTokenAt(loc.value());
    if (!declTok) {
        return {};
    }
    const syntax::SyntaxNode* declSyntax = analysis->syntaxes.getTokenParent(declTok);
    if (!declSyntax) {
        return {};
    }

    auto isMacroRef = [&]() {
        // Normal macro usage (`FOO) or usage inside a `define body
        return declTok->kind == parsing::TokenKind::Directive &&
               (declSyntax->kind == syntax::SyntaxKind::MacroUsage ||
                declSyntax->kind == syntax::SyntaxKind::DefineDirective);
    };
    auto isUndefRef = [&]() {
        // Identifier in `undef FOO
        return declTok->kind == parsing::TokenKind::Identifier &&
               declSyntax->kind == syntax::SyntaxKind::UndefDirective;
    };
    auto isIfdefRef = [&]() {
        // Identifier in `ifdef FOO / `ifndef FOO / `elsif FOO
        return declTok->kind == parsing::TokenKind::Identifier &&
               declSyntax->kind == syntax::SyntaxKind::NamedConditionalDirectiveExpression;
    };

    if (isMacroRef() || isUndefRef() || isIfdefRef())
        return getMacroDefinitionInfo(*analysis, *declTok, *declSyntax, ctx);

    if (declTok->kind == parsing::TokenKind::SystemIdentifier) {
        auto knownName = declTok->systemName();
        if (knownName == parsing::KnownSystemName::Unknown)
            return {};

        auto* sub = analysis->getCompilation()->getSystemSubroutine(knownName);
        auto* sysDoc = getSystemTaskDoc(knownName);
        if (!sub || !sysDoc)
            return {};

        return DefinitionInfo{sm, DefinitionInfo::SystemSubroutineTarget{
                                      *declTok, sysDoc, sub->kind == ast::SubroutineKind::Task}};
    }
    auto symbolsEquivalent = [&](const ast::Symbol* left, const ast::Symbol* right) {
        if (left == right)
            return true;
        if (left->kind != right->kind ||
            sm.getFullyOriginalLoc(left->location) != sm.getFullyOriginalLoc(right->location)) {
            return false;
        }

        if (ast::ValueSymbol::isKind(left->kind)) {
            auto& leftValue = left->as<ast::ValueSymbol>();
            auto& rightValue = right->as<ast::ValueSymbol>();
            if (!leftValue.getType().isMatching(rightValue.getType()))
                return false;

            auto constantsMatch = [](const ConstantValue& a, const ConstantValue& b) {
                return (a.bad() || b.bad()) ? a.bad() == b.bad() : a == b;
            };
            if (ast::ParameterSymbol::isKind(left->kind)) {
                return constantsMatch(left->as<ast::ParameterSymbol>().getValue(),
                                      right->as<ast::ParameterSymbol>().getValue());
            }
            if (ast::EnumValueSymbol::isKind(left->kind)) {
                return constantsMatch(left->as<ast::EnumValueSymbol>().getValue(),
                                      right->as<ast::EnumValueSymbol>().getValue());
            }
            return true;
        }
        if (ast::Type::isKind(left->kind))
            return left->as<ast::Type>().isMatching(right->as<ast::Type>());
        if (ast::TypeParameterSymbol::isKind(left->kind)) {
            return left->as<ast::TypeParameterSymbol>().targetType.getType().isMatching(
                right->as<ast::TypeParameterSymbol>().targetType.getType());
        }
        return false;
    };

    struct SymbolCandidate {
        const ast::Symbol* symbol;
        std::shared_ptr<ShallowAnalysis> analysis;
        bool renderInterfaceConnection;
    };
    std::vector<SymbolCandidate> symbols;
    auto addSymbol = [&](const ast::Symbol* symbol,
                         const std::shared_ptr<ShallowAnalysis>& symbolAnalysis,
                         bool renderInterfaceConnection = true) {
        if (!symbol)
            return;
        auto duplicate = std::ranges::find_if(symbols, [&](const auto& existing) {
            return (existing.symbol == symbol && existing.analysis == symbolAnalysis) ||
                   (existing.analysis != symbolAnalysis && existing.symbol->kind == symbol->kind &&
                    existing.symbol->location == symbol->location) ||
                   symbolsEquivalent(existing.symbol, symbol);
        });
        if (duplicate != symbols.end()) {
            duplicate->renderInterfaceConnection |= renderInterfaceConnection;
        }
        else {
            symbols.push_back({symbol, symbolAnalysis, renderInterfaceConnection});
        }
    };

    auto localSymbols = analysis->getSymbolsAtToken(declTok);
    for (auto* symbol : localSymbols) {
        addSymbol(symbol, analysis);
        if (auto* port = symbol->as_if<ast::InterfacePortSymbol>()) {
            if (auto* connection = analysis->getActiveInterfaceConnection(*port)) {
                for (auto* designSymbol : connection->sourcePath)
                    addSymbol(designSymbol, analysis, false);
            }
        }
    }
    if (std::ranges::any_of(localSymbols, [](const auto* symbol) {
            return symbol->kind == ast::SymbolKind::Genvar;
        })) {
        for (auto* parameter : analysis->getGenvarIterationParametersAtToken(declTok))
            addSymbol(parameter, analysis);
    }

    auto getGeneratedSignalCount = [&](const ast::Symbol* symbol) {
        if (symbol->kind != ast::SymbolKind::Net && symbol->kind != ast::SymbolKind::Variable)
            return size_t{1};

        bool isGenerated = false;
        for (auto* scope = symbol->getHierarchicalParent(); scope;
             scope = scope->asSymbol().getHierarchicalParent()) {
            if (scope->asSymbol().kind == ast::SymbolKind::GenerateBlock) {
                isGenerated = true;
                break;
            }
        }
        if (!isGenerated)
            return size_t{1};

        std::vector<const ast::Symbol*> generatedSignals;
        for (auto* candidate : localSymbols) {
            if ((candidate->kind == ast::SymbolKind::Net ||
                 candidate->kind == ast::SymbolKind::Variable) &&
                symbolsEquivalent(symbol, candidate) &&
                std::ranges::find(generatedSignals, candidate) == generatedSignals.end()) {
                generatedSignals.push_back(candidate);
            }
        }
        return std::max(size_t{1}, generatedSignals.size());
    };

    bool hasTopLevelResolution = std::ranges::any_of(localSymbols, [](const ast::Symbol* symbol) {
        return symbol->kind == ast::SymbolKind::Definition ||
               symbol->kind == ast::SymbolKind::Package;
    });
    bool hasLocalTopLevelResolution =
        std::ranges::any_of(localSymbols, [&](const ast::Symbol* symbol) {
            return (symbol->kind == ast::SymbolKind::Definition ||
                    symbol->kind == ast::SymbolKind::Package) &&
                   sm.getFullyExpandedLoc(symbol->location).buffer() == analysis->getBuffer();
        });

    // A declaration in the queried document is authoritative. Other top-level names retain
    // every indexed definition tied at the nearest directory distance.
    bool searchIndex = localSymbols.empty() ||
                       (hasTopLevelResolution && !hasLocalTopLevelResolution);
    if (searchIndex) {
        auto matchesResolvedKind = [&](const ast::Symbol* candidate) {
            if (localSymbols.empty())
                return true;
            return std::ranges::any_of(localSymbols, [&](const ast::Symbol* resolved) {
                if (resolved->kind != candidate->kind)
                    return false;
                if (auto* resolvedDef = resolved->as_if<ast::DefinitionSymbol>()) {
                    auto* candidateDef = candidate->as_if<ast::DefinitionSymbol>();
                    return candidateDef &&
                           candidateDef->definitionKind == resolvedDef->definitionKind;
                }
                return true;
            });
        };

        auto nearest = m_indexer.getNearestSymbolLocs(declTok->valueText(),
                                                      doc->getURI().getPath());
        if (!nearest.empty()) {
            std::erase_if(symbols, [&](const auto& candidate) {
                if (candidate.symbol->kind != ast::SymbolKind::Definition &&
                    candidate.symbol->kind != ast::SymbolKind::Package)
                    return false;
                auto path = sm.getFullPath(
                    sm.getFullyExpandedLoc(candidate.symbol->location).buffer());
                return std::ranges::none_of(nearest, [&](const auto& entry) {
                    return URI::fromFile(*entry.uri) == URI::fromFile(path);
                });
            });
        }
        for (const auto& entry : nearest) {
            auto symbolDoc = getDocument(URI::fromFile(*entry.uri));
            if (!symbolDoc)
                continue;
            auto symbolAnalysis = symbolDoc->getAnalysis(ctx);
            auto addGlobalDefinition = [&](const ast::Symbol* symbol) {
                if (symbol && symbol->name == declTok->valueText() &&
                    sm.getFullyExpandedLoc(symbol->location).buffer() ==
                        symbolAnalysis->getBuffer() &&
                    matchesResolvedKind(symbol)) {
                    addSymbol(symbol, symbolAnalysis);
                }
            };

            for (auto* definition : symbolAnalysis->getCompilation()->getDefinitions())
                addGlobalDefinition(definition);

            for (auto* unit : symbolAnalysis->getCompilation()->getCompilationUnits()) {
                for (auto& member : unit->members()) {
                    if (member.kind == ast::SymbolKind::Package)
                        addGlobalDefinition(&member);
                }
            }
        }
    }

    auto makeSyntaxTarget =
        [&](const ast::Symbol* symbol) -> std::optional<DefinitionInfo::SyntaxTarget> {
        auto* symSyntax = symbol->getSyntax();
        if (!symSyntax) {
            if (auto* typeParam = symbol->as_if<ast::TypeParameterSymbol>())
                symSyntax = typeParam->getTypeAlias().getSyntax();
        }
        if (!symSyntax) {
            ERROR("Failed to get syntax for symbol {} of kind {}", symbol->name,
                  toString(symbol->kind));
            return {};
        }

        // Modports have a directional declaration around their name syntax.
        if ((symbol->kind == ast::SymbolKind::Modport ||
             symbol->kind == ast::SymbolKind::ModportPort) &&
            symSyntax->parent) {
            symSyntax = symSyntax->parent;
        }

        auto foundNameToken = findNameToken(symSyntax, symbol->name);
        if (!foundNameToken) {
            ERROR("Failed to find name token for symbol '{}' of kind {} = {}", symbol->name,
                  toString(symbol->kind), symSyntax->toString());
        }
        parsing::Token nameToken = foundNameToken ? *foundNameToken : symSyntax->getFirstToken();

        auto result = DefinitionInfo::SyntaxTarget::fromNode(symSyntax, nameToken, sm);
        if (sm.isMacroLoc(nameToken.location()) &&
            result.macroUsageRange == SourceRange::NoLocation) {
            ERROR("Couldn't get original range for symbol {}", nameToken.valueText());
        }
        return result;
    };

    auto makeSymbolTarget =
        [&](const ast::Symbol* symbol, const std::shared_ptr<ShallowAnalysis>& symbolAnalysis,
            bool renderInterfaceConnection = true) -> std::optional<DefinitionInfo::SymbolTarget> {
        if (!symbol)
            return {};
        auto syntaxTarget = makeSyntaxTarget(symbol);
        if (!syntaxTarget)
            return {};

        std::vector<DefinitionInfo::SyntaxTarget> syntaxes;
        syntaxes.push_back(std::move(*syntaxTarget));
        if (auto* modportPort = symbol->as_if<ast::ModportPortSymbol>()) {
            if (modportPort->internalSymbol) {
                if (auto internalSyntax = makeSyntaxTarget(modportPort->internalSymbol))
                    syntaxes.push_back(std::move(*internalSyntax));
            }
        }
        auto* designSymbol = symbolAnalysis->getDesignSymbol(*symbol);
        return DefinitionInfo::SymbolTarget{.syntaxes = std::move(syntaxes),
                                            .symbol = designSymbol ? designSymbol : symbol,
                                            .analysis = symbolAnalysis,
                                            .renderInterfaceConnection = renderInterfaceConnection,
                                            .generatedSignalCount = getGeneratedSignalCount(
                                                symbol)};
    };

    if (declSyntax && declSyntax->kind == syntax::SyntaxKind::NamedPortConnection &&
        !declSyntax->as<syntax::NamedPortConnectionSyntax>().openParen && localSymbols.size() > 1) {
        std::vector<DefinitionInfo::Target> targets;
        for (size_t i = 0; i + 1 < localSymbols.size(); i += 2) {
            auto outer = makeSymbolTarget(localSymbols[i], analysis);
            auto inner = makeSymbolTarget(localSymbols[i + 1], analysis);
            if (outer && inner) {
                inner->renderInputPortDriver = true;
                DefinitionInfo::PortConnectionTarget port{std::move(*outer), std::move(*inner)};
                auto duplicate = std::ranges::any_of(targets, [&](const auto& target) {
                    auto* existing = std::get_if<DefinitionInfo::PortConnectionTarget>(&target);
                    return existing &&
                           symbolsEquivalent(existing->outer.symbol, port.outer.symbol) &&
                           symbolsEquivalent(existing->inner.symbol, port.inner.symbol);
                });
                if (!duplicate)
                    targets.emplace_back(std::move(port));
            }
            else if (outer) {
                targets.emplace_back(std::move(*outer));
            }
            else if (inner) {
                targets.emplace_back(std::move(*inner));
            }
        }
        for (const auto& [symbol, symbolAnalysis, renderInterfaceConnection] : symbols) {
            auto alreadyRepresented = std::ranges::any_of(localSymbols, [&](const auto* local) {
                return local == symbol ||
                       (local->kind == symbol->kind && local->getSyntax() == symbol->getSyntax());
            });
            if (!alreadyRepresented) {
                if (auto target = makeSymbolTarget(symbol, symbolAnalysis,
                                                   renderInterfaceConnection)) {
                    targets.emplace_back(std::move(*target));
                }
            }
        }
        if (!targets.empty())
            return DefinitionInfo{sm, std::move(targets)};
    }

    std::vector<DefinitionInfo::Target> targets;
    std::vector<const ast::Symbol*> foldedSymbols;
    for (const auto& [symbol, symbolAnalysis, renderInterfaceConnection] : symbols) {
        if (std::ranges::find(foldedSymbols, symbol) != foldedSymbols.end())
            continue;

        auto target = makeSymbolTarget(symbol, symbolAnalysis, renderInterfaceConnection);
        if (!target)
            continue;

        if (auto* connection = declSyntax->as_if<syntax::NamedPortConnectionSyntax>();
            connection && connection->expr) {
            target->renderInputPortDriver = true;
        }

        if (auto* modportPort = symbol->as_if<ast::ModportPortSymbol>()) {
            if (modportPort->internalSymbol)
                foldedSymbols.push_back(modportPort->internalSymbol);
        }
        targets.emplace_back(std::move(*target));
    }

    if (targets.empty())
        return {};
    return DefinitionInfo{sm, std::move(targets)};
}

std::optional<std::string> ServerDriver::getDesignInstancePathAt(const URI& uri,
                                                                 const lsp::Position& position,
                                                                 const lsp::RequestContext& ctx) {
    auto doc = getDocument(uri);
    if (!doc)
        return {};

    auto analysis = doc->getAnalysis(ctx);
    auto loc = toSourceLocation(analysis->getBuffer(), position, sm);
    if (!loc)
        return {};
    return analysis->getDesignInstancePathAtToken(analysis->syntaxes.getWordTokenAt(*loc));
}

std::optional<lsp::Hover> ServerDriver::getDocHover(
    const URI& uri, const lsp::Position& position, const lsp::RequestContext& ctx,
    std::optional<Config::HoverConfig::MacroView> macroView) {
    const auto doc = getDocument(uri);
    if (!doc) {
        return {};
    }
    auto analysis = doc->getAnalysis(ctx);
    auto loc = toSourceLocation(analysis->getBuffer(), position, sm);
    if (!loc) {
        return {};
    }
    auto maybeInfo = getDefinitionInfoAt(uri, position, ctx);
    if (!maybeInfo) {
        if (s_debugHoversEnabled) {
            // Shows debug info for the token under cursor when debugging.
            markup::Document markup;
            markup.addParagraph(analysis->getDebugHover(loc.value()));
            return lsp::Hover{.contents = markup.build()};
        }
        return {};
    }
    const auto& info = *maybeInfo;
    auto hovers = m_config.hovers.value();
    if (macroView)
        hovers.macroView = *macroView;
    return lsp::Hover{.contents = info.getHover(analysis->getBuffer(), hovers)};
}

namespace {
std::optional<lsp::Location> physicalMemberDeclaration(const ast::Symbol& symbol,
                                                       const SourceManager& sources) {
    if (symbol.kind != ast::SymbolKind::Field && symbol.kind != ast::SymbolKind::Subroutine &&
        symbol.kind != ast::SymbolKind::MethodPrototype)
        return std::nullopt;
    auto start = sources.getFullyOriginalLoc(symbol.location);
    auto end = start + symbol.name.size();
    if (auto syntax = symbol.getSyntax();
        syntax && syntax->kind == syntax::SyntaxKind::Declarator) {
        const auto& name = syntax->as<syntax::DeclaratorSyntax>().name;
        start = sources.getFullyOriginalLoc(name.range().start());
        end = sources.getFullyOriginalLoc(name.range().end());
    }
    // Projected fields and imported functions can lack declarations in the
    // current local token index, especially in an invalid-parameter fallback.
    // Only actual physical spellings can participate in reference / rename edits.
    if (start && end && start.buffer() == end.buffer() && start.offset() <= end.offset() &&
        end.offset() <= sources.getSourceText(start.buffer()).size() &&
        sources.getSourceText(SourceRange(start, end)) == symbol.name)
        return toOriginalLocation(SourceRange(start, end), sources);
    return std::nullopt;
}
} // namespace

std::optional<std::vector<lsp::DocumentHighlight>> ServerDriver::getDocDocumentHighlight(
    const URI& uri, const lsp::Position& position) {
    auto doc = getDocument(uri);
    if (!doc) {
        return std::nullopt;
    }
    auto analysis = doc->getAnalysis();

    // Get the symbol at the position
    auto loc = toSourceLocation(analysis->getBuffer(), position, sm);
    if (!loc) {
        return std::nullopt;
    }
    auto declTok = analysis->syntaxes.getWordTokenAt(loc.value());
    if (!declTok) {
        return std::nullopt;
    }
    auto symbol = analysis->getSymbolAtToken(declTok);
    if (!symbol) {
        return std::nullopt;
    }

    // Find all references to the symbol in the current document
    std::vector<lsp::Location> references;
    analysis->addLocalReferences(references, symbol->location, symbol->name);
    if (auto declaration = physicalMemberDeclaration(*symbol, sm);
        declaration && declaration->uri == uri &&
        std::ranges::none_of(references, [&](const auto& reference) {
            return reference.uri == declaration->uri && reference.range == declaration->range;
        }))
        references.push_back(*declaration);
    if (references.empty()) {
        return std::nullopt;
    }

    std::vector<lsp::DocumentHighlight> highlights;
    highlights.reserve(references.size());
    for (auto& ref : references) {
        highlights.push_back(lsp::DocumentHighlight{
            .range = ref.range,
        });
    }

    return highlights;
}

void ServerDriver::addMemberReferences(std::vector<lsp::Location>& references,
                                       const ast::Symbol& parentSymbol,
                                       const ast::Symbol& targetSymbol, bool isTypeMember,
                                       const lsp::RequestContext& ctx) {

    auto targetBuffer = sm.getFullyOriginalLoc(targetSymbol.location).buffer();
    auto targetDoc = getDocument(URI::fromFile(sm.getFullPath(targetBuffer)));
    auto targetName = targetSymbol.name;

    auto referencingFiles = m_indexer.getFilesReferencingSymbol(parentSymbol.name);
    for (auto& filePath : referencingFiles) {
        ctx.throwIfCancelled("while finding member references");
        URI fileUri = URI::fromFile(filePath.string());

        // Skip the file where targetSymbol is defined to avoid duplicates
        if (fileUri == targetDoc->getURI()) {
            continue;
        }

        auto fileDoc = getDocument(fileUri);
        if (!fileDoc) {
            continue;
        }

        // The workspace index can include files outside a complete compilation
        // filelist. They are search candidates, not additional semantic inputs.
        getIncludeContext(fileUri);
        if (hasExplicitContextError(fileUri))
            continue;

        // if a package, check if we can just use the package ref syntaxes to save on
        // making analysis
        if (!isTypeMember && parentSymbol.kind == ast::SymbolKind::Package) {
            auto& meta = fileDoc->getSyntaxTree()->getMetadata();
            bool hasWildcard = [&] {
                for (auto ref : meta.packageImports) {
                    for (auto item : ref->items) {
                        if (item->package.valueText() == parentSymbol.name &&
                            item->item.kind == parsing::TokenKind::Star) {
                            return true;
                        }
                    }
                }
                return false;
            }();
            if (!hasWildcard) {
                // no wildcard, just check cases of pkg::<targetName>
                for (auto ref : meta.classPackageNames) {
                    if (ref->identifier.valueText() != parentSymbol.name) {
                        continue;
                    }
                    auto tok = ref->parent->as<syntax::ScopedNameSyntax>().right->getFirstToken();
                    if (tok.valueText() == targetName) {
                        references.push_back(toOriginalLocation(tok.range(), sm));
                    }
                }
                continue;
            }
        }

        auto fileAnalysis = fileDoc->getAnalysis(ctx);
        fileAnalysis->addLocalReferences(references, targetSymbol.location, targetName);
    }
}

std::optional<std::vector<lsp::Location>> ServerDriver::getDocReferences(
    const URI& srcUri, const lsp::Position& position, bool includeDeclaration,
    const lsp::RequestContext& ctx) {
    ctx.throwIfCancelled("before finding references");
    auto doc = getDocument(srcUri);
    if (!doc) {
        return std::nullopt;
    }

    // Get the symbol at the position. Hold the analysis via shared_ptr so that symbols remain
    // valid even if getAnalysis() is called on this doc again.
    auto analysis = doc->getAnalysis(ctx);
    ctx.throwIfCancelled("before resolving reference target");
    auto loc = toSourceLocation(analysis->getBuffer(), position, sm);
    if (!loc) {
        return std::nullopt;
    }

    const parsing::Token* declTok = analysis->syntaxes.getWordTokenAt(loc.value());
    if (!declTok) {
        return std::nullopt;
    }

    struct ReferenceTarget {
        const ast::Symbol* symbol;
        std::shared_ptr<ShallowAnalysis> analysis;
        BufferID analysisBuffer;
    };
    SmallVector<ReferenceTarget, 2> targetSymbols;
    auto addTargetSymbol = [&](const ast::Symbol* symbol,
                               const std::shared_ptr<ShallowAnalysis>& symbolAnalysis,
                               BufferID symbolAnalysisBuffer) {
        if (!symbol)
            return;

        // A top level of a shallow compilation is an instance body; get the definition instead
        if (symbol->kind == ast::SymbolKind::InstanceBody)
            symbol = &symbol->as<ast::InstanceBodySymbol>().getDefinition();

        auto symbolLocation = sm.getFullyOriginalLoc(symbol->location);
        auto duplicate = std::ranges::any_of(targetSymbols, [&](const ReferenceTarget& existing) {
            return existing.symbol == symbol ||
                   (existing.symbol->kind == symbol->kind &&
                    sm.getFullyOriginalLoc(existing.symbol->location) == symbolLocation);
        });
        if (!duplicate)
            targetSymbols.push_back({symbol, symbolAnalysis, symbolAnalysisBuffer});
    };

    auto addTargetsAt = [&](const std::shared_ptr<SlangDoc>& referenceDoc,
                            const std::shared_ptr<ShallowAnalysis>& referenceAnalysis,
                            const parsing::Token* token) {
        auto symbols = referenceAnalysis->getSymbolsAtToken(token);
        bool hasGlobal = std::ranges::any_of(symbols, [](const auto* symbol) {
            return symbol->kind == ast::SymbolKind::Definition ||
                   symbol->kind == ast::SymbolKind::Package ||
                   symbol->kind == ast::SymbolKind::InstanceBody;
        });
        if (hasGlobal || symbols.empty()) {
            auto info = getDefinitionInfoAt(referenceDoc->getURI(),
                                            toRange(token->range(), sm).start);
            if (info) {
                bool found = false;
                for (const auto& entry : info->targets) {
                    auto* target = std::get_if<DefinitionInfo::SymbolTarget>(&entry);
                    if (target && (target->symbol->kind == ast::SymbolKind::Definition ||
                                   target->symbol->kind == ast::SymbolKind::Package)) {
                        addTargetSymbol(target->symbol, target->analysis,
                                        referenceDoc->getBuffer());
                        found = true;
                    }
                }
                if (found)
                    return;
            }
        }
        for (auto* symbol : symbols)
            addTargetSymbol(symbol, referenceAnalysis, referenceDoc->getBuffer());
    };

    addTargetsAt(doc, analysis, declTok);

    if (targetSymbols.empty())
        return std::nullopt;

    std::vector<lsp::Location> references;
    std::vector<lsp::Location> declarations;

    auto targetName = declTok->rawText();

    auto findPkgReferencesInDocument = [&](const parsing::ParserMetadata& meta, const URI&) {
        for (auto ref : meta.packageImports) {
            for (auto item : ref->items) {
                if (item->package.valueText() == targetName) {
                    references.push_back(toOriginalLocation(item->package.range(), sm));
                }
            }
        }
        for (auto ref : meta.classPackageNames) {
            if (ref->identifier.valueText() == targetName) {
                references.push_back(toOriginalLocation(ref->identifier.range(), sm));
            }
        }
    };

    auto findModuleReferencesInDocument = [&](const parsing::ParserMetadata& meta, const URI&) {
        for (auto inst : meta.globalInstances) {
            if (inst->type.valueText() == targetName) {
                references.push_back(toOriginalLocation(inst->type.range(), sm));
            }
        }
    };

    auto findInterfaceReferencesInDocument = [&](const parsing::ParserMetadata& meta, const URI&) {
        for (auto inst : meta.globalInstances) {
            if (inst->type.valueText() == targetName) {
                references.push_back(toOriginalLocation(inst->type.range(), sm));
            }
        }
        for (auto intf : meta.interfacePorts) {
            if (intf->nameOrKeyword.valueText() == targetName) {
                references.push_back(toOriginalLocation(intf->nameOrKeyword.range(), sm));
            }
        }
    };

    for (size_t targetIndex = 0; targetIndex < targetSymbols.size(); targetIndex++) {
        ctx.throwIfCancelled("while finding references");
        const auto& target = targetSymbols[targetIndex];
        const auto* targetSymbol = target.symbol;
        const auto existingReferenceCount = references.size();
        auto targetLoc = sm.getFullyOriginalLoc(targetSymbol->location);
        auto targetDoc = getDocument(URI::fromFile(sm.getFullPath(targetLoc.buffer())));

        const ast::Symbol* indexedSymbol = targetSymbol;
        while (indexedSymbol && indexedSymbol->kind != ast::SymbolKind::Definition &&
               indexedSymbol->kind != ast::SymbolKind::Package) {
            if (auto* body = indexedSymbol->as_if<ast::InstanceBodySymbol>()) {
                indexedSymbol = &body->getDefinition();
                break;
            }
            auto* parent = indexedSymbol->getParentScope();
            indexedSymbol = parent ? &parent->asSymbol() : nullptr;
        }
        const bool hasDuplicates = indexedSymbol &&
                                   m_indexer.getFilesForSymbol(indexedSymbol->name).size() > 1;

        // Helper to process referencing files with a given finder function
        auto processReferencingFiles = [&](std::string_view name, auto&& finder) {
            for (const auto& filePath : m_indexer.getFilesReferencingSymbol(name)) {
                ctx.throwIfCancelled("while finding references");
                if (targetDoc && filePath == targetDoc->getURI().getPath())
                    continue;

                URI fileUri = URI::fromFile(filePath.string());
                auto fileDoc = getDocument(fileUri);
                if (fileDoc) {
                    getIncludeContext(fileUri);
                    if (hasExplicitContextError(fileUri))
                        continue;
                    finder(fileDoc->getSyntaxTree()->getMetadata(), fileUri);
                }
                else {
                    ctx.error("No doc found for {}", filePath.string());
                }
            }
        };

        // Add refs in declaration file, and remove declaration if requested
        if (targetDoc) {
            auto targetAnalysis = targetDoc->getAnalysis(ctx);
            targetAnalysis->addLocalReferences(references, targetSymbol->location, targetName);
            if (auto declaration = physicalMemberDeclaration(*targetSymbol, sm)) {
                declarations.push_back(*declaration);
                if (includeDeclaration && std::ranges::none_of(references, [&](const auto& r) {
                        return r.uri == declaration->uri && r.range == declaration->range;
                    }))
                    references.insert(references.begin() + existingReferenceCount, *declaration);
            }
            if (!includeDeclaration) {
                auto targetLspLoc = lsp::Location{
                    .uri = URI::fromFile(sm.getFullPath(targetLoc.buffer())),
                    .range = toRange(SourceRange(targetLoc, targetLoc + targetSymbol->name.size()),
                                     sm),
                };
                declarations.push_back(targetLspLoc);
                references.erase(std::remove_if(references.begin(), references.end(),
                                                [&](const lsp::Location& loc) {
                                                    return loc.uri == targetLspLoc.uri &&
                                                           loc.range == targetLspLoc.range;
                                                }),
                                 references.end());
            }
        }

        // Add global references
        switch (targetSymbol->kind) {
            case ast::SymbolKind::Instance: {
                processReferencingFiles(
                    targetSymbol->as<ast::InstanceSymbol>().getDefinition().name,
                    findModuleReferencesInDocument);
            } break;
            case ast::SymbolKind::InstanceBody: {
                processReferencingFiles(
                    targetSymbol->as<ast::InstanceBodySymbol>().getDefinition().name,
                    findModuleReferencesInDocument);
            } break;
            case ast::SymbolKind::Definition: {
                const auto& definition = targetSymbol->as<ast::DefinitionSymbol>();
                if (definition.definitionKind == ast::DefinitionKind::Interface) {
                    processReferencingFiles(definition.name, findInterfaceReferencesInDocument);
                }
                else {
                    processReferencingFiles(definition.name, findModuleReferencesInDocument);
                }
            } break;
            case ast::SymbolKind::Package: {
                processReferencingFiles(targetName, findPkgReferencesInDocument);
            } break;
            default: {
                if (targetSymbol->getParentScope() == nullptr ||
                    targetSymbol->getParentScope()->asSymbol().getParentScope() == nullptr) {
                    ctx.error(
                        "Target symbol {}: {} has no parent scope, missed kind case for global "
                        "symbol",
                        targetName, toString(targetSymbol->kind));
                    break;
                }
                auto& parentSymbol = targetSymbol->getParentScope()->asSymbol();
                auto& gParentSymbol = parentSymbol.getParentScope()->asSymbol();
                if (gParentSymbol.kind == ast::SymbolKind::CompilationUnit) {
                    // Package and module members
                    addMemberReferences(references, parentSymbol, *targetSymbol, false, ctx);
                }
                else if (gParentSymbol.kind == ast::SymbolKind::Package &&
                         ast::Type::isKind(parentSymbol.kind)) {
                    // submembers in the case of structs and enums
                    addMemberReferences(references, gParentSymbol, *targetSymbol, true, ctx);
                }
                else if (targetLoc.buffer() != target.analysisBuffer) {
                    target.analysis->addLocalReferences(references, targetSymbol->location,
                                                        targetName);
                }
            }
        }

        if (hasDuplicates) {
            const auto targetLocation = toOriginalLocation(SourceRange(targetLoc, targetLoc), sm);
            auto begin = references.begin() + existingReferenceCount;
            references.erase(
                std::remove_if(
                    begin, references.end(),
                    [&](const auto& reference) {
                        ctx.throwIfCancelled("while matching reference declarations");
                        auto info = getDefinitionInfoAt(reference.uri, reference.range.start);
                        if (!info)
                            return true;
                        auto definitions = info->getDefinitionLspLinks();
                        return std::ranges::none_of(definitions, [&](const auto& definition) {
                            return definition.targetUri == targetLocation.uri &&
                                   definition.targetSelectionRange.start ==
                                       targetLocation.range.start;
                        });
                    }),
                references.end());
        }

        const auto newReferenceEnd = references.size();
        for (size_t i = existingReferenceCount; i < newReferenceEnd; i++) {
            ctx.throwIfCancelled("while resolving reference components");
            auto referenceDoc = getDocument(references[i].uri);
            if (!referenceDoc || !referenceDoc->hasAnalysis())
                continue;

            auto referenceAnalysis = referenceDoc->getAnalysis(ctx);
            auto referenceLoc = toSourceLocation(referenceAnalysis->getBuffer(),
                                                 references[i].range.start, sm);
            if (!referenceLoc)
                continue;

            auto* referenceToken = referenceAnalysis->syntaxes.getWordTokenAt(*referenceLoc);
            if (!referenceToken)
                continue;

            // Tied declarations and multi-symbol tokens join the same reference component.
            addTargetsAt(referenceDoc, referenceAnalysis, referenceToken);
        }

        auto output = references.begin() + existingReferenceCount;
        for (auto it = output; it != references.end(); ++it) {
            auto duplicate = std::any_of(references.begin(),
                                         references.begin() + existingReferenceCount,
                                         [&](const lsp::Location& existing) {
                                             return existing.uri == it->uri &&
                                                    existing.range == it->range;
                                         });
            if (!duplicate) {
                if (output != it)
                    *output = std::move(*it);
                ++output;
            }
        }
        references.erase(output, references.end());
    }

    // Cross-document discovery can append declarations after the local pass.
    // Apply declaration filtering and deduplication to the completed graph.
    auto key = [](const auto& location) {
        return std::tuple(location.uri.str(), location.range.start.line,
                          location.range.start.character, location.range.end.line,
                          location.range.end.character);
    };
    using LocationKey = decltype(key(references.front()));
    std::set<LocationKey> declarationKeys, referenceKeys;
    if (!includeDeclaration)
        for (const auto& declaration : declarations)
            declarationKeys.insert(key(declaration));
    std::vector<lsp::Location> uniqueReferences;
    for (auto& reference : references) {
        auto location = key(reference);
        if (!declarationKeys.contains(location) && referenceKeys.insert(std::move(location)).second)
            uniqueReferences.push_back(std::move(reference));
    }
    return uniqueReferences.empty() ? std::nullopt
                                    : std::make_optional(std::move(uniqueReferences));
}

std::optional<lsp::WorkspaceEdit> ServerDriver::getDocRename(const URI& uri,
                                                             const lsp::Position& position,
                                                             std::string_view newName) {
    // Reuse getDocReferences to find all locations (including declaration)
    auto references = getDocReferences(uri, position, /* includeDeclaration */ true);
    if (!references || references->empty()) {
        return std::nullopt;
    }

    // Group edits by URI
    std::unordered_map<std::string, std::vector<lsp::TextEdit>> changes;

    for (const auto& loc : *references) {
        lsp::TextEdit edit{
            .range = loc.range,
            .newText = std::string(newName),
        };
        changes[loc.uri.str()].push_back(edit);
    }

    return lsp::WorkspaceEdit{.changes = changes};
}

void ServerDriver::publishInactiveRegions(SlangDoc& doc, const lsp::RequestContext& ctx) {
    if (!client.capabilities.inactiveRegionsSupported)
        return;

    ctx.throwIfCancelled("before collecting inactive regions");
    auto regions = doc.getInactiveRegions(ctx);
    ctx.info("Collected {} inactive regions for {}", regions.size(), doc.getWsRelativePath());
    ctx.throwIfCancelled("before publishing inactive regions");

    client.onTextDocumentInactiveRegions(lsp::InactiveRegionsParams{
        .uri = doc.getURI(),
        .regions = std::move(regions),
    });
}

} // namespace server
