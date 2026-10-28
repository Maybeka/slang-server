// SPDX-License-Identifier: MIT
#pragma once
#include "lsp/RequestContext.h"
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

namespace server {
// Optional acceleration was unavailable. Callers must release this invocation's
// descriptors before retrying the complete, ordinary physical proof.
struct DirectoryProofUnavailable : std::runtime_error {
    DirectoryProofUnavailable() : std::runtime_error("directory proof resources unavailable") {}
};

// Retains names only, never directory metadata, descriptors or successful proofs.
class DirectoryProofPlan {
    struct Impl;
    std::unique_ptr<Impl> impl;
    explicit DirectoryProofPlan(std::unique_ptr<Impl>);
public:
    ~DirectoryProofPlan();
    static std::unique_ptr<DirectoryProofPlan> build(std::span<const std::string_view> missing,
                                                    size_t budgetBytes);
    size_t chargedBytes() const;
    // A name-plan token, never evidence that its directory or leaf is absent.
    size_t group(std::string_view path) const;
    class Session {
        struct State;
        std::unique_ptr<State> state;
    public:
        Session(const DirectoryProofPlan&, const lsp::RequestContext&);
        ~Session();
        Session(const Session&) = delete;
        bool covers(std::string_view path) const;
        bool covers(size_t group) const;
        std::optional<int> directory(std::string_view parent) const;
        bool valid(const lsp::RequestContext&) const;
    };
};
} // namespace server
