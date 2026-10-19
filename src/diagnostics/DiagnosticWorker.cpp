#include "diagnostics/DiagnosticWorker.h"

#include "ServerDiagClient.h"
#include "diagnostics/DiagnosticProcess.h"
#include "diagnostics/DiagnosticSnapshotCodec.h"
#include "document/ShallowAnalysis.h"
#include "util/Converters.h"
#include "util/SlangExtensions.h"
#include <cstdio>
#include <cstdlib>
#if defined(__linux__) || defined(__APPLE__)
#    include <unistd.h>
#endif
#include <filesystem>
#include <rfl/json.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "slang/diagnostics/CompilationDiags.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/util/OS.h"

namespace server {
namespace {
constexpr uint64_t metadataLimit = 16 * 1024 * 1024;
diagnostic_snapshot::Limits workerLimits() {
    diagnostic_snapshot::Limits limits;
    limits.maxMetadataBytes = 8 * 1024 * 1024;
    limits.maxFileBytes = 48 * 1024 * 1024;
    limits.maxTotalFileBytes = 48 * 1024 * 1024;
    return limits;
}
void putLength(std::ostream& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.put(char(value >> shift));
}
uint64_t getLength(std::istream& in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        int c = in.get();
        if (c == EOF)
            throw std::runtime_error("truncated diagnostic job metadata");
        value = (value << 8) | uint8_t(c);
    }
    return value;
}
SourceLocation attributedLocation(const SourceManager& sm, SourceLocation loc) {
    while (sm.isMacroLoc(loc))
        loc = sm.isMacroArgLoc(loc) ? sm.getOriginalLoc(loc) : sm.getExpansionLoc(loc);
    return loc;
}
struct DiagnosticLibraryContext {
    std::shared_ptr<diagnostic_snapshot::ReconstructedOptions> options;
    std::vector<std::shared_ptr<diagnostic_snapshot::ReconstructedOptions>> treeOptions;
    std::unique_ptr<SourceManager> sm;
    std::shared_ptr<syntax::SyntaxTree> prefix;
    std::string signature, root;
    uint64_t sortKey = 0;
    size_t budget = 0;
};
struct DiagnosticLibrarySessionState {
    std::unique_ptr<DiagnosticLibraryContext> context;
    bool declined = false;
};
struct PrefixReuseFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void workerCacheTrace(const char* event) {
    if (auto* path = std::getenv("SLANG_UVM_WORKER_CACHE_TRACE"))
        if (auto* file = std::fopen(path, "a")) {
            std::fprintf(file, "%ld\t%s\n", long(slang::OS::getpid()), event);
            std::fclose(file);
        }
}
} // namespace

std::string encodeDiagnosticJob(const DiagnosticJobMetadata& metadata,
                                const diagnostic_snapshot::Input& input) {
    auto json = rfl::json::write<rfl::UnderlyingEnums>(metadata);
    if (json.size() > metadataLimit)
        throw std::runtime_error("diagnostic job metadata exceeds limit");
    std::ostringstream out(std::ios::binary);
    putLength(out, json.size());
    out.write(json.data(), std::streamsize(json.size()));
    diagnostic_snapshot::writeInput(out, input, workerLimits());
    return std::move(out).str();
}

static DiagnosticJobOutput executeJob(std::string_view bytes,
                                      const std::function<void()>& beforeSemantic,
                                      DiagnosticJobStatistics* jobStatistics,
                                      DiagnosticLibrarySessionState* session) {
    std::istringstream in(std::string(bytes), std::ios::binary);
    auto length = getLength(in);
    if (length > metadataLimit || length > bytes.size())
        throw std::runtime_error("invalid diagnostic metadata length");
    std::string json(size_t(length), '\0');
    if (!in.read(json.data(), std::streamsize(length)))
        throw std::runtime_error("truncated diagnostic metadata");
    auto metadata = rfl::json::read<DiagnosticJobMetadata, rfl::UnderlyingEnums>(json).value();
    auto input = diagnostic_snapshot::readInput(in, workerLimits());
    if (in.peek() != EOF || metadata.schema != 1 ||
        metadata.trees.size() != input.treeOptions.size() || metadata.targets.empty())
        throw std::runtime_error("invalid diagnostic job schema");

    bool eligible = false;
    std::string signature, rootPath;
    uint64_t sortKey = 0;
    if (session && !session->declined && metadata.trees.size() == 1 &&
        metadata.trees[0].roots.size() > 1 && input.treeOptions[0].preprocessor &&
        input.treeOptions[0].preprocessor->captureUvmPrefix &&
        input.treeOptions[0].preprocessor->keywordMapping.empty()) {
        const auto& root = metadata.trees[0].roots[0];
        if (root.file && root.file <= input.sources.files.size()) {
            rootPath = input.sources.files[root.file - 1].fullPath;
            auto filename = std::filesystem::path(rootPath).filename();
            auto& explicitRoot = input.treeOptions[0].preprocessor->uvmPrefixRoot;
            eligible = explicitRoot.empty() ? filename == "uvm.sv" || filename == "uvm_pkg.sv"
                                            : rootPath == explicitRoot;
            sortKey = root.sortKey;
        }
        if (eligible)
            signature = rfl::json::write(input.options) + rfl::json::write(input.treeOptions);
    }
    bool reused = false;
    if (session && session->context) {
        auto& context = *session->context;
        auto bytes = context.sm->getSourceLocationTableBytes() + context.sm->getMemoryUsage();
        bool samePredefines = eligible;
        if (samePredefines) {
            auto ids = context.prefix->getPredefineBufferIds();
            const auto& expected = metadata.trees[0].predefines;
            samePredefines = ids.size() == expected.size();
            for (size_t n = 0; samePredefines && n < ids.size(); ++n)
                samePredefines = expected[n] && expected[n] <= input.sources.files.size() &&
                                 context.sm->getFullPath(ids[n]).string() ==
                                     input.sources.files[expected[n] - 1].fullPath;
        }
        if (eligible && context.signature == signature && context.root == rootPath &&
            context.sortKey == sortKey && bytes <= context.budget && samePredefines) {
            auto ids = context.prefix->getSourceBufferIds();
            std::vector<BufferID> retained(ids.begin(), ids.end());
            auto predefines = context.prefix->getPredefineBufferIds();
            retained.insert(retained.end(), predefines.begin(), predefines.end());
            reused = context.sm->tryUpdateSnapshot(input.sources, retained);
        }
        if (!reused) {
            workerCacheTrace(!eligible                        ? "miss-ineligible"
                             : context.signature != signature ? "miss-options"
                             : context.root != rootPath       ? "miss-root"
                             : context.sortKey != sortKey     ? "miss-sort-key"
                             : bytes > context.budget         ? "miss-budget"
                             : !samePredefines                ? "miss-generated-sequence"
                                                              : "miss-snapshot");
            workerCacheTrace("context-reset");
            session->context.reset();
        }
    }
    // Capture only when a worker session can consume the next checkpoint. One-shot
    // workers and native fallbacks keep the original capture-free parsing purpose.
    if (input.options.preprocessor)
        input.options.preprocessor->captureUvmPrefix = false;
    for (auto& opt : input.treeOptions)
        if (opt.preprocessor)
            opt.preprocessor->captureUvmPrefix = eligible && !reused;

    // Backing option storage precedes every tree / context retaining its views.
    auto options = std::make_shared<diagnostic_snapshot::ReconstructedOptions>(
        std::move(input.options));
    std::vector<std::shared_ptr<diagnostic_snapshot::ReconstructedOptions>> treeOptions;
    for (auto& opt : input.treeOptions)
        treeOptions.push_back(
            std::make_shared<diagnostic_snapshot::ReconstructedOptions>(std::move(opt)));
    auto fresh = reused ? nullptr : std::make_unique<SourceManager>();
    if (fresh)
        fresh->loadSnapshot(input.sources);
    SourceManager& sm = reused ? *session->context->sm : *fresh;
    std::vector<std::shared_ptr<syntax::SyntaxTree>> trees;
    for (size_t i = 0; i < metadata.trees.size(); ++i) {
        const auto& spec = metadata.trees[i];
        if (spec.roots.empty())
            throw std::runtime_error("diagnostic tree has no roots");
        std::vector<SourceBuffer> roots;
        for (auto root : spec.roots)
            roots.push_back(sm.createRootBuffer(root.file, root.sortKey));
        std::shared_ptr<syntax::SyntaxTree> tree;
        try {
            sm.setSnapshotGeneratedSequence(spec.predefines);
            if (reused) {
                // Recreate the recorded CLI inputs, validating bytes and generated-source
                // order, before resuming with the retained macro definitions.
                BumpAllocator arena;
                Diagnostics diagnostics;
                parsing::Preprocessor proof(sm, arena, diagnostics, treeOptions[i]->bag());
                if (!diagnostics.empty())
                    throw PrefixReuseFailure("generated CLI proof emitted diagnostics");
                sm.finishSnapshotGeneratedSequence();
                sm.setSnapshotGeneratedSequence({});
                auto suffixOptions = treeOptions[i]->bag();
                auto pp = suffixOptions.getOrDefault<parsing::PreprocessorOptions>();
                pp.predefines.clear();
                suffixOptions.set(pp);
                auto prefix = session->context->prefix;
                auto suffix = syntax::SyntaxTree::afterCachedPrefix(
                    prefix, std::span<const SourceBuffer>(roots).subspan(1), suffixOptions);
                validateUvmMacroEnvironment(*prefix, *suffix);
                tree = syntax::SyntaxTree::withCachedPrefix(prefix, suffix);
            }
            else {
                tree = syntax::SyntaxTree::fromBuffers(roots, sm, treeOptions[i]->bag());
                if (jobStatistics) {
                    ++jobStatistics->parsedTrees;
                    jobStatistics->prefixCaptureParses +=
                        tree->options()
                            .getOrDefault<parsing::PreprocessorOptions>()
                            .captureUvmPrefix;
                }
            }
            sm.finishSnapshotGeneratedSequence();
        }
        catch (const std::exception& error) {
            if (reused)
                throw PrefixReuseFailure(error.what());
            throw;
        }
        auto ids = tree->getSourceBufferIds();
        auto graphFailure = [&](const char* reason) {
            if (reused)
                throw PrefixReuseFailure(reason);
            throw std::runtime_error(reason);
        };
        if (ids.size() != spec.sources.size())
            graphFailure("diagnostic snapshot graph shape changed");
        for (size_t n = 0; n < ids.size(); ++n) {
            auto id = spec.sources[n];
            if (id == 0 || id > input.sources.files.size())
                graphFailure("invalid diagnostic source identity");
            const auto& file = input.sources.files[id - 1];
            if (sm.getFullPath(ids[n]).string() != file.fullPath ||
                sm.getSourceText(ids[n]) != file.bytes)
                graphFailure("diagnostic snapshot source identity changed");
        }
        trees.push_back(std::move(tree));
    }

    SlangLspClient client;
    client.capabilities.utf8Positions = metadata.utf8Positions;
    auto rendered = std::make_shared<ServerDiagClient>(sm, client);
    rendered->disableDestructorPublication();
    std::unordered_map<URI, URI> targetUris;
    for (const auto& target : metadata.targets) {
        auto ids = trees.at(target.tree)->getSourceBufferIds();
        if (target.bufferOrdinal >= ids.size())
            throw std::runtime_error("invalid target occurrence");
        targetUris.emplace(URI::fromFile(sm.getFullPath(ids[target.bufferOrdinal])), target.uri);
    }
    rendered->setUriResolver([&targetUris](const URI& physical) {
        auto found = targetUris.find(physical);
        return found == targetUris.end() ? physical : found->second;
    });
    DiagnosticEngine engine(sm);
    engine.applySettings(input.settings);
    engine.addClient(rendered);
    std::unordered_set<URI> preincludeUris(metadata.preincludeUris.begin(),
                                           metadata.preincludeUris.end());
    for (const auto& uri : metadata.clearUris)
        rendered->replaceRendered(uri, {});
    for (const auto& target : metadata.targets)
        rendered->replaceRendered(target.uri, {});
    if (metadata.preincludeTree) {
        auto tree = trees.at(*metadata.preincludeTree);
        for (auto id : tree->getSourceBufferIds())
            engine.setMappingsFromPragmas(id);
        for (const auto& diag : tree->diagnostics())
            engine.issue(diag);
    }
    std::unordered_set<std::string> explicitTargetPaths;
    for (const auto& target : metadata.targets) {
        auto ids = trees.at(target.tree)->getSourceBufferIds();
        if (target.bufferOrdinal >= ids.size())
            throw std::runtime_error("invalid target occurrence");
        explicitTargetPaths.insert(sm.getFullPath(ids[target.bufferOrdinal]).string());
    }
    DiagnosticJobOutput result;
    SemanticDiagnosticBatch::Statistics statistics;
    SemanticDiagnosticBatch batch(statistics);
    for (const auto& target : metadata.targets) {
        auto tree = trees.at(target.tree);
        auto ids = tree->getSourceBufferIds();
        if (target.bufferOrdinal >= ids.size())
            throw std::runtime_error("invalid diagnostic target occurrence");
        auto buffer = ids[target.bufferOrdinal];
        std::vector<std::shared_ptr<syntax::SyntaxTree>> dependencies;
        for (auto index : target.trees)
            dependencies.push_back(trees.at(index));
        auto analysis = std::make_shared<ShallowAnalysis>(sm, buffer, tree, options->bag(),
                                                          dependencies, nullptr,
                                                          lsp::RequestContext{},
                                                          target.packageOnly);
        if (!analysis->getCompilation()->getRoot().topInstances.empty())
            throw std::runtime_error("isolated diagnostic job requires package exploration");
        engine.setMappingsFromPragmas(buffer);
        bool included = buffer != ids.front();
        for (const auto& diag : tree->diagnostics()) {
            auto loc = attributedLocation(sm, diag.location);
            auto path = loc.valid() ? sm.getFullPath(loc.buffer()) : std::filesystem::path{};
            if (!path.empty() && preincludeUris.contains(URI::fromFile(path)))
                continue;
            // An explicitly open included target supplies its own complete parse
            // diagnostics. Do not append the owner's incidental copy as well.
            if (!path.empty() && explicitTargetPaths.contains(path.string()) &&
                path != sm.getFullPath(buffer))
                continue;
            if (!included || loc.buffer() == buffer)
                engine.issue(diag);
        }
        if (beforeSemantic)
            beforeSemantic();
        auto semantic = batch.collect(analysis);
        for (const auto& diag : semantic.diagnostics) {
            if (attributedLocation(sm, diag.location).buffer() == buffer &&
                diag.code != slang::diag::MaxInstanceDepthExceeded)
                engine.issue(diag);
        }
        if (!semantic.reused) {
            for (const auto& diag : analysis->getAnalysisDiags())
                if (attributedLocation(sm, diag.location).buffer() == buffer)
                    engine.issue(diag);
        }
        RenderedInactiveRegions inactive{target.uri, {}};
        for (auto region : analysis->syntaxes.disabledRegions)
            inactive.regions.push_back(toRange(region, sm));
        result.inactiveRegions.push_back(std::move(inactive));
    }
    if (sm.snapshotIncomplete())
        throw std::runtime_error("incomplete diagnostic snapshot");
    for (const auto& [uri, diagnostics] : rendered->renderedDiagnostics()) {
        std::optional<int> version;
        for (const auto& target : metadata.targets)
            if (target.uri == uri)
                version = target.version;
        result.documents.push_back({uri, version, diagnostics});
    }
    if (reused) {
        workerCacheTrace("prefix-reused");
        if (jobStatistics)
            ++jobStatistics->reusedPrefixes;
    }
    if (eligible && !reused) {
        auto prefix = syntax::SyntaxTree::preparedUvmPrefix(trees[0]);
        if (prefix && prefix->isPrefixResumeSafe().value_or(false) &&
            !prefix->getPrefixSourceEffects().value_or(true) &&
            prefix->root().kind == syntax::SyntaxKind::CompilationUnit) {
            const auto& unit = prefix->root().as<syntax::CompilationUnitSyntax>();
            if (unit.members.size() == 1 &&
                unit.members[0]->kind == syntax::SyntaxKind::PackageDeclaration &&
                unit.members[0]->as<syntax::ModuleDeclarationSyntax>().header->name.valueText() ==
                    "uvm_pkg") {
                try {
                    validateUvmMacroEnvironment(*prefix, *trees[0]);
                    auto context = std::make_unique<DiagnosticLibraryContext>();
                    context->options = options;
                    context->treeOptions = treeOptions;
                    context->prefix = std::move(prefix);
                    context->signature = std::move(signature);
                    context->root = std::move(rootPath);
                    context->sortKey = sortKey;
                    context->budget = std::max<size_t>(16 * 1024 * 1024,
                                                       2 * (sm.getSourceLocationTableBytes() +
                                                            sm.getMemoryUsage()));
                    context->sm = std::move(fresh);
                    session->context = std::move(context);
                    workerCacheTrace("prefix-captured");
                }
                catch (const std::exception&) {
                    workerCacheTrace("capture-declined");
                }
            }
        }
        if (!session->context) {
            session->declined = true;
            workerCacheTrace("capture-declined");
        }
    }
    return result;
}

DiagnosticJobOutput executeDiagnosticJob(std::string_view input,
                                         const std::function<void()>& beforeSemantic,
                                         DiagnosticJobStatistics* statistics) {
    return executeJob(input, beforeSemantic, statistics, nullptr);
}
struct DiagnosticJobSession::Impl : DiagnosticLibrarySessionState {};
DiagnosticJobSession::DiagnosticJobSession() : impl(std::make_unique<Impl>()) {
    // Mechanism control: retain session transport while measuring native parsing.
    // Ordinary runs leave this unset. It cannot enable the experiment.
    impl->declined = std::getenv("SLANG_UVM_WORKER_PREFIX_DISABLE") != nullptr;
}
DiagnosticJobSession::~DiagnosticJobSession() = default;
DiagnosticJobOutput DiagnosticJobSession::execute(std::string_view input,
                                                  const std::function<void()>& beforeSemantic,
                                                  DiagnosticJobStatistics* statistics) {
    try {
        return executeJob(input, beforeSemantic, statistics, impl.get());
    }
    catch (const PrefixReuseFailure&) {
        // A failed resume can poison replay lookups. Restart the whole local job
        // with a fresh native manager, never reuse its partially parsed state.
        impl->context.reset();
        impl->declined = true;
        workerCacheTrace("native-fallback");
        return executeJob(input, beforeSemantic, statistics, nullptr);
    }
    catch (...) {
        impl->context.reset();
        throw;
    }
}

[[noreturn]] void runDiagnosticWorker(const std::function<void()>& beforeSemantic,
                                      bool persistent) {
    // Keep lifetime protection through framing failures, stderr error reporting,
    // and final exit. Worker mode never runs process-wide static destructors.
    std::unique_ptr<isolated_diagnostics::DiagnosticParentGuard> parentGuard;
    try {
#if defined(__linux__) || defined(__APPLE__)
        const int64_t actualParent = ::getppid();
        parentGuard = std::make_unique<isolated_diagnostics::DiagnosticParentGuard>(actualParent);
#else
        const int64_t actualParent = -1;
#endif
        DiagnosticJobSession session;
        do {
            auto input = isolated_diagnostics::readDiagnosticRequest();
            // Verify that the framed job belongs to the exact parent to which this
            // worker was bound, before any parse or semantic work (including the gate).
            if (input.size() < 8)
                throw std::runtime_error("missing diagnostic metadata");
            uint64_t count = 0;
            for (size_t i = 0; i < 8; ++i)
                count = (count << 8) | uint8_t(input[i]);
            if (count > metadataLimit || count > input.size() - 8)
                throw std::runtime_error("invalid diagnostic metadata");
            auto owner = rfl::json::read<DiagnosticJobMetadata, rfl::UnderlyingEnums>(
                             std::string_view(input).substr(8, size_t(count)))
                             .value()
                             .parentPid;
            if (owner != actualParent)
                throw std::runtime_error("diagnostic job owner differs from worker parent");
            auto output = persistent ? session.execute(input, beforeSemantic)
                                     : executeDiagnosticJob(input, beforeSemantic);
            isolated_diagnostics::writeDiagnosticResponse(
                rfl::json::write<rfl::UnderlyingEnums>(output));
        } while (persistent);
        std::_Exit(0);
    }
    catch (const std::exception& error) {
        if (!parentGuard)
            std::_Exit(1); // No lifetime guard: do not risk blocking on stderr.
        // stdout must remain exclusively the framed result. A failed snapshot
        // produces no partially rendered diagnostics.
        std::fprintf(stderr, "Diagnostic worker rejected job: %s\n", error.what());
        std::_Exit(1);
    }
}
} // namespace server
