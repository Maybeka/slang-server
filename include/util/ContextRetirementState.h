// SPDX-License-Identifier: MIT
#pragma once

namespace server {

/// Orthogonal retirement facts, updated only by named lifecycle events.
/// An invalid configuration can clear a retry while editor input still awaits
/// admission. Publication history survives both operations.
class ContextRetirementState {
public:
    bool isPending() const noexcept { return requested; }
    bool awaitsInputs() const noexcept { return admission; }
    bool hasPublishedReplacement() const noexcept { return published; }

    void request() noexcept { requested = true; }
    void clearRequest() noexcept { requested = false; }
    void deferInputs() noexcept { admission = true; }
    void admitInputs() noexcept { admission = false; }
    void notePublishedReplacement() noexcept { published = true; }

private:
    bool requested = false;
    bool admission = false;
    bool published = false;
};

} // namespace server
