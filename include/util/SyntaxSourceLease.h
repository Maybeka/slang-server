// SPDX-License-Identifier: MIT
#pragma once
#include <memory>
#include <vector>

#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"

namespace server {
// CST tokens borrow file / command-line macro text. Tie that storage to the
// published tree, including retained old analyses and cancelled replay recipes.
inline std::shared_ptr<slang::syntax::SyntaxTree> retainSyntaxSourceStorage(
    slang::SourceManager& sources, std::shared_ptr<slang::syntax::SyntaxTree> tree) {
    struct Lease {
        std::vector<std::shared_ptr<void>> sources, macros;
        std::shared_ptr<slang::syntax::SyntaxTree> owner;
    };
    auto lease = std::make_shared<Lease>(sources.retainBuffers(tree->getSourceBufferIds()),
                                        sources.retainBuffers(tree->getPredefineBufferIds()),
                                        std::move(tree));
    // Managed payload destruction releases owner before its bytes at the last
    // strong reference. Expired weak caches retain only the control block.
    return std::shared_ptr<slang::syntax::SyntaxTree>(lease, lease->owner.get());
}
} // namespace server
