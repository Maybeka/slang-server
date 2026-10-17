// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "util/SourceContextHints.h"

#include <algorithm>
#include <limits>
#include <rfl/json/read.hpp>
#include <string_view>

#include "slang/parsing/Lexer.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/BumpAllocator.h"

namespace server {
namespace {

bool isHorizontalSpace(char c) {
    return c == ' ' || c == '\t';
}

std::string_view trimHorizontalSpace(std::string_view text) {
    while (!text.empty() && isHorizontalSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isHorizontalSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

std::optional<std::string_view> directivePayload(std::string_view comment,
                                                 std::string_view command) {
    if (!comment.starts_with("//"))
        return std::nullopt;
    comment = trimHorizontalSpace(comment.substr(2));
    constexpr std::string_view prefix = "slang-server:";
    if (!comment.starts_with(prefix))
        return std::nullopt;
    comment.remove_prefix(prefix.size());
    // A separating space keeps the marker unambiguous and case-sensitive.
    if (comment.empty() || !isHorizontalSpace(comment.front()))
        return std::nullopt;
    comment = trimHorizontalSpace(comment);
    if (!comment.starts_with(command))
        return std::nullopt;
    comment.remove_prefix(command.size());
    if (!comment.empty() && !isHorizontalSpace(comment.front()))
        return std::nullopt;
    return trimHorizontalSpace(comment);
}

std::optional<std::string> parsePath(std::string_view text) {
    // JSON decoding supplies strict quoting, escaping, and UTF-8 validation;
    // its default flags reject trailing tokens and non-JSON escape sequences.
    auto decoded = rfl::json::read<std::string>(text);
    if (!decoded)
        return std::nullopt;
    auto path = std::move(decoded).value();
    if (path.empty() ||
        std::ranges::any_of(path, [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
        return std::nullopt;
    return path;
}

SourceContextHintIssue invalidPath(SourceContextHintRange range) {
    return {SourceContextHintIssueCode::InvalidPath, range,
            "Source context path must be a nonempty JSON string without control characters."};
}

bool isOrdinaryImport(const slang::syntax::SyntaxNode& node, uint32_t bufferId) {
    for (size_t index = 0; index < node.getChildCount(); ++index) {
        if (auto child = node.childNode(index)) {
            if (!isOrdinaryImport(*child, bufferId))
                return false;
        }
        else if (auto token = node.childToken(index)) {
            if (token.isMissing() || token.location().buffer().getId() != bufferId)
                return false;
        }
    }
    return true;
}

} // namespace

SourceContextHints parseSourceContextHints(slang::SourceBuffer buffer,
                                           slang::SourceManager& sourceManager) {
    SourceContextHints result;
    result.bufferId = buffer.id.getId();
    slang::BumpAllocator allocator;
    slang::Diagnostics diagnostics;
    slang::parsing::LexerOptions options;
    // A malformed token elsewhere in an editor buffer must not prevent comment
    // recognition later in that buffer. This pass never publishes lexer errors.
    options.maxErrors = std::numeric_limits<uint32_t>::max();
    slang::parsing::Lexer lexer(buffer, allocator, diagnostics, sourceManager, options);
    bool leading = true;
    for (;;) {
        auto token = lexer.lex();
        for (const auto& trivia : token.trivia()) {
            if (trivia.kind != slang::parsing::TriviaKind::LineComment)
                continue;
            auto raw = trivia.getRawText();
            // Raw Lexer comments point directly into the supplied physical
            // buffer. No preprocessor or synthesized trivia participates here.
            size_t begin = size_t(raw.data() - buffer.data.data());
            SourceContextHintRange range{result.bufferId, begin, begin + raw.size()};
            if (leading) {
                if (auto payload = directivePayload(raw, "owner")) {
                    auto path = parsePath(*payload);
                    if (!path) {
                        result.issues.push_back(invalidPath(range));
                        result.invalidOwner = true;
                    }
                    else if (result.owner && result.owner->path != *path) {
                        result.issues.push_back({SourceContextHintIssueCode::ConflictingOwner,
                                                 range,
                                                 "Conflicting source context owner directives."});
                        result.invalidOwner = true;
                    }
                    else if (!result.owner) {
                        result.owner = SourceContextPathHint{std::move(*path), range};
                    }
                }
            }
            if (auto payload = directivePayload(raw, "source"))
                result.sourceComments.push_back({std::string(*payload), range});
        }
        // EOF carries trailing comments, so inspect its trivia before stopping.
        if (token.kind == slang::parsing::TokenKind::EndOfFile)
            break;
        leading = false;
    }
    if (result.invalidOwner)
        result.owner.reset();
    return result;
}

SourceContextImportHints bindSourceImportHints(const slang::syntax::SyntaxTree& tree,
                                               const SourceContextHints& hints) {
    SourceContextImportHints result;
    const auto& sourceManager = tree.sourceManager();
    for (const auto* declaration : tree.getMetadata().packageImports) {
        if (!isOrdinaryImport(*declaration, hints.bufferId))
            continue;
        const auto end = declaration->semi.range().end();
        if (end.buffer().getId() != hints.bufferId)
            continue;
        auto source = sourceManager.getSourceText(end.buffer());
        for (const auto& comment : hints.sourceComments) {
            if (comment.range.bufferId != hints.bufferId || comment.range.begin < end.offset() ||
                comment.range.end > source.size())
                continue;
            auto gap = source.substr(end.offset(), comment.range.begin - end.offset());
            if (!std::ranges::all_of(gap, isHorizontalSpace))
                continue;
            std::string_view packageName;
            bool ambiguous = false;
            for (const auto* item : declaration->items) {
                auto name = item->package.valueText();
                if (name.empty() || (!packageName.empty() && name != packageName)) {
                    ambiguous = true;
                    break;
                }
                packageName = name;
            }
            if (ambiguous || packageName.empty()) {
                result.issues.push_back(
                    {SourceContextHintIssueCode::AmbiguousImport, comment.range,
                     "Source context hint requires one imported package name."});
                continue;
            }
            auto path = parsePath(comment.pathText);
            if (!path) {
                result.issues.push_back(invalidPath(comment.range));
                continue;
            }
            result.imports.push_back({std::string(packageName), std::move(*path), comment.range});
        }
    }
    return result;
}

} // namespace server
