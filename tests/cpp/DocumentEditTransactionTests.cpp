// SPDX-License-Identifier: MIT
#include "document/DocumentEditTransaction.h"
#include "util/ContextRetirementState.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <string>

using namespace server;

TEST_CASE("An entered native edit cannot replay a relative range after failure",
          "[document-edit-transaction]") {
    const bool mutateBeforeThrow = GENERATE(false, true);
    DocumentEditTransaction transaction;
    std::string text = "abcd";
    int attempts = 0;
    REQUIRE_THROWS_AS(transaction.synchronize([&] {
        ++attempts;
        if (mutateBeforeThrow)
            text.replace(1, 1, "XY");
        throw std::runtime_error("native sync failed");
    }),
                      std::runtime_error);
    CHECK_FALSE(transaction.canSynchronize());
    REQUIRE_THROWS_AS(transaction.synchronize([&] {
        ++attempts;
        text.replace(1, 1, "XY");
    }),
                      std::logic_error);
    CHECK(attempts == 1);
    CHECK(text == (mutateBeforeThrow ? "aXYcd" : "abcd"));
    CHECK_THROWS_AS(transaction.commitOverlay(), std::logic_error);
}

TEST_CASE("A declined local candidate leaves native text application available",
          "[document-edit-transaction]") {
    DocumentEditTransaction transaction;
    std::string text = "abcd";
    CHECK_THROWS_AS(transaction.commitOverlay(), std::logic_error);
    REQUIRE(transaction.canSynchronize());
    transaction.synchronize([&] { text.replace(1, 1, "XY"); });
    CHECK(transaction.state() == DocumentEditTransaction::State::TextApplied);
    transaction.commitOverlay();
    CHECK_FALSE(transaction.canSynchronize());
    CHECK_THROWS_AS(transaction.synchronize([&] { text.replace(1, 1, "XY"); }), std::logic_error);
    CHECK(text == "aXYcd");
}

TEST_CASE("Retirement retry reset preserves unadmitted editor input and publication history",
          "[context-retirement-state]") {
    ContextRetirementState state;
    state.request();
    state.deferInputs();
    state.clearRequest(); // Invalid config stops idle retry, retaining latest text.
    CHECK_FALSE(state.isPending());
    CHECK(state.awaitsInputs());
    CHECK_FALSE(state.hasPublishedReplacement());
    state.request();
    state.notePublishedReplacement();
    state.admitInputs();
    state.clearRequest();
    CHECK_FALSE(state.isPending());
    CHECK_FALSE(state.awaitsInputs());
    CHECK(state.hasPublishedReplacement());
    state.request();
    CHECK(state.hasPublishedReplacement());
}
