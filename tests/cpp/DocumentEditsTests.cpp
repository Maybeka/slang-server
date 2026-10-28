// SPDX-License-Identifier: MIT
#include "document/DocumentEdits.h"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace server;
namespace {
using Change = lsp::TextDocumentContentChangeEvent;
Change partial(unsigned line, unsigned first, unsigned last, std::string text) {
    return lsp::TextDocumentContentChangePartial{
        .range = {.start = {.line = line, .character = first},
                  .end = {.line = line, .character = last}},
        .text = std::move(text)};
}
std::string apply(std::string text, const std::vector<Change>& changes, bool utf8 = false) {
    text.push_back('\0');
    auto result = applyDocumentChanges(text, changes, utf8);
    REQUIRE_FALSE(result.empty());
    REQUIRE(result.back() == '\0');
    return std::string(result.data(), result.size() - 1);
}
} // namespace

TEST_CASE("Independent document edits preserve sequential range coordinates", "[document-edits]") {
    CHECK(apply("abc\ndef", {partial(0, 1, 2, "XYZ"), partial(0, 2, 4, "!"),
                             partial(1, 3, 3, "?")}) == "aX!c\ndef?");
    CHECK(apply("abc", {partial(0, 0, 3, ""), partial(0, 0, 0, "repair")}) == "repair");
    CHECK(apply("abc", {lsp::TextDocumentContentChangeWholeDocument{.text = "xyz"},
                        partial(0, 1, 2, "!")}) == "x!z");
    CHECK(apply("abc", {partial(0, 0, 1, "!"),
                        lsp::TextDocumentContentChangeWholeDocument{.text = "final"}}) == "final");
}

TEST_CASE("Independent document edits retain negotiated Unicode coordinates", "[document-edits]") {
    CHECK(apply("a😀b\r\ncafé", {partial(0, 1, 3, "X"), partial(1, 3, 4, "e")}) == "aXb\r\ncafe");
    CHECK(apply("a😀b", {partial(0, 1, 5, "X")}, true) == "aXb");
    CHECK(apply("", {partial(0, 0, 0, "😀")}) == "😀");
}

TEST_CASE("Independent document edits leave the source untouched after an invalid later range",
          "[document-edits]") {
    std::string input = "abc";
    input.push_back('\0');
    const auto original = input;
    const std::vector<Change> changes = {partial(0, 0, 1, "valid"), partial(2, 0, 0, "bad")};
    CHECK_THROWS(applyDocumentChanges(input, changes, false));
    CHECK(input == original);
    CHECK_THROWS(applyDocumentChanges(input, {}, false));
}
