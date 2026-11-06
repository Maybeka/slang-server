// SPDX-License-Identifier: MIT
#include "SlangServer.h"
#include "util/SourceHistoryBudget.h"
#include "util/SourceHistoryPolicy.h"
#include "util/UvmPhaseTrace.h"
#include <cstdio>
#include <cstdlib>

#include "slang/util/OS.h"
#include "slang/util/ScopeGuard.h"

namespace server {
// Access remains serialized by serverStateMutex. Private candidates do not
// become the current driver until loadConfig publishes a complete replacement.
void SlangServer::admitRetirementInputs(const lsp::RequestContext& ctx) {
    if (m_contextRetirement.awaitsInputs()) {
        ctx.checkpoint("before deferred editor admission");
        maybeRetireParsedContext(&ctx);
        if (m_contextRetirement.awaitsInputs())
            throw lsp::RequestFailed(
                "Pending editor text could not be admitted; retry after fixing inputs");
        ctx.checkpoint("after deferred editor admission");
    }
}

bool SlangServer::retirementIsOnlyBackgroundWork() const {
    if (m_contextRetirement.awaitsInputs())
        return !m_methodInputReloadPending && !m_configReloadPending;
    return m_contextRetirement.isPending() && m_driver && !m_methodInputReloadPending &&
           !m_configReloadPending && !m_activeDiagnosticSnapshot &&
           !m_diagnosticWake->ready.load() && !m_driver->hasPendingDiagnostics() &&
           !m_driver->hasCertifiedLocalDiagnostics() && !m_driver->hasDeferredSaveIndexes();
}

void SlangServer::resetSourceHistoryBudget(size_t initialBytes) {
    if (m_driver && !m_driver->deferredEditorBytes())
        m_contextRetirement.admitInputs();
    m_contextEdits = 0;
    m_contextRetirement.clearRequest();
    m_contextRetry.reset();
    if (!sourceHistoryEnabled(m_config)) {
        m_contextEpoch = 0;
        m_contextBaseBytes = m_contextBudget = m_contextHardBudget = 0;
        return;
    }
    // Tickets emitted before the first retirement must already identify their
    // namespace, including cooperative Native mode without any frozen library.
    m_driver->completions.requireCompletionEpoch();
    m_contextEpoch = m_driver->completions.getCompletionEpoch();
    m_contextBaseBytes = initialBytes;
    auto limits = SourceHistoryBudget::limits(initialBytes, m_config.sourceHistoryMaxMiB.value());
    m_contextBudget = limits.soft;
    m_contextHardBudget = limits.hard;
}

void SlangServer::maybeRetireParsedContext(const lsp::RequestContext* retirementContext) {
    if (!sourceHistoryEnabled(m_config) || std::getenv("SLANG_UVM_GENERATION_DISABLE") ||
        !m_driver) {
        m_contextRetirement.clearRequest();
        m_contextRetry.reset();
        return;
    }
    auto epoch = m_driver->completions.getCompletionEpoch();
    auto table = m_driver->sm.getSourceLocationTableBytes();
    auto bytes = table + m_driver->sm.getMemoryUsage();
    if (m_contextEpoch != epoch) {
        m_contextEpoch = epoch;
        resetSourceHistoryBudget(bytes);
        m_contextEdits = 0;
        m_contextRetirement.clearRequest();
        m_contextRetry.reset();
    }
    uint64_t forced = 0;
    if (auto* value = std::getenv("SLANG_UVM_GENERATION_EDIT_LIMIT"))
        forced = std::strtoull(value, nullptr, 10);
    if (!m_contextRetirement.awaitsInputs() && bytes <= m_contextBudget &&
        (!forced || m_contextEdits < forced)) {
        ++m_contextEdits;
        m_contextRetirement.clearRequest();
        m_contextRetry.reset();
        return;
    }
    if (!retirementContext && m_config.experimentalCooperativeAnalysis.value()) {
        // At the hard mark, stop adding editor generations to this namespace.
        // Subsequent edits retain only their latest owned text until a private,
        // cancellable replacement admits it. The source allowance is unchanged.
        if (bytes >= m_contextHardBudget && !m_contextRetirement.awaitsInputs()) {
            m_contextRetirement.deferInputs();
            // No semantic consumer may use this driver before admission. Drop
            // unborrowed state now, rather than overlap it with each cancelled
            // replacement. Consumers that still own a model keep it alive.
            discardMethodScope();
            if (m_diagnosticProcess)
                m_diagnosticProcess->cancel();
            m_activeDiagnosticSnapshot.reset();
            m_driver->prepareForContextRetirement(true);
        }
        m_contextRetirement.request();
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
    if (!retirementContext) {
        discardMethodScope();
        if (m_diagnosticProcess)
            m_diagnosticProcess->cancel();
        m_activeDiagnosticSnapshot.reset();
        m_driver->prepareForContextRetirement(true);
    }
    m_driver->preserveTrustedUvmForRetirement();
    auto clearPreservation = slang::ScopeGuard([this] {
        if (m_driver)
            m_driver->preserveTrustedUvmForRetirement(false);
    });
    phases.mark("retirement-release");
    const auto attemptStarted = MaintenanceRetry::Clock::now();
    try {
        if (!loadConfig(m_config, false, true, true, true, retirementContext)) {
            ++m_contextEdits;
            // Invalid configuration is retried after another edit, without an idle spin.
            m_contextRetirement.clearRequest();
            m_contextRetry.reset();
            trace("replacement-failed", epoch, bytes);
            return;
        }
        phases.mark("retirement-load-publish");
        m_contextRetirement.notePublishedReplacement();
        m_driver->completions.requireCompletionEpoch();
        m_contextEpoch = m_driver->completions.getCompletionEpoch();
        auto fresh = m_driver->sm.getSourceLocationTableBytes() + m_driver->sm.getMemoryUsage();
        resetSourceHistoryBudget(fresh);
        m_contextEdits = 0;
        m_contextRetry.reset();
        trace("retired", m_contextEpoch, fresh);
    }
    catch (const lsp::RequestCancelled&) {
        m_contextRetirement.request();
        m_contextRetry.defer(MaintenanceRetry::Clock::now() - attemptStarted);
        trace("replacement-cancelled", epoch, bytes);
        throw;
    }
    catch (const std::exception&) {
        m_contextRetirement.clearRequest();
        m_contextRetry.reset();
        // A failed construction keeps the old owner. If a later publication step threw after
        // the swap, fence that new owner too; the incoming text change is still applied once.
        auto actual = m_driver->completions.getCompletionEpoch();
        if (actual != epoch) {
            m_contextRetirement.notePublishedReplacement();
            m_driver->completions.requireCompletionEpoch();
            m_contextEpoch = actual;
            auto fresh = m_driver->sm.getSourceLocationTableBytes() + m_driver->sm.getMemoryUsage();
            resetSourceHistoryBudget(fresh);
            m_contextEdits = 0;
        }
        else
            ++m_contextEdits;
        trace("replacement-exception", actual,
              m_driver->sm.getSourceLocationTableBytes() + m_driver->sm.getMemoryUsage());
    }
}

void SlangServer::observeSourceHistory() {
    // A query can allocate source names or complete a previously deferred parse.
    // Observe that growth after publishing the reply, without counting queries
    // as edits or introducing a settling delay. Soft pressure schedules private
    // maintenance; only the existing hard bound requires immediate retirement.
    if (m_driver && sourceHistoryEnabled(m_config) &&
        !std::getenv("SLANG_UVM_GENERATION_DISABLE")) {
        const auto epoch = m_driver->completions.getCompletionEpoch();
        const auto bytes = m_driver->sm.getSourceLocationTableBytes() +
                           m_driver->sm.getMemoryUsage();
        if (m_contextEpoch != epoch) {
            m_contextEpoch = epoch;
            resetSourceHistoryBudget(bytes);
        }
        if (bytes > m_contextBudget) {
            m_contextRetirement.request();
            if (bytes >= m_contextHardBudget)
                maybeRetireParsedContext();
        }
    }
}

} // namespace server
