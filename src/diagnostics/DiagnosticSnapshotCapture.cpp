#include "ServerDriver.h"
#include "diagnostics/DiagnosticSnapshotCodec.h"
#include "document/SlangDoc.h"
#include "util/Logging.h"
#include "util/SlangExtensions.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "slang/syntax/AllSyntax.h"
#include "slang/util/OS.h"

namespace server {
std::optional<ServerDriver::DiagnosticSnapshot> ServerDriver::captureDiagnosticSnapshot(
    const lsp::RequestContext& ctx) {
    // Trusted semantics are owned by the parent process. Keep diagnostics on
    // that same owner instead of re-elaborating UVM in a one-shot child or
    // transferring live compiler pointers through the value-only protocol.
    if (m_trustedScopes || m_trustedUvm || driver.precompiledUvm)
        return std::nullopt;
    if (comp || m_pendingIncludedDiagnostics.empty())
        return std::nullopt;
    auto cwdPhase = sm.beginFastCwdPhase();
    // Discover edited owners before resolving any target, independent of URI
    // sort order. This includes open owners that newly adopt an earlier-sorted
    // header, and closed owners retained for pending include discovery.
    for (const auto& [uri, _] : m_pendingIncludedDiagnostics) {
        ctx.throwIfCancelled("before discovering snapshot owners");
        auto found = docs.find(uri);
        if (found != docs.end()) {
            bool explicitContext = false;
            getExplicitIncludeContext(uri, explicitContext, ctx);
            if (!explicitContext)
                found->second->getSyntaxTree(ctx);
        }
    }
    const auto indexAtStart = m_indexer.getRevision();
    DiagnosticSnapshot snapshot;
    snapshot.owner = m_snapshotOwner;
    snapshot.revision = m_snapshotRevision;
    snapshot.diagnosticGeneration = m_diagnosticGeneration;
    snapshot.metadata.parentPid = slang::OS::getpid();
    snapshot.metadata.utf8Positions = client.capabilities.utf8Positions;
    auto input = std::make_shared<diagnostic_snapshot::Input>();
    input->options = diagnostic_snapshot::captureOptions(options);
    if (input->options.preprocessor)
        input->options.preprocessor->captureUvmPrefix &= isUvmPrefixCacheEnabled();
    input->settings = diagEngine.captureSettings();
    std::vector<std::shared_ptr<syntax::SyntaxTree>> trees;
    std::unordered_map<const syntax::SyntaxTree*, uint32_t> treeIndices;
    auto addTree = [&](const std::shared_ptr<syntax::SyntaxTree>& tree) -> uint32_t {
        for (const auto& unit : m_configuredUnits)
            if (unit->getCachedSyntaxTree() == tree && unit->requiresNativeReplay())
                throw std::runtime_error(
                    "isolated diagnostics do not yet encode native inherited macro units");
        if (&tree->sourceManager() != &sm || tree->isLibraryUnit || tree->getSourceLibrary())
            throw std::runtime_error("isolated diagnostics require ordinary local syntax trees");
        auto [it, inserted] = treeIndices.try_emplace(tree.get(), uint32_t(trees.size()));
        if (inserted)
            trees.push_back(tree);
        return it->second;
    };

    // Capture the complete open exploration set. A newly parsed owner can adopt
    // a header during this pass; each target independently resolves its current
    // owner/dependency graph. No workspace rescan occurs in the child.
    std::vector<URI> open(m_openDocs.begin(), m_openDocs.end());
    std::sort(open.begin(), open.end(),
              [](const URI& a, const URI& b) { return a.getPath() < b.getPath(); });
    std::unordered_set<std::string> targetPaths;
    for (const auto& uri : open) {
        ctx.throwIfCancelled("before capturing diagnostic target");
        auto found = docs.find(uri);
        if (found == docs.end())
            continue;
        auto graph = found->second->getAnalysisInputs(ctx);
        if (!targetPaths.insert(sm.getFullPath(graph.buffer).string()).second)
            throw std::runtime_error("multiple editor URIs name the same snapshot source");
        if (isPackageContextDeferred(uri))
            throw std::runtime_error("isolated diagnostics require a resolved package context");
        // The first prototype isolates package/class exploration. Full design and
        // direct module/interface/program targets retain the sequential path.
        for (const auto& [node, _] : graph.tree->getMetadata().nodeMeta) {
            if (!isPackageOnlyContext(uri) &&
                (node->kind == syntax::SyntaxKind::ModuleDeclaration ||
                 node->kind == syntax::SyntaxKind::InterfaceDeclaration ||
                 node->kind == syntax::SyntaxKind::ProgramDeclaration))
                throw std::runtime_error(
                    "isolated diagnostics do not support direct design targets");
        }
        DiagnosticTargetInput target{uri, found->second->getVersion(), addTree(graph.tree), 0, {}};
        target.packageOnly = isPackageOnlyContext(uri);
        auto ids = graph.tree->getSourceBufferIds();
        auto pos = std::find(ids.begin(), ids.end(), graph.buffer);
        if (pos == ids.end())
            throw std::runtime_error("target buffer absent from snapshot tree");
        target.bufferOrdinal = uint32_t(pos - ids.begin());
        for (const auto& tree : graph.trees)
            target.trees.push_back(addTree(tree));
        snapshot.metadata.targets.push_back(std::move(target));
    }
    if (snapshot.metadata.targets.empty()) {
        cwdPhase.finish();
        return std::nullopt;
    }
    if (m_explorePreincludeTree)
        snapshot.metadata.preincludeTree = addTree(m_explorePreincludeTree);
    snapshot.metadata.preincludeUris.assign(m_explorePreincludeUris.begin(),
                                            m_explorePreincludeUris.end());
    snapshot.metadata.clearUris.assign(m_publishedPreincludeUris.begin(),
                                       m_publishedPreincludeUris.end());
    for (const auto& uri : m_explorePreincludeUris)
        snapshot.metadata.clearUris.push_back(uri);

    // Metadata indices temporarily address the capture buffer vector. Convert
    // them to frozen file identities only after the single consistent capture.
    for (const auto& tree : trees) {
        ctx.throwIfCancelled("before capturing diagnostic tree");
        DiagnosticTreeInput spec;
        for (auto id : tree->getSourceBufferIds()) {
            auto index = uint32_t(snapshot.buffers.size());
            snapshot.buffers.push_back(id);
            spec.sources.push_back(index);
            if (!sm.getIncludedFrom(id).valid())
                spec.roots.push_back({index, sm.getSortKey(id)});
        }
        std::reverse(spec.roots.begin(), spec.roots.end());
        if (isFrozenSourcesEnabled() && m_frozenWitnessUsable && m_frozenSources) {
            const auto config = m_config.frozenSources.value().value_or(
                Config::FrozenSourcesConfig{});
            std::vector<std::filesystem::path> paths;
            for (const auto& root : spec.roots)
                paths.push_back(sm.getFullPath(snapshot.buffers.at(root.file)));
            auto plan = std::ranges::find_if(m_frozenPlans.units, [&](const auto& candidate) {
                return candidate.roots == paths;
            });
            if (config.maxMiB.value() && config.maxMiB.value() <= 512 &&
                plan != m_frozenPlans.units.end() && plan->frozenRootCount) {
                spec.frozenPrefix =
                    DiagnosticFrozenPrefix{uint32_t(plan->frozenRootCount), plan->packages,
                                           uint64_t(config.maxMiB.value()) * 1024 * 1024,
                                           m_frozenDeclinedUnits.contains(paths), plan->modules};
            }
        }
        for (auto id : tree->getPredefineBufferIds()) {
            spec.predefines.push_back(uint32_t(snapshot.buffers.size()));
            snapshot.buffers.push_back(id);
        }
        input->treeOptions.push_back(diagnostic_snapshot::captureOptions(tree->options()));
        if (input->treeOptions.back().preprocessor)
            input->treeOptions.back().preprocessor->captureUvmPrefix &= isUvmPrefixCacheEnabled();
        snapshot.metadata.trees.push_back(std::move(spec));
    }
    input->sources = sm.captureSnapshot(snapshot.buffers);
    for (auto& tree : snapshot.metadata.trees) {
        for (auto& root : tree.roots)
            root.file = input->sources.graphFileIds.at(root.file);
        for (auto& file : tree.sources)
            file = input->sources.graphFileIds.at(file);
        for (auto& file : tree.predefines)
            file = input->sources.graphFileIds.at(file);
    }
    snapshot.indexRevision = m_indexer.getRevision();
    if (snapshot.indexRevision != indexAtStart)
        throw std::runtime_error("dependency index changed during snapshot capture");
    snapshot.input = std::move(input);
    ctx.throwIfCancelled("after capturing diagnostic snapshot");
    cwdPhase.finish();
    return snapshot;
}

bool ServerDriver::isDiagnosticSnapshotCurrent(const DiagnosticSnapshot& snapshot) const {
    if (snapshot.owner != m_snapshotOwner || snapshot.revision != m_snapshotRevision ||
        snapshot.indexRevision != m_indexer.getRevision() || comp ||
        snapshot.diagnosticGeneration != m_diagnosticGeneration)
        return false;
    for (auto id : snapshot.buffers)
        if (!sm.isLatestData(id))
            return false;
    for (const auto& target : snapshot.metadata.targets) {
        auto found = docs.find(target.uri);
        if (!m_openDocs.contains(target.uri) || found == docs.end() ||
            found->second->getVersion() != target.version)
            return false;
    }
    return snapshot.metadata.targets.size() == m_openDocs.size();
}

void ServerDriver::commitDiagnosticSnapshot(const DiagnosticSnapshot& snapshot,
                                            DiagnosticJobOutput output,
                                            const lsp::RequestContext& ctx) {
    // This is a short state-thread transaction. Once a result is taken from
    // the helper it must either commit atomically or be rejected; an incoming
    // notification cannot interrupt it halfway through publication.

    if (!isDiagnosticSnapshotCurrent(snapshot))
        throw std::runtime_error("obsolete diagnostic snapshot cannot commit");
    if (output.schema != 1)
        throw std::runtime_error("invalid rendered diagnostic schema");
    // Validate complete target coverage before replacing even one document.
    for (const auto& target : snapshot.metadata.targets) {
        auto found = std::find_if(output.documents.begin(), output.documents.end(),
                                  [&](const auto& value) { return value.uri == target.uri; });
        if (found == output.documents.end() || found->version != target.version)
            throw std::runtime_error("incomplete rendered diagnostic coverage");
    }
    auto cwdPhase = sm.beginFastCwdPhase();
    auto update = diagClient->beginUpdate();
    for (auto& document : output.documents) {
        diagClient->replaceRendered(document.uri, std::move(document.diagnostics));
        diagClient->setVersion(document.uri, document.version);
    }
    cwdPhase.finish();
    update.commit();
    m_publishedPreincludeRevision = m_explorePreincludeRevision;
    m_publishedPreincludeUris = m_explorePreincludeUris;
    std::optional<URI> priority;
    uint64_t newest = 0;
    for (const auto& [uri, generation] : m_pendingIncludedDiagnostics) {
        if (m_openDocs.contains(uri) && (!priority || generation > newest)) {
            priority = uri;
            newest = generation;
        }
    }
    m_pendingIncludedDiagnostics.clear();
    if (priority)
        diagClient->pushDiags(*priority);
    else
        diagClient->pushDiags();
    if (client.capabilities.inactiveRegionsSupported) {
        for (auto& inactive : output.inactiveRegions)
            client.onTextDocumentInactiveRegions({inactive.uri, std::move(inactive.regions)});
    }
    ctx.info("Committed isolated diagnostic snapshot generation {}", snapshot.diagnosticGeneration);
}
} // namespace server
