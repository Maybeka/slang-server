// SPDX-License-Identifier: MIT
#pragma once

#include <stdexcept>
#include <utility>

namespace server {

/// Request-local, exactly-once boundary for an incremental document notification.
/// Owns no text or semantic model; cancellation cannot reset an entered boundary.
class DocumentEditTransaction {
public:
    enum class State { Pending, Synchronizing, TextApplied, OverlayCommitted };

    DocumentEditTransaction() = default;
    DocumentEditTransaction(const DocumentEditTransaction&) = delete;
    DocumentEditTransaction& operator=(const DocumentEditTransaction&) = delete;

    State state() const noexcept { return current; }
    bool canSynchronize() const noexcept { return current == State::Pending; }

    template<typename Apply>
    void synchronize(Apply&& apply) {
        if (!canSynchronize())
            throw std::logic_error("document edit synchronization already started");
        // Record entry before calling native code: it may throw after mutating text.
        current = State::Synchronizing;
        std::forward<Apply>(apply)();
        current = State::TextApplied;
    }

    void commitOverlay() {
        if (current != State::TextApplied)
            throw std::logic_error("document overlay commit requires synchronized text");
        current = State::OverlayCommitted;
    }

private:
    State current = State::Pending;
};

} // namespace server
