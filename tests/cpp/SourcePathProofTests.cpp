// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>
#include <vector>
#if defined(__APPLE__)
#    include "util/SourcePathProof.h"
#endif

#if defined(__APPLE__)
TEST_CASE("Fast pathname proof implies the complete filesystem lexical proof",
          "[body-certification][mac-path-proof]") {
    const std::vector<std::string> paths = {"/a/source.sv",
                                            "/a/b/source.svh",
                                            "/a/.hidden/source.sv",
                                            "/a/.../source.sv",
                                            "/路径/源文件.sv",
                                            "/a/name with spaces.sv",
                                            "/a/name\nline.sv",
                                            "relative/source.sv",
                                            "./source.sv",
                                            "../source.sv",
                                            "",
                                            "/",
                                            "//a/source.sv",
                                            "/a//source.sv",
                                            "/a/./source.sv",
                                            "/a/b/../source.sv",
                                            "/a/source.sv/",
                                            "/a/.",
                                            "/a/..",
                                            "///a/source.sv",
                                            "/a/source.sv.alias"};
    size_t fastCases = 0;
    for (const auto& input : paths)
        for (const auto& resolved : paths) {
            if (!server::exactNormalizedPosixPath(input, resolved))
                continue;
            ++fastCases;
            INFO(input << " resolved=" << resolved);
            // Independent standard-library oracle: bypassing construction is
            // allowed only when the prior full lexical proof would succeed.
            CHECK(std::filesystem::path(resolved) == std::filesystem::path(input));
            CHECK(std::filesystem::absolute(input).lexically_normal() ==
                  std::filesystem::path(input));
        }
    CHECK(fastCases == 8);
}
#endif
