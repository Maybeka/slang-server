// SPDX-License-Identifier: MIT
#pragma once
#include "lsp/RequestContext.h"
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace server {
class BodyDiagnosticCache;
struct BodyDiagnosticBaseline;

/// Owns one original capture and one prepared local cache, never an edit chain.
/// Attempt versions, cancellation retries and failure provenance travel with them.
class LocalDiagnosticSession {
public:
    LocalDiagnosticSession();
    ~LocalDiagnosticSession();
    BodyDiagnosticCache* cache() noexcept { return preparedCache.get(); }
    const BodyDiagnosticCache* cache() const noexcept { return preparedCache.get(); }
    const std::shared_ptr<const BodyDiagnosticBaseline>& baseline() const { return captured; }
    const std::string& decline() const noexcept { return failure; }
    bool isPrepared() const noexcept { return bool(preparedCache) && failure.empty(); }
    uint64_t publications() const noexcept { return published; }
    void seed(std::shared_ptr<const BodyDiagnosticBaseline>);
    void releasePrepared();
    void publicationSucceeded();
    void publicationFailed(std::string reason);

    template<typename Capture>
    void capture(int version, Capture&& capture) {
        if (preparedCache || captured || captureVersion == version)
            return;
        captureVersion = version;
        try {
            captured = std::forward<Capture>(capture)();
            if (!captured)
                throw std::logic_error("diagnostic capture returned no baseline");
            failure.clear();
            captureFailure.clear();
        }
        catch (const lsp::RequestCancelled&) {
            captureVersion = -1;
            throw;
        }
        catch (const std::exception& error) {
            failure = error.what();
            captureFailure = failure;
        }
    }

    template<typename Prepare>
    void prepare(int version, bool completeDeferred, Prepare&& prepare) {
        if ((hasPendingCapture() && !completeDeferred) || preparedCache ||
            prepareVersion == version)
            return;
        prepareVersion = version;
        try {
            preparedCache = std::forward<Prepare>(prepare)(captured);
            if (!preparedCache)
                throw std::logic_error("diagnostic preparation returned no cache");
            captured.reset();
            failure.clear();
            captureFailure.clear();
        }
        catch (const lsp::RequestCancelled&) {
            prepareVersion = -1;
            throw;
        }
        catch (const std::exception& error) {
            // Preserve the original eligibility failure rather than subsequent
            // missing-baseline fallout. Successful capture/preparation clears it.
            failure = captureFailure.empty() ? error.what() : captureFailure;
            captured.reset();
        }
    }

private:
    bool hasPendingCapture() const;
    // Reverse destruction drops the consumer before the original baseline.
    std::shared_ptr<const BodyDiagnosticBaseline> captured;
    std::unique_ptr<BodyDiagnosticCache> preparedCache;
    std::string failure, captureFailure;
    int captureVersion = -1, prepareVersion = -1;
    uint64_t published = 0;
};
} // namespace server
