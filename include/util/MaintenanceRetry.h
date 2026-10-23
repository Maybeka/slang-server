// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace server {
// Throttle discarded maintenance candidates, never foreground work or diagnostics.
class MaintenanceRetry {
public:
    using Clock = std::chrono::steady_clock;
    void reset() {
        failures = 0;
        deadline = {};
        wait = {};
    }
    void defer(Clock::duration spent, Clock::time_point now = Clock::now()) {
        using namespace std::chrono;
        failures = std::min(failures + 1, uint32_t(10));
        auto cost = std::clamp(ceil<milliseconds>(spent), milliseconds(1), milliseconds(1000));
        wait = std::min(milliseconds(1000), cost * (uint32_t(1) << failures));
        deadline = now + wait;
    }
    // Once maintenance lost to foreground activity, wait for that activity to
    // subside before retrying. This does not delay the activity itself.
    void noteActivity(Clock::time_point now = Clock::now()) {
        if (failures)
            deadline = std::max(deadline, now + wait);
    }
    std::chrono::milliseconds remaining(Clock::time_point now = Clock::now()) const {
        return now >= deadline ? std::chrono::milliseconds(0)
                               : std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    }

private:
    uint32_t failures = 0;
    Clock::time_point deadline{};
    std::chrono::milliseconds wait{};
};
} // namespace server
