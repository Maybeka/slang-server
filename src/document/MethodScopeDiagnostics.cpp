// SPDX-License-Identifier: MIT
#include "document/BodyDiagnosticCache.h"
#include "document/MethodScopeOverlay.h"
#include "util/BodyMacroProof.h"
#include "util/Converters.h"
#include "util/MethodPreprocessingProof.h"
#include "util/QueryText.h"
#include "util/UvmPhaseTrace.h"
#include <algorithm>
#include <tuple>

#include "slang/syntax/AllSyntax.h"
namespace server {
using namespace slang;
void MethodScopeOverlay::captureLocalBaseline(const Config& config,
                                              const lsp::RequestContext& ctx) {
    if (!optimizedCertification(config) || !config.experimentalMethodBodyDiagnostics.value())
        return;
    localDiagnostics.capture(version, [&] {
        if (bodyMacroProof)
            bodyMacroProof->validateRaw(
                std::string_view(text).substr(functionBegin, functionEnd - functionBegin));
        else
            validateBodyPreprocessing(text.substr(functionBegin, functionEnd - functionBegin));
        UvmPhaseTrace phases;
        auto baseline = BodyDiagnosticCache::capture(*driver, uri, functionBegin, functionEnd, ctx,
                                                     true);
        phases.mark("body-diagnostic-capture");
        return baseline;
    });
}

void MethodScopeOverlay::prepareLocalDiagnostics(const Config& config,
                                                 const lsp::RequestContext& ctx,
                                                 bool completeDeferred) {
    if (!generalBody || !config.experimentalMethodBodyDiagnostics.value())
        return;
    localDiagnostics.prepare(version, completeDeferred, [&](auto& baseline) {
        UvmPhaseTrace phases;
        if (baseline && baseline->pendingDesign)
            baseline = BodyDiagnosticCache::completeCapture(*driver, baseline, ctx);
        const auto baselineEnd = baseline ? baseline->end : functionEnd;
        auto baselineText = baseline ? std::string_view(baseline->text) : std::string_view(text);
        if (bodyMacroProof)
            bodyMacroProof->validateRaw(
                baselineText.substr(functionBegin, baselineEnd - functionBegin));
        else
            validateBodyPreprocessing(
                std::string(baselineText.substr(functionBegin, baselineEnd - functionBegin)));
        std::string padded(baselineText.substr(0, functionBegin));
        for (auto& c : padded)
            if (c != '\n' && c != '\r')
                c = ' ';
        padded += baselineText.substr(functionBegin, baselineEnd - functionBegin);
        QueryText query(driver->sm,
                        driver->sm.getFullPath(driver->getDocument(uri)->getBuffer()).string(),
                        padded);
        const auto& source = query.buffer;
        for (const auto& line :
             driver->sm.getLineDirectives(BufferID(originalBuffer, "body lines"))) {
            auto at = driver->sm.getSourceLocation(BufferID(originalBuffer, "body lines"),
                                                   line.lineInFile, 1);
            if (at && at->offset() < functionBegin)
                driver->sm.addLineDirective(SourceLocation(source.id, at->offset()),
                                            line.lineOfDirective, line.name, line.level, true);
        }
        auto directives = driver->sm.getDiagnosticDirectives(
            BufferID(originalBuffer, "body pragmas"));
        auto inherited = std::vector<SourceManager::DiagnosticDirectiveInfo>(directives.begin(),
                                                                             directives.end());
        for (const auto& directive : inherited)
            if (directive.offset < functionBegin)
                driver->sm.addDiagnosticDirective(SourceLocation(source.id, directive.offset),
                                                  directive.name, directive.severity, true);
        auto tree = query.parse(treeOptions->bag(), bodyMacros);
        for (const auto& reference : tree->getPreprocessorMetadata().macroRefs)
            validateMacroPreprocessing(reference, bodyMacros, ctx);
        auto cache = std::make_unique<BodyDiagnosticCache>(
            *driver, uri, functionBegin, baselineEnd, tree,
            bodyLineSensitiveDeclaration || !bodyLineDependents.empty(), ctx, baseline,
            optimizedCertification(config) ? miniTree : nullptr);
        phases.mark("body-diagnostic-certification");
        return cache;
    });
}

uint64_t MethodScopeOverlay::localDiagnosticUpdates() const {
    return localDiagnostics.publications();
}
std::pair<uint64_t, uint64_t> MethodScopeOverlay::localDiagnosticProcedureCounts() const {
    return localDiagnostics.cache() ? std::pair{localDiagnostics.cache()->reusedProcedures,
                                                localDiagnostics.cache()->recomputedProcedures}
                                    : std::pair<uint64_t, uint64_t>{};
}
std::pair<uint64_t, uint64_t> MethodScopeOverlay::localDiagnosticRenderingCounts() const {
    return localDiagnostics.cache() ? std::pair{localDiagnostics.cache()->reusedRenderings,
                                                localDiagnostics.cache()->freshRenderings}
                                    : std::pair<uint64_t, uint64_t>{};
}
std::string MethodScopeOverlay::localDiagnosticDecline() const {
    return localDiagnostics.decline();
}

std::pair<size_t, size_t> MethodScopeOverlay::localDiagnosticRenderingCacheUsage() const {
    return localDiagnostics.cache() ? localDiagnostics.cache()->targetRenderingCacheUsage()
                                    : std::pair<size_t, size_t>{};
}

bool MethodScopeOverlay::publishLocalDiagnostics(const Config& config,
                                                 const lsp::RequestContext& ctx,
                                                 bool deferTransport) {
    if (!generalBody || !accepted || !config.experimentalMethodBodyDiagnostics.value() ||
        (!localDiagnostics.cache() && !optimizedCertification(config)) ||
        !validForWork(config, ctx))
        return false;
    try {
        ensureGeneralBinding(config, ctx);
        if (optimizedCertification(config) && !localDiagnostics.cache())
            prepareLocalDiagnostics(config, ctx, true);
        if (!localDiagnostics.cache())
            return false;
        localDiagnostics.cache()->publish(
            miniTree, functionEnd, ctx,
            [&] { require(valid(config, ctx), "inputs changed during local diagnostics"); },
            deferTransport);
        localDiagnostics.publicationSucceeded();
        if (driver->client.capabilities.inactiveRegionsSupported) {
            BodySourceProjection projection(
                driver->sm, driver->getDocument(uri)->getBuffer(), miniAnalysis->getBuffer(),
                originalBodyEnd, bodyEnd, bodyBufferLimit,
                miniAnalysis->getShallowCompilation()->getBodyQueryRoots(), bodyQueryScopeDisplay);
            std::vector<lsp::Range> regions;
            for (auto region : bodyOwner->syntaxes.disabledRegions) {
                auto start = driver->sm.getFullyExpandedLoc(region.start());
                if (bodyBegin <= start.offset() && start.offset() < originalBodyEnd)
                    continue;
                regions.push_back(toRange(region, driver->sm));
            }
            for (auto region : miniAnalysis->syntaxes.disabledRegions)
                regions.push_back(toRange(region, driver->sm));
            std::sort(regions.begin(), regions.end(), [](const auto& a, const auto& b) {
                return std::tie(a.start.line, a.start.character) <
                       std::tie(b.start.line, b.start.character);
            });
            driver->publishCertifiedLocalInactiveRegions(uri, std::move(regions), deferTransport);
        }
        return true;
    }
    catch (const lsp::RequestCancelled&) {
        throw;
    }
    catch (const std::exception& error) {
        localDiagnostics.publicationFailed(error.what());
        return false;
    }
}

std::optional<ServerDriver::DiagnosticSnapshot> MethodScopeOverlay::diagnostics(
    const Config& config, const lsp::RequestContext& ctx) const {
    if (bodyQueries || !accepted || !valid(config, ctx))
        return {};
    auto out = frozen;
    out.owner = driver->m_snapshotOwner;
    out.revision = driver->m_snapshotRevision;
    out.indexRevision = driver->getDependencyIndexRevision();
    out.diagnosticGeneration = driver->m_diagnosticGeneration;
    auto input = std::make_shared<diagnostic_snapshot::Input>(*frozen.input);
    size_t changed = 0;
    auto document = driver->getDocument(uri);
    auto path = driver->sm.getFullPath(document->getBuffer()).string();
    for (auto& file : input->sources.files)
        if (file.fullPath == path) {
            file.bytes = std::string(document->getText());
            ++changed;
        }
    require(changed == 1, "diagnostic source identity");
    for (auto& id : out.buffers)
        if (driver->sm.getFullPath(id).string() == path)
            id = document->getBuffer();
    for (auto& target : out.metadata.targets)
        target.version = driver->docs.at(target.uri)->getVersion();
    out.input = std::move(input);
    ctx.checkpoint("after method diagnostic freeze");
    return out;
}

} // namespace server
