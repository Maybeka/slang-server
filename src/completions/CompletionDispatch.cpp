//------------------------------------------------------------------------------
// CompletionDispatch.cpp
// Completion site classification and shared query dispatch.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "completions/CompletionDispatch.h"

#include "ServerDriver.h"
#include "completions/AssignmentPatternCompletions.h"
#include "completions/InstanceCompletions.h"
#include "completions/MacroCompletions.h"
#include "completions/MemberCompletions.h"
#include "completions/SystemTaskCompletions.h"
#include "diagnostics/DiagnosticSnapshotCodec.h"
#include "document/ShallowAnalysis.h"
#include "lsp/SnippetString.h"
#include "util/Converters.h"
#include "util/Logging.h"
#include "util/SlangExtensions.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "slang/parsing/Parser.h"
#include "slang/parsing/Token.h"
#include "slang/parsing/TokenKind.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxVisitor.h"
#include "slang/util/OS.h"

namespace server {

namespace completions {

const std::vector<std::string>& completionTriggerCharacters() {
    static const std::vector<std::string> triggerCharacters{
        "`", // macros
        "#", // hierarchical instantiation: modules and interfaces
        ".", // hierarchical references
        "(", // function calls
        ":", // package scope (::), wire width
        "[", // wire width, array indexing
        "$", // system tasks and functions
        "{", // assignment patterns
    };
    return triggerCharacters;
}

} // namespace completions

namespace {

struct CompletionSite {
    lsp::Range replacementRange;
    const slang::parsing::Token* targetToken = nullptr;
    const slang::parsing::Token* tokenBefore = nullptr;
    const slang::parsing::Token* tokenAfter = nullptr;
    std::string typedPrefix;
};

CompletionSite getCompletionSite(const SlangDoc& doc, const ShallowAnalysis& analysis,
                                 slang::SourceLocation cursor) {
    using slang::parsing::TokenKind;

    // Probe the previous byte because a cursor at a token's end can fall outside token lookup.
    auto* targetToken = analysis.getWordTokenAt(cursor);
    if (!targetToken && cursor.offset() > 0)
        targetToken = analysis.getWordTokenAt(cursor - 1);

    // The fallback probe is valid only within the token or exactly at its end.
    if (targetToken &&
        (cursor < targetToken->range().start() || targetToken->range().end() < cursor)) {
        targetToken = nullptr;
    }

    // Replace the whole token so completion in its middle also removes the existing suffix.
    auto start = cursor;
    auto end = cursor;
    if (targetToken) {
        start = targetToken->range().start();
        end = targetToken->range().end();
    }

    auto* tokenBefore = analysis.syntaxes.getTokenBefore(start);
    if (!targetToken) {
        // Standalone `$` and backtick markers are not words but belong to the replacement range.
        auto* marker = analysis.syntaxes.getTokenBefore(cursor);
        if (marker && marker->range().end() == cursor &&
            (marker->kind == TokenKind::Dollar || marker->rawText() == "`")) {
            targetToken = marker;
            start = marker->range().start();
            end = marker->range().end();
            tokenBefore = analysis.syntaxes.getTokenBefore(start);
        }
    }

    // Filtering uses only text before the cursor even though replacement spans the whole token.
    std::string typedPrefix;
    if (targetToken) {
        auto length = std::min<size_t>(cursor.offset() - start.offset(),
                                       targetToken->rawText().size());
        typedPrefix = targetToken->rawText().substr(0, length);
    }

    // Neighboring tokens are measured outside the replacement range for query classification.
    return CompletionSite{
        .replacementRange = lsp::Range{.start = toPosition(start, doc.getSourceManager()),
                                       .end = toPosition(end, doc.getSourceManager())},
        .targetToken = targetToken,
        .tokenBefore = tokenBefore,
        .tokenAfter = analysis.syntaxes.getTokenAfter(end),
        .typedPrefix = std::move(typedPrefix),
    };
}

bool isSeparatedOnlyByWhitespace(const slang::parsing::Token& token) {
    return std::ranges::all_of(token.trivia(), [](const slang::parsing::Trivia& trivia) {
        return trivia.kind == slang::parsing::TriviaKind::Whitespace ||
               trivia.kind == slang::parsing::TriviaKind::EndOfLine;
    });
}

void setCompletionEdit(lsp::CompletionItem& item, const lsp::Range& replacementRange,
                       bool followedByCall, bool followedByInstantiation) {
    // Existing calls and instances still resolve documentation but retain their source shape.
    if ((followedByCall && item.kind == lsp::CompletionItemKind::Constant) ||
        (followedByInstantiation && item.kind == lsp::CompletionItemKind::Module)) {
        item.insertText = item.label;
        item.insertTextFormat = lsp::InsertTextFormat::PlainText;
    }

    auto newText = item.insertText.value_or(item.label);
    auto useLabelOnly = (followedByCall &&
                         item.insertTextFormat == lsp::InsertTextFormat::Snippet) ||
                        (followedByInstantiation && item.kind == lsp::CompletionItemKind::Module);
    if (useLabelOnly && item.insertTextFormat == lsp::InsertTextFormat::Snippet) {
        SnippetString escapedLabel;
        escapedLabel.appendText(item.label);
        newText = escapedLabel.getValue();
    }
    else if (useLabelOnly) {
        newText = item.label;
    }
    item.textEdit = lsp::TextEdit{.range = replacementRange, .newText = std::move(newText)};
}

} // namespace

std::unique_ptr<CompletionQuery> CompletionQuery::fromLocation(
    const SlangDoc& doc, const std::shared_ptr<ShallowAnalysis>& analysis,
    slang::SourceLocation cursor, const lsp::CompletionContext& lspContext) {
    using slang::parsing::TokenKind;

    auto site = getCompletionSite(doc, *analysis, cursor);
    auto followedByCall = site.tokenAfter && site.tokenAfter->kind == TokenKind::OpenParenthesis &&
                          isSeparatedOnlyByWhitespace(*site.tokenAfter);
    auto followedByInstantiation = site.tokenAfter &&
                                   isSeparatedOnlyByWhitespace(*site.tokenAfter) &&
                                   (site.tokenAfter->kind == TokenKind::Identifier ||
                                    site.tokenAfter->kind == TokenKind::Hash);
    auto followedByColon = site.tokenAfter && site.tokenAfter->kind == TokenKind::Colon;

    if (site.targetToken && site.targetToken->rawText().starts_with('$')) {
        return completions::SystemSubroutineCompletionQuery::create(
            std::move(site.replacementRange), followedByCall);
    }
    if (site.targetToken && site.targetToken->rawText().starts_with('`')) {
        return completions::MacroCompletionQuery::create(std::move(site.replacementRange),
                                                         std::move(site.typedPrefix),
                                                         followedByCall);
    }
    if (site.tokenBefore && site.tokenBefore->kind == TokenKind::DoubleColon) {
        return completions::MemberCompletionQuery::createScopedAccess(
            std::move(site.replacementRange),
            analysis->syntaxes.getTokenBefore(site.tokenBefore->location()), followedByCall);
    }
    if (site.tokenBefore && site.tokenBefore->kind == TokenKind::Dot) {
        return completions::MemberCompletionQuery::createMemberAccess(
            std::move(site.replacementRange),
            analysis->syntaxes.getTokenBefore(site.tokenBefore->location()), followedByCall);
    }
    if (!site.targetToken && site.tokenBefore && site.tokenBefore->kind == TokenKind::Hash) {
        return completions::InstanceCompletionQuery::create(std::move(site.replacementRange),
                                                            analysis->syntaxes.getTokenBefore(
                                                                site.tokenBefore->location()));
    }
    if (analysis->getAssignmentPatternCompletionScope(cursor)) {
        if (lspContext.triggerKind == lsp::CompletionTriggerKind::TriggerCharacter &&
            lspContext.triggerCharacter == "{") {
            return completions::StructAssignCompletionQuery::create(
                std::move(site.replacementRange), cursor);
        }
        return completions::StructMemberCompletionQuery::create(std::move(site.replacementRange),
                                                                cursor, followedByColon);
    }

    return completions::MemberCompletionQuery::createLexical(std::move(site.replacementRange),
                                                             followedByCall,
                                                             followedByInstantiation);
}

void CompletionQuery::setCompletionEdit(lsp::CompletionItem& item) const {
    server::setCompletionEdit(item, replacementRange, followedByCall, followedByInstantiation);
}

ServerDriver& CompletionQuery::getDriver(CompletionDispatch& dispatch) {
    return dispatch.m_driver;
}

const Indexer& CompletionQuery::getIndexer(const CompletionDispatch& dispatch) {
    return dispatch.m_indexer;
}

slang::SourceManager& CompletionQuery::getSourceManager(CompletionDispatch& dispatch) {
    return dispatch.m_sourceManager;
}

slang::Bag& CompletionQuery::getOptions(CompletionDispatch& dispatch) {
    return dispatch.m_options;
}

namespace {
// Dispatch owns cache state. Compiler option Bags remain byte-for-byte ordinary option types.
struct ObservableDirectives : slang::syntax::SyntaxVisitor<ObservableDirectives> {
    bool present = false;
    void visitToken(slang::parsing::Token token) {
        for (auto t : token.trivia())
            if (t.kind == slang::parsing::TriviaKind::Directive &&
                (t.syntax()->kind == slang::syntax::SyntaxKind::LineDirective ||
                 t.syntax()->kind == slang::syntax::SyntaxKind::PragmaDirective))
                present = true;
    }
};
bool hasObservableDirectives(const slang::syntax::SyntaxTree& tree) {
    if (auto recorded = tree.getPrefixSourceEffects())
        return *recorded;
    ObservableDirectives directives;
    tree.root().visit(directives);
    return directives.present;
}
// A sole same-directory literal include adds no preprocessing input state. Only
// the macro/type resolve consumers use this equivalence; compilation keeps the
// original wrapper ancestry and source IDs. Inspect with a private SourceManager
// so comment handlers can never mutate the live manager while proving eligibility.
bool isTransparentPrefixRoot(slang::syntax::SyntaxTree& prefix,
                             const slang::SourceBuffer& requested) {
    using namespace slang;
    if (prefix.getSourceBufferIds().empty())
        return false;
    // Warnings do not change the macro/type declarations consumed here. Errors
    // can indicate truncated includes (including a wrapper-dependent depth limit).
    for (const auto& diagnostic : prefix.diagnostics())
        if (getDefaultSeverity(diagnostic.code) >= DiagnosticSeverity::Error)
            return false;
    auto& sm = prefix.sourceManager();
    auto wrapper = prefix.getSourceBufferIds().front();
    auto wrapperPath = sm.getFullPath(wrapper);
    auto requestedPath = sm.getFullPath(requested.id);
    if (wrapperPath.filename() != "uvm.sv" || requestedPath.filename() != "uvm_pkg.sv" ||
        wrapperPath.parent_path() != requestedPath.parent_path())
        return false;
    size_t topIncludes = 0;
    bool includesRequested = false;
    for (const auto& include : prefix.getIncludeDirectives()) {
        auto location = sm.getFullyOriginalLoc(include.syntax->getFirstToken().location());
        if (location.buffer() != wrapper)
            continue;
        ++topIncludes;
        includesRequested |= include.buffer && sm.getFullPath(include.buffer.id) == requestedPath &&
                             !include.isSystem;
    }
    if (topIncludes != 1 || !includesRequested)
        return false;
    SourceManager proof;
    auto input = proof.assignText(wrapperPath.string(), sm.getSourceText(wrapper));
    BumpAllocator arena;
    Diagnostics diagnostics;
    parsing::Lexer lexer(input, arena, diagnostics, proof,
                         prefix.options().getOrDefault<parsing::LexerOptions>());
    auto directive = lexer.lex();
    auto path = lexer.lex();
    auto end = lexer.lex();
    return diagnostics.empty() && proof.getDiagnosticDirectives(input.id).empty() &&
           directive.kind == parsing::TokenKind::Directive &&
           directive.directiveKind() == syntax::SyntaxKind::IncludeDirective &&
           path.kind == parsing::TokenKind::StringLiteral && path.rawText() == "\"uvm_pkg.sv\"" &&
           end.kind == parsing::TokenKind::EndOfFile;
}
std::string parsingSignature(const slang::Bag& source) {
    slang::Bag relevant;
    relevant.set(source.getOrDefault<slang::parsing::LexerOptions>());
    relevant.set(source.getOrDefault<slang::parsing::PreprocessorOptions>());
    relevant.set(source.getOrDefault<slang::parsing::ParserOptions>());
    return rfl::json::write(diagnostic_snapshot::captureOptions(relevant));
}
std::atomic<uint64_t> nextCompletionEpoch{1};
struct CompletionEpochPayload {
    uint64_t contextEpoch;
    rfl::Generic payload;
};
struct IndexedTreeEntry {
    std::string path;
    std::filesystem::path cwd;
    std::shared_ptr<slang::syntax::SyntaxTree> tree;
    std::vector<std::shared_ptr<void>> pins;
    size_t bytes;
};
void indexedCacheTrace(const char* event, std::string_view path, size_t bytes) {
    if (auto* file = std::getenv("SLANG_UVM_RESOLVE_CACHE_TRACE"))
        if (auto* out = std::fopen(file, "a")) {
            std::fprintf(out, "%ld\t%s\t%zu\t%.*s\n", long(slang::OS::getpid()), event, bytes,
                         int(path.size()), path.data());
            std::fclose(out);
        }
}
} // namespace
struct CompletionDispatch::ParsedLibraryCacheState {
    const slang::SourceManager* owner = nullptr;
    std::vector<IndexedTreeEntry> entries;
    size_t bytes = 0;
    std::string signature;
    uint64_t epoch = nextCompletionEpoch.fetch_add(1, std::memory_order_relaxed);
    bool requireEpoch = false;
};
CompletionDispatch::~CompletionDispatch() = default;

std::shared_ptr<slang::syntax::SyntaxTree> CompletionQuery::getIndexedSyntaxTree(
    CompletionDispatch& dispatch, std::string_view path) {
    using namespace slang;
    auto& sm = getSourceManager(dispatch);
    auto& options = getOptions(dispatch);
    auto parse = [&](const Bag& settings) {
        auto result = syntax::SyntaxTree::fromFile(path, sm, settings);
        return result ? result.value() : std::shared_ptr<syntax::SyntaxTree>{};
    };
    if (getDriver(dispatch).isFrozenSourcesEnabled()) {
        if (auto tree = getDriver(dispatch).getFrozenIndependentTree(path, options))
            return tree;
    }
    if (!getDriver(dispatch).isUvmPrefixCacheEnabled())
        return parse(options);
    if (options.getOrDefault<parsing::PreprocessorOptions>().bufferChangeCB) {
        indexedCacheTrace("callback-bypass", path, 0);
        return parse(options);
    }
    auto& holder = dispatch.m_parsedLibraryCache;
    if (!holder || holder->owner != &sm) {
        holder = std::make_unique<CompletionDispatch::ParsedLibraryCacheState>();
        holder->owner = &sm;
    }
    auto& state = *holder;
    std::string signature;
    try {
        signature = parsingSignature(options);
    }
    catch (const std::exception&) {
        dispatch.invalidateParsedLibraryCache();
        indexedCacheTrace("option-bypass", path, 0);
        return parse(options);
    }
    if (state.signature != signature) {
        state.entries.clear();
        state.bytes = 0;
        state.signature = signature;
        indexedCacheTrace("context-change", path, 0);
    }
    std::error_code error;
    auto cwd = std::filesystem::current_path(error);
    // Same absolute root and immutable Driver options preserve fromFile's independent PP context.
    auto buffer = sm.readSource(path);
    if (error || !buffer) {
        indexedCacheTrace("bypass", path, 0);
        return parse(options);
    }
    auto key = sm.getFullPath(buffer->id).string();
    for (auto it = state.entries.begin(); it != state.entries.end(); ++it) {
        if (it->path != key || it->cwd != cwd)
            continue;
        if (server::hasValidBuffers(sm, it->tree)) {
            auto hit = std::move(*it);
            auto result = hit.tree;
            state.entries.erase(it);
            state.entries.push_back(std::move(hit));
            indexedCacheTrace("hit", key, state.bytes);
            return result;
        }
        state.bytes -= it->bytes;
        state.entries.erase(it);
        break;
    }
    std::shared_ptr<syntax::SyntaxTree> tree;
    for (const auto& [uri, doc] : getDriver(dispatch).docs) {
        auto prefix = doc->getPreparedLibraryPrefix();
        if (!prefix || prefix->isLibraryUnit || prefix->getSourceLibrary() != buffer->library ||
            &prefix->sourceManager() != &sm || prefix->getSourceBufferIds().empty() ||
            prefix->getPrefixParseCwd() != cwd.string() || !server::hasValidBuffers(sm, prefix))
            continue;
        try {
            if (parsingSignature(prefix->options()) != signature)
                continue;
        }
        catch (const std::exception&) {
            continue;
        } // Preserve strict unsupported-options rejection.
        if (hasObservableDirectives(*prefix))
            continue;
        const bool sameRoot = sm.getFullPath(prefix->getSourceBufferIds()[0]).string() == key;
        if (!sameRoot && !isTransparentPrefixRoot(*prefix, *buffer))
            continue;
        tree = std::move(prefix);
        indexedCacheTrace(sameRoot ? "shared-prefix" : "shared-wrapper-prefix", key,
                          tree->getRetainedSyntaxBytes());
        break;
    }
    if (!tree)
        tree = parse(options);
    if (!tree)
        return {};
    if (hasObservableDirectives(*tree)) {
        indexedCacheTrace("directive-bypass", key, 0);
        return tree;
    }
    constexpr size_t budget = 64 * 1024 * 1024;
    auto bytes = tree->getRetainedSyntaxBytes() + tree->getRetainedSourceBytes();
    if (bytes > budget) {
        indexedCacheTrace("oversize", key, bytes);
        return tree;
    }
    while (!state.entries.empty() && (state.entries.size() >= 64 || state.bytes + bytes > budget)) {
        state.bytes -= state.entries.front().bytes;
        state.entries.erase(state.entries.begin());
    }
    auto pins = sm.retainBuffers(tree->getSourceBufferIds());
    auto macroPins = sm.retainBuffers(tree->getPredefineBufferIds());
    pins.insert(pins.end(), macroPins.begin(), macroPins.end());
    state.entries.push_back({key, cwd, tree, std::move(pins), bytes});
    state.bytes += bytes;
    indexedCacheTrace("miss", key, state.bytes);
    return tree;
}

uint64_t CompletionDispatch::getCompletionEpoch() {
    if (!m_parsedLibraryCache) {
        m_parsedLibraryCache = std::make_unique<ParsedLibraryCacheState>();
        m_parsedLibraryCache->owner = &m_sourceManager;
    }
    return m_parsedLibraryCache->epoch;
}
void CompletionDispatch::requireCompletionEpoch() {
    getCompletionEpoch();
    m_parsedLibraryCache->requireEpoch = true;
}

void CompletionDispatch::invalidateParsedLibraryCache() {
    if (m_parsedLibraryCache) {
        m_parsedLibraryCache->entries.clear();
        m_parsedLibraryCache->bytes = 0;
        indexedCacheTrace("invalidated", "", 0);
    }
}

bool CompletionQuery::resolvesCompletionEdits(const CompletionDispatch& dispatch) {
    return dispatch.resolveEdits;
}

void CompletionQuery::updateCompletionEditText(lsp::CompletionItem& item) {
    if (!item.insertText || !item.textEdit)
        return;

    if (rfl::holds_alternative<lsp::TextEdit>(*item.textEdit))
        rfl::get<lsp::TextEdit>(*item.textEdit).newText = *item.insertText;
    else
        rfl::get<lsp::InsertReplaceEdit>(*item.textEdit).newText = *item.insertText;
}

CompletionDispatch::CompletionDispatch(ServerDriver& driver, const Indexer& indexer,
                                       SourceManager& sourceManager, slang::Bag& options) :
    m_driver(driver), m_indexer(indexer), m_sourceManager(sourceManager), m_options(options) {
}

void CompletionDispatch::getCompletions(std::vector<lsp::CompletionItem>& results,
                                        std::shared_ptr<SlangDoc> doc,
                                        const CompletionContext& context) {
    SLANG_ASSERT(context.query);
    if (context.lspContext.triggerKind == lsp::CompletionTriggerKind::TriggerCharacter &&
        context.lspContext.triggerCharacter == "{" &&
        context.query->kind() != CompletionQueryKind::StructAssign) {
        return;
    }

    context.query->getCompletions(results, *this, doc, context);

    for (auto& item : results) {
        // Resolution is a separate request and must retain the requesting file's context.
        if (item.kind == lsp::CompletionItemKind::Module)
            item.data = std::string(doc->getURI().str());
        context.query->setCompletionEdit(item);
        completions::MemberCompletionQuery::retainReceiverText(item, *doc);
        if ((m_driver.isUvmPrefixCacheEnabled() || m_driver.isFrozenSourcesEnabled() ||
             (m_parsedLibraryCache && m_parsedLibraryCache->requireEpoch)) &&
            item.data && item.data->to_object())
            item.data = rfl::to_generic(
                CompletionEpochPayload{getCompletionEpoch(), std::move(*item.data)});
    }

    INFO("Returning {} completions for {} query in {} context", results.size(),
         toString(context.query->kind()), toString(context.kind));
}

void CompletionDispatch::getCompletionItemResolve(lsp::CompletionItem& item,
                                                  const lsp::RequestContext& ctx) {
    INFO("Resolving completion item: {}", item.label);
    ctx.throwIfCancelled("before resolving completion item");
    bool wrapped = false;
    const auto* dataObject = item.data ? std::get_if<rfl::Generic::Object>(&item.data->variant())
                                       : nullptr;
    const bool expectsEpoch = m_driver.isUvmPrefixCacheEnabled() ||
                              m_driver.isFrozenSourcesEnabled() ||
                              (m_parsedLibraryCache && m_parsedLibraryCache->requireEpoch);
    if (dataObject && (expectsEpoch || std::ranges::any_of(*dataObject, [](const auto& entry) {
                           return entry.first == "contextEpoch";
                       }))) {
        auto owner = rfl::from_generic<CompletionEpochPayload>(*item.data);
        if (owner) {
            if (owner->contextEpoch != getCompletionEpoch()) {
                item.data = std::move(owner->payload);
                if (item.kind != lsp::CompletionItemKind::Constant &&
                    item.kind != lsp::CompletionItemKind::Module) {
                    completions::MemberCompletionQuery::resolveLogical(*this, item, ctx);
                    return;
                }
            }
            else
                item.data = std::move(owner->payload);
            wrapped = true;
        }
        else if (m_parsedLibraryCache && m_parsedLibraryCache->requireEpoch) {
            item.data.reset();
            return;
        }
    }

    if (!item.label.empty() && item.label[0] == '$')
        return;

    switch (*item.kind) {
        case lsp::CompletionItemKind::Constant:
            completions::MacroCompletionQuery::resolve(*this, item);
            break;
        case lsp::CompletionItemKind::Module:
            completions::InstanceCompletionQuery::resolve(*this, item);
            break;
        default:
            completions::MemberCompletionQuery::resolve(*this, item, ctx);
            break;
    }
    if (wrapped && item.data && item.data->to_object())
        item.data = rfl::to_generic(
            CompletionEpochPayload{getCompletionEpoch(), std::move(*item.data)});
}

} // namespace server
