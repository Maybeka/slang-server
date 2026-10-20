// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>

#include "slang/ast/Compilation.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

namespace slang::driver {
class Driver;
}

namespace server {
/// One explicitly trusted UVM generation. The original SDK driver retains source
/// storage / source-library identities, but never retains mutable consumer ASTs.
struct TrustedUvmLibrary {
    std::shared_ptr<slang::driver::Driver> sourceOwner;
    std::shared_ptr<slang::ast::Compilation> compilation;
    std::shared_ptr<const slang::SourceManager::ImmutablePrefix> sources;
    uint64_t generation = 0;
    size_t semanticBytes = 0;
    std::optional<std::string> shippedProfile;
    std::set<std::string> preparseDependencies;
    std::filesystem::path parseCachePath;
    std::string preparseKey;
    std::unordered_set<std::string> files;
    std::atomic<uint64_t> consumers = 0;
};

struct TrustedUvmImport {
    std::shared_ptr<TrustedUvmLibrary> library;
    std::shared_ptr<slang::driver::Driver> sourceOwner;
};

inline bool importTrustedUvm(slang::ast::Compilation& compilation, slang::SourceManager& sources,
                             const slang::Bag& options) {
    auto setting = options.get<TrustedUvmImport>();
    if (!setting || !setting->library)
        return false;
    if (setting->sourceOwner)
        compilation.retainSourceOwner(setting->sourceOwner);
    compilation.importPackage(setting->library->compilation, "uvm_pkg", sources);
    ++setting->library->consumers;
    return true;
}
} // namespace server
