// SPDX-License-Identifier: MIT
#pragma once
#include "completions/MemberCompletions.h"
#include <cstdint>
#include <optional>
#include <rfl/Generic.hpp>
#include <string>

#include "slang/ast/Symbol.h"

// Value-only completion identities shared by capture and response adaptation.
// These structures contain no model or source-owner references.
namespace server::method_completion {
struct Formatting : completions::MemberCompletionQuery {
    using MemberCompletionQuery::addCompletions;
    using MemberCompletionQuery::updateCompletionEditText;
};
struct Data {
    std::string documentUri, symbolPath, symbolName;
    uint32_t bufferId = 0;
    uint64_t offset = 0;
    slang::ast::SymbolKind symbolKind = slang::ast::SymbolKind::Unknown;
    bool labelOnly = false;
    std::optional<uint32_t> receiverBufferId;
    std::optional<uint64_t> receiverOffset;
    std::optional<uint64_t> receiverTextIdentity;
    bool operator==(const Data&) const = default;
};
struct QueryPayload {
    uint64_t contextEpoch;
    rfl::Generic payload;
    std::optional<uint64_t> methodBodyCertificate;
    std::optional<int> methodBodyVersion;
};
struct NativePayload {
    uint64_t contextEpoch;
    rfl::Generic payload;
};
} // namespace server::method_completion
