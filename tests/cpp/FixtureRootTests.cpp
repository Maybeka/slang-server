// SPDX-License-Identifier: MIT
#include "utils/Utils.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Fixture discovery finds the source root from a nested test directory",
          "[fixture-root]") {
    const auto root = findSlangRoot();
    CHECK(findSlangRoot(root / "tests" / "cpp") == root);
}

TEST_CASE("Fixture discovery terminates at the filesystem root", "[fixture-root]") {
    const auto root = fs::current_path().root_path();
    if (fs::exists(root / "tests"))
        CHECK(findSlangRoot(root) == root);
    else
        CHECK_THROWS_AS(findSlangRoot(root), std::runtime_error);
}
