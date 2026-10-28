// SPDX-License-Identifier: MIT
#pragma once

#include "lsp/LspTypes.h"
#include <span>
#include <string_view>

#include "slang/util/SmallVector.h"

namespace server {
/// Apply every change in protocol order to owned, null-terminated text.
/// currentText includes the terminal null, as returned by SlangDoc::getText().
/// Does not allocate a SourceManager buffer or change the input on failure.
slang::SmallVector<char> applyDocumentChanges(
    std::string_view currentText,
    std::span<const lsp::TextDocumentContentChangeEvent> contentChanges, bool utf8Columns);
} // namespace server
