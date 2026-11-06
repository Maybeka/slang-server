// SPDX-License-Identifier: MIT
#include "diagnostics/DiagnosticIdentity.h"
#include "document/BodyDiagnosticCache.h"

namespace server {
using namespace slang;
using diagnostic_identity::rawDiagnosticHash;
using diagnostic_identity::scalarDiagnosticEqual;

// The immutable rendering index borrows only from the cache's retained snapshot.
// This unit performs no binding, dataflow or transport publication.
void BodyDiagnosticCache::prepareFormatted(const lsp::RequestContext& ctx) {
    if (formattedPrepared || !formattedPublication)
        return;
    ctx.checkpoint("before immutable diagnostic rendering index");
    const auto& raw = formattedPublication->diagnostics;
    const auto path = driver.sm.getFullPath(driver.getDocument(uri)->getBuffer()).generic_string();
    auto targetUri = [&](const URI& value) { return value.getPath() == path; };
    auto touchesTarget = [&](auto&& self, const Diagnostic& value) -> bool {
        auto location = [&](SourceLocation loc) {
            return loc &&
                   (nativeTargetBuffers.contains(driver.sm.getFullyExpandedLoc(loc).buffer()) ||
                    nativeTargetBuffers.contains(driver.sm.getFullyOriginalLoc(loc).buffer()));
        };
        if (location(value.location))
            return true;
        for (auto range : value.ranges)
            if (location(range.start()) || location(range.end()))
                return true;
        for (const auto& note : value.notes)
            if (self(self, note))
                return true;
        return false;
    };
    // Build privately: cancellation may occur while an existing local cache is
    // being promoted to a full effect replay. No partial index can escape.
    std::vector<CachedFormatted> nextEntries = formattedEntries;
    std::unordered_multimap<size_t, size_t> nextByHash;
    std::unordered_map<const Diagnostic*, size_t> nextRetained = formattedRetained;
    size_t total = 0;
    for (const auto& values : raw)
        total += values.size();
    nextEntries.reserve(total);
    nextByHash.reserve(total);
    nextRetained.reserve(total);
    for (size_t phase = 0; phase < raw.size(); ++phase) {
        const auto& rendered = formattedPublication->rendered[phase];
        if (rendered.size() != raw[phase].size())
            continue;
        std::vector<size_t> entries(raw[phase].size(), size_t(-1));
        for (size_t i = 0; i < raw[phase].size(); ++i) {
            ctx.checkpoint("prepare immutable diagnostic rendering");
            if (!rendered[i].key || rendered[i].values.size() != 1 ||
                touchesTarget(touchesTarget, raw[phase][i]))
                continue;
            const auto& value = rendered[i].values.front();
            bool sensitive = targetUri(value.uri);
            if (value.value.relatedInformation)
                for (const auto& note : *value.value.relatedInformation)
                    sensitive |= targetUri(note.location.uri);
            if (sensitive)
                continue;
            entries[i] = nextEntries.size();
            nextByHash.emplace(rawDiagnosticHash(raw[phase][i]), entries[i]);
            nextEntries.push_back({&raw[phase][i], &rendered[i]});
        }
        for (const auto& [key, indices] : retainedNativeIndices[phase]) {
            const auto& diagnostics = retained[phase].at(key);
            for (size_t i = 0; i < indices.size(); ++i)
                if (entries[indices[i]] != size_t(-1))
                    nextRetained.emplace(&diagnostics[i], entries[indices[i]]);
        }
    }
    ctx.checkpoint("commit immutable diagnostic rendering index");
    formattedEntries = std::move(nextEntries);
    formattedByHash = std::move(nextByHash);
    formattedRetained = std::move(nextRetained);
    formattedPrepared = true;
    retainedNativeIndices = {};
    nativeTargetBuffers = {};
}

const BodyDiagnosticCache::CachedFormatted* BodyDiagnosticCache::findFormatted(
    const Diagnostic& value) const {
    if (auto found = formattedRetained.find(&value); found != formattedRetained.end())
        return &formattedEntries[found->second];
    auto [begin, end] = formattedByHash.equal_range(rawDiagnosticHash(value));
    for (auto it = begin; it != end; ++it) {
        const auto& entry = formattedEntries[it->second];
        if (scalarDiagnosticEqual(value, *entry.raw))
            return &entry;
    }
    return nullptr;
}

} // namespace server
