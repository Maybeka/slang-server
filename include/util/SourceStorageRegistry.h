// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <memory>
#include <vector>

#include "slang/text/SourceManager.h"

namespace server {
/// One server's source namespaces across successful and failed replacements.
/// Observers never keep an unused manager or driver alive. State-worker only.
class SourceStorageRegistry {
public:
    struct Usage {
        slang::SourceManager::StorageUsage current, retained, total;
    };

    void track(std::weak_ptr<const slang::SourceManager> source) {
        std::erase_if(sources, [](const auto& value) { return value.expired(); });
        sources.push_back(std::move(source));
    }

    Usage usage(const slang::SourceManager& current) {
        std::erase_if(sources, [](const auto& value) { return value.expired(); });
        slang::SourceManager::StorageUsageAccumulator active, retained, total;
        // Keep the observed namespaces alive for this one accounting pass so
        // address-based de-duplication cannot encounter a recycled allocation.
        std::vector<std::shared_ptr<const slang::SourceManager>> observed;
        for (const auto& source : sources)
            if (auto live = source.lock())
                observed.push_back(std::move(live));
        for (const auto& source : observed) {
            total.add(*source);
            if (source.get() == &current)
                active.add(*source);
            else
                retained.add(*source);
        }
        return {active.usage(), retained.usage(), total.usage()};
    }

private:
    std::vector<std::weak_ptr<const slang::SourceManager>> sources;
};
} // namespace server
