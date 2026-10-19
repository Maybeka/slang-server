// Private, value-only diagnostic job protocol. No compiler object crosses it.
#pragma once
#include "lsp/LspTypes.h"
#include "lsp/URI.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace server {
struct DiagnosticRootInput {
    uint32_t file;
    uint64_t sortKey;
};
struct DiagnosticTreeInput {
    std::vector<DiagnosticRootInput> roots;
    // File identities in SyntaxTree::getSourceBufferIds order. Reparse must
    // reproduce this graph, including repeated occurrences of one header.
    std::vector<uint32_t> sources;
    std::vector<uint32_t> predefines;
};
struct DiagnosticTargetInput {
    URI uri;
    std::optional<int> version;
    uint32_t tree;
    uint32_t bufferOrdinal;
    std::vector<uint32_t> trees;
    bool packageOnly = false;
};
struct DiagnosticJobMetadata {
    uint32_t schema = 1;
    int parentPid = 0;
    bool utf8Positions = false;
    std::vector<DiagnosticTreeInput> trees;
    std::vector<DiagnosticTargetInput> targets;
    std::optional<uint32_t> preincludeTree;
    std::vector<URI> preincludeUris;
    std::vector<URI> clearUris;
};
struct RenderedDiagnosticDocument {
    URI uri;
    std::optional<int> version;
    std::vector<lsp::Diagnostic> diagnostics;
};
struct RenderedInactiveRegions {
    URI uri;
    std::vector<lsp::Range> regions;
};
struct DiagnosticJobOutput {
    uint32_t schema = 1;
    std::vector<RenderedDiagnosticDocument> documents;
    std::vector<RenderedInactiveRegions> inactiveRegions;
};
namespace diagnostic_snapshot {
struct Input;
}
std::string encodeDiagnosticJob(const DiagnosticJobMetadata&, const diagnostic_snapshot::Input&);
struct DiagnosticJobStatistics {
    size_t parsedTrees = 0;
    size_t prefixCaptureParses = 0;
    size_t reusedPrefixes = 0;
};
DiagnosticJobOutput executeDiagnosticJob(std::string_view input,
                                         const std::function<void()>& beforeSemantic = {},
                                         DiagnosticJobStatistics* statistics = nullptr);

// Serial worker-local ownership only. No SyntaxTree, SourceManager or physical ID
// crosses the value-only job protocol. Destroy the session on parent epoch change.
class DiagnosticJobSession {
public:
    DiagnosticJobSession();
    ~DiagnosticJobSession();
    DiagnosticJobSession(const DiagnosticJobSession&) = delete;
    DiagnosticJobSession& operator=(const DiagnosticJobSession&) = delete;
    DiagnosticJobOutput execute(std::string_view input,
                                const std::function<void()>& beforeSemantic = {},
                                DiagnosticJobStatistics* statistics = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Worker mode is entered before constructing the LSP server or its threads.
// Writes only framed protocol output, then exits without static teardown.
[[noreturn]] void runDiagnosticWorker(const std::function<void()>& beforeSemantic = {},
                                      bool persistent = false);
} // namespace server
