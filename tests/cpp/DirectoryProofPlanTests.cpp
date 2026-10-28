// SPDX-License-Identifier: MIT
#include "util/DirectoryProofPlan.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <vector>
#if defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
struct Fixture {
    fs::path root;
    std::vector<std::string> paths;
    Fixture(size_t groups = 1) {
        auto pattern = (fs::temp_directory_path() / "slang-directory-proof-XXXXXX").string();
        auto result = ::mkdtemp(pattern.data());
        if (!result)
            throw std::runtime_error("temporary directory unavailable");
        root = fs::canonical(result);
        for (size_t g = 0; g < groups; ++g) {
            auto parent = root / ("group" + std::to_string(100 + g));
            fs::create_directory(parent);
            std::ofstream(parent / "present.sv") << "module m; endmodule\n";
            for (size_t i = 0; i < 8; ++i)
                paths.push_back((parent / ("missing" + std::to_string(i) + ".svh")).string());
        }
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    auto plan(size_t budget = 512 * 1024) const {
        std::vector<std::string_view> views(paths.begin(), paths.end());
        return server::DirectoryProofPlan::build(views, budget);
    }
    fs::path parent() const { return fs::path(paths.front()).parent_path(); }
};
size_t descriptors() {
    size_t count = 0;
    for (int fd = 0; fd < 256; ++fd)
        if (::fcntl(fd, F_GETFD) >= 0)
            ++count;
    return count;
}
} // namespace

TEST_CASE("Directory proofs retain names but never successful invocation state",
          "[directory-proof][body-certification]") {
    Fixture f;
    REQUIRE_FALSE(f.plan(0));
    REQUIRE_FALSE(f.plan(1));
    auto plan = f.plan();
    REQUIRE(plan);
    CHECK(plan->chargedBytes() <= 512 * 1024);
    const auto before = descriptors();
    int borrowed = -1;
    {
        server::DirectoryProofPlan::Session session(*plan, {});
        REQUIRE(session.directory(f.parent().string()));
        borrowed = *session.directory(f.parent().string());
        CHECK((::fcntl(borrowed, F_GETFD) & FD_CLOEXEC) != 0);
        for (const auto& path : f.paths) {
            CHECK(session.covers(path));
            CHECK(session.covers(plan->group(path)));
        }
        CHECK_FALSE(session.covers(size_t(-1)));
        CHECK_FALSE(session.covers((f.parent() / "unplanned.svh").string()));
        CHECK(session.valid({}));
    }
    CHECK(::fcntl(borrowed, F_GETFD) == -1);
    CHECK(descriptors() == before);
    std::ofstream(f.paths.front()) << "new dependency";
    {
        server::DirectoryProofPlan::Session session(*plan, {});
        CHECK_FALSE(session.covers(f.paths.front()));
        CHECK_FALSE(session.directory(f.parent().string()));
    }
    fs::remove(f.paths.front());
    {
        server::DirectoryProofPlan::Session session(*plan, {});
        CHECK(session.covers(f.paths.front()));
    }
    CHECK(descriptors() == before);
}

TEST_CASE("Directory proof final closure rejects namespace and permission mutations",
          "[directory-proof][body-certification]") {
    Fixture f;
    auto plan = f.plan();
    REQUIRE(plan);
    server::DirectoryProofPlan::Session session(*plan, {});
    REQUIRE(session.covers(f.paths.front()));
    SECTION("negative input appears") { std::ofstream(f.paths.front()) << "new"; }
    SECTION("parent moves and an identical-looking replacement appears") {
        fs::rename(f.parent(), f.root / "old");
        fs::create_directory(f.parent());
    }
    SECTION("search permission changes") { REQUIRE(::chmod(f.parent().c_str(), 0700) == 0); }
    SECTION("ancestor moves") {
        auto old = f.root;
        f.root += "-moved";
        fs::rename(old, f.root);
    }
    CHECK_FALSE(session.valid({}));
}

TEST_CASE("Directory proofs preserve alias and ambiguous-name fallback",
          "[directory-proof][body-certification]") {
    Fixture f;
    SECTION("dangling matching link requires ordinary lookup") {
        fs::create_symlink(f.root / "absent", f.paths.front());
        auto plan = f.plan();
        REQUIRE(plan);
        server::DirectoryProofPlan::Session session(*plan, {});
        CHECK_FALSE(session.covers(f.paths.front()));
    }
    SECTION("a fingerprint collision cannot certify a present filename as absent") {
        // Distinct lowercase ASCII names sharing the conservative 32-bit key.
        f.paths.front() = (f.parent() / "collision_f0df1bd2872d4291.svh").string();
        const auto present = f.parent() / "collision_82fb87ac61e84c6f.svh";
        std::ofstream(present) << "present dependency";
        auto plan = f.plan();
        REQUIRE(plan);
        {
            server::DirectoryProofPlan::Session session(*plan, {});
            CHECK_FALSE(session.covers(present.string()));
        }
        fs::remove(present);
        server::DirectoryProofPlan::Session session(*plan, {});
        CHECK(session.covers(f.paths.front()));
    }
    SECTION("non-ASCII entries require ordinary lookup") {
        std::ofstream(f.parent() / "符号.svh") << "";
        auto plan = f.plan();
        REQUIRE(plan);
        server::DirectoryProofPlan::Session session(*plan, {});
        CHECK_FALSE(session.covers(f.paths.front()));
    }
    SECTION("logical parent alias stays bound to one physical directory") {
        auto original = f.parent();
        auto alias = f.root / "alias";
        fs::create_directory_symlink(original, alias);
        for (auto& path : f.paths)
            path = (alias / fs::path(path).filename()).string();
        auto plan = f.plan();
        REQUIRE(plan);
        server::DirectoryProofPlan::Session session(*plan, {});
        CHECK(session.covers(f.paths.front()));
        CHECK_FALSE(session.directory(alias.string()));
        fs::remove(alias);
        fs::create_directory(f.root / "replacement");
        fs::create_directory_symlink(f.root / "replacement", alias);
        CHECK_FALSE(session.valid({}));
    }
}

TEST_CASE("Directory proof cancellation releases every partially acquired lease",
          "[directory-proof][body-certification]") {
    Fixture f(3);
    auto plan = f.plan();
    REQUIRE(plan);
    size_t checkpoints = 0;
    lsp::RequestContext count("test", 1);
    count.setCheckpointObserverForTesting([&](auto) { ++checkpoints; });
    { server::DirectoryProofPlan::Session session(*plan, count); REQUIRE(session.valid(count)); }
    REQUIRE(checkpoints > 10);
    const auto before = descriptors();
    for (size_t stop = 1; stop <= checkpoints; ++stop) {
        lsp::RequestContext ctx("test", 1);
        size_t seen = 0;
        ctx.setCheckpointObserverForTesting([&](auto) { if (++seen == stop) ctx.cancel(); });
        CHECK_THROWS_AS(([&] {
            server::DirectoryProofPlan::Session session(*plan, ctx);
            (void)session.valid(ctx);
        }()), lsp::RequestCancelled);
        CHECK(descriptors() == before);
    }
}

TEST_CASE("Directory proof leases obey cap and unwind descriptor exhaustion",
          "[directory-proof][body-certification]") {
    Fixture f(65);
    auto plan = f.plan();
    REQUIRE(plan);
    const auto before = descriptors();
    {
        server::DirectoryProofPlan::Session session(*plan, {});
        CHECK(session.covers(f.paths.front()));
        CHECK_FALSE(session.covers(f.paths.back()));
        CHECK(descriptors() <= before + 64);
        REQUIRE(session.valid({}));
    }
    CHECK(descriptors() == before);
    auto child = ::fork();
    REQUIRE(child >= 0);
    if (!child) {
        struct rlimit limit;
        if (::getrlimit(RLIMIT_NOFILE, &limit)) ::_exit(10);
        limit.rlim_cur = 32;
        if (::setrlimit(RLIMIT_NOFILE, &limit)) ::_exit(11);
        auto original = descriptors();
        try { server::DirectoryProofPlan::Session session(*plan, {}); ::_exit(12); }
        catch (const server::DirectoryProofUnavailable&) {
            ::_exit(descriptors() == original ? 0 : 13);
        }
        catch (...) { ::_exit(14); }
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(descriptors() == before);
}
#endif
