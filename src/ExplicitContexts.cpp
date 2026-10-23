// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
#include "ServerDriver.h"
#include "document/SlangDoc.h"
#include "util/Converters.h"
#include "util/DisposableParseRequest.h"
#include "util/SlangExtensions.h"
#include <algorithm>
#include <set>

#include "slang/driver/SourceLoader.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"

namespace server {
namespace {
std::filesystem::path normalized(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : result;
}
bool containsPath(const SourceManager& sm, const syntax::SyntaxTree& tree,
                  const std::filesystem::path& path) {
    for (auto id : tree.getSourceBufferIds())
        if (sm.getFullPath(id) == path)
            return true;
    return false;
}
std::vector<BufferID> packageIncludes(const SourceManager& sm, const syntax::SyntaxTree& tree,
                                      const std::filesystem::path& path) {
    std::vector<BufferID> result;
    for (const auto& include : tree.getIncludeDirectives()) {
        if (!include.buffer.id.valid() || sm.getFullPath(include.buffer.id) != path)
            continue;
        bool inPackage = false;
        for (const auto& [node, _] : tree.getMetadata().nodeMeta) {
            if (node->kind != syntax::SyntaxKind::PackageDeclaration)
                continue;
            auto range = sm.getFullyExpandedRange(node->sourceRange());
            for (auto location = sm.getIncludedFrom(include.buffer.id); location.valid();
                 location = sm.getIncludedFrom(location.buffer()))
                inPackage |= range.contains(location);
        }
        if (inPackage && std::find(result.begin(), result.end(), include.buffer.id) == result.end())
            result.push_back(include.buffer.id);
    }
    return result;
}
bool declaresPackageFrom(const SourceManager& sm, const syntax::SyntaxTree& tree,
                         std::string_view name, const std::filesystem::path& path) {
    for (const auto& [node, _] : tree.getMetadata().nodeMeta) {
        if (node->kind != syntax::SyntaxKind::PackageDeclaration ||
            node->header->name.valueText() != name)
            continue;
        for (auto location = sm.getFullyExpandedLoc(node->header->name.location());
             location.valid(); location = sm.getIncludedFrom(location.buffer()))
            if (sm.getFullPath(location.buffer()) == path)
                return true;
    }
    return false;
}
} // namespace

std::optional<BufferID> ServerDriver::findPackageInclude(
    const syntax::SyntaxTree& tree, const std::filesystem::path& target) const {
    auto matches = packageIncludes(sm, tree, target);
    return matches.empty() ? std::nullopt : std::optional{matches.front()};
}

void ServerDriver::installConfiguredUnits(std::vector<std::shared_ptr<syntax::SyntaxTree>> trees) {
    configureFrozenSources(trees);
    auto previous = std::move(m_configuredUnits);
    m_configuredUnits.clear();
    m_currentConfiguredTrees = trees;
    const bool inherited = driver.options.singleUnit.value_or(false) && trees.size() > 1;
    for (auto& tree : trees) {
        auto unit = SlangDoc::fromTree(*this, tree, inherited);
        m_configuredUnits.push_back(unit);
        for (const auto& openUri : m_openDocs) {
            auto open = docs.find(openUri);
            if (open != docs.end() &&
                sm.getFullPath(open->second->getBuffer()) == sm.getFullPath(unit->getBuffer()))
                open->second->adoptNativeCompilationUnit(*unit);
        }
        if (!m_openDocs.contains(unit->getURI()))
            docs[unit->getURI()] = unit;
        for (auto buffer : tree->getSourceBufferIds()) {
            auto path = sm.getFullPath(buffer);
            if (!path.empty())
                m_buildSourceUris.emplace(URI::fromFile(path));
        }
    }
    m_configuredUnitRevision = m_snapshotRevision;
}

void ServerDriver::refreshConfiguredUnits(const lsp::RequestContext& ctx) {
    if (m_configuredUnits.empty() || m_configuredUnitRevision == m_snapshotRevision)
        return;
    bool stale = false;
    bool nativeReplay = false;
    for (const auto& unit : m_configuredUnits) {
        stale |= !hasValidBuffers(sm, unit->getCachedSyntaxTree());
        nativeReplay |= unit->requiresNativeReplay();
    }
    if (stale && nativeReplay) {
        ctx.checkpoint("before configured syntax parsing");
        DisposableParseRequest request(ctx, isCooperativeAnalysisEnabled());
        auto previousCheckpoint = std::move(driver.options.parseCheckpoint);
        auto restore = slang::ScopeGuard(
            [&] { driver.options.parseCheckpoint = std::move(previousCheckpoint); });
        driver.options.parseCheckpoint = request.callback();
        const bool parsed = driver.parseAllSources();
        ctx.checkpoint("after configured syntax parsing");
        request.finish(); // Installing complete units is an atomic state transition.
        m_nativeInputsFailed = m_configurationInputsFailed || !parsed;
        installConfiguredUnits(driver.syntaxTrees);
    }
    else {
        std::vector<std::shared_ptr<syntax::SyntaxTree>> trees;
        for (const auto& unit : m_configuredUnits)
            trees.push_back(unit->getSyntaxTree(ctx));
        ctx.checkpoint("after configured syntax parsing");
        m_currentConfiguredTrees = std::move(trees);
        m_configuredUnitRevision = m_snapshotRevision;
    }
}

SourceContextHints ServerDriver::getSourceHints(BufferID buffer) {
    auto text = sm.getSourceText(buffer);
    auto& cached = m_sourceHints[sm.getFullPath(buffer).string()];
    if (cached.data != text.data() || cached.size != text.size()) {
        cached.data = text.data();
        cached.size = text.size();
        cached.hints = {};
        if (text.find("slang-server:") != std::string_view::npos)
            cached.hints = parseSourceContextHints({.data = text, .id = buffer}, sm);
    }
    auto result = cached.hints;
    result.bufferId = buffer.getId();
    if (result.owner)
        result.owner->range.bufferId = buffer.getId();
    for (auto& comment : result.sourceComments)
        comment.range.bufferId = buffer.getId();
    for (auto& issue : result.issues)
        issue.range.bufferId = buffer.getId();
    return result;
}

const SourceContextImportHints& ServerDriver::getImportHints(
    const std::shared_ptr<syntax::SyntaxTree>& tree) {
    if (m_boundHintsRevision != m_snapshotRevision) {
        std::erase_if(m_boundSourceHints, [](const auto& item) { return item.first.expired(); });
        m_boundHintsRevision = m_snapshotRevision;
    }
    auto [it, inserted] = m_boundSourceHints.try_emplace(tree);
    if (inserted) {
        for (auto id : tree->getSourceBufferIds()) {
            auto hints = getSourceHints(id);
            if (hints.sourceComments.empty())
                continue;
            auto bound = bindSourceImportHints(*tree, hints);
            it->second.imports.insert(it->second.imports.end(), bound.imports.begin(),
                                      bound.imports.end());
            it->second.issues.insert(it->second.issues.end(), bound.issues.begin(),
                                     bound.issues.end());
        }
    }
    return it->second;
}

void ServerDriver::addExplicitContextError(const URI& target, std::string message,
                                           std::optional<SourceContextHintRange> hint) {
    m_explicitContexts.insert(target);
    lsp::Diagnostic diagnostic{.range = {{0, 0}, {0, 0}},
                               .severity = lsp::DiagnosticSeverity::Error,
                               .code = std::string("context-unavailable"),
                               .source = "slang-server",
                               .message =
                                   std::move(message) +
                                   "; semantic diagnostics are not available for this context."};
    std::vector<lsp::DiagnosticRelatedInformation> related;
    if (hint) {
        auto location = toLocation(SourceLocation{BufferID{hint->bufferId, ""}, hint->begin}, sm);
        if (location.uri == target)
            diagnostic.range = location.range;
        related.push_back({.location = location, .message = "Source context directive"});
    }
    else {
        if (m_config.compilationFilelist.value()) {
            auto path = std::filesystem::path(*m_config.compilationFilelist.value());
            if (path.is_relative())
                path = std::filesystem::path(m_workspacePathPrefix) / path;
            related.push_back(
                {.location = {.uri = URI::fromFile(normalized(path)), .range = {{0, 0}, {0, 0}}},
                 .message = "Configured compilation filelist"});
        }
        for (const auto& source : m_config.flagsByFile.value())
            related.push_back(
                {.location = {.uri = URI::fromFile(source.filePath), .range = {{0, 0}, {0, 0}}},
                 .message = "Configured compilation input"});
    }
    if (!related.empty())
        diagnostic.relatedInformation = std::move(related);
    auto& errors = m_explicitContextErrors[target];
    if (std::none_of(errors.begin(), errors.end(), [&](const auto& old) {
            return old.message == diagnostic.message && old.range == diagnostic.range;
        }))
        errors.push_back(std::move(diagnostic));
}
bool ServerDriver::hasExplicitContextError(const URI& uri) const {
    auto found = m_explicitContextErrors.find(uri);
    return found != m_explicitContextErrors.end() && !found->second.empty();
}
void ServerDriver::requireExplicitContext(const URI& uri) const {
    if (hasExplicitContextError(uri))
        throw lsp::RequestFailed(m_explicitContextErrors.at(uri).front().message);
}
void ServerDriver::issueExplicitContextDiagnostics(const URI& uri) {
    auto found = m_explicitContextErrors.find(uri);
    if (found == m_explicitContextErrors.end())
        return;
    std::vector<lsp::Diagnostic> result;
    if (auto old = diagClient->renderedDiagnostics().find(uri);
        old != diagClient->renderedDiagnostics().end())
        result = old->second;
    result.insert(result.end(), found->second.begin(), found->second.end());
    diagClient->replaceRendered(uri, std::move(result));
}
bool ServerDriver::isPackageOnlyContext(const URI& uri) const {
    return !comp && m_packageOnlyContexts.contains(uri);
}
void ServerDriver::updatePackageContextKind(const URI& uri, const syntax::SyntaxTree& tree) {
    if (comp || !m_packageOnlyContexts.contains(uri))
        return;
    auto document = docs.find(uri);
    if (document == docs.end())
        return;
    const auto& path = sm.getFullPath(document->second->getBuffer());
    bool directDesign = false;
    for (const auto& [node, _] : tree.getMetadata().nodeMeta) {
        auto location = sm.getFullyExpandedLoc(node->header->name.location());
        if (node->kind != syntax::SyntaxKind::PackageDeclaration && location.valid() &&
            sm.getFullPath(location.buffer()) == path)
            directDesign = true;
    }
    if (directDesign)
        m_packageOnlyContexts.erase(uri);
    else
        m_packageOnlyContexts.insert(uri);
}
uint64_t ServerDriver::getExplicitContextRevision(const URI& uri) const {
    return m_explicitContexts.contains(uri) || hasConfiguredCompilationInputs() ? m_snapshotRevision
                                                                                : 0;
}

std::vector<std::filesystem::path> ServerDriver::getContextWatchPaths() const {
    if (!isFrozenSourcesEnabled() && (comp || !hasConfiguredCompilationInputs()) &&
        !hasSourceContextHints() && !m_config.uvm.value())
        return {};
    std::set<std::filesystem::path> paths;
    paths.insert(m_uvmWatchPaths.begin(), m_uvmWatchPaths.end());
    for (const auto& [path, _] : driver.getCommandFileMetadata())
        paths.insert(path);
    for (const auto& path : driver.sourceLoader.getFailedLiteralPaths())
        paths.insert(path);
    if (m_config.compilationFilelist.value()) {
        auto path = std::filesystem::path(*m_config.compilationFilelist.value());
        if (path.is_relative())
            path = std::filesystem::path(m_workspacePathPrefix) / path;
        paths.insert(normalized(path));
    }
    auto collect = [&](const std::shared_ptr<syntax::SyntaxTree>& tree) {
        if (tree)
            for (auto buffer : tree->getSourceBufferIds())
                if (!sm.getFullPath(buffer).empty())
                    paths.insert(sm.getFullPath(buffer));
    };
    if (hasConfiguredCompilationInputs() || hasSourceContextHints()) {
        for (const auto& tree : m_currentConfiguredTrees)
            collect(tree);
        for (const auto& [uri, doc] : docs) {
            paths.insert(sm.getFullPath(doc->getBuffer()));
            collect(doc->getCachedSyntaxTree());
            collect(doc->getCachedAnalysisTree());
        }
    }
    // Failed literal targets must remain watched so recreation can repair context.
    // Only current physical owner metadata and active import bindings participate.
    for (const auto& [source, cached] : m_sourceHints) {
        const auto& hints = cached.hints;
        if (hints.owner && sm.isLatestData(BufferID{hints.bufferId, ""}))
            paths.insert(
                normalized(std::filesystem::path(source).parent_path() / hints.owner->path));
    }
    for (const auto& [weakTree, hints] : m_boundSourceHints) {
        auto tree = weakTree.lock();
        if (!tree || !hasValidBuffers(sm, tree))
            continue;
        for (const auto& hint : hints.imports)
            paths.insert(normalized(
                sm.getFullPath(BufferID{hint.range.bufferId, ""}).parent_path() / hint.path));
    }
    paths.erase(std::filesystem::path{});
    return {paths.begin(), paths.end()};
}

std::optional<ServerDriver::PackageIncludeContext> ServerDriver::getExplicitIncludeContext(
    const URI& uri, bool& handled) {
    handled = false;
    if (comp) {
        refreshConfiguredUnits();
        // A configured package inclusion is authoritative. Cached standalone documents can
        // otherwise select a different, nearer package with the same name after an edit.
        if (m_designUsesConfiguredUnits) {
            auto document = docs.find(uri);
            if (document == docs.end())
                return {};
            auto target = sm.getFullPath(document->second->getBuffer());
            std::vector<PackageIncludeContext> matches;
            for (const auto& tree : m_currentConfiguredTrees)
                for (auto buffer : packageIncludes(sm, *tree, target))
                    matches.push_back({tree, buffer});
            if (!matches.empty()) {
                handled = true;
                // Preserve full-design source views' first-include ordering for
                // multiply included library headers.
                m_packageOnlyContexts.insert(uri);
                return matches.front();
            }
        }
        return {};
    }
    auto document = docs.find(uri);
    if (document == docs.end())
        return {};
    if (m_explicitResolutionRevision[uri] != m_snapshotRevision) {
        m_explicitContextErrors.erase(uri);
        m_packageOnlyContexts.erase(uri);
        m_explicitResolutionRevision[uri] = m_snapshotRevision;
    }
    auto targetPath = sm.getFullPath(document->second->getBuffer());
    auto hints = getSourceHints(document->second->getBuffer());
    for (const auto& issue : hints.issues)
        addExplicitContextError(uri, issue.message, issue.range);
    if (hints.invalidOwner) {
        handled = true;
        return {};
    }
    refreshConfiguredUnits();
    if (m_nativeInputsFailed && (hasConfiguredCompilationInputs() || hints.owner)) {
        handled = true;
        addExplicitContextError(uri, "Configured compilation inputs could not be loaded");
        return {};
    }
    std::vector<std::filesystem::path> ownerChain;
    std::optional<SourceContextPathHint> hint = hints.owner;
    SourceContextHintRange firstRange{};
    if (hint)
        firstRange = hint->range;
    std::set<std::filesystem::path> seen{targetPath};
    std::shared_ptr<SlangDoc> hintedRoot;
    while (hint) {
        handled = true;
        m_explicitContexts.insert(uri);
        auto base = sm.getFullPath(BufferID{hint->range.bufferId, ""}).parent_path();
        auto path = normalized(base / hint->path);
        if (ownerChain.size() == 8 || !seen.insert(path).second) {
            addExplicitContextError(uri, "Owner directive cycle or depth limit (8)", hint->range);
            return {};
        }
        ownerChain.push_back(path);
        try {
            hintedRoot = getDocument(URI::fromFile(path));
        }
        catch (const std::exception&) {
            addExplicitContextError(uri, "Owner source is unavailable: " + path.string(),
                                    hint->range);
            return {};
        }
        auto next = getSourceHints(hintedRoot->getBuffer());
        for (const auto& issue : next.issues)
            addExplicitContextError(uri, issue.message, issue.range);
        if (next.invalidOwner)
            return {};
        hint = next.owner;
    }
    std::vector<std::shared_ptr<syntax::SyntaxTree>> candidates = m_currentConfiguredTrees;
    if (candidates.empty() && hintedRoot && !hasConfiguredCompilationInputs())
        candidates.push_back(hintedRoot->getSyntaxTree());
    if (candidates.empty()) {
        if (hasConfiguredCompilationInputs()) {
            handled = true;
            addExplicitContextError(uri, "Configured compilation contains no source context");
        }
        return {};
    }
    std::vector<PackageIncludeContext> matches;
    for (const auto& tree : candidates) {
        if (!ownerChain.empty() && !containsPath(sm, *tree, ownerChain.back()))
            continue;
        for (auto buffer : packageIncludes(sm, *tree, targetPath)) {
            bool chainMatches = true;
            auto location = sm.getIncludedFrom(buffer);
            for (const auto& ownerPath : ownerChain) {
                while (location.valid() && sm.getFullPath(location.buffer()) != ownerPath)
                    location = sm.getIncludedFrom(location.buffer());
                if (!location.valid()) {
                    chainMatches = false;
                    break;
                }
                location = sm.getIncludedFrom(location.buffer());
            }
            if (chainMatches)
                matches.push_back({tree, buffer});
        }
        if (!ownerChain.empty())
            continue;
        for (auto buffer : tree->getSourceBufferIds())
            if (sm.getFullPath(buffer) == targetPath &&
                std::none_of(matches.begin(), matches.end(), [&](const auto& match) {
                    return match.tree == tree && match.buffer == buffer;
                }))
                matches.push_back({tree, buffer});
    }
    if (matches.size() > 1) {
        handled = true;
        addExplicitContextError(uri, "Multiple configured source contexts include this file",
                                hints.owner ? std::optional{firstRange} : std::nullopt);
        return {};
    }
    if (matches.empty()) {
        if (hints.owner) {
            addExplicitContextError(
                uri, "Owner directive does not identify a configured, actual package inclusion",
                firstRange);
            handled = true;
        }
        else if (hasConfiguredCompilationInputs() && !m_uvmLibraryOnly) {
            handled = true;
            addExplicitContextError(
                uri, "Document has no source context in the configured compilation inputs");
        }
        return {};
    }
    handled = true;
    m_explicitContexts.insert(uri);
    auto selected = matches.front();
    bool directDesign = false;
    for (const auto& [node, _] : selected.tree->getMetadata().nodeMeta) {
        auto location = sm.getFullyExpandedLoc(node->header->name.location());
        if (node->kind != syntax::SyntaxKind::PackageDeclaration && location.valid() &&
            sm.getFullPath(location.buffer()) == targetPath)
            directDesign = true;
    }
    if (!directDesign)
        m_packageOnlyContexts.insert(uri);
    return selected;
}

std::vector<std::shared_ptr<syntax::SyntaxTree>> ServerDriver::getExplicitDependencies(
    const std::shared_ptr<syntax::SyntaxTree>&, const URI& target, bool& handled) {
    handled = !comp && hasConfiguredCompilationInputs();
    if (!handled)
        return {};
    refreshConfiguredUnits();
    std::map<std::string, size_t> packageCounts;
    std::map<std::string, std::filesystem::path> packageHints;
    for (const auto& unit : m_currentConfiguredTrees)
        for (const auto& [node, _] : unit->getMetadata().nodeMeta)
            if (node->kind == syntax::SyntaxKind::PackageDeclaration)
                ++packageCounts[std::string(node->header->name.valueText())];
    for (const auto& tree : m_currentConfiguredTrees) {
        tree->getMetadata().visitReferencedSymbols([&](std::string_view name) {
            if (packageCounts[std::string(name)] > 1)
                addExplicitContextError(target,
                                        "Configured compilation declares multiple packages named " +
                                            std::string(name));
        });
        const auto& imports = getImportHints(tree);
        for (const auto& issue : imports.issues)
            addExplicitContextError(target, issue.message, issue.range);
        for (const auto& item : imports.imports) {
            auto path = normalized(sm.getFullPath(BufferID{item.range.bufferId, ""}).parent_path() /
                                   item.path);
            auto [entry, inserted] = packageHints.emplace(item.packageName, path);
            if (!inserted && entry->second != path)
                addExplicitContextError(
                    target, "Conflicting package source directives for " + item.packageName,
                    item.range);
            size_t providers = 0;
            for (const auto& candidate : m_currentConfiguredTrees)
                providers += declaresPackageFrom(sm, *candidate, item.packageName, path);
            if (providers != 1)
                addExplicitContextError(target,
                                        "Package source directive is outside or ambiguous in the "
                                        "configured compilation: " +
                                            item.packageName + " at " + path.string(),
                                        item.range);
        }
    }
    return m_currentConfiguredTrees;
}

std::shared_ptr<syntax::SyntaxTree> ServerDriver::getHintedPackageTree(
    const std::shared_ptr<syntax::SyntaxTree>& tree, std::string_view name, const URI& target,
    bool& handled) {
    handled = false;
    std::optional<std::filesystem::path> selected;
    std::optional<SourceContextHintRange> range;
    const auto& imports = getImportHints(tree);
    for (const auto& issue : imports.issues)
        addExplicitContextError(target, issue.message, issue.range);
    for (const auto& item : imports.imports) {
        if (item.packageName != name)
            continue;
        handled = true;
        m_explicitContexts.insert(target);
        if (m_nativeInputsFailed) {
            addExplicitContextError(target, "Configured options could not be loaded", item.range);
            return {};
        }
        m_packageOnlyContexts.insert(target);
        auto path = normalized(sm.getFullPath(BufferID{item.range.bufferId, ""}).parent_path() /
                               item.path);
        if (selected && *selected != path) {
            addExplicitContextError(
                target, "Conflicting package source directives for " + std::string(name),
                item.range);
            return {};
        }
        selected = std::move(path);
        range = item.range;
    }
    if (!selected)
        return {};
    if (declaresPackageFrom(sm, *tree, name, *selected))
        return tree;
    try {
        auto result = getDocument(URI::fromFile(*selected))->getSyntaxTree();
        if (declaresPackageFrom(sm, *result, name, *selected))
            return result;
    }
    catch (const std::exception&) {
    }
    addExplicitContextError(
        target, "Package source does not declare " + std::string(name) + ": " + selected->string(),
        range);
    return {};
}
} // namespace server
