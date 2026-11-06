// SPDX-License-Identifier: MIT
#pragma once
#include "lsp/RequestContext.h"
#include <span>
#include <string>
namespace slang::parsing {
struct MacroRefMetadata;
}
namespace slang::syntax {
struct DefineDirectiveSyntax;
}
namespace server {
void validateBodyPreprocessing(const std::string&);
void validateMacroPreprocessing(
    const slang::parsing::MacroRefMetadata&,
    std::span<const slang::syntax::DefineDirectiveSyntax* const> inherited = {},
    const lsp::RequestContext& ctx = {});
} // namespace server
