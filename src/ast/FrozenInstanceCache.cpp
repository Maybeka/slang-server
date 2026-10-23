// SPDX-License-Identifier: MIT
#include "ast/FrozenInstanceCache.h"

#include "util/SlangExtensions.h"
#include <algorithm>
#include <set>

#include "slang/syntax/AllSyntax.h"

namespace server {
using namespace slang;
namespace {
void part(std::string& out, std::string_view value) {
    auto size = uint64_t(value.size());
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(char((size >> (i * 8)) & 255));
    out.append(value);
}
struct TaskRanges {
    SourceManager& sm;
    const std::set<std::string>* affectedAncestors = nullptr;
    size_t visited = 0;
    std::map<std::string, std::vector<std::pair<size_t, size_t>>> ranges;
    bool declarations(const syntax::SyntaxNode& node) {
        ++visited;
        const auto name = syntax::toString(node.kind);
        if (name.find("Declaration") != std::string_view::npos ||
            name.find("Directive") != std::string_view::npos ||
            name.find("DefParam") != std::string_view::npos ||
            name.find("Loop") != std::string_view::npos ||
            name.find("BlockStatement") != std::string_view::npos)
            return true;
        for (size_t i = 0; i < node.getChildCount(); ++i)
            if (auto child = node.childNode(i); child && declarations(*child))
                return true;
        return false;
    }
    void walk(const syntax::SyntaxNode& node, unsigned classes = 0, unsigned routines = 0) {
        using syntax::SyntaxKind;
        ++visited;
        // A declaration can include other physical files or span input roots.
        // Skip it only when one physical occurrence contains both ends and no
        // changed source is included through that occurrence's physical file.
        // Candidate failure still rebuilds the entire forest before admission.
        if (affectedAncestors && (syntax::ModuleDeclarationSyntax::isKind(node.kind) ||
                                  node.kind == SyntaxKind::ClassDeclaration)) {
            auto first = sm.getFullyExpandedLoc(node.getFirstToken().location());
            auto last = sm.getFullyExpandedLoc(node.getLastToken().location());
            if (first.valid() && last.valid() && first.buffer() == last.buffer() &&
                !affectedAncestors->contains(sm.getFullPath(first.buffer()).string()))
                return;
        }
        classes += node.kind == SyntaxKind::ClassDeclaration;
        if (node.kind == SyntaxKind::TaskDeclaration && classes && !routines) {
            const auto& task = node.as<syntax::FunctionDeclarationSyntax>();
            bool eligible = task.prototype->lifetime.kind != parsing::TokenKind::StaticKeyword;
            if (auto parent = node.parent.get();
                parent && parent->kind == SyntaxKind::ClassMethodDeclaration)
                for (auto token : parent->as<syntax::ClassMethodDeclarationSyntax>().qualifiers)
                    eligible &= token.kind != parsing::TokenKind::StaticKeyword;
            for (const auto* item : task.items)
                eligible &= syntax::StatementSyntax::isKind(item->kind) && !declarations(*item);
            auto begin = task.semi.location();
            auto end = task.end.location();
            if (eligible && begin.valid() && end.valid() && !sm.isMacroLoc(begin) &&
                !sm.isMacroLoc(end) && begin.buffer() == end.buffer()) {
                const auto bytes = sm.getSourceText(begin.buffer());
                const size_t first = begin.offset() + task.semi.rawText().size();
                const size_t last = end.offset();
                if (first <= last && last <= bytes.size() &&
                    bytes.substr(first, last - first).find('`') == std::string_view::npos) {
                    ranges[sm.getFullPath(begin.buffer()).string()].emplace_back(first, last);
                    return;
                }
            }
        }
        routines += node.kind == SyntaxKind::TaskDeclaration ||
                    node.kind == SyntaxKind::FunctionDeclaration;
        for (size_t i = 0; i < node.getChildCount(); ++i)
            if (auto child = node.childNode(i))
                walk(*child, classes, routines);
    }
    std::optional<std::string> normalize(const std::string& path, std::string_view bytes) {
        auto& values = ranges[path];
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        std::string key;
        part(key, std::to_string(values.size()));
        size_t offset = 0;
        for (const auto& [begin, end] : values) {
            if (begin < offset || end > bytes.size())
                return {};
            part(key, bytes.substr(offset, begin - offset));
            // __LINE__ and line-directive state after this task depend on its
            // physical line count, even though its runtime statements do not
            // participate in elaboration. Treat CRLF as one logical newline.
            size_t lines = 0;
            for (size_t i = begin; i < end; ++i) {
                if (bytes[i] == '\r') {
                    ++lines;
                    if (i + 1 < end && bytes[i + 1] == '\n')
                        ++i;
                }
                else if (bytes[i] == '\n')
                    ++lines;
            }
            part(key, std::to_string(lines));
            offset = end;
        }
        part(key, bytes.substr(offset));
        return key;
    }
};
} // namespace

FrozenInstanceCache::FrozenInstanceCache(std::vector<std::string> paths, size_t budgetBytes,
                                         SourceManager& sourceManager, Witness witness,
                                         Witness requestWitness) :
    m_budgetBytes(budgetBytes), m_sourceManager(sourceManager), m_witness(std::move(witness)),
    m_requestWitness(std::move(requestWitness)) {
    for (auto& path : paths)
        m_selections.push_back({.instance = std::move(path)});
}
void FrozenInstanceCache::resolveSelections(ast::Compilation& compilation,
                                            const lsp::RequestContext& ctx) {
    commitSelections(prepareSelections(compilation, ctx));
}
std::vector<FrozenInstanceCache::Selection> FrozenInstanceCache::prepareSelections(
    ast::Compilation& compilation, const lsp::RequestContext& ctx) const {
    auto selections = m_selections;
    for (auto& selection : selections) {
        ctx.checkpoint("during full-model frozen selection");
        selection.resolved = false;
        selection.modules.clear();
        selection.reason = "exact instance path was not found";
        if (selection.instance.empty())
            continue;
        ast::LookupResult result;
        ast::ASTContext context(compilation.getRoot(), ast::LookupLocation::max);
        ast::Lookup::name(compilation.parseName(selection.instance), context,
                          ast::LookupFlags::AllowUnnamedGenerate, result);
        const auto* instance = result.found ? result.found->as_if<ast::InstanceSymbol>() : nullptr;
        if (!instance || instance->getHierarchicalPath() != selection.instance) {
            if (result.found)
                selection.reason = "selection must name one canonical module/interface instance";
            continue;
        }
        std::set<std::string> names;
        auto visit = [&](auto&& self, const ast::Scope& scope) -> void {
            for (const auto& member : scope.members()) {
                ctx.throwIfCancelled("during full-model frozen subtree");
                if (auto* child = member.as_if<ast::InstanceSymbol>()) {
                    if (child->getDefinition().definitionKind == ast::DefinitionKind::Module)
                        names.emplace(child->getDefinition().name);
                    self(self, child->body);
                }
                else if (auto* array = member.as_if<ast::InstanceArraySymbol>())
                    self(self, *array);
                else if (auto* block = member.as_if<ast::GenerateBlockSymbol>())
                    self(self, *block);
                else if (auto* blocks = member.as_if<ast::GenerateBlockArraySymbol>())
                    for (const auto* block : blocks->entries)
                        self(self, *block);
            }
        };
        if (instance->getDefinition().definitionKind == ast::DefinitionKind::Module)
            names.emplace(instance->getDefinition().name);
        visit(visit, instance->body);
        selection.modules.assign(names.begin(), names.end());
        selection.resolved = true;
        selection.reason =
            "instance subtree selected; hierarchy values require an input certificate";
    }
    ctx.checkpoint("after full-model frozen selection");
    return selections;
}
bool FrozenInstanceCache::selected(std::string_view path) const {
    return std::ranges::any_of(m_selections, [&](const auto& selection) {
        return selection.resolved &&
               (path == selection.instance ||
                (path.starts_with(selection.instance) && path.size() > selection.instance.size() &&
                 path[selection.instance.size()] == '.'));
    });
}
std::vector<std::string> FrozenInstanceCache::modules() const {
    std::set<std::string> names;
    for (const auto& selection : m_selections)
        names.insert(selection.modules.begin(), selection.modules.end());
    return {names.begin(), names.end()};
}
void FrozenInstanceCache::clear(std::string reason) {
    m_entries.clear();
    m_sourceProofs.clear();
    std::string{}.swap(m_certificate);
    std::string{}.swap(m_checkedGeneration);
    m_statistics.retainedBytes = 0;
    m_statistics.eligibleTaskBodies = 0;
    m_statistics.lastReason = std::move(reason);
}
void FrozenInstanceCache::decline(std::string reason) {
    clear(std::move(reason));
    ++m_statistics.declines;
}
size_t FrozenInstanceCache::baseBytes() const {
    size_t bytes = m_certificate.capacity() + m_checkedGeneration.capacity();
    for (const auto& [path, proof] : m_sourceProofs)
        bytes += sizeof(std::pair<const std::string, SourceProof>) + 64 + path.capacity() +
                 proof.normalized.capacity();
    return bytes;
}
std::optional<FrozenInstanceCache::Certificate> FrozenInstanceCache::certify(
    std::span<const Tree> trees, const lsp::RequestContext& ctx, bool syntaxUnchanged) {
    auto& sm = m_sourceManager;
    if (!m_budgetBytes || !m_witness) {
        decline("instance hierarchy retention disabled by budget or witness");
        return {};
    }
    Certificate result;
    std::string structure;
    part(structure, std::to_string(trees.size()));
    part(result.generation, std::to_string(trees.size()));
    std::map<std::string, BufferID> buffers;
    for (const auto& tree : trees) {
        ctx.throwIfCancelled("while certifying instance hierarchy inputs");
        if (!tree || &tree->sourceManager() != &sm || tree->getSourceLibrary() ||
            !hasValidBuffers(sm, tree) ||
            std::ranges::any_of(tree->diagnostics(),
                                [](const auto& diag) { return diag.isError(); })) {
            decline("instance certificate requires current, error-free ordinary syntax inputs");
            return {};
        }
        part(result.generation, tree->isLibraryUnit ? "library" : "ordinary");
        part(structure, tree->isLibraryUnit ? "library" : "ordinary");
        part(structure, std::to_string(tree->getSourceBufferIds().size()));
        part(result.generation, std::to_string(tree->getSourceBufferIds().size()));
        for (auto buffer : tree->getSourceBufferIds()) {
            auto path = sm.getFullPath(buffer).string();
            if (path.empty()) {
                decline("instance certificate cannot identify a physical source");
                return {};
            }
            part(result.generation, path);
            part(structure, path);
            part(result.generation, std::to_string(buffer.getId()));
            buffers.try_emplace(path, buffer);
            const auto bytes = sm.getSourceText(buffer);
            auto [it, inserted] = result.sources.try_emplace(path, bytes);
            if (!inserted && it->second != bytes) {
                decline("instance certificate found conflicting physical source occurrences");
                return {};
            }
        }
    }
    const bool sameGeneration = result.generation == m_checkedGeneration;
    if (!sameGeneration) {
        std::set<std::string> changed, ancestors;
        bool compatible = structure == m_certificate && buffers.size() == m_sourceProofs.size();
        for (const auto& [path, buffer] : buffers) {
            const auto found = m_sourceProofs.find(path);
            if (found == m_sourceProofs.end() || !sm.isLatestData(found->second.buffer) ||
                sm.getFullPath(found->second.buffer).string() != path ||
                sm.getSourceText(found->second.buffer) != result.sources.at(path))
                changed.insert(path);
        }
        for (const auto& tree : trees)
            for (auto buffer : tree->getSourceBufferIds())
                if (changed.contains(sm.getFullPath(buffer).string())) {
                    auto location = SourceLocation(buffer, 0);
                    while (location.valid() && location != SourceLocation::NoLocation) {
                        location = sm.getFullyExpandedLoc(location);
                        ancestors.insert(sm.getFullPath(location.buffer()).string());
                        location = sm.getIncludedFrom(location.buffer());
                    }
                }
        size_t visited = 0;
        if (compatible) {
            TaskRanges candidate{sm, &ancestors};
            if (!changed.empty())
                for (const auto& tree : trees) {
                    ctx.throwIfCancelled("before comparing changed instance source roots");
                    candidate.walk(tree->root());
                }
            for (const auto& path : changed) {
                auto normalized = candidate.normalize(path, result.sources.at(path));
                if (!normalized || *normalized != m_sourceProofs.at(path).normalized) {
                    compatible = false;
                    break;
                }
            }
            visited += candidate.visited;
        }
        if (!compatible) {
            // Changed preprocessing or declaration structure can alter the class
            // context of an unchanged physical file. Never admit a new baseline
            // using its old masks after the partial comparison fails.
            TaskRanges full{sm};
            for (const auto& tree : trees) {
                ctx.throwIfCancelled("before rebuilding an instance input certificate");
                full.walk(tree->root());
            }
            std::map<std::string, SourceProof> proofs;
            for (const auto& [path, bytes] : result.sources) {
                ctx.throwIfCancelled("while rebuilding instance source proofs");
                auto normalized = full.normalize(path, bytes);
                if (!normalized) {
                    decline("overlapping instance certificate task ranges");
                    return {};
                }
                proofs.emplace(path, SourceProof{buffers.at(path), std::move(*normalized),
                                                 full.ranges.at(path).size()});
            }
            visited += full.visited;
            if (!m_certificate.empty())
                ++m_statistics.invalidations;
            clear("instance elaboration inputs changed");
            m_sourceProofs = std::move(proofs);
            m_certificate = std::move(structure);
            ++m_statistics.fullCertifications;
        }
        else {
            for (const auto& [path, buffer] : buffers)
                m_sourceProofs.at(path).buffer = buffer;
            ++m_statistics.incrementalCertifications;
            m_statistics.reusedSourceFiles += buffers.size() - changed.size();
        }
        m_statistics.eligibleTaskBodies = 0;
        for (const auto& [_, proof] : m_sourceProofs)
            m_statistics.eligibleTaskBodies += proof.taskBodies;
        m_statistics.lastVisitedSyntaxNodes = visited;
        m_statistics.visitedSyntaxNodes += visited;
        m_checkedGeneration = result.generation;
        if (baseBytes() > m_budgetBytes) {
            decline("instance input certificate exceeds its reserved budget");
            return {};
        }
        m_statistics.retainedBytes = baseBytes();
        for (const auto& entry : m_entries)
            m_statistics.retainedBytes += entry.bytes;
        while (!m_entries.empty() && m_statistics.retainedBytes > m_budgetBytes) {
            m_statistics.retainedBytes -= m_entries.front().bytes;
            m_entries.pop_front();
            ++m_statistics.evictions;
        }
    }
    ctx.throwIfCancelled("before accepting instance input proof");
    if (sameGeneration && syntaxUnchanged && m_requestWitness && m_requestWitness(ctx))
        ++m_statistics.requestWitnessReuses;
    else if (!m_witness(ctx)) {
        decline("instance source witness unavailable");
        return {};
    }
    return result;
}
std::optional<std::string> FrozenInstanceCache::find(std::string_view key,
                                                     const Certificate& certificate) {
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->key != key)
            continue;
        for (const auto& [path, bytes] : it->locations) {
            const auto found = certificate.sources.find(path);
            if (found == certificate.sources.end() || found->second != bytes) {
                m_statistics.retainedBytes -= it->bytes;
                m_entries.erase(it);
                ++m_statistics.invalidations;
                m_statistics.lastReason = "cached hierarchy source locations changed";
                return {};
            }
        }
        if (it->generation != certificate.generation) {
            ++m_statistics.crossEditHits;
            m_statistics.retainedBytes -= it->generation.capacity();
            it->bytes -= it->generation.capacity();
            it->generation = certificate.generation;
            it->bytes += it->generation.capacity();
            m_statistics.retainedBytes += it->generation.capacity();
        }
        auto payload = it->payload;
        m_entries.splice(m_entries.end(), m_entries, it);
        while (!m_entries.empty() && m_statistics.retainedBytes > m_budgetBytes) {
            m_statistics.retainedBytes -= m_entries.front().bytes;
            m_entries.pop_front();
            ++m_statistics.evictions;
        }
        ++m_statistics.hits;
        m_statistics.lastReason = "certified instance hierarchy values reused";
        return payload;
    }
    return {};
}
void FrozenInstanceCache::store(std::string key, std::string payload,
                                const std::vector<hier::HierItem_t>& values,
                                const Certificate& certificate, const lsp::RequestContext& ctx) {
    if (!m_witness || !m_witness(ctx))
        return;
    Entry entry{.key = std::move(key),
                .payload = std::move(payload),
                .generation = certificate.generation};
    bool complete = true;
    auto location = [&](const lsp::Location& loc) {
        const std::string path(loc.uri.getPath());
        auto found = certificate.sources.find(path);
        if (found == certificate.sources.end())
            complete = false;
        else
            entry.locations.try_emplace(path, found->second);
    };
    auto visit = [&](auto&& self, const std::vector<hier::HierItem_t>& items) -> void {
        for (const auto& item : items)
            rfl::visit(
                [&](const auto& value) {
                    location(value.instLoc);
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, hier::Instance>)
                        location(value.declLoc);
                    if constexpr (!std::is_same_v<T, hier::Var>)
                        self(self, value.children);
                },
                item);
    };
    visit(visit, values);
    if (!complete) {
        m_statistics.lastReason = "hierarchy location is outside the certified source graph";
        ++m_statistics.declines;
        return;
    }
    entry.bytes = sizeof(Entry) + 3 * sizeof(void*) + entry.key.capacity() +
                  entry.payload.capacity() + entry.generation.capacity();
    for (const auto& [path, bytes] : entry.locations)
        entry.bytes += sizeof(std::pair<const std::string, std::string>) + 64 + path.capacity() +
                       bytes.capacity();
    if (entry.bytes > m_budgetBytes - baseBytes()) {
        m_statistics.lastReason = "hierarchy values exceed the reserved instance budget";
        ++m_statistics.declines;
        return;
    }
    while (!m_entries.empty() &&
           (m_entries.size() >= 128 || entry.bytes > m_budgetBytes - m_statistics.retainedBytes)) {
        m_statistics.retainedBytes -= m_entries.front().bytes;
        m_entries.pop_front();
        ++m_statistics.evictions;
    }
    m_statistics.retainedBytes += entry.bytes;
    m_entries.push_back(std::move(entry));
    ++m_statistics.preparations;
    m_statistics.lastReason = "instance hierarchy values retained";
}
} // namespace server
