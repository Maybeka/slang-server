// SPDX-License-Identifier: MIT
#include "document/LocalDiagnosticSession.h"

#include "document/BodyDiagnosticCache.h"

namespace server {
LocalDiagnosticSession::LocalDiagnosticSession() = default;
LocalDiagnosticSession::~LocalDiagnosticSession() = default;

bool LocalDiagnosticSession::hasPendingCapture() const {
    return captured && bool(captured->pendingDesign);
}

void LocalDiagnosticSession::seed(std::shared_ptr<const BodyDiagnosticBaseline> baseline) {
    if (preparedCache || captured || !baseline)
        throw std::logic_error("diagnostic session already owns a baseline or cache");
    captured = std::move(baseline);
}

void LocalDiagnosticSession::releasePrepared() {
    preparedCache.reset();
}

void LocalDiagnosticSession::publicationSucceeded() {
    ++published;
    failure.clear();
}

void LocalDiagnosticSession::publicationFailed(std::string reason) {
    failure = std::move(reason);
    preparedCache.reset();
    captured.reset();
}
} // namespace server
