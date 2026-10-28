// SPDX-License-Identifier: MIT
#pragma once

#include "lsp/LspTypes.h"
#include "lsp/URI.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "slang/diagnostics/Diagnostics.h"

namespace slang::ast {
class Compilation;
}
namespace server {
// One committed native publication. Weak semantic ownership never keeps a retired
// compilation alive. Consumers must lock and verify the owner before reading raw values.
struct NativeDiagnosticSnapshot {
    using OrderKey = std::tuple<std::string, unsigned, unsigned, slang::DiagCode>;
    struct PhysicalDiagnostic {
        URI uri;
        lsp::Diagnostic value;
    };
    struct Rendered {
        std::optional<OrderKey> key;
        std::vector<PhysicalDiagnostic> values;
    };
    std::weak_ptr<slang::ast::Compilation> owner;
    std::weak_ptr<const void> context;
    uint64_t generation = 0;
    std::string config;
    std::array<slang::Diagnostics, 3> diagnostics;
    // Physical routes are captured by the real client after filtering and macro mapping.
    // Editor aliases are reapplied by the consumer, never stored as permanent identities.
    // An empty route means that this raw diagnostic produced no LSP diagnostic.
    std::array<std::vector<std::vector<URI>>, 3> routes;
    // Aligned with diagnostics. Stored before editor alias resolution.
    std::array<std::vector<Rendered>, 3> rendered;
};
} // namespace server
