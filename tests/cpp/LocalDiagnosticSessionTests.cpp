// SPDX-License-Identifier: MIT
#include "document/BodyDiagnosticCache.h"
#include "document/LocalDiagnosticSession.h"
#include <catch2/catch_test_macros.hpp>

using namespace server;
namespace {
auto baseline() {
    return std::make_shared<const BodyDiagnosticBaseline>(
        URI::fromFile(std::filesystem::temp_directory_path() / "diagnostic-session.sv"));
}
} // namespace

TEST_CASE("Cancelled diagnostic attempts can retry the same version without losing capture",
          "[local-diagnostic-session]") {
    LocalDiagnosticSession session;
    lsp::RequestContext cancelled("capture", 903);
    cancelled.cancel();
    CHECK_THROWS_AS(session.capture(7,
                                    [&] {
                                        cancelled.checkpoint("capture baseline");
                                        return baseline();
                                    }),
                    lsp::RequestCancelled);
    CHECK_FALSE(session.baseline());
    auto saved = baseline();
    session.capture(7, [&] { return saved; });
    REQUIRE(session.baseline() == saved);
    CHECK_THROWS_AS(session.prepare(7, true,
                                    [&](auto&) -> std::unique_ptr<BodyDiagnosticCache> {
                                        cancelled.checkpoint("prepare baseline");
                                        return {};
                                    }),
                    lsp::RequestCancelled);
    CHECK(session.baseline() == saved);
    size_t attempts = 0;
    session.prepare(7, true, [&](auto&) -> std::unique_ptr<BodyDiagnosticCache> {
        ++attempts;
        throw std::runtime_error("new eligibility failure");
    });
    CHECK(attempts == 1);
    CHECK(session.decline() == "new eligibility failure");
    CHECK_FALSE(session.baseline());
    session.prepare(7, true, [&](auto&) -> std::unique_ptr<BodyDiagnosticCache> {
        ++attempts;
        return {};
    });
    CHECK(attempts == 1);
}

TEST_CASE("Diagnostic preparation preserves capture failure until a new successful capture",
          "[local-diagnostic-session]") {
    LocalDiagnosticSession session;
    size_t captures = 0;
    auto unavailable = [&]() -> std::shared_ptr<const BodyDiagnosticBaseline> {
        ++captures;
        throw std::runtime_error("external receiver is not eligible");
    };
    session.capture(1, unavailable);
    session.capture(1, unavailable);
    CHECK(captures == 1);
    session.prepare(1, true, [](auto&) -> std::unique_ptr<BodyDiagnosticCache> {
        throw std::runtime_error("completed baseline is unavailable");
    });
    CHECK(session.decline() == "external receiver is not eligible");
    session.capture(2, [&] { return baseline(); });
    CHECK(session.decline().empty());
    session.prepare(2, true, [](auto&) -> std::unique_ptr<BodyDiagnosticCache> {
        throw std::runtime_error("different eligibility boundary");
    });
    CHECK(session.decline() == "different eligibility boundary");
}
