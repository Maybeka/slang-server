// SPDX-License-Identifier: MIT
#include "ast/TrustedUvmLibrary.h"
#include "completions/InstanceCompletions.h"
#include "document/MethodCompletionData.h"
#include "document/MethodScopeOverlay.h"
#include "util/Converters.h"
#include "util/SourceSyntaxReachability.h"
#include "util/UvmPhaseTrace.h"
#include <algorithm>
#include <tuple>

#include "slang/ast/EvalContext.h"
#include "slang/ast/symbols/ClassSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/syntax/AllSyntax.h"

namespace server {
using namespace slang;
using method_completion::Data;
using method_completion::Formatting;
using method_completion::NativePayload;
using method_completion::QueryPayload;

bool MethodScopeOverlay::generalSite(const lsp::Position& position) const {
    auto at = toSourceLocation(driver->getDocument(uri)->getBuffer(), position, driver->sm);
    return at && bodyBegin <= at->offset() && at->offset() < bodyEnd;
}

const MethodScopeOverlay::Site* MethodScopeOverlay::site(const lsp::Position& position) const {
    auto at = toSourceLocation(driver->getDocument(uri)->getBuffer(), position, driver->sm);
    if (!at)
        return nullptr;
    for (auto& value : sites)
        if (value.begin <= at->offset() && at->offset() <= value.end)
            return &value;
    return nullptr;
}
lsp::Range MethodScopeOverlay::range(size_t begin, size_t end) const {
    auto id = driver->getDocument(uri)->getBuffer();
    return {toPosition(SourceLocation(id, begin), driver->sm),
            toPosition(SourceLocation(id, end), driver->sm)};
}
lsp::CompletionItem MethodScopeOverlay::rebase(lsp::CompletionItem item, bool methodOwned) const {
    if (item.data) {
        auto data = rfl::from_generic<Data, rfl::UnderlyingEnums>(*item.data);
        require(bool(data), "completion source identity");
        require(!data->receiverBufferId, "receiver handles unsupported");
        if (data->bufferId == originalBuffer) {
            if (bodyQueries && !methodOwned && data->offset >= originalBodyEnd)
                data->offset = bodyEnd + (data->offset - originalBodyEnd);
            data->bufferId = currentBuffer;
        }
        item.data = rfl::to_generic<rfl::UnderlyingEnums>(*data);
    }
    return item;
}
const MethodScopeOverlay::DTO& MethodScopeOverlay::completion(size_t context, size_t index) const {
    const auto& slots = methodSlots[context];
    auto slot = std::lower_bound(slots.begin(), slots.end(), index);
    if (!methodValues[context].empty() && slot != slots.end() && *slot == index)
        return methodValues[context][size_t(slot - slots.begin())];
    return lexical[context][index];
}

std::optional<std::vector<lsp::CompletionItem>> MethodScopeOverlay::complete(
    const lsp::CompletionParams& p, const Config& config, const lsp::RequestContext& ctx) try {
    auto result = completeImpl(p, config, ctx);
    if (result && optimizedCertification(config)) {
        ctx.checkpoint("before method complete certification");
        if (!valid(config, ctx))
            return {};
        ctx.checkpoint("after method complete certification");
    }
    return result;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<std::vector<lsp::LocationLink>> MethodScopeOverlay::define(
    const lsp::DefinitionParams& p, const Config& config, const lsp::RequestContext& ctx) try {
    auto result = defineImpl(p, config, ctx);
    if (result && optimizedCertification(config)) {
        ctx.checkpoint("before method define certification");
        if (!valid(config, ctx))
            return {};
        ctx.checkpoint("after method define certification");
    }
    return result;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<std::optional<lsp::Hover>> MethodScopeOverlay::hover(
    const URI& target, const lsp::Position& position, const Config& config,
    const lsp::RequestContext& ctx, std::optional<Config::HoverConfig::MacroView> view) try {
    auto result = hoverImpl(target, position, config, ctx, view);
    if (result && optimizedCertification(config)) {
        ctx.checkpoint("before method hover certification");
        if (!valid(config, ctx))
            return {};
        ctx.checkpoint("after method hover certification");
    }
    return result;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<lsp::CompletionItem> MethodScopeOverlay::resolve(const lsp::CompletionItem& item,
                                                               const Config& config,
                                                               const lsp::RequestContext& ctx) try {
    auto result = resolveImpl(item, config, ctx);
    if (result && optimizedCertification(config)) {
        ctx.checkpoint("before method resolve certification");
        if (!valid(config, ctx))
            return {};
        ctx.checkpoint("after method resolve certification");
    }
    return result;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<std::vector<lsp::CompletionItem>> MethodScopeOverlay::completeImpl(
    const lsp::CompletionParams& p, const Config& config, const lsp::RequestContext& ctx) try {
    if (generalBody) {
        if (!accepted || p.textDocument.uri != uri || !validForWork(config, ctx) ||
            !generalSite(p.position))
            return {};
        ensureGeneralBinding(config, ctx);
        ast::BodyQueryEvaluationGuard guard(bodyBlocked, bodyBlockedSyntax);
        BodySourceProjection projection(driver->sm, driver->getDocument(uri)->getBuffer(),
                                        miniAnalysis->getBuffer(), originalBodyEnd, bodyEnd,
                                        bodyBufferLimit,
                                        miniAnalysis->getShallowCompilation()->getBodyQueryRoots(),
                                        bodyQueryScopeDisplay);
        auto doc = driver->getDocument(uri);
        auto location = toSourceLocation(miniAnalysis->getBuffer(), p.position, driver->sm);
        if (!location)
            return {};
        auto context = CompletionContext::fromLocation(*doc, *location,
                                                       p.context.value_or(lsp::CompletionContext{}),
                                                       ctx, miniAnalysis);
        if (context.query->isIncomplete())
            return {};
        std::vector<lsp::CompletionItem> out;
        driver->completions.getCompletions(out, doc, context);
        for (auto& item : out)
            if (item.data && item.data->to_object()) {
                auto native = rfl::from_generic<NativePayload>(*item.data);
                if (native)
                    item.data = rfl::to_generic(QueryPayload{frozen.revision,
                                                             std::move(native->payload),
                                                             frozen.indexRevision, version});
            }
        return out;
    }
    if (!accepted || p.textDocument.uri != uri || !validForWork(config, ctx) ||
        (p.context && p.context->triggerKind == lsp::CompletionTriggerKind::TriggerCharacter)) {
        return {};
    }
    auto* where = site(p.position);
    if (!where) {
        return {};
    }
    auto edit = completions::MemberCompletionQuery::createLexical(range(where->begin, where->end),
                                                                  false, false);
    std::vector<lsp::CompletionItem> out;
    for (size_t i = 0; i < lexical[where->context].size(); ++i) {
        const auto& dto = completion(where->context, i);
        if (dto.item.documentation && dto.changedDocumentation) {
            return {};
        }
        auto item = rebase(dto.item, dto.methodOwned);
        // CompletionDispatch supplies the requesting document for items whose
        // producer has no symbol handle (including indexed package/module items).
        if (item.kind == lsp::CompletionItemKind::Module)
            item.data = uri.str();
        edit->setCompletionEdit(item);
        if (bodyQueries && item.data && item.data->to_object())
            item.data = rfl::to_generic(QueryPayload{frozen.revision, std::move(*item.data),
                                                     frozen.indexRevision, version});
        out.push_back(std::move(item));
    }
    ctx.checkpoint("before method completion response");
    return out;
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<std::vector<lsp::LocationLink>> MethodScopeOverlay::defineImpl(
    const lsp::DefinitionParams& p, const Config& config, const lsp::RequestContext& ctx) try {
    if (!accepted || p.textDocument.uri != uri || !validForWork(config, ctx))
        return {};
    if (generalBody) {
        if (!generalSite(p.position))
            return {};
        ensureGeneralBinding(config, ctx);
        ast::BodyQueryEvaluationGuard guard(bodyBlocked, bodyBlockedSyntax);
        BodySourceProjection projection(driver->sm, driver->getDocument(uri)->getBuffer(),
                                        miniAnalysis->getBuffer(), originalBodyEnd, bodyEnd,
                                        bodyBufferLimit,
                                        miniAnalysis->getShallowCompilation()->getBodyQueryRoots(),
                                        bodyQueryScopeDisplay);
        auto info = driver->getDefinitionInfoAt(uri, p.position, ctx, miniAnalysis);
        return info ? info->getDefinitionLspLinks() : std::vector<lsp::LocationLink>{};
    }
    auto* where = site(p.position);
    if (!where)
        return {};
    auto at = toSourceLocation(driver->getDocument(uri)->getBuffer(), p.position, driver->sm);
    if (at->offset() == where->end && !where->definitionAtEnd)
        return std::vector<lsp::LocationLink>{};
    auto target = range(where->declarationBegin, where->declarationEnd);
    ctx.checkpoint("before method definition response");
    return std::vector<lsp::LocationLink>{
        {.targetUri = uri, .targetRange = target, .targetSelectionRange = target}};
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<std::optional<lsp::Hover>> MethodScopeOverlay::hoverImpl(
    const URI& target, const lsp::Position& position, const Config& config,
    const lsp::RequestContext& ctx, std::optional<Config::HoverConfig::MacroView> view) try {
    if (!generalBody || !accepted || target != uri || !validForWork(config, ctx))
        return {};
    const auto at = toSourceLocation(driver->getDocument(uri)->getBuffer(), position, driver->sm);
    // The isolated parse includes the unchanged prototype. generalApply also
    // checks current return/argument types and defaults, including non-ANSI
    // declarations in the body, before exposing any of these symbols.
    if (!at || at->offset() < functionBegin || at->offset() >= bodyEnd)
        return {};
    ensureGeneralBinding(config, ctx);
    std::optional<ast::BodyQueryEvaluationGuard> guard(std::in_place, bodyBlocked,
                                                       bodyBlockedSyntax);
    std::optional<BodySourceProjection> projection(
        std::in_place, driver->sm, driver->getDocument(uri)->getBuffer(), miniAnalysis->getBuffer(),
        originalBodyEnd, bodyEnd, bodyBufferLimit,
        miniAnalysis->getShallowCompilation()->getBodyQueryRoots(), bodyQueryScopeDisplay);
    auto info = driver->getDefinitionInfoAt(uri, position, ctx, miniAnalysis);
    auto requiresCurrent = [&](const DefinitionInfo::SyntaxTarget& target) {
        if (!target.node)
            return false;
        auto first = driver->sm.getFullyExpandedLoc(target.node->getFirstToken().location());
        auto last = driver->sm.getFullyExpandedLoc(target.node->getLastToken().range().end());
        if (first.buffer().getId() >= bodyBufferLimit)
            return false;
        const auto path = driver->sm.getFullPath(driver->getDocument(uri)->getBuffer());
        if (first.buffer().getId() != originalBuffer &&
            driver->sm.getFullPath(first.buffer()) != path)
            return false;
        // Enclosing code contains the old body; later code can contain old
        // intrinsic values and source links. Render those through the native view.
        return first.offset() >= originalBodyEnd || last.offset() > bodyBegin;
    };
    if (info)
        for (const auto& target : info->targets) {
            bool current = false;
            if (auto symbol = std::get_if<DefinitionInfo::SymbolTarget>(&target))
                current = std::ranges::any_of(symbol->syntaxes, requiresCurrent);
            else if (auto port = std::get_if<DefinitionInfo::PortConnectionTarget>(&target))
                current = std::ranges::any_of(port->outer.syntaxes, requiresCurrent) ||
                          std::ranges::any_of(port->inner.syntaxes, requiresCurrent);
            else if (auto macro = std::get_if<DefinitionInfo::MacroTarget>(&target))
                if (auto syntax = macro->syntaxTarget())
                    current = requiresCurrent(*syntax);
            if (current) {
                // Keep this value certificate and its completion tickets. Returning
                // an unsupported-query sentinel would make the dispatcher discard
                // the entire overlay for this one native documentation request.
                projection.reset();
                guard.reset();
                auto hover = driver->getDocHover(uri, position, ctx, view);
                ctx.checkpoint("before current native method hover response");
                return std::optional<std::optional<lsp::Hover>>(std::in_place, std::move(hover));
            }
        }
    auto hovers = config.hovers.value();
    if (view)
        hovers.macroView = *view;
    auto result = info ? std::optional<lsp::Hover>(lsp::Hover{
                             .contents = info->getHover(miniAnalysis->getBuffer(), hovers)})
                       : driver->getDocHover(uri, position, ctx, view, miniAnalysis);
    ctx.checkpoint("before method hover response");
    return std::optional<std::optional<lsp::Hover>>(std::in_place, std::move(result));
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

std::optional<lsp::CompletionItem> MethodScopeOverlay::resolveImpl(
    const lsp::CompletionItem& item, const Config& config, const lsp::RequestContext& ctx) try {
    if (!accepted || !validForWork(config, ctx))
        return {};
    if (item.data) {
        auto payload = *item.data;
        if (bodyQueries && payload.to_object()) {
            auto owner = rfl::from_generic<QueryPayload>(payload);
            if (!owner)
                return {}; // Untagged handles belong to the native dispatcher.
            if (owner->contextEpoch != frozen.revision ||
                (owner->methodBodyCertificate &&
                 (*owner->methodBodyCertificate != frozen.indexRevision ||
                  owner->methodBodyVersion != version))) {
                auto out = item;
                out.data.reset();
                return out;
            }
            payload = std::move(owner->payload);
        }
        if (generalBody) {
            if (!rfl::from_generic<QueryPayload>(*item.data))
                return {};
            ensureGeneralBinding(config, ctx);
            // A containing class / outer routine's documentation includes this edited
            // body. Resolve that rare enclosing declaration through the current native
            // view rather than print its retained old syntax.
            auto data = rfl::from_generic<Data, rfl::UnderlyingEnums>(payload);
            if (data && data->documentUri == uri.str()) {
                auto position = driver->sm.getFullyExpandedLoc(SourceLocation(
                    BufferID(data->bufferId, "body completion declaration"), data->offset));
                if (driver->sm.getFullPath(position.buffer()) ==
                    driver->sm.getFullPath(driver->getDocument(uri)->getBuffer())) {
                    bool enclosing = false;
                    for (auto original : bodyOriginals)
                        for (auto parent = original->getParentScope(); parent;
                             parent = parent->asSymbol().getParentScope()) {
                            const auto& symbol = parent->asSymbol();
                            if (!symbol.name.empty() && symbol.name == data->symbolName &&
                                driver->sm.getFullyExpandedLoc(symbol.location) == position)
                                enclosing = true;
                        }
                    const bool retainedLater = data->bufferId < bodyBufferLimit &&
                                               position.offset() >= originalBodyEnd;
                    if (enclosing || retainedLater) {
                        auto native = driver->getDocument(uri)->getAnalysis(ctx);
                        data->bufferId = native->getBuffer().getId();
                        data->offset = retainedLater
                                           ? size_t(int64_t(position.offset()) + int64_t(bodyEnd) -
                                                    int64_t(originalBodyEnd))
                                           : position.offset();
                        if (data->receiverBufferId)
                            data->receiverBufferId = native->getBuffer().getId();
                        auto out = item;
                        out.data = rfl::to_generic<rfl::UnderlyingEnums>(*data);
                        completions::MemberCompletionQuery::resolve(driver->completions, out, ctx,
                                                                    native);
                        return out;
                    }
                }
            }
            ast::BodyQueryEvaluationGuard guard(bodyBlocked, bodyBlockedSyntax);
            BodySourceProjection projection(
                driver->sm, driver->getDocument(uri)->getBuffer(), miniAnalysis->getBuffer(),
                originalBodyEnd, bodyEnd, bodyBufferLimit,
                miniAnalysis->getShallowCompilation()->getBodyQueryRoots(), bodyQueryScopeDisplay);
            auto out = item;
            out.data = std::move(payload);
            completions::MemberCompletionQuery::resolve(driver->completions, out, ctx,
                                                        miniAnalysis);
            return out;
        }
        auto data = rfl::from_generic<Data, rfl::UnderlyingEnums>(payload);
        if (!data || data->documentUri != uri.str())
            return {};
        // Automatic locals/formals from any earlier generation never bind a new declaration.
        if (data->bufferId != currentBuffer && data->symbolPath.starts_with(functionPath + ".") &&
            (data->symbolKind == ast::SymbolKind::Variable ||
             data->symbolKind == ast::SymbolKind::FormalArgument)) {
            auto out = item;
            out.data.reset();
            return out;
        }
        // Old automatic locals are rejected above. Variable-length edits also
        // move unchanged declarations after the method: compare current handles
        // against projected DTOs instead of treating offsets as generation-free.
        if (bodyQueries && data->bufferId != currentBuffer &&
            std::find(handles.begin(), handles.end(), data->bufferId) != handles.end())
            return {};
        if (!bodyQueries &&
            std::find(handles.begin(), handles.end(), data->bufferId) != handles.end())
            data->bufferId = originalBuffer;
        for (size_t kind = 0; kind < lexical.size(); ++kind)
            for (size_t i = 0; i < lexical[kind].size(); ++i) {
                const auto& dto = completion(kind, i);
                if (dto.item.data) {
                    auto projected = bodyQueries ? rebase(dto.item, dto.methodOwned) : dto.item;
                    auto expected = rfl::from_generic<Data, rfl::UnderlyingEnums>(*projected.data);
                    if (expected && *data == *expected && item.label == dto.item.label &&
                        item.kind == dto.item.kind) {
                        if (dto.changedDocumentation) {
                            if (!bodyQueries)
                                return {};
                            auto out = item;
                            out.data = rfl::to_generic(NativePayload{frozen.revision, payload});
                            driver->completions.getCompletionItemResolve(out, ctx);
                            return out;
                        }
                        auto out = item;
                        out.data.reset();
                        out.documentation = dto.documentation;
                        Formatting::updateCompletionEditText(out);
                        ctx.checkpoint("before method resolve response");
                        return out;
                    }
                }
            }
    }
    else
        for (size_t kind = 0; kind < lexical.size(); ++kind)
            for (size_t i = 0; i < lexical[kind].size(); ++i) {
                const auto& dto = completion(kind, i);
                if (!dto.item.data && item.label == dto.item.label && item.kind == dto.item.kind) {
                    if (dto.changedDocumentation)
                        return {};
                    auto out = item;
                    out.documentation = dto.documentation;
                    out.insertText = dto.insertText;
                    out.insertTextFormat = dto.insertTextFormat;
                    Formatting::updateCompletionEditText(out);
                    return out;
                }
            }
    return {};
}
catch (const lsp::RequestCancelled&) {
    throw;
}
catch (const std::exception&) {
    return {};
}

} // namespace server
