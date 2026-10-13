// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "util/SourceContextHints.h"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"

namespace {
struct HintSource {
    slang::SourceManager sourceManager;
    slang::SourceBuffer buffer;
    server::SourceContextHints hints;

    explicit HintSource(std::string_view text) {
        buffer = sourceManager.assignText("hints.sv", text);
        hints = server::parseSourceContextHints(buffer, sourceManager);
    }

    server::SourceContextImportHints bind() {
        auto tree = slang::syntax::SyntaxTree::fromBuffer(buffer, sourceManager);
        return server::bindSourceImportHints(*tree, hints);
    }
};
} // namespace

TEST_CASE("Source owner hints accept leading physical comments and retain byte ranges",
          "[source-context-hints]") {
    const std::string text = "// Copyright\r\n/* leading block */\r\n"
                             "  // slang-server: owner \"../package owner.sv\"\r\n"
                             "class item; endclass\n";
    HintSource source(text);
    REQUIRE(source.hints.owner);
    CHECK(source.hints.owner->path == "../package owner.sv");
    CHECK_FALSE(source.hints.invalidOwner);
    CHECK(source.hints.issues.empty());
    const auto& range = source.hints.owner->range;
    CHECK(range.bufferId == source.buffer.id.getId());
    CHECK(text.substr(range.begin, range.end - range.begin) ==
          "// slang-server: owner \"../package owner.sv\"");
}

TEST_CASE("Source owner hints read EOF trivia and tolerate identical duplicates",
          "[source-context-hints]") {
    HintSource source("// slang-server: owner \"owner.sv\"\n"
                      "// slang-server: owner \"owner.sv\"");
    REQUIRE(source.hints.owner);
    CHECK(source.hints.owner->path == "owner.sv");
    CHECK(source.hints.issues.empty());
}

TEST_CASE("Source owner hints reject conflicts without choosing an owner",
          "[source-context-hints]") {
    HintSource source("// slang-server: owner \"first.sv\"\n"
                      "// slang-server: owner \"second.sv\"\n");
    CHECK_FALSE(source.hints.owner);
    CHECK(source.hints.invalidOwner);
    REQUIRE(source.hints.issues.size() == 1);
    CHECK(source.hints.issues.front().code == server::SourceContextHintIssueCode::ConflictingOwner);
}

TEST_CASE("Owner hints ignore string block inactive and late examples", "[source-context-hints]") {
    const std::vector<std::string_view> examples{
        R"sv(string example = "// slang-server: owner \"wrong.sv\"";)sv",
        "/*\n// slang-server: owner \"wrong.sv\"\n*/\n",
        "`ifdef UNKNOWN_HINT_CONDITION\n// slang-server: owner \"wrong.sv\"\n`endif\n",
        "`define EARLIER_TOKEN\n// slang-server: owner \"wrong.sv\"\n",
        "class item; endclass\n// slang-server: owner \"wrong.sv\"\n",
        "// Example: // slang-server: owner \"wrong.sv\"\n",
        "/// slang-server: owner \"wrong.sv\"\n",
        "// slang-server: ownership \"wrong.sv\"\n",
        "// SLANG-SERVER: owner \"wrong.sv\"\n",
        "// slang-server:owner \"wrong.sv\"\n",
    };
    for (auto text : examples) {
        INFO(text);
        HintSource source(text);
        CHECK_FALSE(source.hints.owner);
        CHECK_FALSE(source.hints.invalidOwner);
        CHECK(source.hints.issues.empty());
    }
}

TEST_CASE("Source hint paths use strict JSON strings and preserve literal path spelling",
          "[source-context-hints]") {
    HintSource source(R"sv(// slang-server: owner "../$HOME/a\\b\"c\u0064.sv")sv");
    REQUIRE(source.hints.owner);
    CHECK(source.hints.owner->path == "../$HOME/a\\b\"cd.sv");
    CHECK(source.hints.issues.empty());
}

TEST_CASE("Source owner hints reject malformed paths and control characters",
          "[source-context-hints]") {
    const std::vector<std::string_view> paths{
        "",
        "owner.sv",
        "'owner.sv'",
        "123",
        "null",
        "[]",
        "{}",
        "\"\"",
        "\"unterminated",
        R"("owner.sv" extra)",
        R"("bad\q.sv")",
        R"("bad\u0000.sv")",
        R"("bad\n.sv")",
        R"("bad\t.sv")",
        R"("bad\u007f.sv")",
        R"("bad\ud800.sv")",
    };
    for (auto path : paths) {
        INFO(path);
        HintSource source("// slang-server: owner " + std::string(path) + "\n");
        CHECK_FALSE(source.hints.owner);
        CHECK(source.hints.invalidOwner);
        REQUIRE(source.hints.issues.size() == 1);
        CHECK(source.hints.issues.front().code == server::SourceContextHintIssueCode::InvalidPath);
    }
}

TEST_CASE("An invalid owner directive is not rescued by a later valid directive",
          "[source-context-hints]") {
    HintSource source("// slang-server: owner broken\n"
                      "// slang-server: owner \"owner.sv\"\n");
    CHECK_FALSE(source.hints.owner);
    CHECK(source.hints.invalidOwner);
    REQUIRE(source.hints.issues.size() == 1);
}

TEST_CASE("Source hints bind active trailing imports including EOF and multiline declarations",
          "[source-context-hints]") {
    HintSource source("import first_pkg::*; // slang-server: source \"../first.sv\"\r\n"
                      "import\n second_pkg::item;\t// slang-server: source \"second.sv\"");
    auto bound = source.bind();
    REQUIRE(bound.imports.size() == 2);
    CHECK(bound.imports[0].packageName == "first_pkg");
    CHECK(bound.imports[0].path == "../first.sv");
    CHECK(bound.imports[1].packageName == "second_pkg");
    CHECK(bound.imports[1].path == "second.sv");
    CHECK(bound.issues.empty());
    const auto& range = bound.imports.back().range;
    CHECK(source.sourceManager.getSourceText(source.buffer.id)
              .substr(range.begin, range.end - range.begin) ==
          "// slang-server: source \"second.sv\"");
}

TEST_CASE("Source hints infer one exact package name for comma separated items",
          "[source-context-hints]") {
    HintSource source("import pkg::a, pkg::b; // slang-server: source \"pkg.sv\"\n");
    auto bound = source.bind();
    REQUIRE(bound.imports.size() == 1);
    CHECK(bound.imports.front().packageName == "pkg");
    CHECK(bound.issues.empty());
}

TEST_CASE("Source hints reject distinct package names in one declaration",
          "[source-context-hints]") {
    HintSource source("import first_pkg::a, second_pkg::b; "
                      "// slang-server: source \"pkg.sv\"\n");
    auto bound = source.bind();
    CHECK(bound.imports.empty());
    REQUIRE(bound.issues.size() == 1);
    CHECK(bound.issues.front().code == server::SourceContextHintIssueCode::AmbiguousImport);
}

TEST_CASE("Source hints ignore inactive macro block string and unattached examples",
          "[source-context-hints]") {
    const std::vector<std::string_view> examples{
        "`ifdef UNKNOWN_HINT_CONDITION\n"
        "import pkg::*; // slang-server: source broken\n`endif\n",
        "`define IMPORT import pkg::*;\n`IMPORT // slang-server: source \"pkg.sv\"\n",
        "`define NAME pkg\nimport `NAME::*; // slang-server: source \"pkg.sv\"\n",
        "/* import pkg::*; // slang-server: source \"pkg.sv\" */\n",
        "import pkg::*; /* // slang-server: source \"pkg.sv\" */\n",
        R"sv(string example = "import pkg::*; // slang-server: source \"pkg.sv\"";)sv",
        "import pkg::*;\n// slang-server: source \"pkg.sv\"\n",
        "// slang-server: source \"pkg.sv\"\nimport pkg::*;\n",
        "import pkg::*; /* gap */ // slang-server: source \"pkg.sv\"\n",
        "import pkg::* // slang-server: source \"pkg.sv\"\n",
        "int x; // slang-server: source broken\n",
        "// import pkg::*; // slang-server: source \"pkg.sv\"\n",
    };
    for (auto text : examples) {
        INFO(text);
        HintSource source(text);
        auto bound = source.bind();
        CHECK(bound.imports.empty());
        CHECK(bound.issues.empty());
    }
}

TEST_CASE("Source hint after two same line imports belongs only to the final declaration",
          "[source-context-hints]") {
    HintSource source("import first_pkg::*; import second_pkg::*; "
                      "// slang-server: source \"second.sv\"\n");
    auto bound = source.bind();
    REQUIRE(bound.imports.size() == 1);
    CHECK(bound.imports.front().packageName == "second_pkg");
    CHECK(bound.issues.empty());
}

TEST_CASE("Malformed source paths produce issues only for active import sites",
          "[source-context-hints]") {
    HintSource source("import pkg::*; // slang-server: source broken\n");
    CHECK(source.hints.issues.empty());
    auto bound = source.bind();
    CHECK(bound.imports.empty());
    REQUIRE(bound.issues.size() == 1);
    CHECK(bound.issues.front().code == server::SourceContextHintIssueCode::InvalidPath);
}

TEST_CASE("Source context hint results own all strings after source manager teardown",
          "[source-context-hints]") {
    server::SourceContextHints hints;
    server::SourceContextImportHints imports;
    {
        HintSource source("// slang-server: owner \"../owner.sv\"\n"
                          "import exact_pkg::*; // slang-server: source \"../package.sv\"\n");
        hints = source.hints;
        imports = source.bind();
    }
    REQUIRE(hints.owner);
    CHECK(hints.owner->path == "../owner.sv");
    REQUIRE(hints.sourceComments.size() == 1);
    CHECK(hints.sourceComments.front().pathText == "\"../package.sv\"");
    REQUIRE(imports.imports.size() == 1);
    CHECK(imports.imports.front().packageName == "exact_pkg");
    CHECK(imports.imports.front().path == "../package.sv");
}
