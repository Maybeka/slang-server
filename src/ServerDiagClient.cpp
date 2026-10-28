//------------------------------------------------------------------------------
// ServerDiagClient.cpp
// Server diagnostic client implementation
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "ServerDiagClient.h"

#include "fmt/format.h"
#include "lsp/LspTypes.h"
#include "lsp/URI.h"
#include "util/Converters.h"
#include "util/Logging.h"
#include <optional>
#include <string_view>
#include <sys/types.h>
#include <unordered_map>

#include "slang/diagnostics/AnalysisDiags.h"
#include "slang/diagnostics/CompilationDiags.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/DriverDiags.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"
namespace server {
namespace {
template<typename T, typename Equal>
bool sameOptional(const std::optional<T>& a, const std::optional<T>& b, Equal equal) {
    return a.has_value() == b.has_value() && (!a || equal(*a, *b));
}

// Compare the complete LSP payload before reflection and JSON encoding. Keep
// related notes and optional metadata: matching just message/range loses edits.
bool sameDiagnostic(const lsp::Diagnostic& a, const lsp::Diagnostic& b) {
    return a.range == b.range && a.severity == b.severity && a.source == b.source &&
           a.message == b.message && a.tags == b.tags &&
           sameOptional(a.code, b.code,
                        [](const auto& x, const auto& y) {
                            return x.visit([&](const auto& value) {
                                auto other = rfl::get_if<std::decay_t<decltype(value)>>(&y);
                                return other && value == *other;
                            });
                        }) &&
           sameOptional(a.codeDescription, b.codeDescription,
                        [](const auto& x, const auto& y) { return x.href == y.href; }) &&
           sameOptional(a.relatedInformation, b.relatedInformation,
                        [](const auto& x, const auto& y) {
                            return std::ranges::equal(x, y, [](const auto& l, const auto& r) {
                                return l.location.uri == r.location.uri &&
                                       l.location.range == r.location.range &&
                                       l.message == r.message;
                            });
                        }) &&
           sameOptional(a.data, b.data, [](const auto& x, const auto& y) {
               return rfl::json::write(x) == rfl::json::write(y);
           });
}

size_t diagnosticHash(const lsp::Diagnostic& diagnostic) {
    size_t result = 0;
    auto add = [&]<typename T>(const T& value) {
        result ^= std::hash<T>{}(value) + size_t(0x9e3779b9) + (result << 6) + (result >> 2);
    };
    auto range = [&](const lsp::Range& value) {
        add(value.start.line);
        add(value.start.character);
        add(value.end.line);
        add(value.end.character);
    };
    range(diagnostic.range);
    add(diagnostic.message);
    add(diagnostic.severity);
    add(diagnostic.source);
    add(bool(diagnostic.code));
    if (diagnostic.code) {
        add(diagnostic.code->index());
        diagnostic.code->visit([&](const auto& value) { add(value); });
    }
    add(bool(diagnostic.codeDescription));
    if (diagnostic.codeDescription)
        add(diagnostic.codeDescription->href);
    add(bool(diagnostic.tags));
    if (diagnostic.tags)
        for (auto tag : *diagnostic.tags)
            add(tag);
    add(bool(diagnostic.relatedInformation));
    if (diagnostic.relatedInformation)
        for (const auto& note : *diagnostic.relatedInformation) {
            add(note.location.uri);
            range(note.location.range);
            add(note.message);
        }
    // A hash only selects candidates; every match is checked with the complete
    // payload comparator, including arbitrary data. Collisions cannot hide edits.
    return result;
}
} // namespace

ServerDiagClient::Update::Update(ServerDiagClient& client) : client(client) {
    SLANG_ASSERT(!client.m_update);
    client.m_update = this;
}

void ServerDiagClient::Update::remember(const URI& uri) {
    if (previous.contains(uri))
        return;
    auto found = client.m_diagnostics.find(uri);
    previous.emplace(uri,
                     Previous{found == client.m_diagnostics.end() ? std::nullopt
                                                                  : std::optional{found->second},
                              client.getVersion(uri), client.m_dirtyUris.contains(uri)});
}

void ServerDiagClient::Update::commit() {
    committed = true;
    client.m_update = nullptr;
}

ServerDiagClient::Update::~Update() {
    if (committed)
        return;
    for (auto& [uri, state] : previous) {
        client.setVersion(uri, state.version);
        if (state.diagnostics)
            client.m_diagnostics.insert_or_assign(uri, std::move(*state.diagnostics));
        else
            client.m_diagnostics.erase(uri);
        if (state.dirty)
            client.m_dirtyUris.emplace(uri);
        else
            client.m_dirtyUris.erase(uri);
    }
    client.m_update = nullptr;
}

lsp::DiagnosticSeverity convertSeverity(slang::DiagnosticSeverity severity) {
    switch (severity) {
        case slang::DiagnosticSeverity::Ignored:
            return lsp::DiagnosticSeverity::Hint;
        case slang::DiagnosticSeverity::Note:
            return lsp::DiagnosticSeverity::Information;
        case slang::DiagnosticSeverity::Warning:
            return lsp::DiagnosticSeverity::Warning;
        case slang::DiagnosticSeverity::Error:
            return lsp::DiagnosticSeverity::Error;
        case slang::DiagnosticSeverity::Fatal:
            return lsp::DiagnosticSeverity::Error;
    }
    return lsp::DiagnosticSeverity::Error;
}

bool isUnusedCode(DiagCode code) {
    if (code.getSubsystem() != slang::DiagSubsystem::Analysis)
        return false;

    // From AnalysisDiags.h - need to keep up to date
    switch (code.getCode()) {
        case slang::diag::UnusedArgument.getCode():
        case slang::diag::UnusedAssertionDecl.getCode():
        // Not quite unused, i.e. not safe for removal
        // case slang::diag::UnusedButSetNet.getCode():
        // case slang::diag::UnusedButSetPort.getCode():
        // case slang::diag::UnusedButSetVariable.getCode():
        case slang::diag::UnusedDefinition.getCode():
        case slang::diag::UnusedGenvar.getCode():
        case slang::diag::UnusedImplicitNet.getCode():
        case slang::diag::UnusedImport.getCode():
        case slang::diag::UnusedNet.getCode():
        case slang::diag::UnusedParameter.getCode():
        case slang::diag::UnusedPort.getCode():
        case slang::diag::UnusedTypeParameter.getCode():
        case slang::diag::UnusedTypedef.getCode():
        case slang::diag::UnusedVariable.getCode():
        case slang::diag::UnusedWildcardImport.getCode():
            return true;
    }
    return false;
}

void ServerDiagClient::report(const slang::ReportedDiagnostic& diag) {

    // Ignore this- happens all the time in explore mode, for example when looking at include files
    if (diag.originalDiagnostic.code == slang::diag::NoTopModules) {
        return;
    }

    // Notes are processed as relatedInformation from the parent diagnostic
    if (diag.severity == slang::DiagnosticSeverity::Note) {
        return;
    }

    if (diag.originalDiagnostic.code == slang::diag::CommandLineWarning) {
        m_client.showWarning(std::string(diag.formattedMessage));
        return;
    }

    if (diag.originalDiagnostic.code == slang::diag::CommandLineError) {
        m_client.showError(std::string(diag.formattedMessage));
        return;
    }

    // TODO: show include stack?
    // if (diag.shouldShowIncludeStack) {
    //     SmallVector<SourceLocation> includeStack;
    //     getIncludeStack(diag.location.buffer(), includeStack);

    //     // Show the stack in reverse.
    //     for (int i = int(includeStack.size()) - 1; i >= 0; i--) {
    //         SourceLocation loc = includeStack[size_t(i)];
    //         buffer->format("in file included from {}:{}:\n", getFileName(loc),
    //                        sourceManager->getLineNumber(loc));
    //     }
    // }

    // TODO: show hierarchy? Maybe also set the hierarchy when clicking on it?
    // Print out the hierarchy where the diagnostic occurred, if we know it.
    // auto& od = diag.originalDiagnostic;
    // auto& symbolPathCB = engine->getSymbolPathCB();
    // if (od.symbol && symbolPathCB && (od.coalesceCount)) {
    //     if (!od.coalesceCount || od.coalesceCount == 1u)
    //         buffer->append("  in instance: "sv);
    //     else
    //         buffer->format("  in {} instances, e.g. ", *od.coalesceCount);

    //     buffer->append(fmt::emphasis::bold, symbolPathCB(*od.symbol));
    //     buffer->append("\n"sv);
    // }

    // Slang diags have a location and range:
    //       /location
    // ~~~~~^~~~~~
    // range range (in ranges)
    // But LSP diags just have a range, so we need to combine them.

    auto getLocation = [&](SourceLocation loc, std::span<const SourceRange> ranges,
                           std::string_view message) -> std::optional<lsp::Location> {
        bool hasLocation = loc.buffer() != SourceLocation::NoLocation.buffer();
        if (ranges.empty()) {
            if (hasLocation) {
                return toLocation(loc, m_sourceManager);
            }
            else {
                ERROR("Diagnostic has no ranges and no location: {}", message);
            }
        }
        else {
            // collapse ranges into one, if they're all in the same buffer
            SourceRange totalRange = ranges[0];
            for (auto& range : ranges.subspan(1)) {
                if (range.start().buffer() != totalRange.start().buffer()) {
                    ERROR("Diagnostic has ranges in multiple buffers: {}", message);
                }
                else {
                    totalRange.start() = std::min(totalRange.start(), range.start());
                    totalRange.end() = std::max(totalRange.end(), range.end());
                }
            }
            if (hasLocation) {
                if (loc.buffer() != totalRange.start().buffer()) {
                    ERROR("Diagnostic location and ranges are in different buffers: {}", message);
                }
                else {
                    totalRange.start() = std::min(totalRange.start(), loc);
                    totalRange.end() = std::max(totalRange.end(), loc);
                }
            }
            return toLocation(totalRange, m_sourceManager);
        }
        return std::nullopt;
    };

    // Code similar to TextDiagnosticClient::report
    SmallVector<SourceRange> mappedRanges;
    engine->mapSourceRanges(diag.location, diag.ranges, mappedRanges);

    auto mainLoc = getLocation(diag.location, mappedRanges, diag.formattedMessage);
    if (!mainLoc) {
        return;
    }

    std::vector<lsp::DiagnosticRelatedInformation> related;
    for (auto it = diag.expansionLocs.rbegin(); it != diag.expansionLocs.rend(); it++) {
        SourceLocation loc = *it;
        std::string name(sourceManager->getMacroName(loc));
        if (name.empty())
            name = "expanded from here";
        else
            name = fmt::format("expanded from macro '{}'", name);

        SmallVector<SourceRange> macroRanges;
        engine->mapSourceRanges(loc, diag.ranges, macroRanges);

        auto relatedLoc = getLocation(sourceManager->getFullyOriginalLoc(loc), macroRanges, name);
        if (relatedLoc) {
            related.emplace_back(lsp::DiagnosticRelatedInformation{
                .location = *relatedLoc, .message = std::string{diag.formattedMessage}});
        }
    }
    // end of text diag related code

    const auto physicalUri = mainLoc->uri;
    mainLoc->uri = resolveUri(mainLoc->uri);
    auto uri = mainLoc->uri;
    if (m_update)
        m_update->remember(uri);
    // An owner can append parse diagnostics to an included URI whose previous
    // semantic diagnostics are still retained. Such an incidental batch has no
    // single editor version; only a complete document update stamps a version.
    m_versions.erase(uri);
    m_dirtyUris.emplace(uri);

    // Add notes from the original diagnostic as relatedInformation
    for (const auto& note : diag.originalDiagnostic.notes) {
        if (note.location == SourceLocation::NoLocation) {
            if (note.code.showNoteWithNoLocation()) {
                related.emplace_back(lsp::DiagnosticRelatedInformation{
                    .location = {physicalUri, mainLoc->range},
                    .message = engine->formatMessage(note),
                });
            }
            else {
                WARN("Diagnostic note has no location: {}", engine->formatMessage(note));
            }
        }
        else {
            related.emplace_back(lsp::DiagnosticRelatedInformation{
                .location = toLocation(note.location, m_sourceManager),
                .message = engine->formatMessage(note),
            });
        }
    }

    std::vector<URI> physicalRelated;
    if (m_physicalReportObserver) {
        physicalRelated.reserve(related.size());
        for (const auto& information : related)
            physicalRelated.push_back(information.location.uri);
    }
    for (auto& information : related)
        information.location.uri = resolveUri(information.location.uri);
    m_diagnostics[uri].push_back(lsp::Diagnostic{
        .range = mainLoc->range,
        .severity = convertSeverity(diag.severity),
        .message = std::string{diag.formattedMessage},
        .relatedInformation = related.empty() ? std::nullopt : std::optional{related},
    });

    // Add diag code link if any
    std::string_view optionName = engine->getOptionName(diag.originalDiagnostic.code);
    if (!optionName.empty()) {
        m_diagnostics[uri].back().code = {std::string(optionName)};
        m_diagnostics[uri].back().codeDescription = lsp::CodeDescription{
            .href = URI::fromWeb("sv-lang.com/warning-ref.html#" + std::string(optionName))};

        if (isUnusedCode(diag.originalDiagnostic.code)) {
            m_diagnostics[uri].back().tags = {lsp::DiagnosticTag::Unnecessary};
        }
    }
    if (m_physicalReportObserver) {
        auto physical = m_diagnostics[uri].back();
        if (physical.relatedInformation)
            for (size_t i = 0; i < physicalRelated.size(); ++i)
                (*physical.relatedInformation)[i].location.uri = physicalRelated[i];
        m_physicalReportObserver(physicalUri, std::move(physical));
    }
    if (m_reportObserver)
        m_reportObserver(physicalUri);
}

void ServerDiagClient::appendPhysicalRendered(const URI& physical, lsp::Diagnostic value) {
    const auto uri = resolveUri(physical);
    if (value.relatedInformation)
        for (auto& information : *value.relatedInformation)
            information.location.uri = resolveUri(information.location.uri);
    if (m_update)
        m_update->remember(uri);
    m_versions.erase(uri);
    m_dirtyUris.emplace(uri);
    m_diagnostics[uri].push_back(std::move(value));
    if (m_reportObserver)
        m_reportObserver(physical);
}

bool ServerDiagClient::replaceRenderedIfChanged(URI uri, std::vector<lsp::Diagnostic> diagnostics) {
    uri = resolveUri(uri);
    auto current = m_diagnostics.find(uri);
    if (current != m_diagnostics.end() &&
        std::ranges::equal(current->second, diagnostics, sameDiagnostic))
        return false;
    if (m_update)
        m_update->remember(uri);
    m_diagnostics.insert_or_assign(uri, std::move(diagnostics));
    m_dirtyUris.emplace(uri);
    return true;
}

bool ServerDiagClient::replaceRenderedViewsIfChanged(
    URI uri, std::span<const lsp::Diagnostic* const> diagnostics) {
    uri = resolveUri(uri);
    auto current = m_diagnostics.find(uri);
    if (current != m_diagnostics.end() &&
        std::ranges::equal(current->second, diagnostics,
                           [](const auto& a, const auto* b) { return sameDiagnostic(a, *b); }))
        return false;
    std::vector<lsp::Diagnostic> owned;
    owned.reserve(diagnostics.size());
    for (const auto* diagnostic : diagnostics)
        owned.push_back(*diagnostic);
    if (m_update)
        m_update->remember(uri);
    m_diagnostics.insert_or_assign(uri, std::move(owned));
    m_dirtyUris.emplace(uri);
    return true;
}

void ServerDiagClient::clear(URI uri) {
    uri = resolveUri(uri);
    if (m_update)
        m_update->remember(uri);
    auto it = m_diagnostics.find(uri);
    if (it != m_diagnostics.end()) {
        m_diagnostics.erase(it);
    }
    m_dirtyUris.emplace(uri);
}

void ServerDiagClient::adoptDocumentUri(const URI& physical, const URI& opened) {
    if (physical == opened)
        return;
    if (m_update)
        m_update->remember(physical);
    m_diagnostics.erase(physical);
    m_versions.erase(physical);
    m_dirtyUris.erase(physical);
    // Keep this retirement key literal: resolving it would clear only the new
    // alias and leave the client's previously published physical error behind.
    if (m_publications->nonempty.contains(physical))
        m_dirtyUris.emplace(physical);
}

void ServerDiagClient::publish(const URI& uri, const std::vector<lsp::Diagnostic>& diagnostics) {
    auto version = getVersion(uri);
    if (m_deltaPublishing) {
        auto previous = m_publications->published.find(uri);
        if (previous != m_publications->published.end() && previous->second.version == version &&
            previous->second.diagnostics.size() == diagnostics.size()) {
            const auto& published = previous->second;
            std::vector<bool> matched(diagnostics.size(), false);
            const bool same = std::ranges::all_of(diagnostics, [&](const auto& diagnostic) {
                auto [begin, end] = published.index.equal_range(diagnosticHash(diagnostic));
                for (auto it = begin; it != end; ++it) {
                    if (!matched[it->second] &&
                        sameDiagnostic(published.diagnostics[it->second], diagnostic)) {
                        matched[it->second] = true;
                        return true;
                    }
                }
                return false;
            });
            if (same)
                return;
        }
    }
    lsp::PublishDiagnosticsParams params{
        .uri = uri, .version = version, .diagnostics = diagnostics};
    m_client.onDocPublishDiagnostics(params);
    if (m_deltaPublishing) {
        // The virtual callback consumed this owning snapshot synchronously.
        // Retain that exact sent payload instead of copying the current bucket
        // a second time for the delta ledger.
        Published published{version, std::move(params.diagnostics), {}};
        published.index.reserve(published.diagnostics.size());
        for (size_t i = 0; i < published.diagnostics.size(); ++i)
            published.index.emplace(diagnosticHash(published.diagnostics[i]), i);
        m_publications->published.insert_or_assign(uri, std::move(published));
    }
    if (diagnostics.empty())
        m_publications->nonempty.erase(uri);
    else
        m_publications->nonempty.emplace(uri);
}

void ServerDiagClient::clearAndPush() {
    if (m_deltaPublishing) {
        // A replacement may not have recomputed every closed source yet. Final
        // retirement must still clear all nonempty results known by the editor.
        const std::vector<URI> published(m_publications->nonempty.begin(),
                                         m_publications->nonempty.end());
        for (const auto& uri : published)
            publish(uri, {});
    }
    for (const auto& [uri, diags] : m_diagnostics) {
        publish(uri, {});
    }
    for (auto& uri : m_dirtyUris) {
        publish(uri, {});
    }
    m_diagnostics.clear();
    m_dirtyUris.clear();
}

void ServerDiagClient::clear() {
    // Add all URIs to dirty set to notify client on next publish
    for (const auto& [uri, _] : m_diagnostics) {
        m_dirtyUris.emplace(uri);
    }
    m_diagnostics.clear();
}

void ServerDiagClient::pushClearedDiags() {
    // Clients can map a retired physical URI and its opened alias to one buffer.
    // Retire old keys before publishing current diagnostics for that buffer.
    std::vector<URI> cleared;
    for (const auto& uri : m_dirtyUris) {
        if (!m_diagnostics.contains(uri))
            cleared.push_back(uri);
    }
    for (const auto& uri : cleared) {
        publish(uri, {});
        m_dirtyUris.erase(uri);
    }
}

void ServerDiagClient::pushDiags(const URI& priorityUri) {
    pushClearedDiags();
    auto it = m_diagnostics.find(priorityUri);
    if (it != m_diagnostics.end()) {
        publish(priorityUri, it->second);
    }
    else if (m_dirtyUris.contains(priorityUri)) {
        publish(priorityUri, {});
    }
    m_dirtyUris.erase(priorityUri);
    pushDiags();
}

void ServerDiagClient::pushDiags() {
    pushClearedDiags();
    for (auto& uri : m_dirtyUris) {
        auto it = m_diagnostics.find(uri);
        if (it != m_diagnostics.end()) {
            publish(uri, it->second);
        }
        else {
            publish(uri, {});
        }
    }
    m_dirtyUris.clear();
}

} // namespace server
