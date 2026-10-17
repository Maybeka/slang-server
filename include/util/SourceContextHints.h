// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace slang {
class SourceManager;
struct SourceBuffer;
namespace syntax {
class SyntaxTree;
}
} // namespace slang

namespace server {

// Half-open physical byte range. Numeric IDs deliberately avoid retaining the
// debug-only string_view carried by slang::BufferID and SourceLocation.
struct SourceContextHintRange {
    uint32_t bufferId = 0;
    size_t begin = 0;
    size_t end = 0;
};

enum class SourceContextHintIssueCode { InvalidPath, ConflictingOwner, AmbiguousImport };

struct SourceContextHintIssue {
    SourceContextHintIssueCode code;
    SourceContextHintRange range;
    std::string message;
};

struct SourceContextPathHint {
    std::string path;
    SourceContextHintRange range;
};

struct SourceContextSourceComment {
    // The owned text following "slang-server: source". Parse this only after
    // binding to an active import, so inactive examples cannot produce issues.
    std::string pathText;
    SourceContextHintRange range;
};

struct SourceContextHints {
    uint32_t bufferId = 0;
    std::optional<SourceContextPathHint> owner;
    bool invalidOwner = false;
    std::vector<SourceContextSourceComment> sourceComments;
    std::vector<SourceContextHintIssue> issues;
};

struct SourceContextImportHint {
    std::string packageName;
    std::string path;
    SourceContextHintRange range;
};

struct SourceContextImportHints {
    std::vector<SourceContextImportHint> imports;
    std::vector<SourceContextHintIssue> issues;
};

// Reads only genuine physical line comments. Owner directives are recognized
// before the first nontrivia token; source comments are bound separately below.
// The buffer must be NUL-terminated, as are SourceManager's source buffers.
// No paths are resolved, files loaded, macros expanded, or diagnostics published.
SourceContextHints parseSourceContextHints(slang::SourceBuffer buffer,
                                           slang::SourceManager& sourceManager);

// Accepts a source comment only immediately after an active, complete, ordinary
// import declaration in the same physical buffer and on the same line. One
// distinct package name is required, including for comma-separated imports.
// The supplied tree must contain the same immutable buffer used by the scan.
// The caller validates declarations, canonical paths, and cross-file conflicts.
SourceContextImportHints bindSourceImportHints(const slang::syntax::SyntaxTree& tree,
                                               const SourceContextHints& hints);

} // namespace server
