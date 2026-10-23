// SPDX-License-Identifier: MIT
#pragma once

#include "ast/HierarchicalView.h"
#include "lsp/RequestContext.h"
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "slang/ast/Compilation.h"
#include "slang/syntax/SyntaxTree.h"

namespace server {
/// Value-only hierarchy results. No compiler pointers or old source managers are retained.
/// Cross-edit reuse is limited to certified, declaration-free automatic class-task bodies.
class FrozenInstanceCache {
public:
    using Tree = std::shared_ptr<slang::syntax::SyntaxTree>;
    using Witness = std::function<bool(const lsp::RequestContext&)>;
    struct Selection {
        std::string instance;
        bool resolved = false;
        std::string reason;
        std::vector<std::string> modules;
    };
    struct Statistics {
        uint64_t preparations = 0, hits = 0, crossEditHits = 0, invalidations = 0;
        uint64_t evictions = 0, declines = 0;
        uint64_t fullCertifications = 0, incrementalCertifications = 0;
        uint64_t visitedSyntaxNodes = 0, lastVisitedSyntaxNodes = 0, reusedSourceFiles = 0;
        uint64_t requestWitnessReuses = 0;
        size_t retainedBytes = 0, eligibleTaskBodies = 0;
        std::string lastReason;
    };
    struct Certificate {
        std::string generation;
        std::map<std::string, std::string_view> sources;
    };
    FrozenInstanceCache(std::vector<std::string> paths, size_t budgetBytes,
                        slang::SourceManager& sourceManager, Witness witness,
                        Witness requestWitness = {});
    FrozenInstanceCache(const FrozenInstanceCache&) = delete;
    FrozenInstanceCache& operator=(const FrozenInstanceCache&) = delete;
    void resolveSelections(slang::ast::Compilation& compilation,
                           const lsp::RequestContext& ctx = {});
    std::vector<Selection> prepareSelections(slang::ast::Compilation& compilation,
                                             const lsp::RequestContext& ctx) const;
    void commitSelections(std::vector<Selection> selections) noexcept {
        m_selections.swap(selections);
    }
    bool selected(std::string_view path) const;
    std::vector<std::string> modules() const;
    const std::vector<Selection>& selections() const { return m_selections; }
    const Statistics& statistics() const { return m_statistics; }
    size_t budgetBytes() const { return m_budgetBytes; }
    void clear(std::string reason);
    std::optional<Certificate> certify(std::span<const Tree> trees, const lsp::RequestContext& ctx,
                                       bool syntaxUnchanged = false);
    std::optional<std::string> find(std::string_view key, const Certificate& certificate);
    void store(std::string key, std::string payload, const std::vector<hier::HierItem_t>& values,
               const Certificate& certificate, const lsp::RequestContext& ctx);

private:
    struct Entry {
        std::string key, payload, generation;
        std::map<std::string, std::string> locations;
        size_t bytes = 0;
    };
    void decline(std::string reason);
    size_t baseBytes() const;
    struct SourceProof {
        slang::BufferID buffer;
        std::string normalized;
        size_t taskBodies = 0;
    };
    std::vector<Selection> m_selections;
    size_t m_budgetBytes;
    /// Non-owning, fixed for this cache's lifetime; its host compilation dies before this manager.
    slang::SourceManager& m_sourceManager;
    Witness m_witness;
    Witness m_requestWitness;
    std::string m_certificate, m_checkedGeneration;
    std::map<std::string, SourceProof> m_sourceProofs;
    std::list<Entry> m_entries;
    Statistics m_statistics;
};
} // namespace server
