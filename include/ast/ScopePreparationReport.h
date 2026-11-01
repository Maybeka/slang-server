// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace server {
// Values only: a report must never keep a rejected syntax tree or compiler alive.
struct ScopePreparationReport {
    struct Attempt {
        std::string route, outcome, category;
    };
    struct Refusal {
        std::string category;
        size_t declarations = 0;
        std::vector<std::string> sampleDeclarations, sampleBindings;
    };
    std::string status = "not-requested", route, failureCategory;
    size_t candidateDeclarations = 0, preparedDeclarations = 0, excludedDeclarations = 0;
    size_t deferredModuleNames = 0, preparedUnitTypes = 0, preparedUnitSubroutines = 0;
    size_t uncategorizedDeclarations = 0, samples = 0, bindings = 0;
    bool samplesTruncated = false, attemptsTruncated = false;
    std::vector<Attempt> attempts;
    std::vector<Refusal> refusals;

    static std::string_view bounded(std::string_view text) {
        if (text.size() <= 160) return text;
        size_t end = 160;
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        return text.substr(0, end);
    }
    void attempt(std::string_view name, bool candidate, std::string_view category = {}) {
        if (attempts.size() < 8)
            attempts.push_back({std::string(bounded(name)), candidate ? "candidate" : "not-applicable",
                                std::string(bounded(category))});
        else attemptsTruncated = true;
    }
    void exclude(std::string_view category, std::string_view name, std::string_view binding = {}) {
        ++excludedDeclarations;
        category = bounded(category);
        auto found = std::ranges::find(refusals, category, &Refusal::category);
        if (found == refusals.end()) {
            if (refusals.size() == 32) {
                ++uncategorizedDeclarations;
                samplesTruncated = true;
                return;
            }
            refusals.push_back({std::string(category), 0, {}, {}});
            found = std::prev(refusals.end());
        }
        ++found->declarations;
        if (!binding.empty() && std::ranges::find(found->sampleBindings, bounded(binding)) ==
                                    found->sampleBindings.end()) {
            if (bindings < 8 && found->sampleBindings.size() < 2) {
                found->sampleBindings.emplace_back(bounded(binding));
                ++bindings;
                samplesTruncated |= binding.size() > 160;
            }
            else samplesTruncated = true;
        }
        if (samples < 8) {
            found->sampleDeclarations.emplace_back(bounded(name));
            ++samples;
            samplesTruncated |= name.size() > 160;
        }
        else
            samplesTruncated = true;
    }
    void choose(std::string_view selected) {
        selected = bounded(selected);
        route = selected;
        for (auto& item : attempts)
            if (item.outcome == "candidate" || item.outcome == "used")
                item.outcome = item.route == selected ? "candidate" : "superseded";
        if (std::ranges::none_of(attempts, [&](const auto& item) { return item.route == selected; })) {
            if (attempts.size() == 8) {
                attempts.back() = {std::string(selected), "candidate", {}};
                attemptsTruncated = true;
            }
            else attempt(selected, true);
        }
    }
    void decline(std::string_view category) {
        status = "declined";
        failureCategory = bounded(category);
        route = "native";
        for (auto& item : attempts)
            if (item.outcome == "candidate" || item.outcome == "used")
                item.outcome = "discarded";
        // Tentative graph exclusions are not final exclusions when no provider
        // was published, or when a subsequent route might retain those names.
        refusals.clear();
        candidateDeclarations = preparedDeclarations = excludedDeclarations = deferredModuleNames = preparedUnitTypes = preparedUnitSubroutines = 0;
        uncategorizedDeclarations = samples = bindings = 0;
        samplesTruncated = false;
    }
    void commit() {
        status = "prepared";
        for (auto& item : attempts)
            if (item.route == route)
                item.outcome = "used";
    }
    size_t chargedBytes() const {
        size_t bytes = sizeof(*this) + status.capacity() + route.capacity() + failureCategory.capacity() +
                       attempts.capacity() * sizeof(Attempt) + refusals.capacity() * sizeof(Refusal);
        for (const auto& item : attempts)
            bytes += item.route.capacity() + item.outcome.capacity() + item.category.capacity();
        for (const auto& item : refusals) {
            bytes += item.category.capacity() + item.sampleDeclarations.capacity() * sizeof(std::string);
            for (const auto& name : item.sampleDeclarations)
                bytes += name.capacity();
            bytes += item.sampleBindings.capacity() * sizeof(std::string);
            for (const auto& name : item.sampleBindings)
                bytes += name.capacity();
        }
        return bytes;
    }
};
} // namespace server
