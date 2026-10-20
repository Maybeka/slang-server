// SPDX-License-Identifier: MIT
#include "ServerDriver.h"
#include "ast/TrustedUvmLibrary.h"
#include "diagnostics/DiagnosticProcess.h"
#include "diagnostics/DiagnosticSnapshotCodec.h"
#include "util/Logging.h"
#include "util/UvmParseArtifact.h"
#include <chrono>
#include <rfl/json.hpp>
#include <set>
#include <sstream>
#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

#include <rfl/to_generic.hpp>

#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/driver/SourceLoader.h"
#include "slang/parsing/ParserMetadata.h"
#include "slang/syntax/AllSyntax.h"

namespace server {
namespace fs = std::filesystem;
namespace {
fs::path bundledUvmDirectory() {
    fs::path executable;
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!size || size >= path.size())
        throw std::runtime_error("cannot determine executable path for bundled UVM");
    path.resize(size);
    executable = path;
#elif defined(__linux__)
    executable = fs::canonical("/proc/self/exe");
#else
    executable = isolated_diagnostics::currentDiagnosticExecutable();
#endif
    executable = fs::canonical(executable);
    // Release archives keep the executable at their root for existing editor
    // installers. CMake build/install trees use bin plus a sibling share tree.
    const auto adjacent = executable.parent_path() / "resources/uvm/1.2";
    std::error_code error;
    if (fs::is_directory(adjacent, error))
        return adjacent / "src";
    return executable.parent_path() / SLANG_SERVER_BUNDLED_UVM_RELATIVE_PATH / "src";
}
} // namespace

bool ServerDriver::isUvmConfigurationInput(const fs::path& path) const {
    for (const auto& candidate : m_uvmWatchPaths) {
        std::error_code error;
        auto normalized = fs::weakly_canonical(candidate, error);
        if ((error ? candidate.lexically_normal() : normalized) == path)
            return true;
    }
    return false;
}

bool ServerDriver::uvmConfigurationError(std::string_view message, bool requireValidConfig) {
    auto text = fmt::format("UVM configuration: {}", message);
    if (requireValidConfig) {
        WARN("{}", text);
    }
    else {
        client.showError(text);
    }
    return false;
}

bool ServerDriver::configureUvmLibrary(const fs::path& base, bool requireValidConfig) {
    if (!m_config.uvm.value())
        return true;
    const auto& config = *m_config.uvm.value();
    if (config.useBundled.value() &&
        (!config.sourceDirectory.value().empty() || config.entryFile.value()))
        return uvmConfigurationError(
            "useBundled cannot be combined with sourceDirectory or entryFile", requireValidConfig);
    if (!config.useBundled.value() && config.sourceDirectory.value().empty())
        return uvmConfigurationError("sourceDirectory must not be empty", requireValidConfig);
    auto directory = fs::path(config.sourceDirectory.value());
    if (config.useBundled.value()) {
        try {
            directory = bundledUvmDirectory();
            m_uvmDirectory = directory;
            m_indexer.installPreparsedLibrary(directory);
        }
        catch (const std::exception& error) {
            return uvmConfigurationError(error.what(), requireValidConfig);
        }
        for (const auto* name : {"MANIFEST.json", "LICENSE.txt", "NOTICE.txt"}) {
            std::error_code error;
            if (!fs::is_regular_file(directory.parent_path() / name, error))
                return uvmConfigurationError(
                    "bundled UVM resources are missing; install slang-server together with "
                    "its share directory, or configure uvm.sourceDirectory",
                    requireValidConfig);
        }
    }
    if (directory.is_relative())
        directory = base / directory;
    // Keep the requested path, including directory aliases, for subsequent
    // entry-resolution witnesses. Keeping only its canonical target would miss
    // a symlink retarget between requests.
    m_uvmDirectory = directory;
    std::error_code ec;
    directory = fs::weakly_canonical(directory, ec);
    if (ec)
        return uvmConfigurationError(ec.message(), requireValidConfig);
    m_uvmWatchPaths.push_back(directory);
    // Register exact negative candidates even if the distribution directory does
    // not exist yet. A later file creation must repair the configuration.
    if (config.entryFile.value()) {
        auto requested = fs::path(*config.entryFile.value());
        m_uvmWatchPaths.push_back(requested.is_absolute() ? requested : directory / requested);
    }
    else {
        m_uvmWatchPaths.insert(m_uvmWatchPaths.end(),
                               {directory / "uvm_pkg.sv", directory / "src/uvm_pkg.sv",
                                directory / "uvm.sv"});
    }
    if (!fs::is_directory(directory, ec))
        return uvmConfigurationError(fmt::format("sourceDirectory is not a directory: {}",
                                                 directory.string()),
                                     requireValidConfig);
    auto available = [&](const fs::path& path) {
        std::error_code error;
        return sm.isCached(path) || fs::is_regular_file(path, error);
    };
    fs::path entry;
    if (config.useBundled.value())
        entry = directory / "uvm_pkg.sv";
    else if (config.entryFile.value()) {
        if (config.entryFile.value()->empty())
            return uvmConfigurationError("entryFile must not be empty", requireValidConfig);
        entry = fs::path(*config.entryFile.value());
        if (entry.is_relative())
            entry = directory / entry;
    }
    else {
        auto direct = directory / "uvm_pkg.sv";
        auto nested = directory / "src/uvm_pkg.sv";
        m_uvmWatchPaths.insert(m_uvmWatchPaths.end(), {direct, nested, directory / "uvm.sv"});
        if (available(direct) && available(nested))
            return uvmConfigurationError(
                "both uvm_pkg.sv and src/uvm_pkg.sv exist; specify entryFile", requireValidConfig);
        entry = available(direct) ? direct : available(nested) ? nested : directory / "uvm.sv";
    }
    entry = fs::weakly_canonical(entry, ec);
    if (ec)
        return uvmConfigurationError(ec.message(), requireValidConfig);
    m_uvmWatchPaths.push_back(entry);
    if (!available(entry))
        return uvmConfigurationError(fmt::format("entry file is unavailable: {}", entry.string()),
                                     requireValidConfig);
    std::set<fs::path> includes{directory, entry.parent_path()};
    if (fs::is_directory(directory / "src", ec))
        includes.insert(directory / "src");
    for (auto& path : includes) {
        if (auto error = sm.addUserDirectory(path))
            return uvmConfigurationError(fmt::format("include directory {}: {}", path.string(),
                                                     error.message()),
                                         requireValidConfig);
    }
    m_uvmEntryPath = entry;
    driver.options.uvmPrefixRoot = entry.string();
    if (config.useBundled.value() || m_config.experimentalUvmSemanticCache.value()) {
        try {
            using Clock = std::chrono::steady_clock;
            auto elapsed = [](auto start) {
                return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            };
            auto parseOptions = driver.createParseOptionBag();
            auto pp = parseOptions.getOrDefault<parsing::PreprocessorOptions>();
            pp.captureUvmPrefix = true;
            pp.uvmPrefixRoot = entry.string();
            parseOptions.set(pp);
            auto normalized = [](const Bag& bag) {
                auto owned = diagnostic_snapshot::captureOptions(bag);
                owned.source.reset();
                if (owned.preprocessor) {
                    owned.preprocessor->captureUvmPrefix = false;
                    owned.preprocessor->uvmPrefixRoot.clear();
                }
                return rfl::json::write(owned);
            };
            const auto includeConfig = sm.getIncludeConfiguration();
            const auto syntaxConfiguration = normalized(parseOptions);
            driver::Driver defaults;
            defaults.addStandardArgs();
            const bool shipped = config.useBundled.value() &&
                                 syntaxConfiguration ==
                                     normalized(defaults.createParseOptionBag()) &&
                                 !includeConfig.disableLocalIncludes && !includeConfig.incDirFirst;
            // Every parse-affecting flag and ordered include path is part of the key.
            // Library contents are explicitly trusted until manual reconstruction.
            struct Configuration {
                std::string sdk, root, entry, cwd, syntax;
                SourceManager::IncludeConfiguration includes;
            };
            m_uvmPreparseKey = rfl::json::write(
                Configuration{SLANG_UVM_ARCHIVE_IDENTITY, directory.string(), entry.string(),
                              fs::current_path().string(), syntaxConfiguration, includeConfig});
            if (m_trustedUvm && m_trustedUvm->preparseKey != m_uvmPreparseKey)
                throw std::runtime_error("retained UVM configuration changed; reload the project");
            if (shipped)
                m_shippedUvmProfile = "default";
            else {
                m_uvmParseCachePath = base / ".slang/cache/uvm" /
                                      (std::to_string(uvm_artifact::checksum(m_uvmPreparseKey)) +
                                       ".cst");
            }
            if (m_trustedUvm && m_trustedUvm->preparseKey == m_uvmPreparseKey) {
                m_uvmPreparseDependencies = m_trustedUvm->preparseDependencies;
                m_uvmProjectCacheHit = !shipped;
                driver.precompiledUvm = syntax::SyntaxTree::sharedPrefixView(
                    m_trustedUvm->compilation->getSyntaxTrees().front(), sm);
            }
            else {
                auto load = [&](const fs::path& path, std::string_view expected) {
                    const auto start = Clock::now();
                    auto artifact = uvm_artifact::read(path);
                    if (artifact.configuration != expected)
                        throw std::runtime_error("UVM parse cache configuration mismatch");
                    m_uvmPreparseDependencies = std::move(artifact.macroDependencies);
                    std::istringstream stream(std::move(artifact.syntax), std::ios::binary);
                    auto result = syntax::SyntaxTree::readParseArchive(stream, sm, directory,
                                                                       parseOptions, !shipped);
                    m_uvmArchiveLoadMs = elapsed(start);
                    return result;
                };
                if (shipped) {
                    // A damaged shipped default never silently falls back to source parsing.
                    driver.precompiledUvm = load(directory.parent_path() / "preparsed/default.cst",
                                                 "");
                }
                else {
                    const auto projectLoadStart = Clock::now();
                    std::optional<uvm_artifact::Data> artifact;
                    if (fs::is_regular_file(m_uvmParseCachePath)) {
                        try {
                            artifact = uvm_artifact::read(m_uvmParseCachePath);
                            if (artifact->configuration != m_uvmPreparseKey)
                                throw std::runtime_error("UVM parse cache configuration mismatch");
                        }
                        catch (const std::exception& error) {
                            INFO("Rebuilding invalid project UVM cache: {}", error.what());
                            artifact.reset();
                        }
                    }
                    std::string syntaxBytes;
                    if (artifact) {
                        syntaxBytes = std::move(artifact->syntax);
                        m_uvmProjectCacheHit = true;
                    }
                    else {
                        SourceManager isolated;
                        isolated.inheritIncludeConfiguration(includeConfig);
                        const auto start = Clock::now();
                        auto source = isolated.readSource(entry, nullptr, 0);
                        if (!source)
                            throw std::runtime_error(source.error().message());
                        auto tree = syntax::SyntaxTree::fromBuffer(*source, isolated, parseOptions);
                        m_uvmParseMs = elapsed(start);
                        const auto writeStart = Clock::now();
                        std::ostringstream stream(std::ios::binary);
                        tree->writeParseArchive(stream, directory, true);
                        syntaxBytes = std::move(stream).str();
                        fs::create_directories(m_uvmParseCachePath.parent_path());
                        uvm_artifact::write(m_uvmParseCachePath, syntaxBytes, {}, m_uvmPreparseKey);
                        m_uvmArchiveWriteMs = elapsed(writeStart);
                    }
                    const auto restoreStart = Clock::now();
                    std::istringstream stream(std::move(syntaxBytes), std::ios::binary);
                    driver.precompiledUvm = syntax::SyntaxTree::readParseArchive(
                        stream, sm, directory, parseOptions, true);
                    m_uvmArchiveLoadMs = elapsed(artifact ? projectLoadStart : restoreStart);
                }
            }
            if (!m_trustedUvm)
                m_indexer.installPreparsedLibrary(directory, driver.precompiledUvm.get());
            driver.precompiledUvmRoot = directory;
            driver.precompiledUvmEntry = entry;
            // Preserve repeatable, unguarded macro headers in custom libraries.
            driver.precompiledUvmStrict = config.useBundled.value();
            driver.sourceLoader.prependFile(entry);
        }
        catch (const std::exception& error) {
            driver.precompiledUvm.reset();
            m_shippedUvmProfile.reset();
            return uvmConfigurationError(error.what(), requireValidConfig);
        }
    }
    return true;
}

bool ServerDriver::completeUvmLibraryInputs(bool requireValidConfig) {
    // Inspect native inputs before injection: an entry may already be included by
    // a wrapper. Adding it again could change macro state or duplicate uvm_pkg.
    auto providers = [&] {
        std::vector<slang::SourceLocation> result;
        for (const auto& tree : driver.syntaxTrees)
            for (const auto& [node, _] : tree->getMetadata().nodeMeta)
                if (node->kind == syntax::SyntaxKind::PackageDeclaration &&
                    node->header->name.valueText() == "uvm_pkg")
                    result.push_back(node->header->name.location());
        return result;
    };
    auto fromSelectedEntry = [&](SourceLocation location) {
        location = sm.getFullyExpandedLoc(location);
        while (location.valid() && location != SourceLocation::NoLocation) {
            if (sm.getFullPath(location.buffer()) == m_uvmEntryPath)
                return true;
            location = sm.getIncludedFrom(location.buffer());
            if (location.valid())
                location = sm.getFullyExpandedLoc(location);
        }
        return false;
    };
    auto existing = providers();
    if (existing.empty() && driver.precompiledUvm) {
        driver.syntaxTrees.insert(driver.syntaxTrees.begin(), driver.precompiledUvm);
        existing = providers();
    }
    if (existing.empty()) {
        driver.sourceLoader.prependFile(m_uvmEntryPath);
        if (!driver.parseAllSources())
            return false;
        existing = providers();
    }
    if (existing.size() != 1)
        return uvmConfigurationError(fmt::format("expected exactly one uvm_pkg, found {}",
                                                 existing.size()),
                                     requireValidConfig);
    if (!fromSelectedEntry(existing.front()))
        return uvmConfigurationError("project uvm_pkg comes from a different entry; fix the "
                                     "project inputs or select that library",
                                     requireValidConfig);
    // Selecting just a library does not declare a complete project filelist.
    // Independently opened user modules can consume this package as a dependency.
    if (!m_config.compilationFilelist.value() && driver.sourceLoader.getFilePaths().size() == 1 &&
        driver.syntaxTrees.size() == 1) {
        const auto& root = driver.syntaxTrees.front()->root().as<syntax::CompilationUnitSyntax>();
        m_uvmLibraryOnly = root.members.size() == 1 &&
                           root.members[0]->kind == syntax::SyntaxKind::PackageDeclaration;
    }
    INFO("Using configured UVM entry {}", m_uvmEntryPath.string());
    return true;
}

bool ServerDriver::prepareTrustedUvmLibrary() {
    if (!m_config.experimentalUvmSemanticCache.value())
        return false;
    if (m_trustedUvm) {
        options.set(TrustedUvmImport{m_trustedUvm, m_driverOwner});
        m_trustedUvmReason = "retained immutable UVM semantic generation";
        return true;
    }
    if (m_uvmEntryPath.empty()) {
        m_trustedUvmReason = "trusted semantic reuse requires explicit uvm inputs";
        return false;
    }
    std::shared_ptr<syntax::SyntaxTree> prefix = driver.precompiledUvm;
    for (auto& tree : driver.syntaxTrees) {
        if (prefix)
            break;
        prefix = syntax::SyntaxTree::preparedUvmPrefix(tree);
        if (!prefix && tree->root().kind == syntax::SyntaxKind::CompilationUnit) {
            const auto& unit = tree->root().as<syntax::CompilationUnitSyntax>();
            if (unit.members.size() == 1 &&
                unit.members[0]->kind == syntax::SyntaxKind::PackageDeclaration &&
                unit.members[0]->as<syntax::ModuleDeclarationSyntax>().header->name.valueText() ==
                    "uvm_pkg")
                prefix = tree;
        }
        if (prefix)
            break;
    }
    if (!prefix) {
        m_trustedUvmReason = "UVM requires a standalone package or a compatible first-root prefix";
        return false;
    }
    if (driver.precompiledUvm) {
        // Importing only uvm_pkg cannot preserve nominal identities of other
        // packages or compilation-unit types declared by a custom wrapper.
        for (const auto* member : prefix->root().as<syntax::CompilationUnitSyntax>().members) {
            if (member->kind == syntax::SyntaxKind::PackageDeclaration &&
                member->as<syntax::ModuleDeclarationSyntax>().header->name.valueText() == "uvm_pkg")
                continue;
            if (member->kind == syntax::SyntaxKind::PackageImportDeclaration ||
                member->kind == syntax::SyntaxKind::EmptyMember)
                continue;
            m_trustedUvmReason =
                "UVM entry contains additional declarations; using native semantic analysis";
            return false;
        }
    }
    auto compilationOptions = options.getOrDefault<ast::CompilationOptions>();
    compilationOptions.flags |= ast::CompilationFlags::PackageAnalysis;
    compilationOptions.topModules.clear();
    auto compilation = std::make_shared<ast::Compilation>(compilationOptions);
    compilation->retainSourceOwner(m_driverOwner);
    compilation->addSyntaxTree(prefix);
    const auto& diagnostics = compilation->getSemanticDiagnostics();
    if (std::ranges::any_of(diagnostics, [](const auto& d) { return d.isError(); })) {
        m_trustedUvmReason = "isolated UVM semantic preparation has errors; using native analysis";
        return false;
    }
    const auto maxMiB = m_config.uvm.value()->semanticMaxMiB.value();
    const size_t charged = compilation->getTotalBytesAllocated() +
                           prefix->getRetainedSyntaxBytes() + prefix->getRetainedSourceBytes() +
                           sm.getSourceLocationTableBytes() + sm.getMemoryUsage();
    if (!maxMiB || maxMiB > 1024 || charged > size_t(maxMiB) * 1024 * 1024) {
        m_trustedUvmReason = "UVM semantic owner exceeds semanticMaxMiB; using native analysis";
        return false;
    }
    static std::atomic<uint64_t> generation = 0;
    m_trustedUvm = std::make_shared<TrustedUvmLibrary>();
    m_trustedUvm->sourceOwner = m_driverOwner;
    m_trustedUvm->compilation = std::move(compilation);
    m_trustedUvm->sources = sm.captureImmutablePrefix(prefix->getSourceBufferIds());
    m_trustedUvm->generation = ++generation;
    m_trustedUvm->semanticBytes = charged;
    m_trustedUvm->shippedProfile = m_shippedUvmProfile;
    m_trustedUvm->preparseDependencies = m_uvmPreparseDependencies;
    m_trustedUvm->parseCachePath = m_uvmParseCachePath;
    m_trustedUvm->preparseKey = m_uvmPreparseKey;
    for (auto id : prefix->getSourceBufferIds()) {
        const auto& path = sm.getFullPath(id);
        if (!path.empty())
            m_trustedUvm->files.emplace(path.lexically_normal().generic_string());
    }
    options.set(TrustedUvmImport{m_trustedUvm, m_driverOwner});
    m_trustedUvmReason =
        "prepared immutable UVM semantics; rebuild explicitly after library changes";
    return true;
}

bool ServerDriver::isTrustedUvmSource(const fs::path& path) const {
    if (m_trustedUvm && m_trustedUvm->files.contains(path.lexically_normal().generic_string()))
        return true;
    if (driver.precompiledUvm) {
        auto relative = path.lexically_normal().lexically_relative(driver.precompiledUvmRoot);
        return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
    }
    return false;
}

std::optional<ServerDriver::PackageIncludeContext> ServerDriver::trustedUvmSourceContext(
    const URI& uri) const {
    if (!isTrustedUvmSource(uri.getPath()))
        return std::nullopt;
    const auto& tree = m_trustedUvm ? m_trustedUvm->compilation->getSyntaxTrees().front()
                                    : driver.precompiledUvm;
    for (auto id : tree->getSourceBufferIds())
        if (sm.getFullPath(id).generic_string() == uri.getPath())
            return PackageIncludeContext{tree, id};
    return std::nullopt;
}

void ServerDriver::invalidateUvmParseCache() {
    if (!m_uvmParseCachePath.empty())
        fs::remove(m_uvmParseCachePath);
}

rfl::Generic ServerDriver::trustedUvmStatus() const {
    struct Status {
        bool enabled;
        bool prepared;
        uint64_t generation;
        uint64_t consumers;
        size_t chargedBytes;
        size_t astBytes;
        size_t fixedSourceFiles;
        bool diagnosticsShareOwner;
        bool shippedPreparse;
        std::string shippedProfile;
        bool projectCacheHit;
        std::string projectCachePath;
        double syntaxParseMs, archiveWriteMs, archiveLoadMs;
        std::string reason;
    };
    return rfl::to_generic(
        Status{m_config.experimentalUvmSemanticCache.value(), bool(m_trustedUvm),
               m_trustedUvm ? m_trustedUvm->generation : 0,
               m_trustedUvm ? m_trustedUvm->consumers.load() : 0,
               m_trustedUvm ? m_trustedUvm->semanticBytes : 0,
               m_trustedUvm ? m_trustedUvm->compilation->getTotalBytesAllocated() : 0,
               m_trustedUvm ? m_trustedUvm->files.size() : 0, bool(m_trustedUvm),
               m_shippedUvmProfile.has_value(), m_shippedUvmProfile.value_or(""),
               m_uvmProjectCacheHit, m_uvmParseCachePath.string(), m_uvmParseMs,
               m_uvmArchiveWriteMs, m_uvmArchiveLoadMs, m_trustedUvmReason});
}

} // namespace server
