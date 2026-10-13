//------------------------------------------------------------------------------
// SlangDoc.cpp
// Implementation of SlangDoc class
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "document/SlangDoc.h"

#include "ServerDriver.h"
#include "document/ShallowAnalysis.h"
#include "lsp/RequestContext.h"
#include "lsp/URI.h"
#include "util/Converters.h"
#include "util/Logging.h"
#include "util/SlangExtensions.h"
#include <algorithm>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdexcept>
#include <string>
#include <string_view>

#include "slang/ast/Compilation.h"
#include "slang/diagnostics/CompilationDiags.h"
#include "slang/diagnostics/DeclarationsDiags.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/driver/SourceLoader.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"
#include "slang/util/FlatMap.h"
namespace server {

using namespace slang;

// Match DiagnosticEngine attribution: macro bodies report at the call site,
// while substituted arguments report at the argument's original location.
static SourceLocation diagnosticLocation(const SourceManager& sm, SourceLocation loc) {
    while (sm.isMacroLoc(loc))
        loc = sm.isMacroArgLoc(loc) ? sm.getOriginalLoc(loc) : sm.getExpansionLoc(loc);
    return loc;
}

SlangDoc::SlangDoc(ServerDriver& driver, URI uri, SourceBuffer buffer) :
    m_driver(driver), m_sourceManager(driver.sm), m_options(driver.options), m_uri(uri),
    m_wsRelativePathOffset(driver.getWsRelativePathOffset(m_uri.getPath())), m_buffer(buffer) {
}

std::optional<SourceLocation> SlangDoc::getLocation(const lsp::Position& position,
                                                    const lsp::RequestContext& ctx) {
    return toSourceLocation(getAnalysis(ctx)->getBuffer(), position, m_sourceManager);
}

std::shared_ptr<SlangDoc> SlangDoc::fromTree(ServerDriver& driver,
                                             std::shared_ptr<syntax::SyntaxTree> tree,
                                             bool requiresNativeReplay) {
    if (tree->getSourceBufferIds().empty())
        throw std::invalid_argument("Native compilation unit has no source buffers");
    auto buffer = SourceBuffer{
        .data = driver.sm.getSourceText(tree->getSourceBufferIds()[0]),
        .library = tree->getSourceLibrary(),
        .id = tree->getSourceBufferIds()[0],
    };
    auto uri = URI::fromFile(driver.sm.getFullPath(tree->getSourceBufferIds()[0]));
    auto ret = std::make_shared<SlangDoc>(driver, uri, buffer);
    NativeCompilationUnit unit;
    unit.options = tree->options();
    unit.library = tree->getSourceLibrary();
    unit.isLibraryUnit = tree->isLibraryUnit;
    unit.requiresNativeReplay = requiresNativeReplay;
    // SyntaxTree pushes roots in reverse order before preprocessing them. Included
    // buffers follow those roots and must never become separately parsed roots.
    for (auto id : tree->getSourceBufferIds()) {
        if (!driver.sm.getIncludedFrom(id).valid()) {
            unit.rootPaths.push_back(driver.sm.getFullPath(id));
            unit.rootSortKeys.push_back(driver.sm.getSortKey(id));
        }
    }
    std::reverse(unit.rootPaths.begin(), unit.rootPaths.end());
    std::reverse(unit.rootSortKeys.begin(), unit.rootSortKeys.end());
    if (unit.rootPaths.empty())
        throw std::invalid_argument("Native compilation unit has no source roots");

    auto sourceOptions = unit.options.getOrDefault<slang::driver::SourceOptions>();
    unit.requiresNativeReplay |= sourceOptions.singleUnit &&
                                 (unit.library ||
                                  (unit.isLibraryUnit && sourceOptions.librariesInheritMacros));
    // Driver callbacks can refer to one parse invocation's stack. They cannot
    // safely be retained for a later local replay of the options bag.
    if (auto* ppOptions = unit.options.get<parsing::PreprocessorOptions>())
        unit.requiresNativeReplay |= bool(ppOptions->bufferChangeCB);
    // Also recognize observed inherited definitions even for callers that did
    // not obtain the tree from Driver. This cannot recover overwritten inputs;
    // callers must explicitly mark other native consumers such as -C units.
    flat_hash_set<BufferID> ownBuffers;
    for (auto id : tree->getSourceBufferIds())
        ownBuffers.insert(id);
    for (auto id : tree->getPredefineBufferIds())
        ownBuffers.insert(id);
    auto externalDefinition = [&](const syntax::DefineDirectiveSyntax* definition) {
        if (!definition)
            return false;
        auto location = driver.sm.getFullyOriginalLoc(definition->name.location());
        if (!location.valid() || location == SourceLocation::NoLocation)
            return false;
        return !ownBuffers.contains(location.buffer());
    };
    for (auto definition : tree->getDefinedMacros())
        unit.requiresNativeReplay |= externalDefinition(definition);
    for (const auto& reference : tree->getMacroRefs()) {
        if (unit.requiresNativeReplay)
            break;
        unit.requiresNativeReplay |= externalDefinition(reference.definition);
    }
    ret->m_nativeCompilationUnit = std::move(unit);
    ret->m_tree = tree;
    return ret;
}

std::shared_ptr<SlangDoc> SlangDoc::fromText(ServerDriver& driver, const URI& uri,
                                             std::string_view text) {
    std::string_view path = uri.getPath();
    SourceBuffer buffer;

    // Includes can cache either successful reads or failed lookups. An editor
    // buffer can supply a file before it has ever existed on disk.
    std::optional<SourceBuffer> existingBuffer;
    if (driver.sm.isCached(path)) {
        auto cached = driver.sm.readSource(path, nullptr);
        if (cached)
            existingBuffer = *cached;
    }
    if (existingBuffer) {
        SmallVector<char> newBuffer;
        newBuffer.insert(newBuffer.end(), text.begin(), text.end());
        if (newBuffer.empty() || newBuffer.back() != '\0')
            newBuffer.push_back('\0');
        buffer = driver.sm.replaceBuffer(existingBuffer->id, std::move(newBuffer));
    }
    else {
        std::error_code error;
        auto canonical = std::filesystem::weakly_canonical(path, error);
        buffer = driver.sm.assignText(error ? std::string(path) : canonical.string(), text);
    }

    return std::make_shared<SlangDoc>(driver, uri, buffer);
}

void SlangDoc::adoptNativeCompilationUnit(const SlangDoc& unit) {
    if (!unit.m_nativeCompilationUnit || !unit.m_tree ||
        m_sourceManager.getFullPath(m_buffer.id) != m_sourceManager.getFullPath(unit.m_buffer.id))
        throw std::invalid_argument("Native unit does not match editor document");
    m_nativeCompilationUnit = unit.m_nativeCompilationUnit;
    m_tree = unit.m_tree;
    m_buffer = unit.m_buffer;
    m_options = unit.m_options;
    m_analysis.reset();
}

std::shared_ptr<SlangDoc> SlangDoc::open(ServerDriver& driver, const URI& uri) {
    auto buffer = driver.sm.readSource(uri.getPath(), nullptr).value();
    return std::make_shared<SlangDoc>(driver, uri, buffer);
}

const std::string_view SlangDoc::getText() const {
    // null terminator is included in data
    return m_sourceManager.getSourceText(m_buffer.id);
}

std::shared_ptr<syntax::SyntaxTree> SlangDoc::getSyntaxTree(const lsp::RequestContext& ctx) {
    auto cwdPhase = m_sourceManager.beginFastCwdPhase();
    ctx.throwIfCancelled("before syntax tree");
    if (!m_tree || !hasValidBuffers(m_sourceManager, m_tree)) {
        if (requiresNativeReplay())
            throw std::runtime_error("Native compilation unit requires SourceLoader replay to "
                                     "preserve preprocessing context");
        if (!m_sourceManager.isLatestData(m_buffer.id))
            m_buffer = m_sourceManager.readSource(m_uri.getPath(), nullptr).value();
        ctx.checkpoint("before syntax parsing");
        if (m_nativeCompilationUnit) {
            const auto& unit = *m_nativeCompilationUnit;
            std::vector<SourceBuffer> buffers;
            buffers.reserve(unit.rootPaths.size());
            for (size_t i = 0; i < unit.rootPaths.size(); ++i) {
                ctx.throwIfCancelled("before compilation-unit root");
                auto buffer = m_sourceManager.readSource(unit.rootPaths[i], unit.library,
                                                         unit.rootSortKeys[i]);
                if (!buffer)
                    throw std::runtime_error(
                        fmt::format("Cannot reload compilation-unit root {}: {}",
                                    unit.rootPaths[i].string(), buffer.error().message()));
                buffers.push_back(*buffer);
                if (unit.rootPaths[i] == std::filesystem::path(m_uri.getPath()))
                    m_buffer = *buffer;
            }
            auto tree = syntax::SyntaxTree::fromBuffers(buffers, m_sourceManager, unit.options);
            tree->isLibraryUnit = unit.isLibraryUnit;
            m_tree = std::move(tree);
        }
        else {
            auto buffers = m_driver.getExplorePreincludeBuffers(m_buffer.id);
            if (buffers.empty()) {
                m_tree = syntax::SyntaxTree::fromBuffer(m_buffer, m_sourceManager, m_options);
            }
            else {
                buffers.push_back(m_buffer);
                // fromBuffers preprocesses in this order, while pushSource registers
                // buffers in reverse. The document remains sourceBufferIds.front(),
                // and every preinclude/nested include participates in normal validity checks.
                m_tree = syntax::SyntaxTree::fromBuffers(buffers, m_sourceManager, m_options);
            }
        }
        // A complete parse remains reusable even if its caller was cancelled.
        ctx.checkpoint("after syntax parsing");
    }
    cwdPhase.finish();
    return m_tree;
}

SlangDoc::AnalysisInputs SlangDoc::getAnalysisInputs(const lsp::RequestContext& ctx) {
    auto cwdPhase = m_sourceManager.beginFastCwdPhase();
    ctx.throwIfCancelled("before analysis");
    auto indexRevision = m_driver.getDependencyIndexRevision();
    ctx.checkpoint("before owner resolution");
    auto previousAnalysis = m_analysis;
    auto context = m_driver.getIncludeContext(m_uri, previousAnalysis
                                                         ? previousAnalysis->getSyntaxTree().get()
                                                         : nullptr);
    ctx.checkpoint("after owner resolution");
    m_driver.requireExplicitContext(m_uri);
    auto tree = context ? context->tree : getSyntaxTree(ctx);
    auto buffer = context ? context->buffer : m_buffer.id;
    ctx.checkpoint("before dependency resolution");
    auto trees = m_driver.getDependentTrees(tree, &m_uri);
    ctx.checkpoint("after dependency resolution");
    // Resolving an explicit package reference can discover the owner for the first time.
    // Do not retain a standalone copy of the header alongside that package's include.
    if (!context) {
        ctx.checkpoint("before owner recheck");
        context = m_driver.getIncludeContext(m_uri);
        ctx.checkpoint("after owner recheck");
        if (context) {
            tree = context->tree;
            buffer = context->buffer;
            ctx.checkpoint("before dependency resolution");
            trees = m_driver.getDependentTrees(tree, &m_uri);
            ctx.checkpoint("after dependency resolution");
        }
    }
    if (std::find(trees.begin(), trees.end(), tree) == trees.end())
        trees.insert(trees.begin(), tree);
    m_driver.requireExplicitContext(m_uri);
    m_driver.updatePackageContextKind(m_uri, *tree);
    cwdPhase.finish();
    return {buffer, std::move(tree), std::move(trees), indexRevision};
}

std::shared_ptr<ShallowAnalysis> SlangDoc::refreshAnalysis(const lsp::RequestContext& ctx) {
    auto cwdPhase = m_sourceManager.beginFastCwdPhase();
    auto [buffer, tree, trees, indexRevision] = getAnalysisInputs(ctx);
    ctx.checkpoint("before analysis construction");
    auto analysis = std::make_shared<ShallowAnalysis>(m_sourceManager, buffer, tree, m_options,
                                                      trees, m_driver.comp.get(), ctx,
                                                      m_driver.isPackageOnlyContext(m_uri));
    auto topNames = analysis->getCompilation()->getRoot().topInstances |
                    std::views::transform([](const auto& top) { return top->name; });
    ctx.info("Analyzed {} with tops: {}", getWsRelativePath(), fmt::join(topNames, ", "));
    // Never publish a partially constructed or explicitly cancelled analysis.
    ctx.checkpoint("before analysis publication");
    cwdPhase.finish();
    m_analysis = analysis;
    m_analysisIndexRevision = indexRevision;
    m_analysisContextRevision = m_driver.getExplicitContextRevision(m_uri);
    return analysis;
}

bool SlangDoc::hasAnalysis() const {
    return m_analysis && m_analysis->hasValidBuffers();
}

std::shared_ptr<ShallowAnalysis> SlangDoc::getAnalysis(const lsp::RequestContext& ctx) {
    auto cwdPhase = m_sourceManager.beginFastCwdPhase();
    ctx.throwIfCancelled("before analysis");
    if (!hasAnalysis() || m_analysisIndexRevision != m_driver.getDependencyIndexRevision() ||
        m_analysisContextRevision != m_driver.getExplicitContextRevision(m_uri)) {
        refreshAnalysis(ctx);
    }
    else {
        ctx.checkpoint("before cached owner resolution");
        auto previousAnalysis = m_analysis;
        auto context = m_driver.getIncludeContext(m_uri, previousAnalysis->getSyntaxTree().get());
        ctx.checkpoint("after cached owner resolution");
        auto expectedTree = context ? context->tree : getSyntaxTree(ctx);
        if (!m_analysis || expectedTree != m_analysis->getSyntaxTree())
            refreshAnalysis(ctx);
    }
    m_driver.requireExplicitContext(m_uri);
    cwdPhase.finish();
    return m_analysis;
}

bool SlangDoc::textMatches(std::string_view text) {
    // Just compute line offsets to validate UTF-8
    auto bufText = getText();
    if (bufText.size() != text.size() + 1) {
        ERROR("Text size mismatch: have {}, expected {}", bufText.size(), text.size() + 1);
        return false;
    }
    if (std::memcmp(bufText.data(), text.data(), bufText.size()) != 0) {
        ERROR("Text content mismatch");
        return false;
    }
    return true;
}

void SlangDoc::onChange(const std::vector<lsp::TextDocumentContentChangeEvent>& contentChanges) {
    // From the LSP spec:
    //
    // The actual content changes. The content changes describe single state changes to the
    // document. So if there are two content changes c1 (at array index 0) and c2 (at array
    // index 1) for a document in state S then c1 moves the document from S to S' and c2 from
    // S' to S''. So c1 is computed on the state S and c2 is computed on the state S'.
    //
    // To mirror the content of a document using change events use the following approach:
    // - start with the same initial content
    // - apply the 'textDocument/didChange' notifications in the order you receive them.
    // - apply the `TextDocumentContentChangeEvent`s in a single notification in the order you
    //   receive them.
    //
    // Partial is defined in the variant first, so it will match first iff range and text are both
    // present. WholeDocument will match if text is present, but only be tried second. Less specific
    // cases are tried later.
    SmallVector<char> buffer;
    std::string_view textView = getText();
    std::vector<size_t> lineOffsets;

    if (contentChanges.size() == 0) {
        ERROR("Empty onChange event");
        return;
    }

    // LSP Position.character is UTF-16 code units unless the client negotiated utf-8
    bool utf8Cols = m_driver.client.capabilities.utf8Positions;

    auto getOffsets = [&](lsp::Range range) {
        // Only one thread is able to call onchange, so the offsets remain valid without locking
        SourceManager::computeLineOffsets(textView, lineOffsets);
        auto& start = range.start;
        auto& end = range.end;
        if (start.line >= lineOffsets.size() || end.line >= lineOffsets.size()) {
            throw std::runtime_error(fmt::format("Range out of bounds: {},{} / {}", start.line,
                                                 end.line, lineOffsets.size()));
        }
        auto colToOffset = [&](lsp::uint line, lsp::uint character) -> size_t {
            size_t lineStart = lineOffsets[line];
            if (utf8Cols)
                return lineStart + character;
            return lineStart + utf16ColumnToByte(textView.substr(lineStart), character);
        };
        auto startOffset = colToOffset(start.line, start.character);
        auto endOffset = colToOffset(end.line, end.character);
        return std::make_pair(startOffset, endOffset);
    };

    auto ensureNullTerminated = [&]() {
        if (buffer.empty() || buffer.back() != '\0')
            buffer.push_back('\0');
    };

    // Single change (most common)
    rfl::visit(
        [&](const auto& change) {
            using T = std::decay_t<decltype(change)>;
            if constexpr (std::is_same_v<T, lsp::TextDocumentContentChangePartial>) {
                auto offsets = getOffsets(change.range);
                buffer.append(textView.begin(), textView.begin() + offsets.first);
                buffer.append(change.text.begin(), change.text.end());
                buffer.append(textView.begin() + offsets.second, textView.end());
            }
            else {
                buffer.append(change.text.begin(), change.text.end());
            }
        },
        contentChanges[0]);
    ensureNullTerminated();

    // More than one change is rare- typically things like rename actions, or if there's some lag.
    for (size_t i = 1; i < contentChanges.size(); i++) {
        textView = std::string_view{buffer.data(), buffer.size()};
        lineOffsets.clear();
        rfl::visit(
            [&](const auto& change) {
                using T = std::decay_t<decltype(change)>;
                if constexpr (std::is_same_v<T, lsp::TextDocumentContentChangePartial>) {
                    auto offsets = getOffsets(change.range);
                    // handle deletes
                    if (offsets.second > offsets.first) {
                        buffer.erase(buffer.begin() + offsets.first,
                                     buffer.begin() + offsets.second);
                    }
                    // handle inserts
                    buffer.insert(buffer.begin() + offsets.first, change.text.begin(),
                                  change.text.end());
                }
                else {
                    // WholeDocument collapses all prior changes
                    buffer.clear();
                    buffer.append(change.text.begin(), change.text.end());
                    ensureNullTerminated();
                }
            },
            contentChanges[i]);
    }
    m_buffer = m_sourceManager.replaceBuffer(m_buffer.id, std::move(buffer));

    // Invalidate pointers to old buffer
    m_tree.reset();
    m_analysis.reset();
}
bool SlangDoc::reloadBuffer() {
    auto result = m_sourceManager.reloadBuffer(m_buffer.id);
    if (!result) {
        ERROR("Failed to re-read buffer for {}: {}", m_uri.getPath(), result.error().message());
        return false;
    }
    m_buffer = *result;
    m_version.reset();
    m_tree.reset();
    m_analysis.reset();
    return true;
}

void SlangDoc::issueParseDiagnostics(DiagnosticEngine& diagEngine) {
    for (auto& diag : getSyntaxTree()->diagnostics()) {
        diagEngine.issue(diag);
    }
}

void SlangDoc::issueDiagnosticsTo(DiagnosticEngine& diagEngine, const lsp::RequestContext& ctx,
                                  SemanticDiagnosticBatch* batch) {
    std::shared_ptr<ShallowAnalysis> analysis;
    try {
        analysis = getAnalysis(ctx);
    }
    catch (const lsp::RequestFailed&) {
        if (!m_driver.hasExplicitContextError(m_uri))
            throw;
        // Invalid explicit context never constructs or binds a Compilation.
        // Preserve the syntax/preprocessor evidence available in the selected
        // owner, or in the document alone when no owner can be selected.
        auto context = m_driver.getIncludeContext(m_uri);
        auto tree = context ? context->tree : getSyntaxTree(ctx);
        auto buffer = context ? context->buffer : m_buffer.id;
        m_driver.issueExplorePreincludeDiagnostics(buffer);
        diagEngine.setMappingsFromPragmas(buffer);
        for (const auto& diag : tree->diagnostics())
            if (diagnosticLocation(m_sourceManager, diag.location).buffer() == buffer)
                diagEngine.issue(diag);
        return;
    }
    m_driver.issueExplorePreincludeDiagnostics(analysis->getBuffer());
    // Without a uniquely validated automatic owner, context-dependent macro/type
    // diagnostics would be misleading. The driver emits a one-time selection warning.
    if (m_driver.isPackageContextDeferred(m_uri))
        return;

    // Included documents must use their owner's preprocessing context, not a standalone parse.
    auto analysisBuffer = analysis->getBuffer();
    diagEngine.setMappingsFromPragmas(analysisBuffer);
    bool included = analysisBuffer != analysis->getSyntaxTree()->getSourceBufferIds().front();
    for (auto& diag : analysis->getSyntaxTree()->diagnostics()) {
        auto location = diagnosticLocation(m_sourceManager, diag.location);
        auto path = location.valid() ? m_sourceManager.getFullPath(location.buffer())
                                     : std::filesystem::path{};
        if (!path.empty() && m_driver.isExplorePreinclude(URI::fromFile(path)))
            continue; // Shared preinclude diagnostics were issued once above.
        if (!included ||
            diagnosticLocation(m_sourceManager, diag.location).buffer() == analysisBuffer)
            diagEngine.issue(diag);
    }

    // Parse and shallow compilation diagnostics
    // There will be many diags outside the buffer, like unknown modules.
    ctx.throwIfCancelled("before collecting semantic diagnostics");
    if (ctx.method() == "background/diagnostics")
        ctx.info("Collecting semantic diagnostics for {}", getWsRelativePath());
    auto semantic = batch ? batch->collect(analysis)
                          : SemanticDiagnosticBatch::Result{analysis->getSemanticDiagnostics(),
                                                            false};
    const auto& semanticDiagnostics = semantic.diagnostics;
    ctx.throwIfCancelled("before publishing semantic diagnostics");
    for (auto& diag : semanticDiagnostics) {
        if (diagnosticLocation(m_sourceManager, diag.location).buffer() != analysisBuffer) {
            continue;
        }
        if (diag.code == slang::diag::MaxInstanceDepthExceeded) {
            continue;
        }
        diagEngine.issue(diag);
    }

    // Analysis on the shallow compilation (unused, multidriven, etc)
    // Eligible reuse requires no top instances, so the producer and recipient have
    // no driver-analysis diagnostics. Avoid triggering private semantic work again.
    auto analysisDiagnostics = semantic.reused ? Diagnostics{} : analysis->getAnalysisDiags();
    ctx.throwIfCancelled("before publishing analysis diagnostics");
    for (auto& diag : analysisDiagnostics) {
        if (diagnosticLocation(m_sourceManager, diag.location).buffer() != analysisBuffer) {
            continue;
        }
        diagEngine.issue(diag);
    }
}

std::vector<lsp::Range> SlangDoc::getInactiveRegions(const lsp::RequestContext& ctx) {
    ctx.throwIfCancelled("before collecting inactive regions");
    std::vector<lsp::Range> result;
    std::shared_ptr<ShallowAnalysis> analysis;
    try {
        analysis = getAnalysis(ctx);
    }
    catch (const lsp::RequestFailed&) {
        if (!m_driver.hasExplicitContextError(m_uri))
            throw;
        return result;
    }
    result.reserve(analysis->syntaxes.disabledRegions.size());

    for (const auto& region : analysis->syntaxes.disabledRegions) {
        result.push_back(toRange(region, m_sourceManager));
    }

    return result;
}

} // namespace server
