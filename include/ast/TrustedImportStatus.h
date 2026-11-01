// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>

namespace server {
// Value-only result of one model's library import. Publication is transactional;
// private queries and aborted candidates must not overwrite the last model result.
struct TrustedImportStatus {
    std::string status = "no-model";
    uint64_t generation = 0;
    std::string failureCategory;
    std::string witnessFailure;
};
} // namespace server
