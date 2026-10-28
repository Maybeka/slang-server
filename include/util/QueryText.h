// SPDX-License-Identifier: MIT
#pragma once
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/ScopeGuard.h"

namespace server {
// Private parse bytes follow their syntax / compilation consumers, never the
// entire shared SourceManager. Query handles are still fenced by version/epoch.
class QueryText {
public:
    QueryText(slang::SourceManager& sources, std::string_view path, std::string_view text) :
        buffer(sources.assignQueryText(path, text)), sources(sources) {
        auto cleanup = slang::ScopeGuard([&] { sources.releaseQueryText(buffer.id); });
        lease = sources.retainQueryText(buffer.id);
        cleanup.release();
    }
    std::shared_ptr<slang::syntax::SyntaxTree> parse(
        const slang::Bag& options, slang::syntax::SyntaxTree::MacroList macros = {}) const {
        auto tree = [&] {
            const auto pp = options.get<slang::parsing::PreprocessorOptions>();
            if (!pp || !pp->trackMacroAccesses)
                return slang::syntax::SyntaxTree::fromBuffer(buffer, sources, options, macros);
            // EOF here closes only this private fragment, never the original
            // compilation unit. Do not allocate inherited-name facts per edit
            // or expose a fragment as a full-source absence proof.
            auto local = options;
            auto untracked = *pp;
            untracked.trackMacroAccesses = false;
            local.set(untracked);
            return slang::syntax::SyntaxTree::fromBuffer(buffer, sources, local, macros);
        }();
        auto result = tree.get();
        return std::shared_ptr<slang::syntax::SyntaxTree>(result, [tree = std::move(tree),
                                                                   lease = lease](auto*) mutable {
            tree.reset();
            lease.reset();
        });
    }
    const slang::SourceBuffer buffer;

private:
    slang::SourceManager& sources;
    std::shared_ptr<void> lease;
};
} // namespace server
