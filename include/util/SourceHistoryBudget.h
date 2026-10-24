// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace server {
/// Bounds growth beyond a generation's initial source working set, including
/// position tables. Retained fixed providers and process RSS are separate.
struct SourceHistoryBudget {
    size_t soft, hard;

    static SourceHistoryBudget limits(size_t initial, uint32_t maxMiB) {
        const auto maximum = std::numeric_limits<size_t>::max();
        const auto allowance = std::min<uint64_t>(uint64_t(maxMiB) * 1024 * 1024, maximum);
        auto add = [&](uint64_t growth) {
            return growth > maximum - initial ? maximum : initial + size_t(growth);
        };
        return {add(allowance / 2), add(allowance)};
    }
};
} // namespace server
