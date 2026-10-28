// SPDX-License-Identifier: MIT
#include "util/BorrowedGenericReader.h"
#include "lsp/LspTypes.h"
#include <catch2/catch_test_macros.hpp>
#include <rfl/json.hpp>

TEST_CASE("Borrowed cancellation validation matches owning parser acceptance and errors",
          "[json-rpc][input-lifetime]") {
    const std::vector<std::string> cases = {
        R"({"textDocument":{"uri":"file:///tmp/a.sv","version":2},"contentChanges":[{"text":"λ\n😀"}]})",
        R"({"textDocument":{"uri":"file:///tmp/a.sv","version":2},"contentChanges":[]})",
        R"({"textDocument":{"uri":"file:///tmp/a.sv","version":2.0},"contentChanges":[{"text":"x","range":{"start":{"line":0,"character":1},"end":{"line":0,"character":2}}}]})",
        R"({"textDocument":{"uri":"file:///tmp/a.sv","version":2},"contentChanges":[{"text":"x","range":false}]})",
        R"({"textDocument":{"uri":"file:///tmp/a.sv","version":2},"contentChanges":[{"text":7}]})",
        R"({"textDocument":{"uri":"file:///tmp/a.sv","version":null},"contentChanges":[{"text":"x"}]})",
        R"({"textDocument":{"uri":"file:///tmp/a.sv","version":2}})",
        R"({"textDocument":{"uri":"file:///tmp/a.sv","version":2},"contentChanges":false})",
        R"({"textDocument":{"uri":"file:///tmp/a.sv","version":2},"contentChanges":[null]})",
        R"({"textDocument":{"uri":"file:///tmp/a.sv","version":2},"textDocument":{"uri":"file:///tmp/b.sv","version":3},"contentChanges":[{"text":"a","text":"b"}]})",
        R"({"textDocument":{"uri":"file:///tmp/a.sv","version":2},"contentChanges":[{"text":"x","range":{"start":{"line":0,"line":1,"character":0},"end":{"line":2,"character":0}}}]})",
        "null", "[]", "42"};
    for (const auto& text : cases) {
        INFO(text);
        const auto source = rfl::json::read<rfl::Generic>(text).value();
        const auto before = rfl::json::write(source);
        auto old = rfl::from_generic<lsp::DidChangeTextDocumentParams, rfl::UnderlyingEnums>(source);
        auto owned = server::readBorrowedGeneric<lsp::DidChangeTextDocumentParams,
                                                 rfl::UnderlyingEnums>(source);
        auto key = server::readBorrowedGeneric<lsp::DidChangeTextDocumentParams,
                                               rfl::UnderlyingEnums>(source, true);
        REQUIRE(bool(old) == bool(owned));
        REQUIRE(bool(old) == bool(key));
        if (old) {
            CHECK(rfl::json::write(*old) == rfl::json::write(*owned));
            CHECK(old->textDocument.uri == key->textDocument.uri);
            CHECK(old->textDocument.version == key->textDocument.version);
        }
        else {
            CHECK(old.error().what() == owned.error().what());
            CHECK(old.error().what() == key.error().what());
        }
        CHECK(rfl::json::write(source) == before);
    }
}

TEST_CASE("Borrowed Generic input never becomes a borrowed typed result",
          "[json-rpc][input-lifetime]") {
    const std::string text = std::string(1024 * 1024, 'x') + "λ😀\n";
    lsp::DidChangeTextDocumentParams expected{
        .textDocument = {.version = 17, .uri = URI("file:///tmp/owned.sv")},
        .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{.text = text}}};
    auto source = rfl::to_generic(expected);
    auto result = server::readBorrowedGeneric<lsp::DidChangeTextDocumentParams,
                                              rfl::UnderlyingEnums>(source);
    REQUIRE(result);
    auto routing = server::readBorrowedGeneric<lsp::DidChangeTextDocumentParams,
                                               rfl::UnderlyingEnums>(source, true);
    REQUIRE(routing);
    CHECK(rfl::json::write(routing->contentChanges).size() < 100);
    source = rfl::Generic{};
    CHECK(rfl::json::write(*result) == rfl::json::write(expected));
    CHECK(result->textDocument.uri == routing->textDocument.uri);
}
