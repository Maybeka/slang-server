// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string_view>

namespace server {
// A fast sufficient condition for the existing filesystem-path comparison.
// Other spellings retain the original proof, including redundant separators.
inline bool exactNormalizedPosixPath(std::string_view input, std::string_view resolved) {
    if (input != resolved || input.size() < 2 || input.front() != '/')
        return false;
    std::size_t start = 1;
    while (start < input.size()) {
        const auto end = input.find('/', start);
        const auto component = input.substr(start,
                                            end == std::string_view::npos ? end : end - start);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (end == std::string_view::npos)
            return true;
        start = end + 1;
    }
    return false;
}
} // namespace server
