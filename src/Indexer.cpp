//------------------------------------------------------------------------------
// Indexer.cpp
// Implementation of the server's workspace indexer.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "Indexer.h"

#include "Config.h"
#include "util/Logging.h"
#include <BS_thread_pool.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fmt/format.h>
#include <queue>
#include <set>
#include <string_view>
#include <unordered_map>

#include "slang/driver/SourceLoader.h"
#include "slang/parsing/Parser.h"
#include "slang/parsing/ParserMetadata.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxKind.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"
#include "slang/util/OS.h"
#include "slang/util/SmallMap.h"
#include "slang/util/Util.h"

namespace fs = std::filesystem;

void Indexer::extractFromRoot(const slang::syntax::CompilationUnitSyntax& root,
                              const slang::parsing::ParserMetadata& meta, IndexedPath& dest) {
    using namespace slang::syntax;

    // Extract top-level symbols
    for (auto* member : root.members) {
        if (ModuleDeclarationSyntax::isKind(member->kind)) {
            auto& decl = member->as<ModuleDeclarationSyntax>();
            std::string_view name = decl.header->name.valueText();
            if (!name.empty()) {
                dest.symbols.push_back(GlobalSymbol{.name = std::string(name), .kind = decl.kind});
            }
        }
    }

    // Extract referenced symbols from metadata
    slang::SmallSet<std::string_view, 8> seenDeps;
    meta.visitReferencedSymbols([&](std::string_view name) {
        if (seenDeps.insert(name).second)
            dest.referencedSymbols.push_back(std::string{name});
    });
}

void Indexer::extractIncludes(const slang::syntax::CompilationUnitSyntax& root,
                              const slang::parsing::PreprocessorMetadata& metadata,
                              const slang::SourceManager& sourceManager, slang::BufferID rootBuffer,
                              IndexedPath& dest) {
    using namespace slang;
    const bool placeholder = rootBuffer == BufferID::getPlaceholder();
    // SourceLocation packs buffer IDs into fewer bits than BufferID itself.
    // Normalize the indexer's sentinel before comparing token locations.
    rootBuffer = SourceLocation(rootBuffer, 0).buffer();
    for (const auto& include : metadata.includeDirectives) {
        auto location = include.syntax->directive.location();
        if (location.buffer() != rootBuffer) {
            if (placeholder)
                continue;
            location = sourceManager.getFullyExpandedLoc(location);
        }
        // A saved parsed tree also contains nested includes. Attribute only its own edges.
        if (location.buffer() != rootBuffer || include.path.empty())
            continue;
        bool inPackage = false;
        for (auto* member : root.members) {
            if (member->kind == syntax::SyntaxKind::PackageDeclaration &&
                member->sourceRange().contains(location)) {
                inPackage = true;
                break;
            }
        }
        dest.includes.push_back({std::string(include.path), include.isSystem, inPackage});
    }
}

template<typename MacroRange>
void Indexer::extractMacros(const MacroRange& macros, IndexedPath& dest) {
    for (const auto* macro : macros) {
        if (!macro)
            continue;

        // Only add macros defined in this file (not included files)
        if (macro->name.location() == slang::SourceLocation::NoLocation)
            continue;

        dest.macros.push_back(std::string(macro->name.valueText()));
    }
}

std::vector<Indexer::IndexedPath> Indexer::indexPaths(const std::vector<fs::path>& paths) const {
    using namespace slang;
    using namespace parsing;

    uint32_t numThreads = numThreads_;
    if (paths.size() < MinFilesForThreading) {
        numThreads = 1;
    }

    std::vector<IndexedPath> loadResults;
    loadResults.resize(paths.size());

    // Lambda that processes a range of files
    // Creates its own SourceManager and options to avoid contention when threaded
    auto processRange = [&loadResults, &paths](size_t start, size_t end) {
        SourceManager sourceManager;
        Bag options;
        options.set(PreprocessorOptions{.maxIncludeDepth = 0});

        SmallVector<char> bufferData;
        for (size_t i = start; i < end; i++) {
            auto& dest = loadResults[i];
            bufferData.clear();
            if (std::error_code ec = OS::readFile(paths[i], bufferData)) {
                continue;
            }

            SourceBuffer buffer{.data = std::string_view(bufferData.data(), bufferData.size()),
                                .id = BufferID::getPlaceholder()};

            BumpAllocator alloc;
            Diagnostics diagnostics;
            Preprocessor preprocessor(sourceManager, alloc, diagnostics, options, {});
            preprocessor.pushSource(buffer);
            Parser parser(preprocessor, options);

            auto& root = parser.parseCompilationUnit();
            const auto& meta = parser.getMetadata();

            // Extract macros only if no global symbols were found (header files)
            if (!meta.nodeMeta.empty()) {
                extractFromRoot(root, meta, dest);
            }
            else if (meta.classDecls.empty()) {
                // If an svh file contains a class, it's likely actually included in a package
                extractMacros(preprocessor.getDefinedMacros(), dest);
            }
            auto ppMetadata = preprocessor.getMetadata();
            extractIncludes(root, ppMetadata, sourceManager, buffer.id, dest);
        }
    };

    if (numThreads != 1) {
        BS::thread_pool threadPool(numThreads);
        threadPool.detach_blocks(size_t(0), paths.size(), processRange);
        threadPool.wait();
    }
    else {
        processRange(0, paths.size());
    }

    return loadResults;
}

Indexer::Indexer() = default;

const fs::path* Indexer::internUri(const fs::path& path) {
    auto [it, inserted] = uniqueUris_.insert(path);
    return &(*it);
}

void Indexer::updateDocument(const fs::path& path, const slang::syntax::SyntaxTree& tree,
                             std::optional<slang::BufferID> targetBuffer) {
    IndexWriteGuard guard(*this);

    // Intern the URI once for all operations
    const fs::path* uriPtr = internUri(path);

    removePathFromIndex(uriPtr);

    // Extract new data
    IndexedPath newPath;
    newPath.path = uriPtr;
    extractFromRoot(tree.root().as<slang::syntax::CompilationUnitSyntax>(), tree.getMetadata(),
                    newPath);
    if (targetBuffer) {
        newPath.symbols.clear();
        for (auto* member : tree.root().as<slang::syntax::CompilationUnitSyntax>().members) {
            if (!slang::syntax::ModuleDeclarationSyntax::isKind(member->kind))
                continue;
            auto& declaration = member->as<slang::syntax::ModuleDeclarationSyntax>();
            auto location = tree.sourceManager().getFullyExpandedLoc(
                declaration.header->name.location());
            if (location.buffer() == *targetBuffer && !declaration.header->name.valueText().empty())
                newPath.symbols.push_back(
                    {std::string(declaration.header->name.valueText()), declaration.kind});
        }
    }

    // Extract macros only if no global symbols were found (header files)
    if (newPath.symbols.empty()) {
        if (targetBuffer) {
            for (const auto* macro : tree.getDefinedMacros())
                if (tree.sourceManager().getFullyExpandedLoc(macro->name.location()).buffer() ==
                    *targetBuffer)
                    newPath.macros.emplace_back(macro->name.valueText());
        }
        else
            extractMacros(tree.getDefinedMacros(), newPath);
    }

    extractIncludes(tree.root().as<slang::syntax::CompilationUnitSyntax>(),
                    tree.getPreprocessorMetadata(), tree.sourceManager(),
                    targetBuffer.value_or(tree.getSourceBufferIds().front()), newPath);
    indexPath(path, newPath);
    ++revision_;
}

void Indexer::indexPath(const fs::path& path, IndexedPath& indexedFile) {
    const fs::path* uriPtr = internUri(path);
    removePathFromIndex(uriPtr);
    indexedFile.path = uriPtr;

    for (const auto& item : indexedFile.symbols)
        symbolToFiles_[item.name].push_back(GlobalSymbolLoc{.uri = uriPtr, .kind = item.kind});

    for (const auto& name : indexedFile.macros)
        macroToFiles_[name].push_back(uriPtr);

    for (const auto& ref : indexedFile.referencedSymbols)
        symbolReferences_[ref].push_back(uriPtr);

    for (const auto& include : indexedFile.includes)
        includeToFiles_[fs::path(include.spelling).filename().string()].push_back(
            {uriPtr, include.inPackage});

    // Store the indexed path for efficient removal later
    indexedFiles[uriPtr] = std::move(indexedFile);
}

void Indexer::addDocuments(const std::vector<fs::path>& paths) {
    IndexWriteGuard guard(*this);

    auto indexedPaths = indexPaths(paths);
    for (size_t i = 0; i < indexedPaths.size(); ++i)
        indexPath(paths[i], indexedPaths[i]);
    ++revision_;
}

void Indexer::removePathFromIndex(const fs::path* pathPtr) {
    // Look up the stored IndexedPath for targeted removal of symbols
    auto it = indexedFiles.find(pathPtr);
    if (it == indexedFiles.end())
        return;

    const IndexedPath& entry = it->second;

    // Remove symbols
    for (const auto& item : entry.symbols) {
        auto mapIt = symbolToFiles_.find(item.name);
        if (mapIt != symbolToFiles_.end()) {
            auto& vec = mapIt->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [&](const GlobalSymbolLoc& loc) {
                                         return loc.uri == pathPtr && loc.kind == item.kind;
                                     }),
                      vec.end());
            if (vec.empty())
                symbolToFiles_.erase(mapIt);
        }
    }

    // Remove macros
    for (const auto& name : entry.macros) {
        auto mapIt = macroToFiles_.find(name);
        if (mapIt != macroToFiles_.end()) {
            auto& vec = mapIt->second;
            vec.erase(std::remove(vec.begin(), vec.end(), pathPtr), vec.end());
            if (vec.empty())
                macroToFiles_.erase(mapIt);
        }
    }

    // Remove references
    for (const auto& ref : entry.referencedSymbols) {
        auto mapIt = symbolReferences_.find(ref);
        if (mapIt != symbolReferences_.end()) {
            auto& vec = mapIt->second;
            vec.erase(std::remove(vec.begin(), vec.end(), pathPtr), vec.end());
            if (vec.empty())
                symbolReferences_.erase(mapIt);
        }
    }

    for (const auto& include : entry.includes) {
        auto key = fs::path(include.spelling).filename().string();
        auto found = includeToFiles_.find(key);
        if (found == includeToFiles_.end())
            continue;
        auto& locations = found->second;
        locations.erase(std::remove_if(locations.begin(), locations.end(),
                                       [&](const auto& location) {
                                           return location.path == pathPtr;
                                       }),
                        locations.end());
        if (locations.empty())
            includeToFiles_.erase(found);
    }

    // Remove from indexedFiles
    indexedFiles.erase(it);
}

uint64_t Indexer::getRevision() const {
    IndexReadGuard guard(*this);
    return revision_;
}

Indexer::PackageOwnerCandidates Indexer::getPackageOwnerCandidates(const fs::path& header) const {
    IndexReadGuard guard(*this);
    PackageOwnerCandidates result{.revision = revision_};
    std::queue<fs::path> pending;
    std::set<fs::path> visited;
    std::set<fs::path> roots;
    pending.push(header);
    visited.insert(header);
    while (!pending.empty()) {
        auto current = std::move(pending.front());
        pending.pop();
        auto found = includeToFiles_.find(current.filename().string());
        if (found == includeToFiles_.end())
            continue;
        for (const auto& include : found->second) {
            if (include.inPackage) {
                roots.insert(*include.path);
                if (roots.size() > MaxOwnerCandidates) {
                    result.limitExceeded = true;
                    return result;
                }
            }
            else if (visited.insert(*include.path).second) {
                if (visited.size() > MaxOwnerGraphFiles) {
                    result.limitExceeded = true;
                    return result;
                }
                pending.push(*include.path);
            }
        }
    }
    result.paths.assign(roots.begin(), roots.end());
    return result;
}

bool isExcluded(const std::string& path, const std::vector<std::string>& excludeDirs) {
    for (const auto& dir : excludeDirs) {
        if (path.find(dir) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<fs::path> Indexer::getFilesForSymbol(std::string_view name) const {
    IndexReadGuard guard(*this);

    auto it = symbolToFiles_.find(std::string(name));

    std::vector<fs::path> result;
    if (it != symbolToFiles_.end()) {
        for (const auto& entry : it->second)
            result.push_back(*entry.uri);
    }

    return result;
}

std::vector<fs::path> Indexer::getFilesForMacro(std::string_view name) const {
    IndexReadGuard guard(*this);

    auto it = macroToFiles_.find(std::string(name));

    std::vector<fs::path> result;
    if (it != macroToFiles_.end()) {
        for (const auto& path : it->second)
            result.push_back(*path);
    }

    return result;
}

std::vector<fs::path> Indexer::getFilesReferencingSymbol(std::string_view name) const {
    IndexReadGuard guard(*this);

    auto it = symbolReferences_.find(std::string(name));

    std::vector<fs::path> result;
    if (it != symbolReferences_.end()) {
        for (const auto& path : it->second)
            result.push_back(*path);
    }

    return result;
}

std::optional<Indexer::GlobalSymbolLoc> Indexer::getFirstSymbolLoc(std::string_view name) const {
    IndexReadGuard guard(*this);

    auto it = symbolToFiles_.find(std::string(name));
    if (it == symbolToFiles_.end() || it->second.empty()) {
        return std::nullopt;
    }
    return it->second[0];
}

std::vector<std::string> Indexer::getAllMacroNames() const {
    IndexReadGuard guard(*this);

    std::vector<std::string> result;
    result.reserve(macroToFiles_.size());
    for (const auto& [name, _] : macroToFiles_) {
        result.push_back(name);
    }
    return result;
}

size_t Indexer::getSymbolCount() const {
    IndexReadGuard guard(*this);
    return symbolToFiles_.size();
}

bool isSystemVerilogFile(const fs::path& path) {
    auto ext = path.extension().string();
    return ext == ".sv" || ext == ".svh" || ext == ".v" || ext == ".vh";
}

void Indexer::onWorkspaceDidChangeWatchedFiles(const lsp::DidChangeWatchedFilesParams& params) {
    IndexWriteGuard guard(*this);

    std::vector<fs::path> pathsToAdd;

    for (const auto& change : params.changes) {
        fs::path path = change.uri.getPath();

        switch (change.type) {
            case lsp::FileChangeType::Created: {
                // Add new file to index
                pathsToAdd.push_back(path);
                break;
            }
            case lsp::FileChangeType::Changed: {
                // Re-index the file: remove old entries, add new ones
                auto it = uniqueUris_.find(path);
                if (it != uniqueUris_.end()) {
                    removePathFromIndex(&(*it));
                }

                // Re-add with new content
                if (fs::exists(path))
                    pathsToAdd.push_back(path);
                break;
            }
            case lsp::FileChangeType::Deleted: {
                // Remove all entries for this file
                auto it = uniqueUris_.find(path);
                if (it != uniqueUris_.end()) {
                    removePathFromIndex(&(*it));
                }
                break;
            }
        }
    }

    // Parse all new/changed files potentially in thread pool
    if (!pathsToAdd.empty()) {
        auto indexedPaths = indexPaths(pathsToAdd);
        for (size_t i = 0; i < indexedPaths.size(); ++i)
            indexPath(pathsToAdd[i], indexedPaths[i]);
    }
    ++revision_;
}

void Indexer::collectFilesFromDirectory(const fs::path& dir,
                                        const std::vector<std::string>& excludeDirs,
                                        std::vector<fs::path>& outFiles) {

    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        return;
    }
    auto startSize = outFiles.size();

    ScopedTimer t_index(fmt::format("Crawling {}", dir.string()));
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); ++it) {

        // Check for exclude when entering a new directory
        if (it->is_directory(ec)) {
            // This map tends to be small; linear search is fine
            if (isExcluded(it->path().string(), excludeDirs)) {
                it.disable_recursion_pending();
                continue;
            }
        }
        else if (it->is_regular_file(ec) && isSystemVerilogFile(it->path())) {
            // Add SystemVerilog files
            outFiles.push_back(it->path());
        }
    }
    // check ec
    if (ec) {
        ERROR("Error while indexing directory {}: {}", dir.string(), ec.message());
    }
    INFO("Found {} files", outFiles.size() - startSize);
}

void Indexer::startIndexing(const std::vector<Config::IndexConfig>& indexConfigs,
                            std::optional<std::string_view> workspaceFolder) {
    std::vector<fs::path> pathsToIndex;

    if (indexConfigs.empty()) {
        // No index configs - index entire workspace
        if (workspaceFolder.has_value()) {
            collectFilesFromDirectory(fs::path(*workspaceFolder), {}, pathsToIndex);
        }
    }
    else {
        for (const auto& cfg : indexConfigs) {
            for (const auto& dir : cfg.dirs.value()) {
                fs::path fullDirPath;
                if (fs::path(dir).is_absolute()) {
                    fullDirPath = fs::path(dir);
                }
                else if (workspaceFolder.has_value()) {
                    fullDirPath = fs::path(*workspaceFolder) / fs::path(dir);
                }
                else {
                    continue;
                }
                collectFilesFromDirectory(
                    fullDirPath, cfg.excludeDirs.value().value_or(std::vector<std::string>{}),
                    pathsToIndex);
            }
        }
    }

    indexAndReport(pathsToIndex);
}

void Indexer::startIndexing(const std::vector<std::string>& globs,
                            const std::vector<std::string>& excludeDirs) {
    std::vector<fs::path> pathsToIndex;
    for (const auto& pattern : globs) {
        ScopedTimer t_glob("Globbing " + pattern);
        size_t beginCount = pathsToIndex.size();

        slang::SmallVector<fs::path> out;
        std::error_code ec;
        svGlob({}, pattern, slang::GlobMode::Files, out, true, ec);

        if (ec) {
            ERROR("Error globbing pattern {}: {}", pattern, ec.message());
            continue;
        }

        for (const auto& path : out) {
            if (!isExcluded(path.string(), excludeDirs))
                pathsToIndex.push_back(path);
        }
        INFO("Found {} files from pattern {}", pathsToIndex.size() - beginCount, pattern);
    }

    indexAndReport(pathsToIndex);
}

void Indexer::indexAndReport(std::vector<fs::path> pathsToIndex) {
    INFO("Indexing {} files", pathsToIndex.size());

    {
        ScopedTimer t_index("Slang Indexing");
        IndexWriteGuard guard(*this);
        auto indexedPaths = indexPaths(pathsToIndex);
        symbolToFiles_.clear();
        macroToFiles_.clear();
        symbolReferences_.clear();
        includeToFiles_.clear();
        indexedFiles.clear();
        for (size_t i = 0; i < pathsToIndex.size(); i++)
            indexPath(pathsToIndex[i], indexedPaths[i]);
        ++revision_;
    }

    // Estimate memory usage
    size_t symbolsSize = 0;
    for (const auto& [name, entries] : symbolToFiles_) {
        symbolsSize += sizeof(
                           std::pair<const std::string, slang::SmallVector<GlobalSymbolLoc, 2>>) +
                       name.capacity();
        symbolsSize += entries.size() * sizeof(GlobalSymbolLoc);
    }

    size_t macrosSize = 0;
    for (const auto& [name, uris] : macroToFiles_) {
        macrosSize += sizeof(std::pair<const std::string, slang::SmallVector<const fs::path*, 2>>) +
                      name.capacity();
        macrosSize += uris.size() * sizeof(const fs::path*);
    }

    size_t refsSize = 0;
    for (const auto& [name, uris] : symbolReferences_) {
        refsSize += sizeof(std::pair<const std::string, slang::SmallVector<const fs::path*, 2>>) +
                    name.capacity();
        refsSize += uris.size() * sizeof(const fs::path*);
    }

    // Count unique URIs storage
    size_t urisSize = 0;
    for (const auto& uri : uniqueUris_) {
        urisSize += sizeof(URI) + uri.string().capacity();
    }

    INFO("Indexing complete: {} symbols (~{} KB), {} macros (~{} KB), {} references (~{} KB), {} "
         "unique URIs (~{} KB)",
         symbolToFiles_.size(), symbolsSize / 1024, macroToFiles_.size(), macrosSize / 1024,
         symbolReferences_.size(), refsSize / 1024, uniqueUris_.size(), urisSize / 1024);
}
