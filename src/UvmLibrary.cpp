// SPDX-License-Identifier: MIT
#include "ServerDriver.h"
#include "util/Logging.h"
#include <set>

#include "slang/driver/SourceLoader.h"
#include "slang/parsing/ParserMetadata.h"
#include "slang/syntax/AllSyntax.h"

namespace server {
namespace fs = std::filesystem;

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
    if (config.sourceDirectory.value().empty())
        return uvmConfigurationError("sourceDirectory must not be empty", requireValidConfig);
    auto directory = fs::path(config.sourceDirectory.value());
    if (directory.is_relative())
        directory = base / directory;
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
    if (config.entryFile.value()) {
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

} // namespace server
