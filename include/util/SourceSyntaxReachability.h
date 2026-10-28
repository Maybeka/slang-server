// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <vector>

#include "slang/syntax/AllSyntax.h"
#include "slang/text/SourceManager.h"

namespace server {
// A parsed declaration whose two physical endpoints belong to another file
// cannot contain this exact include occurrence unless that file is an ancestor.
// Unknown / synthetic endpoints retain the complete traversal.
class SourceSyntaxReachability {
public:
    SourceSyntaxReachability(const slang::SourceManager& sources, slang::BufferID target) :
        sources(sources) {
        for (auto buffer = target; buffer.valid();) {
            if (std::ranges::find(ancestors, buffer) != ancestors.end())
                break;
            ancestors.push_back(buffer);
            const auto parent = sources.getIncludedFrom(buffer);
            if (!parent.valid())
                break;
            buffer = sources.getFullyExpandedLoc(parent).buffer();
        }
    }
    bool excludes(const slang::syntax::SyntaxNode& node) const {
        if (ancestors.empty())
            return false;
        using slang::syntax::SyntaxKind;
        switch (node.kind) {
            case SyntaxKind::ClassDeclaration:
            case SyntaxKind::ModuleDeclaration:
            case SyntaxKind::InterfaceDeclaration:
            case SyntaxKind::ProgramDeclaration:
            case SyntaxKind::PackageDeclaration:
                break;
            default:
                return false;
        }
        const auto first = node.getFirstToken().location();
        const auto last = node.getLastToken().location();
        if (!first.valid() || !last.valid() || first == slang::SourceLocation::NoLocation ||
            last == slang::SourceLocation::NoLocation)
            return false;
        const auto begin = sources.getFullyExpandedLoc(first).buffer();
        const auto end = sources.getFullyExpandedLoc(last).buffer();
        return begin.valid() && begin == end &&
               std::ranges::find(ancestors, begin) == ancestors.end();
    }

private:
    const slang::SourceManager& sources;
    std::vector<slang::BufferID> ancestors;
};
} // namespace server
