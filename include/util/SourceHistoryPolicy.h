// SPDX-License-Identifier: MIT
#pragma once
#include "Config.h"

namespace server {
inline bool sourceHistoryEnabled(const Config& config) {
    return config.experimentalUvmPrefixCache.value() || config.experimentalFrozenSources.value() ||
           config.experimentalUvmSemanticCache.value() ||
           config.experimentalScopeSemanticCache.value() ||
           config.experimentalCooperativeAnalysis.value() ||
           (config.experimentalMethodBodyQueries.value() &&
            config.experimentalMethodBodyCertificateRenewal.value());
}
} // namespace server
