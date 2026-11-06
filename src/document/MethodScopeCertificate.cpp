// SPDX-License-Identifier: MIT
#include "ast/TrustedUvmLibrary.h"
#include "document/MethodScopeOverlay.h"
#include "util/DirectoryProofPlan.h"
#include "util/MethodInputProof.h"
#include "util/UvmPhaseTrace.h"
#include <algorithm>
#include <atomic>
#include <set>
#include <sstream>

#include "slang/driver/SourceLoader.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"

namespace server {
using namespace slang;
namespace fs = std::filesystem;
using method_input::directoryEntries;
namespace {
std::atomic_uint64_t nextBodyCertificate{1};
}

MethodScopeOverlay::ResolvedSource MethodScopeOverlay::resolveSource(const Config& config,
                                                                     const lsp::RequestContext& ctx,
                                                                     UvmPhaseTrace& contextPhases) {
    auto& d = *driver;
    std::shared_ptr<syntax::SyntaxTree> tree;
    std::shared_ptr<ShallowAnalysis> analysis;
    BufferID occurrence;
    auto document = d.getDocument(uri);
    require(document && document->getVersion(), "open target required");
    if (bodyQueries) {
        // Never force a full previous-generation analysis merely to speculate
        // about an edit. Preparation requires a previously queried live view.
        require(document->hasAnalysis(), "current query context required");
        require(!d.completions.resolveEdits, "deferred callable edits required");
        auto [graph, resolvedAnalysis] = document->getAnalysisAndInputs(ctx);
        contextPhases.mark("body-context-resolved-inputs");
        analysis = std::move(resolvedAnalysis);
        tree = graph.tree;
        occurrence = graph.buffer;
        require(tree && occurrence.valid(), "source context unavailable");
        // This first slice does not choose between repeated textual occurrences.
        size_t occurrences = 0;
        for (const auto& candidate : graph.trees)
            for (auto id : candidate->getPreprocessorMetadata().sourceBufferIds)
                occurrences += d.sm.getFullPath(id) == d.sm.getFullPath(occurrence);
        require(occurrences == 1, "nonunique target occurrence");
        contextPhases.mark("body-context-unique-occurrence");
    }
    else {
        require(!d.comp && d.hasConfiguredCompilationInputs() && !d.completions.resolveEdits,
                "configured package exploration required");
        require(config.compilationFilelist.value().has_value() &&
                    config.explorePreincludeFiles.value().empty(),
                "literal filelist without preinclude required");
        auto flags = [](const std::string& value) {
            std::istringstream words(value);
            std::string word;
            while (words >> word) {
                if (word == "--single-unit")
                    continue;
                if (word == "--threads") {
                    require(bool(words >> word) && word == "1", "one parse worker");
                    continue;
                }
                require(false, "unsupported source-loading flag");
            }
        };
        flags(config.flags.value());
        for (const auto& source : config.flagsByFile.value())
            flags(source.flags);
        std::set<fs::path> listings;
        std::vector<fs::path> recipe;
        auto literal = [](std::string_view value) {
            return !value.empty() &&
                   value.find_first_of("*?[$`@{}\"'\\") == std::string_view::npos &&
                   value.find("...") == std::string_view::npos;
        };
        auto visit = [&](auto&& self, fs::path path, size_t depth) -> void {
            path = fs::absolute(path).lexically_normal();
            require(depth < 16 && listings.insert(path).second, "cyclic/deep filelist");
            auto bytes = read(path.string());
            require(bool(bytes), "missing filelist");
            disk[path.string()] = bytes;
            std::istringstream lines(*bytes);
            std::string line;
            while (std::getline(lines, line)) {
                std::istringstream tokens(line);
                std::string token;
                while (tokens >> token) {
                    ctx.checkpoint("method certificate filelist");
                    if (token.starts_with("//"))
                        break;
                    require(literal(token), "nonliteral recipe");
                    if (token == "-F") {
                        require(bool(tokens >> token) && literal(token), "nested filelist");
                        self(self, path.parent_path() / token, depth + 1);
                    }
                    else if (token.starts_with("+incdir+"))
                        continue; // Every actual/failed lookup is frozen below.
                    else {
                        require(!token.starts_with('-') && !token.starts_with('+'),
                                "unsupported recipe option");
                        recipe.push_back(
                            fs::absolute(path.parent_path() / token).lexically_normal());
                    }
                }
            }
        };
        auto list = fs::path(*config.compilationFilelist.value());
        if (list.is_relative())
            list = fs::path(d.m_workspacePathPrefix) / list;
        visit(visit, list, 0);
        ctx.checkpoint("method certificate recipe ready");
        std::shared_ptr<SlangDoc> unit;
        for (auto& [_, document] : d.docs)
            if (document->isNativeCompilationUnit()) {
                require(!unit, "one native unit required");
                unit = document;
            }
        require(unit && unit->getOptions().getOrDefault<driver::SourceOptions>().singleUnit &&
                    !unit->requiresNativeReplay(),
                "single-unit without inherited replay required");
        auto roots = unit->getRootPaths();
        require(roots.size() == recipe.size() &&
                    std::equal(roots.begin(), roots.end(), recipe.begin()),
                "ordered recipe mismatch");
        tree = unit->getSyntaxTree(ctx);
        auto& pp = tree->getPreprocessorMetadata();
        require(pp.sourceBufferIds.size() == roots.size() + pp.includeDirectives.size() &&
                    pp.macroRefs.empty() && pp.predefineBufferIds.empty() &&
                    !tree->getSourceLibrary() && !tree->isLibraryUnit,
                "complete include graph without user macro environment");
        for (auto* macro : pp.definedMacros)
            require(macro->name.location() == SourceLocation::NoLocation, "user macro definition");
        // Certify every occurrence, not just the edited header. Each non-root buffer
        // must have exactly one literal include edge and reach a configured root.
        // Repeated physical files are allowed outside the target, but recursive
        // physical paths and unexplained preprocessing inputs are not.
        std::set<BufferID> buffers(pp.sourceBufferIds.begin(), pp.sourceBufferIds.end());
        require(buffers.size() == pp.sourceBufferIds.size(), "duplicate source buffer identity");
        std::map<BufferID, BufferID> parents;
        std::map<BufferID, std::vector<std::pair<size_t, size_t>>> directives;

        for (const auto& include : pp.includeDirectives) {
            require(include.syntax && include.buffer && !include.isSystem, "unsupported include");
            auto range = include.syntax->sourceRange();
            auto parent = range.start().buffer();
            auto child = include.buffer.id;
            require(buffers.contains(parent) && buffers.contains(child) &&
                        range.end().buffer() == parent && parents.emplace(child, parent).second,
                    "ambiguous include graph");
            directives[parent].emplace_back(range.start().offset(), range.end().offset());
            if (URI::fromFile(d.sm.getFullPath(child)) == uri) {
                require(!occurrence.valid(), "repeated target include");
                occurrence = child;
            }
        }
        require(occurrence.valid(), "target must be included");
        require(std::count_if(pp.sourceBufferIds.begin(), pp.sourceBufferIds.end(),
                              [&](auto id) {
                                  return d.sm.getFullPath(id) == d.sm.getFullPath(occurrence);
                              }) == 1,
                "repeated header occurrence");
        auto options = tree->options().getOrDefault<parsing::PreprocessorOptions>();
        require(options.predefines.empty() && options.undefines.empty() &&
                    options.additionalIncludePaths.empty() && options.ignoreDirectives.empty() &&
                    options.keywordMapping.empty() && !options.bufferChangeCB,
                "preprocessor override");
        for (auto id : pp.sourceBufferIds) {
            std::string bytes(d.sm.getSourceText(id));
            auto& ranges = directives[id];
            std::sort(ranges.begin(), ranges.end());
            size_t previousEnd = 0;
            for (auto [begin, end] : ranges) {
                require(begin >= previousEnd && begin < end && end <= bytes.size(),
                        "invalid include directive range");
                std::fill(bytes.begin() + begin, bytes.begin() + end, ' ');
                previousEnd = end;
            }
            require(bytes.find('`') == std::string::npos, "other directive");
            std::set<fs::path> ancestors;
            auto current = id;
            while (true) {
                require(ancestors.insert(d.sm.getFullPath(current)).second,
                        "recursive include graph");
                auto parent = parents.find(current);
                if (parent == parents.end())
                    break;
                current = parent->second;
            }
        }
        std::multiset<fs::path> actualRoots, expectedRoots(roots.begin(), roots.end());
        for (auto id : buffers)
            if (!parents.contains(id))
                actualRoots.insert(d.sm.getFullPath(id));
        require(actualRoots == expectedRoots, "include graph root mismatch");

        auto graph = document->getAnalysisInputs(ctx);
        require(graph.tree == tree && graph.buffer == occurrence && graph.trees.size() == 1,
                "nonunique owner graph");
    }
    return {std::move(tree), std::move(analysis), occurrence, std::move(document)};
}

void MethodScopeOverlay::captureWitnesses(const Config& config, ast::Compilation& compilation,
                                          BufferID occurrence, const lsp::RequestContext& ctx,
                                          UvmPhaseTrace& phases) {
    auto& d = *driver;
    std::optional<SourceManager::SourceSnapshotView> sourceWitness;
    if (bodyQueries) {
        // Query certificates do not replay diagnostics or serialize semantic
        // owners. Native diagnostics retain the current UVM / RTL owners.
        auto input = std::make_shared<diagnostic_snapshot::Input>();
        std::vector<BufferID> buffers;
        for (const auto& syntaxTree : compilation.getSyntaxTrees()) {
            const auto& pp = syntaxTree->getPreprocessorMetadata();
            buffers.insert(buffers.end(), pp.sourceBufferIds.begin(), pp.sourceBufferIds.end());
        }
        if (optimizedCertification(config)) {
            sourceWitness.emplace(d.sm.captureSnapshotView(buffers));
            input->sources.cwd = sourceWitness->cwd;
        }
        else {
            input->sources = d.sm.captureSnapshot(buffers);
        }
        frozen.owner = d.m_snapshotOwner;
        d.completions.requireCompletionEpoch();
        frozen.revision = d.completions.getCompletionEpoch();
        frozen.indexRevision = nextBodyCertificate.fetch_add(1, std::memory_order_relaxed);
        frozen.input = std::move(input);
    }
    else {
        auto saved = d.m_pendingIncludedDiagnostics;
        d.m_pendingIncludedDiagnostics.emplace(uri, d.m_diagnosticGeneration);
        try {
            auto value = d.captureDiagnosticSnapshot(ctx);
            require(bool(value), "diagnostic freeze unavailable");
            frozen = std::move(*value);
            d.m_pendingIncludedDiagnostics = std::move(saved);
        }
        catch (...) {
            d.m_pendingIncludedDiagnostics = std::move(saved);
            throw;
        }
    }
    if (!bodyQueries)
        require(frozen.metadata.trees.size() == 1, "diagnostic graph is not a single unit");
    phases.mark("body-prepare-snapshot");
    ctx.checkpoint("method certificate snapshot ready");
    for (auto& [openUri, doc] : d.docs)
        if (doc->getVersion() && d.isDocumentOpen(openUri)) {
            require(bodyQueries || openUri == uri, "phase one requires only target header open");
            documents.emplace(openUri, DocumentWitness{doc.get(), doc->getBuffer().getId(),
                                                       *doc->getVersion(), plain(*doc)});
        }
    // Explicit immutable libraries keep their existing manual-rebuild contract.
    // Re-reading those sources on every local query defeats semantic reuse.
    // Hold weak generation identities rather than another live semantic graph.
    std::unordered_set<std::string> immutableFiles;
    if (bodyQueries) {
        immutableUvm = d.m_trustedUvm;
        immutableScopes = d.m_trustedScopes;
        for (const auto& library : {d.m_trustedUvm, d.m_trustedScopes})
            if (library)
                immutableFiles.insert(library->files.begin(), library->files.end());
        require(!immutableFiles.contains(d.sm.getFullPath(occurrence).string()),
                "edited method belongs to an immutable library");
    }
    auto forSourceFiles = [&](auto&& action) {
        if (sourceWitness) {
            for (const auto& file : sourceWitness->files)
                action(file);
        }
        else {
            for (const auto& file : frozen.input->sources.files)
                action(file);
        }
    };
    const bool fastWitnesses = optimizedCertification(config);
    auto readWitness = [fastWitnesses, &ctx](const std::string& path) {
        try {
            return read(path, fastWitnesses, ctx);
        }
        catch (const lsp::RequestCancelled&) {
            throw;
        }
        catch (const std::exception& error) {
            // A rejected physical input must not fall through to an older
            // SourceManager lookup. Rebuild dependencies before serving native.
            throw InputsChanged(error.what());
        }
    };
    if (fastWitnesses) {
        // Collect unique physical paths before verifying parsed bytes.
        // Capturing is independent of the final witness map.
        std::map<std::string, std::optional<std::string>> pending;
        forSourceFiles([&](const auto& file) {
            if (!file.generated && !immutableFiles.contains(file.fullPath) &&
                !disk.contains(file.fullPath))
                pending.try_emplace(file.fullPath);
        });
        if (pending.size() >= 64) {
            std::vector<FileProofInput> inputs;
            inputs.reserve(pending.size());
            for (auto& [path, bytes] : pending)
                inputs.push_back({&path, nullptr, &bytes});
            try {
                (void)readProofInputs(inputs, "method source witness read", ctx);
            }
            catch (const lsp::RequestCancelled&) {
                throw;
            }
            catch (const std::exception& error) {
                throw InputsChanged(error.what());
            }
            disk.merge(pending);
        }
    }
    // A path can appear as a source, lookup and context watch input. Never let
    // a later read replace a witness already checked against the parsed bytes.
    auto witness = [&](const std::string& path) -> const std::optional<std::string>& {
        if (optimizedCertification(config))
            if (auto found = disk.find(path); found != disk.end())
                return found->second;
        ctx.checkpoint("method source witness read");
        auto bytes = readWitness(path);
        auto [it, inserted] = disk.try_emplace(path, bytes);
        if (!inserted && it->second != bytes) {
            throw InputsChanged("input changed during certification");
        }
        return it->second;
    };
    forSourceFiles([&](const auto& file) {
        if (immutableFiles.contains(file.fullPath))
            return;
        if (bodyQueries && file.generated)
            return; // Generated inputs are immutable parts of the captured parse context.
        require(!file.generated, "generated input");
        const auto& bytes = witness(file.fullPath);
        if (URI::fromFile(file.fullPath) != uri) {
            if (!bytes) {
                throw InputsChanged("missing source witness");
            }
            auto source = *bytes;
            if (source.empty() || source.back() != '\0')
                source.push_back('\0');
            if (source != file.bytes) {
                throw InputsChanged("source changed before certification");
            }
        }
    });
    ctx.checkpoint("method certificate sources ready");
    const auto& sourceLookups = sourceWitness ? sourceWitness->lookups
                                              : frozen.input->sources.lookups;
    for (const auto& lookup : sourceLookups) {
        if (immutableFiles.contains(lookup.path))
            continue;
        require(lookup.fileId || lookup.errorValue == int(std::errc::no_such_file_or_directory),
                "unsupported lookup failure");
        if (bool(witness(lookup.path)) != bool(lookup.fileId)) {
            throw InputsChanged("lookup changed");
        }
    }
    // No borrowed source storage survives construction of the physical witnesses.
    sourceWitness.reset();
    for (auto& [path, _] : d.driver.getCommandFileMetadata()) {
        const auto& bytes = witness(path.string());
        // Real -f/-F files were read through the same SourceManager. Compare
        // its retained input to disk before admitting the first certificate.
        // Config flag attribution can name a file that was never a source.
        if (d.sm.isCached(path)) {
            auto loaded = d.sm.readSource(path);
            if (!loaded || !bytes)
                throw InputsChanged("command file changed before certification");
            auto parsed = loaded->data;
            if (!parsed.empty() && parsed.back() == '\0')
                parsed.remove_suffix(1);
            if (parsed != *bytes)
                throw InputsChanged("command file changed before certification");
        }
    }
    for (const auto& input : d.m_config.inputSources.value())
        if (witness(input.path) != input.bytes)
            throw InputsChanged("configuration changed before certification");
    for (auto& path : d.getContextWatchPaths()) {
        if (immutableFiles.contains(path.string()))
            continue;
        if (bodyQueries && fs::is_directory(path))
            directories.emplace(path.string(), directoryEntries(path));
        else
            witness(path.string());
    }
    for (auto path : {fs::path(d.m_workspacePathPrefix) / ".slang/server.json",
                      fs::path(d.m_workspacePathPrefix) / ".slang/local/server.json"})
        witness(path.string());
    if (fastWitnesses) {
        std::vector<std::string_view> missing;
        for (const auto& [path, bytes] : disk)
            if (!bytes)
                missing.push_back(path);
        const auto budget =
            size_t(std::min<uint32_t>(config.methodBodyDirectoryProofCacheKiB.value(), 16384)) *
            1024;
        const auto inputCharge = disk.size() * sizeof(FileProofInput) * 2;
        if (inputCharge < budget)
            directoryProofPlan = DirectoryProofPlan::build(missing, budget - inputCharge);
        if (directoryProofPlan) {
            plannedProofInputs.reserve(disk.size());
            for (const auto& [path, bytes] : disk)
                plannedProofInputs.push_back(
                    {&path, &bytes, nullptr, bytes ? size_t(-1) : directoryProofPlan->group(path)});
        }
    }
    configKey = rfl::json::write(config);
    optionsKey = rfl::json::write(diagnostic_snapshot::captureOptions(
        bodyQueries ? Bag{d.options.getOrDefault<ast::CompilationOptions>()} : d.options));
    inputRevision = d.getContextInputRevision();
    indexRevision = d.getDependencyIndexRevision();
    if (bodyQueries) {
        // Only the cwd is needed after the witnesses / value DTOs are built.
        // Do not keep a second project-sized source snapshot for query reuse.
        auto compact = std::make_shared<diagnostic_snapshot::Input>();
        compact->sources.cwd = frozen.input->sources.cwd;
        frozen.input = std::move(compact);
    }
    // General-body completion tickets carry an epoch and exact editor version;
    // they never consult the legacy buffer-id list. Keep that list bounded to
    // the independently capped legacy overlay instead of growing per keystroke.
    if (!generalBody) {
        handles.reserve(bodyQueries ? 66 : 10);
        handles.push_back(originalBuffer);
        handles.push_back(currentBuffer);
    }
    phases.mark("body-prepare-witnesses");
}

} // namespace server
