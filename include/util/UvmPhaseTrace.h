// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "slang/util/OS.h"

namespace server {
// Optional mechanism observations only; ordinary performance runs leave this unset.
class UvmPhaseTrace {
    using Clock = std::chrono::steady_clock;
    std::string path;
    Clock::time_point last;

public:
    explicit UvmPhaseTrace(bool active = true) {
        if (active)
            if (auto* value = std::getenv("SLANG_UVM_PHASE_TRACE")) {
                path = value;
                last = Clock::now();
            }
    }
    void mark(const char* phase) {
        if (path.empty())
            return;
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - last);
        if (auto* file = std::fopen(path.c_str(), "a")) {
            std::fprintf(file, "%ld\t%s\t%lld\n", long(slang::OS::getpid()), phase,
                         static_cast<long long>(elapsed.count()));
            std::fclose(file);
        }
        last = Clock::now();
    }
};
} // namespace server
