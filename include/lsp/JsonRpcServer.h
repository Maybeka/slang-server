//------------------------------------------------------------------------------
// JsonRpcServer.h
// Template-based JSON-RPC server implementation with type-safe method registration
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#pragma once
#include "JsonRpc.h"
#include "LspTypes.h"
#include "RequestContext.h"
#include "util/Log.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <rfl/generic/Parser.hpp>
#include <rfl/parsing/Parent.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace lsp {

template<typename Impl>
class JsonRpcServer {
protected:
    template<typename T>
    static rfl::Generic makeResponseValue(const T& value) {
        // Use the same public Writer/Parser path as rfl::to_generic, but transfer
        // the completed tree instead of copying the writer's lvalue root.
        using Writer = rfl::generic::Writer;
        using Parent = rfl::parsing::Parent<Writer>;
        Writer writer;
        rfl::generic::Parser<T, rfl::Processors<rfl::UnderlyingEnums>>::write(
            writer, value, typename Parent::Root{});
        return std::move(writer.root());
    }

    std::unordered_map<std::string,
                       std::function<rfl::Generic(rfl::Generic, const RequestContext&)>>
        requests;
    std::unordered_map<std::string, std::function<void(rfl::Generic, const RequestContext&)>>
        notifications;

    template<typename P, typename R, auto Method>
    void registerMethod(const std::string& name) {
        constexpr bool acceptsContext =
            std::is_same_v<P, std::nullopt_t>
                ? std::is_invocable_v<decltype(Method), Impl*, std::monostate,
                                      const RequestContext&>
                : std::is_invocable_v<decltype(Method), Impl*, P&, const RequestContext&>;
        if constexpr (acceptsContext)
            cancellableMethods.emplace(name);

        requests[name] = [this](std::optional<rfl::Generic> paramsJson,
                                const RequestContext& ctx) -> rfl::Generic {
            auto getResult = [&]() -> R {
                if constexpr (!std::is_same_v<P, std::nullopt_t>) {
                    auto params = rfl::from_generic<P, rfl::UnderlyingEnums>(paramsJson.value());
                    if (!params)
                        throw std::runtime_error(params.error().what());

                    if constexpr (std::is_invocable_v<decltype(Method), Impl*, P&,
                                                      const RequestContext&>) {
                        return (static_cast<Impl*>(this)->*Method)(params.value(), ctx);
                    }
                    else {
                        return (static_cast<Impl*>(this)->*Method)(params.value());
                    }
                }
                else {
                    if constexpr (std::is_invocable_v<decltype(Method), Impl*, std::monostate,
                                                      const RequestContext&>) {
                        return (static_cast<Impl*>(this)->*Method)(std::monostate{}, ctx);
                    }
                    else {
                        return (static_cast<Impl*>(this)->*Method)(std::monostate{});
                    }
                }
            };

            if constexpr (std::is_same_v<R, std::monostate>) {
                getResult();
                return std::nullopt;
            }
            else {
                return makeResponseValue(getResult());
            }
        };
    }

    template<typename P, auto Method>
    void registerNotification(const std::string& name) {
        constexpr bool acceptsContext =
            std::is_same_v<P, std::nullopt_t>
                ? std::is_invocable_v<decltype(Method), Impl*, std::nullopt_t,
                                      const RequestContext&>
                : std::is_invocable_v<decltype(Method), Impl*, P&, const RequestContext&>;
        if constexpr (acceptsContext)
            cancellableMethods.emplace(name);

        notifications[name] = [this](std::optional<rfl::Generic> paramsJson,
                                     const RequestContext& ctx) {
            if constexpr (!std::is_same_v<P, std::nullopt_t>) {
                auto params = rfl::from_generic<P, rfl::UnderlyingEnums>(paramsJson.value());
                if (!params)
                    throw std::runtime_error(params.error().what());

                if constexpr (std::is_invocable_v<decltype(Method), Impl*, P&,
                                                  const RequestContext&>) {
                    (static_cast<Impl*>(this)->*Method)(params.value(), ctx);
                }
                else {
                    (static_cast<Impl*>(this)->*Method)(params.value());
                }
            }
            else {
                if constexpr (std::is_invocable_v<decltype(Method), Impl*, std::nullopt_t,
                                                  const RequestContext&>) {
                    (static_cast<Impl*>(this)->*Method)(std::nullopt, ctx);
                }
                else {
                    (static_cast<Impl*>(this)->*Method)(std::nullopt);
                }
            }
        };
    }

    // Called only by the state-owning worker, never under the queue mutex.
    virtual void onForegroundComplete() {}
    virtual void onBeforeMessage(const RequestContext&) {}
    virtual void onProtocolStopped() {}
    virtual bool hasBackgroundWork() const { return false; }
    virtual std::chrono::milliseconds backgroundGracePeriod() const { return {}; }
    virtual void runBackgroundWork(const RequestContext&) {}
    // Foreground requests and notifications report exceptions under the state lock.
    virtual void onMessageException(const std::exception&) {}
    // Only the state worker obtains this callback. Copies are safe on an I/O
    // thread and become inert when the protocol loop exits.
    std::function<void()> backgroundWakeCallback() const { return m_backgroundWake; }
    std::function<void()> m_backgroundWake = [] {};

    RequestContext createContext(const RpcRequest& request) const {
        return RequestContext(request.method, request.id,
                              cancellableMethods.contains(request.method));
    }

    std::optional<std::string> getDidChangeKey(const RpcRequest& request) const {
        if (request.method != "textDocument/didChange" || !request.params)
            return std::nullopt;

        auto params = rfl::from_generic<DidChangeTextDocumentParams, rfl::UnderlyingEnums>(
            *request.params);
        if (!params)
            return std::nullopt;
        return params->textDocument.uri.str();
    }

    // Only position queries whose input belongs to one document can be
    // superseded this way. Commands and workspace queries retain their order.
    std::optional<std::string> getDocumentQueryKey(const RpcRequest& request) const {
        if (!request.id || !request.params ||
            (request.method != "textDocument/completion" &&
             request.method != "textDocument/hover" &&
             request.method != "textDocument/definition" &&
             request.method != "textDocument/signatureHelp"))
            return {};
        auto object = request.params->to_object();
        if (!object)
            return {};
        for (const auto& [key, value] : *object)
            if (key == "textDocument") {
                auto document = rfl::from_generic<TextDocumentIdentifier>(value);
                return document ? std::optional(document->uri.str()) : std::nullopt;
            }
        return {};
    }

    void registerContext(const RpcRequest& request, const RequestContext& ctx) {
        if (!request.id && !ctx.supportsCancellation())
            return;

        std::lock_guard lock(cancellationMutex);
        if (request.id) {
            activeRequests.insert_or_assign(*request.id, ctx);
            if (m_cancelObsoleteDocumentQueries.load(std::memory_order_relaxed) &&
                ctx.supportsCancellation())
                if (auto key = getDocumentQueryKey(request))
                    pendingDocumentQueries[*key].insert_or_assign(*request.id, ctx);
        }

        if (ctx.supportsCancellation()) {
            auto key = getDidChangeKey(request);
            if (!key)
                return;

            if (m_cancelObsoleteDocumentQueries.load(std::memory_order_relaxed))
                if (auto queries = pendingDocumentQueries.find(*key);
                    queries != pendingDocumentQueries.end())
                    for (const auto& [_, query] : queries->second)
                        query.cancelForContentChange();
            auto it = pendingDocumentChanges.find(*key);
            if (it != pendingDocumentChanges.end() && it->second.id() != ctx.id())
                it->second.cancel();
            pendingDocumentChanges.insert_or_assign(std::move(*key), ctx);
        }
    }

    void unregisterContext(const RpcRequest& request, const RequestContext& ctx) {
        if (!request.id && !ctx.supportsCancellation())
            return;

        std::lock_guard lock(cancellationMutex);
        if (request.id) {
            auto it = activeRequests.find(*request.id);
            if (it != activeRequests.end() && it->second.id() == ctx.id())
                activeRequests.erase(it);
            if (auto key = getDocumentQueryKey(request))
                if (auto queries = pendingDocumentQueries.find(*key);
                    queries != pendingDocumentQueries.end()) {
                    auto query = queries->second.find(*request.id);
                    if (query != queries->second.end() && query->second.id() == ctx.id())
                        queries->second.erase(query);
                    if (queries->second.empty())
                        pendingDocumentQueries.erase(queries);
                }
        }

        if (ctx.supportsCancellation()) {
            auto key = getDidChangeKey(request);
            if (!key)
                return;

            auto it = pendingDocumentChanges.find(*key);
            if (it != pendingDocumentChanges.end() && it->second.id() == ctx.id())
                pendingDocumentChanges.erase(it);
        }
    }

    void cancelRequest(ID_t rpcId) {
        {
            std::lock_guard lock(cancellationMutex);
            if (auto it = activeRequests.find(rpcId); it != activeRequests.end()) {
                auto target = it->second;
                if (!target.hasStarted() || target.supportsCancellation()) {
                    target.cancel();
                    target.info("<--- $/cancelRequest - cancelling {}", target.method());
                }
                else {
                    target.info("<--- $/cancelRequest - {} does not support cancellation",
                                target.method());
                }
                return;
            }
        }

        RequestContext cancelCtx("$/cancelRequest", std::move(rpcId), false);
        cancelCtx.info("<--- $/cancelRequest - cancel requested but already returned");
    }

    void startRequest(const RequestContext& ctx) {
        std::lock_guard lock(cancellationMutex);
        ctx.throwIfCancelled("before handler");
        ctx.markStarted();
        ctx.info("Started {}", ctx.method());
    }

    std::variant<rfl::Generic, RpcError, std::nullopt_t> processMessage(RpcRequest request,
                                                                        RequestContext ctx = {},
                                                                        bool logStart = true) {
        struct MessageLog {
            explicit MessageLog(RequestContext ctx, bool logStart) :
                ctx(std::move(ctx)), enabled(this->ctx.method() != "$/cancelRequest") {
                if (enabled && logStart)
                    this->ctx.startInfo("<--- {}", this->ctx.method());
            }

            ~MessageLog() {
                if (!enabled)
                    return;

                if (cancellationPoint) {
                    if (ctx.rpcId()) {
                        ctx.info("-/-> {} (request cancelled {})", ctx.method(),
                                 *cancellationPoint);
                    }
                    else {
                        ctx.info("---- {} (notification superseded {})", ctx.method(),
                                 *cancellationPoint);
                    }
                }
                else if (error) {
                    ctx.error("-/-> {} Error: {}", ctx.method(), *error);
                }
                else if (ctx.rpcId()) {
                    ctx.info("---> {}", ctx.method());
                }
                else {
                    ctx.info("---- {} (notification finished)", ctx.method());
                }
            }

            void setError(std::string message) { error = std::move(message); }
            void setCancelled(std::string checkpoint) { cancellationPoint = std::move(checkpoint); }

            RequestContext ctx;
            bool enabled;
            std::optional<std::string> error;
            std::optional<std::string> cancellationPoint;
        };

        if (!ctx)
            ctx = createContext(request);
        auto inputWitnessScope = ctx.inputWitnessScope();

        if (!request.id) {
            auto it = notifications.find(request.method);
            if (it != notifications.end()) {
                MessageLog messageLog(ctx, logStart);
                try {
                    onBeforeMessage(messageLog.ctx);
                    if (request.params)
                        it->second(*request.params, messageLog.ctx);
                    else
                        it->second(std::nullopt, messageLog.ctx);

                    messageLog.ctx.throwIfCancelled("before completion");
                }
                catch (const RequestCancelled& e) {
                    messageLog.setCancelled(e.what());
                }
                catch (const std::exception& e) {
                    messageLog.setError(e.what());
                    onMessageException(e);
                }
            }
            else if (request.method.starts_with("$/")) {
                server::logging::warn("<-/- {} (ignoring threaded req)", request.method);
            }
            else {
                server::logging::warn("<-/- {} (method not found)", request.method);
            }
            return std::nullopt;
        }

        auto it = requests.find(request.method);
        if (it == requests.end()) {
            server::logging::warn("<-/- {} (not found)", request.method);
            return std::nullopt;
        }

        MessageLog messageLog(ctx, logStart);
        try {
            startRequest(messageLog.ctx);
            onBeforeMessage(messageLog.ctx);

            rfl::Generic response;
            if (request.params)
                response = it->second(*request.params, messageLog.ctx);
            else
                response = it->second(rfl::Generic{}, messageLog.ctx);

            messageLog.ctx.throwIfCancelled("before response");
            return response;
        }
        catch (const RequestCancelled& e) {
            messageLog.setCancelled(e.what());
            return RpcError{.code = static_cast<int>(ctx.isContentModified()
                                                         ? LSPErrorCodes::ContentModified
                                                         : LSPErrorCodes::RequestCancelled),
                            .message = ctx.isContentModified() ? "Document content modified"
                                                               : "Request cancelled"};
        }
        catch (const RequestFailed& e) {
            messageLog.setError(e.what());
            return RpcError{.code = static_cast<int>(LSPErrorCodes::RequestFailed),
                            .message = e.what()};
        }
        catch (const std::exception& e) {
            messageLog.setError(e.what());
            onMessageException(e);
            return RpcError{.code = static_cast<int>(ErrorCodes::InternalError),
                            .message = e.what()};
        }
    }

    void handleMessage(RpcRequest request) {
        auto ctx = createContext(request);
        registerContext(request, ctx);
        handleMessage(std::move(request), std::move(ctx));
    }

    void handleMessage(RpcRequest request, RequestContext ctx, bool logStart = true) {
        std::lock_guard<std::mutex> lock(serverStateMutex);
        auto result = processMessage(request, ctx, logStart);
        unregisterContext(request, ctx);
        std::visit(
            [&request](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, rfl::Generic>) {
                    sendMessage(RpcResponse{
                        .jsonrpc = "2.0",
                        .id = request.id,
                        .result = std::move(value),
                    });
                }
                else if constexpr (std::is_same_v<T, RpcError>) {
                    sendMessage(RpcErrorResponse{
                        .jsonrpc = "2.0",
                        .id = request.id,
                        .error = value,
                    });
                }
            },
            result);
    }

    // Protects server state by serializing LSP and WCP handler execution.
    std::mutex serverStateMutex;

    std::unordered_set<std::string> cancellableMethods;
    // Configured on the state worker, observed by the protocol reader.
    std::atomic_bool m_cancelObsoleteDocumentQueries = false;
    std::unordered_map<std::string, std::unordered_map<ID_t, RequestContext>>
        pendingDocumentQueries;

    // Protects activeRequests and pendingDocumentChanges.
    std::mutex cancellationMutex;

    // Tracks every active request so cancellation can distinguish unsupported routes.
    std::unordered_map<ID_t, RequestContext> activeRequests;

    // Successive /didChange requests should cancel earlier ones doing analysis
    std::unordered_map<std::string, RequestContext> pendingDocumentChanges;

public:
    void run() {
        std::string inputLine;
        std::string inputContent;

        // Init loop
        while (true) {
            auto request = readJson<RpcRequest>(inputLine, inputContent);
            if (!request)
                return;

            if (request->method != "initialize") {
                sendMessage(RpcErrorResponse{
                    .jsonrpc = "2.0",
                    .id = request->id,
                    .error =
                        RpcError{
                            .code = static_cast<int>(ErrorCodes::ServerNotInitialized),
                            .message = "Server not initialized",
                        },
                });
                continue;
            }
            handleMessage(*request);
            server::logging::blankLine();
            break;
        }

        struct QueuedMessage {
            RpcRequest request;
            RequestContext ctx;
        };
        std::deque<QueuedMessage> queue;

        // Protects the queue, worker state, and deferred log separator.
        struct WakeState {
            std::mutex mutex;
            std::condition_variable condition;
            bool requested = false;
        };
        auto wakeState = std::make_shared<WakeState>();
        auto& queueMutex = wakeState->mutex;
        auto& queueCondition = wakeState->condition;
        m_backgroundWake = [weak = std::weak_ptr<WakeState>(wakeState)] {
            if (auto state = weak.lock()) {
                {
                    std::lock_guard lock(state->mutex);
                    state->requested = true;
                }
                state->condition.notify_one();
            }
        };
        bool inputFinished = false;
        bool workerBusy = false;
        bool separatorPending = false;
        bool backgroundPending = false;
        std::optional<RequestContext> activeBackground;

        // worker- processes messages in order
        auto* logOutput = server::logging::getOutput();
        std::thread worker([&, logOutput] {
            server::logging::setOutput(logOutput);
            while (true) {
                std::optional<QueuedMessage> message;
                RequestContext background;
                {
                    std::unique_lock lock(queueMutex);
                    queueCondition.wait(lock, [&] {
                        return inputFinished || !queue.empty() || backgroundPending ||
                               wakeState->requested;
                    });
                    // Only work with expensive, non-interruptible foreground-state
                    // analysis needs to coalesce response-triggered query bursts.
                    if (queue.empty() && !inputFinished) {
                        lock.unlock();
                        std::chrono::milliseconds delay;
                        {
                            std::lock_guard stateLock(serverStateMutex);
                            delay = backgroundGracePeriod();
                        }
                        lock.lock();
                        if (delay.count() > 0 && queue.empty() && !inputFinished) {
                            queueCondition.wait_for(lock, delay, [&] {
                                return inputFinished || !queue.empty();
                            });
                        }
                    }
                    if (!queue.empty()) {
                        message = std::move(queue.front());
                        queue.pop_front();
                    }
                    else if (inputFinished) {
                        return;
                    }
                    else {
                        // Consume the wake here, before calling user code. A completion
                        // racing that code re-arms it and cannot be overwritten below.
                        wakeState->requested = false;
                        backgroundPending = false;
                        // Selection and registration are atomic with enqueue/cancellation.
                        background = RequestContext("background/diagnostics", std::nullopt);
                        activeBackground = background;
                    }
                    workerBusy = true;
                }

                bool pending = false;
                if (message) {
                    handleMessage(std::move(message->request), std::move(message->ctx), false);
                    std::lock_guard stateLock(serverStateMutex);
                    onForegroundComplete();
                    pending = hasBackgroundWork();
                }
                else {
                    std::lock_guard stateLock(serverStateMutex);
                    try {
                        background.startInfo("Started background diagnostics");
                        background.throwIfCancelled("before background work");
                        runBackgroundWork(background);
                        pending = hasBackgroundWork();
                        background.info("Finished background diagnostics");
                    }
                    catch (const RequestCancelled& e) {
                        background.info("Deferred background diagnostics: {}", e.what());
                        pending = hasBackgroundWork();
                    }
                    catch (const std::exception& e) {
                        // Keep driver work pending, but retry only after another foreground
                        // message. A persistent failure must not become an idle busy loop.
                        background.error("Background diagnostics failed: {}", e.what());
                    }
                }
                {
                    std::lock_guard lock(queueMutex);
                    activeBackground.reset();
                    backgroundPending = pending;
                    workerBusy = false;
                    if (queue.empty())
                        separatorPending = true;
                }
            }
        });

        auto printPendingSeparatorLocked = [&] {
            if (separatorPending) {
                server::logging::blankLine();
                separatorPending = false;
            }
        };

        auto enqueue = [&](RpcRequest queuedRequest) {
            auto ctx = createContext(queuedRequest);
            registerContext(queuedRequest, ctx);
            {
                std::lock_guard lock(queueMutex);
                printPendingSeparatorLocked();
                ctx.startInfo("<--- {}", ctx.method());
                if (activeBackground)
                    activeBackground->cancel();
                queue.push_back({std::move(queuedRequest), std::move(ctx)});
            }
            queueCondition.notify_one();
        };

        auto handleCancelRequest = [&](RpcRequest cancellationRequest) {
            {
                std::lock_guard lock(queueMutex);
                printPendingSeparatorLocked();
                if (activeBackground)
                    activeBackground->cancel();
            }
            processMessage(std::move(cancellationRequest));
            {
                std::lock_guard lock(queueMutex);
                if (!workerBusy && queue.empty())
                    separatorPending = true;
            }
        };

        // Main loop - reads stdin
        bool shutdown = false;
        while (auto request = readJson<RpcRequest>(inputLine, inputContent)) {
            shutdown = request->method == "shutdown";
            if (request->method == "$/cancelRequest")
                handleCancelRequest(std::move(*request));
            else
                enqueue(std::move(*request));
            if (shutdown)
                break;
        }

        // Shutdown loop
        if (shutdown) {
            while (auto request = readJson<RpcRequest>(inputLine, inputContent)) {
                if (request->method == "exit")
                    break;

                if (request->method == "$/cancelRequest") {
                    handleCancelRequest(std::move(*request));
                }
                else {
                    sendMessage(RpcErrorResponse{
                        .jsonrpc = "2.0",
                        .id = request->id,
                        .error =
                            RpcError{
                                .code = static_cast<int>(ErrorCodes::InvalidRequest),
                                .message = "Invalid Request",
                            },
                    });
                }
            }
        }

        {
            std::lock_guard lock(queueMutex);
            inputFinished = true;
            if (activeBackground)
                activeBackground->cancel();
        }
        queueCondition.notify_one();
        worker.join();
        onProtocolStopped();
        m_backgroundWake = [] {};
    }
};
} // namespace lsp
