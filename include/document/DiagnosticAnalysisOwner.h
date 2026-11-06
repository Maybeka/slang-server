// SPDX-License-Identifier: MIT
#pragma once
#include <memory>
#include <utility>

namespace lsp {
class RequestContext;
}
namespace slang::ast {
class Compilation;
class SubroutineSymbol;
} // namespace slang::ast
namespace slang::analysis {
class AnalysisManager;
}

namespace server {
class ActiveDesignContext;
class BodyDiagnosticCache;

/// Pins the original analysis wrapper together with all borrowed analysis views.
/// Construction from a design preserves its source-namespace ownership lease.
class DiagnosticAnalysisOwner {
public:
    DiagnosticAnalysisOwner() = default;
    DiagnosticAnalysisOwner(const DiagnosticAnalysisOwner&) = default;
    DiagnosticAnalysisOwner& operator=(const DiagnosticAnalysisOwner&) = default;
    DiagnosticAnalysisOwner(DiagnosticAnalysisOwner&& other) noexcept :
        owner(std::move(other.owner)), full(std::exchange(other.full, nullptr)),
        intrinsic(std::exchange(other.intrinsic, nullptr)),
        task(std::exchange(other.task, nullptr)) {}
    DiagnosticAnalysisOwner& operator=(DiagnosticAnalysisOwner&& other) noexcept {
        if (this != &other) {
            owner = std::move(other.owner);
            full = std::exchange(other.full, nullptr);
            intrinsic = std::exchange(other.intrinsic, nullptr);
            task = std::exchange(other.task, nullptr);
        }
        return *this;
    }
    static DiagnosticAnalysisOwner pending(const ActiveDesignContext&);
    static DiagnosticAnalysisOwner prepared(const ActiveDesignContext&, const lsp::RequestContext&);
    void selectTaskTiming(const slang::ast::SubroutineSymbol&);

    const std::shared_ptr<slang::ast::Compilation>& compilation() const { return owner; }
    const slang::analysis::AnalysisManager* complete() const { return full; }
    const slang::analysis::AnalysisManager* procedures() const { return intrinsic; }
    const slang::analysis::AnalysisManager* timing() const { return task; }
    bool isPrepared() const { return full != nullptr; }

private:
    std::shared_ptr<slang::ast::Compilation> owner;
    slang::analysis::AnalysisManager* full = nullptr;
    const slang::analysis::AnalysisManager* intrinsic = nullptr;
    const slang::analysis::AnalysisManager* task = nullptr;
    slang::analysis::AnalysisManager& manager() const;
    friend class BodyDiagnosticCache;
};
} // namespace server
