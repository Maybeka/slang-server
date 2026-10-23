// SPDX-License-Identifier: MIT
#pragma once

#include "lsp/URI.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "slang/diagnostics/Diagnostics.h"

namespace slang::ast {
class Compilation;
}
namespace server {
// One committed native publication. Weak semantic ownership never keeps a retired
// compilation alive. Consumers must lock and verify the owner before reading raw values.
struct NativeDiagnosticSnapshot {
    std::weak_ptr<slang::ast::Compilation> owner;
    std::weak_ptr<const void> context;
    uint64_t generation = 0;
    std::string config;
    std::array<slang::Diagnostics, 3> diagnostics;
    // Physical routes are captured by the real client after filtering and macro mapping.
    // Editor aliases are reapplied by the consumer, never stored as permanent identities.
    // An empty route means that this raw diagnostic produced no LSP diagnostic.
    std::array<std::vector<std::vector<URI>>, 3> routes;
};
} // namespace server
