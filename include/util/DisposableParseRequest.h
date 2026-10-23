// SPDX-License-Identifier: MIT
#pragma once
#include "lsp/RequestContext.h"

#include "slang/parsing/Preprocessor.h"
#include "slang/util/Bag.h"

namespace server {
// Only use for syntax parses whose incomplete result can be discarded. Syntax
// option bags may escape into caches, so no callback may retain a live request.
class DisposableParseRequest {
    std::shared_ptr<std::optional<lsp::RequestContext>> m_request;

public:
    DisposableParseRequest(const lsp::RequestContext& ctx, bool enabled) {
        if (enabled && ctx)
            m_request = std::make_shared<std::optional<lsp::RequestContext>>(ctx);
    }
    ~DisposableParseRequest() { finish(); }
    void finish() {
        if (m_request)
            m_request->reset();
    }
    DisposableParseRequest(const DisposableParseRequest&) = delete;
    DisposableParseRequest& operator=(const DisposableParseRequest&) = delete;
    std::function<void()> callback() const {
        if (!m_request)
            return {};
        return [request = m_request] {
            if (*request)
                (*request)->checkpoint("during disposable syntax parsing");
        };
    }
    void apply(slang::Bag& options) const {
        if (m_request)
            options.insertOrGet<slang::parsing::PreprocessorOptions>().parseCheckpoint = callback();
    }
};
} // namespace server
