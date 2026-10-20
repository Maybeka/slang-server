// SPDX-License-Identifier: MIT
#include "ServerDriver.h"
#include "ast/TrustedUvmLibrary.h"
#include "diagnostics/DiagnosticProcess.h"
#include "util/Logging.h"
#include <set>
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
    std::shared_ptr<syntax::SyntaxTree> prefix;
    for (auto& tree : driver.syntaxTrees) {
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
    return m_trustedUvm && m_trustedUvm->files.contains(path.lexically_normal().generic_string());
}

std::optional<ServerDriver::PackageIncludeContext> ServerDriver::trustedUvmSourceContext(
    const URI& uri) const {
    if (!isTrustedUvmSource(uri.getPath()))
        return std::nullopt;
    const auto& tree = m_trustedUvm->compilation->getSyntaxTrees().front();
    for (auto id : tree->getSourceBufferIds())
        if (sm.getFullPath(id).generic_string() == uri.getPath())
            return PackageIncludeContext{tree, id};
    return std::nullopt;
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
        std::string reason;
    };
    return rfl::to_generic(Status{
        m_config.experimentalUvmSemanticCache.value(), bool(m_trustedUvm),
        m_trustedUvm ? m_trustedUvm->generation : 0,
        m_trustedUvm ? m_trustedUvm->consumers.load() : 0,
        m_trustedUvm ? m_trustedUvm->semanticBytes : 0,
        m_trustedUvm ? m_trustedUvm->compilation->getTotalBytesAllocated() : 0,
        m_trustedUvm ? m_trustedUvm->files.size() : 0, bool(m_trustedUvm), m_trustedUvmReason});
}

} // namespace server
