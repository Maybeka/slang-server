// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <stdexcept>

#include "slang/util/Util.h"

namespace fs = std::filesystem;

static std::filesystem::path findSlangRoot(fs::path path = fs::current_path()) {
    const auto start = path;
    while (!fs::exists(path / "tests")) {
        const auto parent = path.parent_path();
        if (parent == path || parent.empty())
            throw std::runtime_error("Cannot find test source root from " + start.string());
        path = parent;
    }
    return path;
}
