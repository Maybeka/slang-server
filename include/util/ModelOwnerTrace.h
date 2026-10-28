// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "slang/util/OS.h"

namespace server {
inline bool modelOwnerTraceEnabled() {
    const auto value = std::getenv("SLANG_MODEL_OWNER_TRACE");
    return value && *value;
}
// Mechanism-only ownership observations, never enabled in acceptance timing.
inline void traceModelOwner(const char* event, const void* owner, const void* parent = nullptr,
                            size_t astBytes = 0, size_t analysisEstimate = 0, long references = 0) {
    if (auto path = std::getenv("SLANG_MODEL_OWNER_TRACE"); path && *path) {
        const auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
        if (auto file = std::fopen(path, "a")) {
            std::fprintf(file, "%ld\t%lld\t%s\t%p\t%p\t%zu\t%zu\t%ld\n", long(slang::OS::getpid()),
                         static_cast<long long>(time), event, owner, parent, astBytes,
                         analysisEstimate, references);
            std::fclose(file);
        }
    }
}
} // namespace server
