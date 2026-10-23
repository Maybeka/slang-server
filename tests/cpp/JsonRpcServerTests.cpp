#include "lsp/JsonRpcServer.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "slang/util/ScopeGuard.h"

namespace {

struct CancelParams {
    lsp::ID_t id;
};

struct WirePayload {
    std::string label;
    std::vector<int> values;
    lsp::CompletionItemKind kind;
    std::optional<std::string> omitted;
};

class TestJsonRpcServer : public lsp::JsonRpcServer<TestJsonRpcServer> {
public:
    TestJsonRpcServer() {
        registerMethod<std::nullopt_t, int, &TestJsonRpcServer::succeedRequest>("success");
        registerMethod<std::nullopt_t, int, &TestJsonRpcServer::failRequest>("fail");
        registerMethod<std::nullopt_t, int, &TestJsonRpcServer::contextRequest>("context");
        registerMethod<std::nullopt_t, std::vector<WirePayload>,
                       &TestJsonRpcServer::payloadRequest>("payload");
        registerMethod<std::nullopt_t, std::vector<WirePayload>, &TestJsonRpcServer::emptyRequest>(
            "empty");
        registerMethod<std::nullopt_t, std::optional<WirePayload>, &TestJsonRpcServer::nullRequest>(
            "null");
        registerMethod<std::nullopt_t, std::monostate, &TestJsonRpcServer::contextVoidRequest>(
            "context-void");
        registerNotification<std::nullopt_t, &TestJsonRpcServer::succeedNotification>(
            "notification");
        registerNotification<std::nullopt_t, &TestJsonRpcServer::failNotification>(
            "failed-notification");
        registerNotification<std::nullopt_t, &TestJsonRpcServer::contextNotification>(
            "context-notification");
        for (const auto* name : {"textDocument/completion", "textDocument/hover",
                                 "textDocument/definition", "textDocument/signatureHelp"})
            registerMethod<lsp::TextDocumentPositionParams, int, &TestJsonRpcServer::documentQuery>(
                name);
        registerNotification<CancelParams, &TestJsonRpcServer::cancelRequestNotification>(
            "$/cancelRequest");
        registerNotification<lsp::DidChangeTextDocumentParams,
                             &TestJsonRpcServer::didChangeNotification>("textDocument/didChange");
    }

    int succeedRequest(std::monostate) { return 42; }

    int failRequest(std::monostate) { throw std::runtime_error("failed"); }

    std::vector<WirePayload> payloadRequest(std::monostate) {
        return {{"quote\"\\\nλ", {1, 2, 3}, lsp::CompletionItemKind::Variable, std::nullopt}};
    }

    std::vector<WirePayload> emptyRequest(std::monostate) { return {}; }

    std::optional<WirePayload> nullRequest(std::monostate) { return std::nullopt; }

    int contextRequest(std::monostate, const lsp::RequestContext& ctx) {
        contextRequestCalls++;
        ctx.info("request checkpoint");
        return 42;
    }

    std::monostate contextVoidRequest(std::monostate, const lsp::RequestContext& ctx) {
        ctx.info("void request checkpoint");
        return {};
    }

    void succeedNotification(std::nullopt_t) {}

    void failNotification(std::nullopt_t) { throw std::runtime_error("notification failed"); }

    void contextNotification(std::nullopt_t, const lsp::RequestContext& ctx) {
        ctx.info("notification checkpoint");
    }

    void cancelRequestNotification(const CancelParams& params) { cancelRequest(params.id); }

    void didChangeNotification(const lsp::DidChangeTextDocumentParams&,
                               const lsp::RequestContext& ctx) {
        ctx.throwIfCancelled("before analysis");
    }

    lsp::RequestContext track(const lsp::RpcRequest& request) {
        auto ctx = createContext(request);
        registerContext(request, ctx);
        return ctx;
    }

    void untrack(const lsp::RpcRequest& request, const lsp::RequestContext& ctx) {
        unregisterContext(request, ctx);
    }

    int documentQuery(const lsp::TextDocumentPositionParams&, const lsp::RequestContext&) {
        ++contextRequestCalls;
        return 42;
    }
    void cancelQueriesOnEdit(bool enabled) { m_cancelObsoleteDocumentQueries = enabled; }
    size_t documentQueryCount() const { return pendingDocumentQueries.size(); }
    void start(const lsp::RequestContext& ctx) { startRequest(ctx); }

    int contextRequestCalls = 0;

    using JsonRpcServer::handleMessage;
    using JsonRpcServer::processMessage;
};

class LogCapture {
public:
    LogCapture() {
        output = std::tmpfile();
        if (!output)
            throw std::runtime_error("Failed to create stderr capture file");
        original = server::logging::setOutput(output);
    }

    ~LogCapture() {
        server::logging::setOutput(original);
        std::fclose(output);
    }

    std::string str() {
        std::fflush(output);
        const auto size = std::ftell(output);
        if (size < 0 || std::fseek(output, 0, SEEK_SET) != 0)
            throw std::runtime_error("Failed to read captured stderr");

        std::string result(static_cast<size_t>(size), '\0');
        if (!result.empty() && std::fread(result.data(), 1, result.size(), output) != result.size())
            throw std::runtime_error("Failed to read captured stderr");

        std::fseek(output, 0, SEEK_END);
        return result;
    }

    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

private:
    FILE* output = nullptr;
    FILE* original = nullptr;
};

} // namespace

TEST_CASE("JSON-RPC request IDs support integers and strings") {
    auto numeric = rfl::json::read<lsp::RpcRequest>(
        R"({"jsonrpc":"2.0","id":17,"method":"test","params":null})");
    REQUIRE(numeric);
    REQUIRE(numeric->id);
    CHECK(std::get<int>(*numeric->id) == 17);

    auto string = rfl::json::read<lsp::RpcRequest>(
        R"({"jsonrpc":"2.0","id":"request-id","method":"test","params":null})");
    REQUIRE(string);
    REQUIRE(string->id);
    CHECK(std::get<std::string>(*string->id) == "request-id");
}

TEST_CASE("JSON-RPC response ownership preserves exact wire output", "[json-rpc][serialization]") {
    TestJsonRpcServer server;
    auto capture = [&](const lsp::RpcRequest& request, const lsp::RequestContext* ctx = nullptr) {
        std::ostringstream output;
        {
            auto* original = std::cout.rdbuf(output.rdbuf());
            slang::ScopeGuard restore([&] { std::cout.rdbuf(original); });
            if (ctx)
                server.handleMessage(request, *ctx);
            else
                server.handleMessage(request);
        }
        return output.str();
    };

    auto frame = [](const std::string& body) {
        return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    };
    for (const auto& id :
         {lsp::ID_t{17}, lsp::ID_t{std::string{"request-λ"}}, lsp::ID_t{std::string{}}}) {
        const auto idJson = std::holds_alternative<int>(id)
                                ? std::string{"17"}
                                : "\"" + std::get<std::string>(id) + "\"";
        const auto prefix = "{\"jsonrpc\":\"2.0\",\"id\":" + idJson;
        auto check = [&](const std::string& method, const std::string& suffix) {
            auto output = capture(lsp::RpcRequest{.jsonrpc = "2.0", .id = id, .method = method});
            CHECK(output == frame(prefix + suffix));
        };

        // Every response consumes its own tree; subsequent responses must remain intact.
        check("payload",
              R"json(,"result":[{"label":"quote\"\\\nλ","values":[1,2,3],"kind":6}]})json");
        check("payload",
              R"json(,"result":[{"label":"quote\"\\\nλ","values":[1,2,3],"kind":6}]})json");
        check("empty", R"(,"result":[]})");
        check("null", R"(,"result":null})");
        check("context-void", R"(,"result":null})");
        check("success", R"(,"result":42})");
        check("fail", R"(,"error":{"code":-32603,"message":"failed"}})");

        lsp::RpcRequest request{.jsonrpc = "2.0", .id = id, .method = "context"};
        auto ctx = server.track(request);
        ctx.cancel();
        CHECK(capture(request, &ctx) ==
              frame(prefix + R"(,"error":{"code":-32800,"message":"Request cancelled"}})"));
        CHECK(server.contextRequestCalls == 0);
    }

    CHECK(capture(lsp::RpcRequest{.jsonrpc = "2.0", .method = "notification"}).empty());
}

TEST_CASE("JSON-RPC server separates activity batches and exits when input closes") {
    TestJsonRpcServer server;
    LogCapture capture;
    auto frame = [](std::string_view message) {
        return fmt::format("Content-Length: {}\r\n\r\n{}", message.size(), message);
    };
    const std::string initialize =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":null})";
    const std::string notification = R"({"jsonrpc":"2.0","method":"notification","params":null})";
    std::istringstream input(frame(initialize) + frame(notification) + frame(notification));
    const auto originalState = std::cin.rdstate();
    auto* originalBuffer = std::cin.rdbuf(input.rdbuf());
    slang::ScopeGuard restoreInput([&] {
        std::cin.rdbuf(originalBuffer);
        std::cin.clear(originalState);
    });

    server.run();
    const auto output = capture.str();
    const auto separator = output.find("\n\n");
    REQUIRE(separator != std::string::npos);

    const auto firstCompletion = output.find("(notification finished)");
    REQUIRE(firstCompletion != std::string::npos);
    CHECK(separator < firstCompletion);
    CHECK(output.find("(notification finished)", firstCompletion + 1) != std::string::npos);
}

TEST_CASE("JSON-RPC messages log their latency") {
    TestJsonRpcServer server;
    LogCapture capture;

    SECTION("successful request") {
        auto result = server.processMessage({
            .jsonrpc = "2.0",
            .id = 7,
            .method = "success",
            .params = std::nullopt,
        });

        CHECK(std::holds_alternative<rfl::Generic>(result));
        CHECK(std::regex_match(
            capture.str(),
            std::regex(
                R"(\[#7 [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] INFO: <--- success\n\[#7 {8}\+\.[0-9]{3}\] INFO: Started success\n\[#7 {8}\+\.[0-9]{3}\] INFO: ---> success\n)")));
    }

    SECTION("failed request") {
        auto result = server.processMessage({
            .jsonrpc = "2.0",
            .id = std::string("request-id"),
            .method = "fail",
            .params = std::nullopt,
        });

        CHECK(std::holds_alternative<lsp::RpcError>(result));
        CHECK(std::regex_match(
            capture.str(),
            std::regex(
                R"(\[#request-id [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] INFO: <--- fail\n\[#request-id {8}\+\.[0-9]{3}\] INFO: Started fail\n\[#request-id {8}\+\.[0-9]{3}\] ERROR: -/-> fail Error: failed\n)")));
    }

    SECTION("successful notification") {
        auto result = server.processMessage({
            .jsonrpc = "2.0",
            .id = std::nullopt,
            .method = "notification",
            .params = std::nullopt,
        });

        CHECK(std::holds_alternative<std::nullopt_t>(result));
        CHECK(std::regex_match(
            capture.str(),
            std::regex(
                R"(\[n([0-9]+) [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] INFO: <--- notification\n\[n\1 {8}\+\.[0-9]{3}\] INFO: ---- notification \(notification finished\)\n)")));
    }

    SECTION("failed notification") {
        auto result = server.processMessage({
            .jsonrpc = "2.0",
            .id = std::nullopt,
            .method = "failed-notification",
            .params = std::nullopt,
        });

        CHECK(std::holds_alternative<std::nullopt_t>(result));
        CHECK(std::regex_match(
            capture.str(),
            std::regex(
                R"(\[n([0-9]+) [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] INFO: <--- failed-notification\n\[n\1 {8}\+\.[0-9]{3}\] ERROR: -/-> failed-notification Error: notification failed\n)")));
    }

    SECTION("handler receives request context") {
        auto result = server.processMessage({
            .jsonrpc = "2.0",
            .id = 9,
            .method = "context",
            .params = std::nullopt,
        });

        CHECK(std::holds_alternative<rfl::Generic>(result));
        CHECK(std::regex_match(
            capture.str(),
            std::regex(
                R"(\[#9 [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] INFO: <--- context\n\[#9 {8}\+\.[0-9]{3}\] INFO: Started context\n\[#9 {8}\+\.[0-9]{3}\] INFO: request checkpoint\n\[#9 {8}\+\.[0-9]{3}\] INFO: ---> context\n)")));
    }

    SECTION("notification handler receives request context") {
        auto result = server.processMessage({
            .jsonrpc = "2.0",
            .id = std::nullopt,
            .method = "context-notification",
            .params = std::nullopt,
        });

        CHECK(std::holds_alternative<std::nullopt_t>(result));
        CHECK(std::regex_match(
            capture.str(),
            std::regex(
                R"(\[n([0-9]+) [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] INFO: <--- context-notification\n\[n\1 {8}\+\.[0-9]{3}\] INFO: notification checkpoint\n\[n\1 {8}\+\.[0-9]{3}\] INFO: ---- context-notification \(notification finished\)\n)")));
    }

    SECTION("void request handler receives request context") {
        auto result = server.processMessage({
            .jsonrpc = "2.0",
            .id = 10,
            .method = "context-void",
            .params = std::nullopt,
        });

        CHECK(std::holds_alternative<rfl::Generic>(result));
        CHECK(std::regex_match(
            capture.str(),
            std::regex(
                R"(\[#10 [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] INFO: <--- context-void\n\[#10 {8}\+\.[0-9]{3}\] INFO: Started context-void\n\[#10 {8}\+\.[0-9]{3}\] INFO: void request checkpoint\n\[#10 {8}\+\.[0-9]{3}\] INFO: ---> context-void\n)")));
    }

    SECTION("cancel request marks matching context") {
        lsp::RpcRequest request{
            .jsonrpc = "2.0",
            .id = 11,
            .method = "context",
            .params = std::nullopt,
        };
        auto ctx = server.track(request);

        auto cancelResult = server.processMessage({
            .jsonrpc = "2.0",
            .id = std::nullopt,
            .method = "$/cancelRequest",
            .params = rfl::to_generic<rfl::UnderlyingEnums>(CancelParams{.id = 11}),
        });
        CHECK(std::holds_alternative<std::nullopt_t>(cancelResult));
        CHECK(ctx.isCancelled());
        CHECK(std::regex_match(
            capture.str(),
            std::regex(
                R"(\[#11 {8}\+\.[0-9]{3}\] INFO: <--- \$/cancelRequest - cancelling context\n)")));

        auto result = server.processMessage(request, ctx);
        REQUIRE(std::holds_alternative<lsp::RpcError>(result));
        CHECK(std::get<lsp::RpcError>(result).code ==
              static_cast<int>(lsp::LSPErrorCodes::RequestCancelled));
        CHECK(server.contextRequestCalls == 0);
        server.untrack(request, ctx);
    }

    SECTION("queued request without context support can be cancelled") {
        lsp::RpcRequest request{
            .jsonrpc = "2.0",
            .id = 14,
            .method = "success",
            .params = std::nullopt,
        };
        auto ctx = server.track(request);

        server.processMessage({
            .jsonrpc = "2.0",
            .id = std::nullopt,
            .method = "$/cancelRequest",
            .params = rfl::to_generic<rfl::UnderlyingEnums>(CancelParams{.id = 14}),
        });
        CHECK(ctx.isCancelled());
        CHECK(std::regex_match(
            capture.str(),
            std::regex(
                R"(\[#14 {8}\+\.[0-9]{3}\] INFO: <--- \$/cancelRequest - cancelling success\n)")));

        auto result = server.processMessage(request, ctx);
        REQUIRE(std::holds_alternative<lsp::RpcError>(result));
        CHECK(std::get<lsp::RpcError>(result).code ==
              static_cast<int>(lsp::LSPErrorCodes::RequestCancelled));
        server.untrack(request, ctx);
    }

    SECTION("running request without context support ignores cancellation") {
        lsp::RpcRequest request{
            .jsonrpc = "2.0",
            .id = 15,
            .method = "success",
            .params = std::nullopt,
        };
        auto ctx = server.track(request);
        server.start(ctx);

        server.processMessage({
            .jsonrpc = "2.0",
            .id = std::nullopt,
            .method = "$/cancelRequest",
            .params = rfl::to_generic<rfl::UnderlyingEnums>(CancelParams{.id = 15}),
        });
        CHECK_FALSE(ctx.isCancelled());
        CHECK(std::regex_match(
            capture.str(),
            std::regex(
                R"(\[#15 {8}\+\.[0-9]{3}\] INFO: Started success\n\[#15 {8}\+\.[0-9]{3}\] INFO: <--- \$/cancelRequest - success does not support cancellation\n)")));
        server.untrack(request, ctx);
    }

    SECTION("late cancellation is reported") {
        auto result = server.processMessage({
            .jsonrpc = "2.0",
            .id = std::nullopt,
            .method = "$/cancelRequest",
            .params = rfl::to_generic<rfl::UnderlyingEnums>(CancelParams{.id = 404}),
        });

        CHECK(std::holds_alternative<std::nullopt_t>(result));
        CHECK(std::regex_match(
            capture.str(),
            std::regex(
                R"(\[#404 {8}\+\.[0-9]{3}\] INFO: <--- \$/cancelRequest - cancel requested but already returned\n)")));
    }

    SECTION("numeric and string request IDs are distinct") {
        lsp::RpcRequest numericRequest{
            .jsonrpc = "2.0",
            .id = 12,
            .method = "context",
            .params = std::nullopt,
        };
        lsp::RpcRequest stringRequest{
            .jsonrpc = "2.0",
            .id = std::string("12"),
            .method = "context",
            .params = std::nullopt,
        };
        auto numericContext = server.track(numericRequest);
        auto stringContext = server.track(stringRequest);

        server.processMessage({
            .jsonrpc = "2.0",
            .id = std::nullopt,
            .method = "$/cancelRequest",
            .params = rfl::to_generic<rfl::UnderlyingEnums>(CancelParams{.id = 12}),
        });
        CHECK(numericContext.isCancelled());
        CHECK_FALSE(stringContext.isCancelled());

        server.processMessage({
            .jsonrpc = "2.0",
            .id = std::nullopt,
            .method = "$/cancelRequest",
            .params = rfl::to_generic<rfl::UnderlyingEnums>(CancelParams{.id = std::string("12")}),
        });
        CHECK(stringContext.isCancelled());

        server.untrack(numericRequest, numericContext);
        server.untrack(stringRequest, stringContext);
    }

    SECTION("new didChange cancels pending work for the same document") {
        auto makeChange = [](std::string uri, int version) {
            auto params = lsp::DidChangeTextDocumentParams{
                .textDocument = lsp::VersionedTextDocumentIdentifier{.version = version,
                                                                     .uri = URI(std::move(uri))},
                .contentChanges = {lsp::TextDocumentContentChangeWholeDocument{
                    .text = fmt::format("version {}", version)}},
            };
            return lsp::RpcRequest{
                .jsonrpc = "2.0",
                .id = std::nullopt,
                .method = "textDocument/didChange",
                .params = rfl::to_generic<rfl::UnderlyingEnums>(params),
            };
        };

        auto first = makeChange("file:///same.sv", 1);
        auto second = makeChange("file:///same.sv", 2);
        auto other = makeChange("file:///other.sv", 1);
        auto firstContext = server.track(first);
        auto secondContext = server.track(second);
        auto otherContext = server.track(other);

        CHECK(firstContext.isCancelled());
        CHECK_FALSE(secondContext.isCancelled());
        CHECK_FALSE(otherContext.isCancelled());

        auto result = server.processMessage(first, firstContext);
        CHECK(std::holds_alternative<std::nullopt_t>(result));
        CHECK(std::regex_match(
            capture.str(),
            std::regex(
                R"(\[n([0-9]+) [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] INFO: <--- textDocument/didChange\n\[n\1 {8}\+\.[0-9]{3}\] INFO: ---- textDocument/didChange \(notification superseded before analysis\)\n)")));

        server.untrack(first, firstContext);
        server.untrack(second, secondContext);
        server.untrack(other, otherContext);
    }
}

#include "lsp/JsonRpcServer.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

// No get area is exposed, so every extraction is synchronized. An empty-read
// boundary is stronger than "all bytes consumed": readJson has returned and the
// server has dispatched/enqueued that frame before attempting the next read.
class IdleBlockingInput : public std::streambuf {
public:
    size_t append(std::string_view text) {
        std::lock_guard lock(mutex);
        bytes.insert(bytes.end(), text.begin(), text.end());
        appended += text.size();
        changed.notify_all();
        return appended;
    }

    bool waitForReadBoundary(size_t target) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 10s, [&] { return lastEmptyRead >= target; });
    }

    void close() {
        std::lock_guard lock(mutex);
        closed = true;
        changed.notify_all();
    }

protected:
    int_type underflow() override {
        std::unique_lock lock(mutex);
        waitForData(lock);
        return bytes.empty() ? traits_type::eof() : traits_type::to_int_type(bytes.front());
    }

    int_type uflow() override {
        std::unique_lock lock(mutex);
        waitForData(lock);
        if (bytes.empty())
            return traits_type::eof();
        auto value = traits_type::to_int_type(bytes.front());
        bytes.pop_front();
        ++consumed;
        return value;
    }

    std::streamsize xsgetn(char* destination, std::streamsize requested) override {
        std::unique_lock lock(mutex);
        std::streamsize copied = 0;
        while (copied < requested) {
            waitForData(lock);
            if (bytes.empty())
                break;
            auto count = std::min(bytes.size(), static_cast<size_t>(requested - copied));
            for (size_t i = 0; i < count; ++i) {
                destination[copied++] = bytes.front();
                bytes.pop_front();
            }
            consumed += count;
        }
        return copied;
    }

private:
    void waitForData(std::unique_lock<std::mutex>& lock) {
        if (bytes.empty()) {
            lastEmptyRead = consumed;
            changed.notify_all();
            changed.wait(lock, [&] { return closed || !bytes.empty(); });
        }
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<char> bytes;
    size_t appended = 0;
    size_t consumed = 0;
    size_t lastEmptyRead = 0;
    bool closed = false;
};

class IdleCapturedOutput : public std::streambuf {
public:
    std::string str() {
        std::lock_guard lock(mutex);
        return text;
    }

protected:
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return traits_type::not_eof(value);
        std::lock_guard lock(mutex);
        text.push_back(traits_type::to_char_type(value));
        return value;
    }

    std::streamsize xsputn(const char* source, std::streamsize count) override {
        std::lock_guard lock(mutex);
        text.append(source, static_cast<size_t>(count));
        return count;
    }

    int sync() override { return 0; }

private:
    std::mutex mutex;
    std::string text;
};

class IdlePermitGate {
public:
    void wait(size_t attempt) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return allowedThrough >= attempt; });
    }

    void releaseThrough(size_t attempt) {
        std::lock_guard lock(mutex);
        allowedThrough = std::max(allowedThrough, attempt);
        changed.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    size_t allowedThrough = 0;
};

struct IdleSnapshotParams {
    std::string uri;
};

struct IdleCancelParams {
    lsp::ID_t id;
};

class IdleQueueTestServer : public lsp::JsonRpcServer<IdleQueueTestServer> {
public:
    explicit IdleQueueTestServer(bool gateBackground, std::chrono::milliseconds delay = 2ms) :
        gateBackground(gateBackground), delay(delay) {
        registerMethod<std::nullopt_t, std::monostate, &IdleQueueTestServer::initialize>(
            "initialize");
        registerMethod<std::nullopt_t, std::monostate, &IdleQueueTestServer::hold>("test/hold");
        registerMethod<std::nullopt_t, std::monostate, &IdleQueueTestServer::captureWake>(
            "test/captureWake");
        registerMethod<IdleSnapshotParams, std::string, &IdleQueueTestServer::snapshot>(
            "test/snapshot");
        registerMethod<std::nullopt_t, std::monostate, &IdleQueueTestServer::shutdown>("shutdown");
        registerNotification<lsp::DidChangeTextDocumentParams, &IdleQueueTestServer::edit>(
            "textDocument/didChange");
        registerNotification<IdleCancelParams, &IdleQueueTestServer::cancel>("$/cancelRequest");
    }

    bool waitForEvent(std::string_view wanted) {
        std::unique_lock lock(observationMutex);
        return observationChanged.wait_for(lock, 10s, [&] {
            return std::ranges::find(events, wanted) != events.end();
        });
    }

    std::vector<std::string> trace() {
        std::lock_guard lock(observationMutex);
        return events;
    }

    std::vector<uint64_t> backgroundContexts() {
        std::lock_guard lock(observationMutex);
        return contextIds;
    }

    std::function<void()> copiedWake() {
        std::lock_guard lock(observationMutex);
        return externalWake;
    }

    void releaseForeground() { foregroundGate.releaseThrough(1); }
    void releaseBackground(size_t attempt) { backgroundGate.releaseThrough(attempt); }
    void releaseAll() {
        foregroundGate.releaseThrough(std::numeric_limits<size_t>::max());
        backgroundGate.releaseThrough(std::numeric_limits<size_t>::max());
    }

protected:
    std::chrono::milliseconds backgroundGracePeriod() const override {
        if (delay > 2ms)
            record("idle:waiting");
        return delay;
    }

    bool hasBackgroundWork() const override {
        if (shuttingDown) {
            record("shutdown:idle-disabled");
            return false;
        }
        return pending;
    }

    void runBackgroundWork(const lsp::RequestContext& context) override {
        const auto attempt = ++backgroundAttempts;
        {
            std::lock_guard lock(observationMutex);
            contextIds.push_back(context.id());
        }
        record("idle:start:" + std::to_string(attempt));
        if (gateBackground)
            backgroundGate.wait(attempt);
        try {
            context.throwIfCancelled("test background checkpoint");
        }
        catch (const lsp::RequestCancelled&) {
            record("idle:cancel:" + std::to_string(attempt));
            throw;
        }

        std::string publication = "publish:";
        for (const auto& [uri, _] : documents) {
            if (publication != "publish:")
                publication += "|";
            publication += describe(uri);
        }
        record(std::move(publication));
        pending = false;
        record("idle:finish:" + std::to_string(attempt));
    }

private:
    struct Document {
        std::string text;
        int version = 0;
    };

    std::monostate initialize(std::monostate) {
        record("initialize");
        return {};
    }

    std::monostate captureWake(std::monostate) {
        {
            std::lock_guard lock(observationMutex);
            externalWake = backgroundWakeCallback();
        }
        record("wake:captured");
        return {};
    }

    std::monostate hold(std::monostate) {
        record("hold");
        foregroundGate.wait(1);
        record("hold:released");
        return {};
    }

    void edit(const lsp::DidChangeTextDocumentParams& params, const lsp::RequestContext&) {
        // Like the real driver, synchronization runs even if a later same-document
        // notification has already superseded this notification's analysis context.
        auto uri = params.textDocument.uri.str();
        auto& document = documents[uri];
        for (const auto& change : params.contentChanges) {
            rfl::visit(
                [&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, lsp::TextDocumentContentChangePartial>) {
                        if (value.range.start.line || value.range.end.line ||
                            value.range.end.character < value.range.start.character ||
                            value.range.end.character > document.text.size())
                            throw std::runtime_error("Invalid test incremental edit");
                        document.text.replace(
                            value.range.start.character,
                            value.range.end.character - value.range.start.character, value.text);
                    }
                    else {
                        document.text = value.text;
                    }
                },
                change);
        }
        document.version = params.textDocument.version;
        pending = true;
        record("edit:" + describe(uri));
    }

    std::string snapshot(const IdleSnapshotParams& params) {
        auto value = describe(params.uri);
        record("query:" + value);
        return value;
    }

    std::monostate shutdown(std::monostate) {
        shuttingDown = true;
        record("shutdown");
        return {};
    }

    void cancel(const IdleCancelParams& params) {
        cancelRequest(params.id);
        record("cancel-notification");
    }

    std::string describe(const std::string& uri) const {
        const auto& document = documents.at(uri);
        return uri.substr(uri.find_last_of('/') + 1) + "@" + std::to_string(document.version) +
               ":" + document.text;
    }

    void record(std::string event) const {
        std::lock_guard lock(observationMutex);
        events.push_back(std::move(event));
        observationChanged.notify_all();
    }

    const bool gateBackground;
    const std::chrono::milliseconds delay;
    bool pending = false;
    bool shuttingDown = false;
    size_t backgroundAttempts = 0;
    std::map<std::string, Document> documents;
    IdlePermitGate foregroundGate;
    IdlePermitGate backgroundGate;
    mutable std::mutex observationMutex;
    mutable std::condition_variable observationChanged;
    mutable std::vector<std::string> events;
    std::vector<uint64_t> contextIds;
    std::function<void()> externalWake;
};

std::string idleFrame(const lsp::RpcRequest& request) {
    auto body = rfl::json::write<rfl::UnderlyingEnums>(request);
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string idleMethodFrame(std::string method, std::optional<lsp::ID_t> id = std::nullopt) {
    return idleFrame({.jsonrpc = "2.0", .id = std::move(id), .method = std::move(method)});
}

std::string idleEditFrame(std::string uri, int version, lsp::uint offset, std::string append) {
    lsp::DidChangeTextDocumentParams params{
        .textDocument = {.version = version, .uri = URI(std::move(uri))},
        .contentChanges = {lsp::TextDocumentContentChangePartial{
            .range = {{0, offset}, {0, offset}}, .text = std::move(append)}}};
    return idleFrame({.jsonrpc = "2.0",
                      .method = "textDocument/didChange",
                      .params = rfl::to_generic<rfl::UnderlyingEnums>(params)});
}

std::string idleSnapshotFrame(std::string uri, int id) {
    return idleFrame({.jsonrpc = "2.0",
                      .id = id,
                      .method = "test/snapshot",
                      .params = rfl::to_generic(IdleSnapshotParams{std::move(uri)})});
}

// Restores global streams only after both the reader and its worker have joined.
// Gates are released and input is closed even when REQUIRE aborts a test section.
class IdleWireHarness {
public:
    explicit IdleWireHarness(bool gateBackground, std::chrono::milliseconds delay = 2ms) :
        server(gateBackground, delay) {
        originalInputState = std::cin.rdstate();
        originalInput = std::cin.rdbuf(&input);
        originalOutput = std::cout.rdbuf(&output);
        originalTie = std::cin.tie(nullptr);
        std::cin.clear();
        reader = std::thread([this] {
            try {
                server.run();
            }
            catch (...) {
                readerFailure = std::current_exception();
            }
        });
        send(idleMethodFrame("initialize", lsp::ID_t{1}));
    }

    ~IdleWireHarness() {
        finish();
        std::cin.rdbuf(originalInput);
        std::cin.clear(originalInputState);
        std::cin.tie(originalTie);
        std::cout.rdbuf(originalOutput);
    }

    size_t send(std::string_view frames) { return input.append(frames); }
    bool readThrough(size_t target) { return input.waitForReadBoundary(target); }
    std::string wireOutput() { return output.str(); }

    void finish() {
        server.releaseAll();
        input.close();
        if (reader.joinable())
            reader.join();
    }

    bool readerSucceeded() const { return !readerFailure; } // Inspect after finish().
    IdleQueueTestServer server;

private:
    IdleBlockingInput input;
    IdleCapturedOutput output;
    std::streambuf* originalInput = nullptr;
    std::streambuf* originalOutput = nullptr;
    std::ostream* originalTie = nullptr;
    std::ios::iostate originalInputState{};
    std::thread reader;
    std::exception_ptr readerFailure;
};

constexpr const char* idleA = "file:///idle-a.sv";
constexpr const char* idleB = "file:///idle-b.sv";

} // namespace

TEST_CASE("JSON-RPC queued edit and query snapshots precede idle diagnostics", "[json-rpc][idle]") {
    IdleWireHarness wire(false);
    REQUIRE(wire.server.waitForEvent("initialize"));
    wire.send(idleMethodFrame("test/hold", lsp::ID_t{2}));
    REQUIRE(wire.server.waitForEvent("hold"));

    auto boundary = wire.send(idleEditFrame(idleA, 1, 0, "x") + idleSnapshotFrame(idleA, 3) +
                              idleEditFrame(idleB, 1, 0, "z") + idleEditFrame(idleA, 2, 1, "y") +
                              idleSnapshotFrame(idleA, 4));
    REQUIRE(wire.readThrough(boundary));
    wire.server.releaseForeground();
    REQUIRE(wire.server.waitForEvent("idle:finish:1"));
    wire.finish();
    REQUIRE(wire.readerSucceeded());

    const std::vector<std::string> expected{
        "initialize",          "hold",
        "hold:released",       "edit:idle-a.sv@1:x",
        "query:idle-a.sv@1:x", "edit:idle-b.sv@1:z",
        "edit:idle-a.sv@2:xy", "query:idle-a.sv@2:xy",
        "idle:start:1",        "publish:idle-a.sv@2:xy|idle-b.sv@1:z",
        "idle:finish:1"};
    CHECK(wire.server.trace() == expected);
    CHECK(wire.wireOutput().find("\"result\":\"idle-a.sv@1:x\"") != std::string::npos);
    CHECK(wire.wireOutput().find("\"result\":\"idle-a.sv@2:xy\"") != std::string::npos);
}

TEST_CASE("New input wakes a maintenance retry wait before its deadline",
          "[json-rpc][idle][maintenance-retry]") {
    IdleWireHarness wire(false, 1h);
    REQUIRE(wire.server.waitForEvent("initialize"));
    wire.send(idleEditFrame(idleA, 1, 0, "x"));
    REQUIRE(wire.server.waitForEvent("idle:waiting"));
    wire.send(idleEditFrame(idleA, 2, 1, "y") + idleSnapshotFrame(idleA, 3));
    REQUIRE(wire.server.waitForEvent("query:idle-a.sv@2:xy"));
    wire.finish();
    REQUIRE(wire.readerSucceeded());
    CHECK(wire.server.backgroundContexts().empty());
    CHECK(wire.wireOutput().find("\"result\":\"idle-a.sv@2:xy\"") != std::string::npos);
}

TEST_CASE("A maintenance retry deadline runs without another input or wake",
          "[json-rpc][idle][maintenance-retry]") {
    IdleWireHarness wire(false, 20ms);
    REQUIRE(wire.server.waitForEvent("initialize"));
    wire.send(idleEditFrame(idleA, 1, 0, "x"));
    REQUIRE(wire.server.waitForEvent("idle:waiting"));
    REQUIRE(wire.server.waitForEvent("idle:finish:1"));
    wire.finish();
    REQUIRE(wire.readerSucceeded());
    CHECK(wire.server.backgroundContexts().size() == 1);
    const auto events = wire.server.trace();
    CHECK(std::ranges::count(events, "publish:idle-a.sv@1:x") == 1);
}

TEST_CASE("An asynchronous result interrupts a maintenance retry wait",
          "[json-rpc][idle][maintenance-retry]") {
    IdleWireHarness wire(false, 1h);
    REQUIRE(wire.server.waitForEvent("initialize"));
    wire.send(idleMethodFrame("test/captureWake", lsp::ID_t{2}));
    REQUIRE(wire.server.waitForEvent("wake:captured"));
    auto wake = wire.server.copiedWake();
    REQUIRE(bool(wake));
    wire.send(idleEditFrame(idleA, 1, 0, "x"));
    REQUIRE(wire.server.waitForEvent("idle:waiting"));
    wake();
    REQUIRE(wire.server.waitForEvent("idle:finish:1"));
    wire.finish();
    REQUIRE(wire.readerSucceeded());
    CHECK(wire.server.backgroundContexts().size() == 1);
}

TEST_CASE("JSON-RPC new query interrupts idle diagnostics before a fresh retry",
          "[json-rpc][idle]") {
    IdleWireHarness wire(true);
    REQUIRE(wire.server.waitForEvent("initialize"));
    wire.send(idleEditFrame(idleA, 1, 0, "x"));
    REQUIRE(wire.server.waitForEvent("idle:start:1"));

    auto boundary = wire.send(idleSnapshotFrame(idleA, 2));
    REQUIRE(wire.readThrough(boundary));
    wire.server.releaseBackground(1);
    REQUIRE(wire.server.waitForEvent("idle:start:2"));
    wire.server.releaseBackground(2);
    REQUIRE(wire.server.waitForEvent("idle:finish:2"));
    wire.finish();
    REQUIRE(wire.readerSucceeded());

    const std::vector<std::string> expected{
        "initialize",          "edit:idle-a.sv@1:x", "idle:start:1",          "idle:cancel:1",
        "query:idle-a.sv@1:x", "idle:start:2",       "publish:idle-a.sv@1:x", "idle:finish:2"};
    CHECK(wire.server.trace() == expected);
    auto contexts = wire.server.backgroundContexts();
    REQUIRE(contexts.size() == 2);
    CHECK(contexts[0] != contexts[1]);
}

TEST_CASE("JSON-RPC cancellation-only input retries pending idle diagnostics", "[json-rpc][idle]") {
    IdleWireHarness wire(true);
    REQUIRE(wire.server.waitForEvent("initialize"));
    wire.send(idleEditFrame(idleA, 1, 0, "x"));
    REQUIRE(wire.server.waitForEvent("idle:start:1"));

    auto boundary = wire.send(idleFrame({.jsonrpc = "2.0",
                                         .method = "$/cancelRequest",
                                         .params = rfl::to_generic(IdleCancelParams{.id = 404})}));
    REQUIRE(wire.readThrough(boundary));
    wire.server.releaseBackground(1);
    REQUIRE(wire.server.waitForEvent("idle:start:2"));
    wire.server.releaseBackground(2);
    REQUIRE(wire.server.waitForEvent("idle:finish:2"));
    wire.finish();
    REQUIRE(wire.readerSucceeded());

    const std::vector<std::string> expected{
        "initialize",    "edit:idle-a.sv@1:x", "idle:start:1",          "cancel-notification",
        "idle:cancel:1", "idle:start:2",       "publish:idle-a.sv@1:x", "idle:finish:2"};
    CHECK(wire.server.trace() == expected);
    auto contexts = wire.server.backgroundContexts();
    REQUIRE(contexts.size() == 2);
    CHECK(contexts[0] != contexts[1]);
}

TEST_CASE("JSON-RPC shutdown cancels idle work and prevents its restart", "[json-rpc][idle]") {
    IdleWireHarness wire(true);
    REQUIRE(wire.server.waitForEvent("initialize"));
    wire.send(idleEditFrame(idleA, 1, 0, "x"));
    REQUIRE(wire.server.waitForEvent("idle:start:1"));

    // Keep the shutdown reader alive until the held worker has processed shutdown.
    // Sending exit here too would allow EOF cancellation to mask the shutdown case.
    auto boundary = wire.send(idleMethodFrame("shutdown", lsp::ID_t{2}));
    REQUIRE(wire.readThrough(boundary));
    wire.server.releaseBackground(1);
    // hasBackgroundWork is queried after the foreground response was written.
    // Observe its explicit false result before exit can terminate the worker.
    REQUIRE(wire.server.waitForEvent("shutdown:idle-disabled"));
    wire.send(idleMethodFrame("exit"));
    wire.finish();
    REQUIRE(wire.readerSucceeded());

    const std::vector<std::string> expected{"initialize",   "edit:idle-a.sv@1:x",
                                            "idle:start:1", "idle:cancel:1",
                                            "shutdown",     "shutdown:idle-disabled"};
    CHECK(wire.server.trace() == expected);
    CHECK(wire.wireOutput().find("\"id\":2,\"result\":null") != std::string::npos);
}

TEST_CASE("JSON-RPC asynchronous completion wake survives an active idle pass",
          "[json-rpc][idle]") {
    std::function<void()> expiredWake;
    {
        IdleWireHarness wire(true);
        REQUIRE(wire.server.waitForEvent("initialize"));
        wire.send(idleMethodFrame("test/captureWake", lsp::ID_t{2}));
        REQUIRE(wire.server.waitForEvent("wake:captured"));
        auto wake = wire.server.copiedWake();
        REQUIRE(bool(wake));
        // No foreground message or preexisting pending work triggers either pass.
        wake();
        REQUIRE(wire.server.waitForEvent("idle:start:1"));
        wake(); // A second completion races the first active idle callback.
        wire.server.releaseBackground(1);
        REQUIRE(wire.server.waitForEvent("idle:start:2"));
        wire.server.releaseBackground(2);
        REQUIRE(wire.server.waitForEvent("idle:finish:2"));
        expiredWake = wake;
        wire.finish();
        REQUIRE(wire.readerSucceeded());
        CHECK(wire.server.backgroundContexts().size() == 2);
    }
    // Helper teardown may race loop shutdown. The retained callback is inert.
    expiredWake();
}

TEST_CASE("New document edits cancel only superseded position queries when enabled",
          "[query-scheduling]") {
    for (bool enabled : {false, true}) {
        for (bool started : {false, true}) {
            for (const auto* method : {"textDocument/completion", "textDocument/hover",
                                       "textDocument/definition", "textDocument/signatureHelp"}) {
                CAPTURE(enabled, started, method);
                TestJsonRpcServer server;
                server.cancelQueriesOnEdit(enabled);
                auto query = [&](int id, const char* uri) {
                    return lsp::RpcRequest{
                        .jsonrpc = "2.0",
                        .id = id,
                        .method = method,
                        .params = rfl::to_generic(lsp::TextDocumentPositionParams{
                            .textDocument = {.uri = URI(uri)}, .position = {0, 0}})};
                };
                auto previous = query(20, "file:///tmp/edited.sv");
                auto other = query(21, "file:///tmp/other.sv");
                auto oldContext = server.track(previous);
                auto otherContext = server.track(other);
                if (started)
                    server.start(oldContext);
                lsp::RpcRequest change{
                    .jsonrpc = "2.0",
                    .method = "textDocument/didChange",
                    .params = rfl::to_generic<rfl::UnderlyingEnums>(
                        lsp::DidChangeTextDocumentParams{
                            .textDocument = {.version = 2, .uri = URI("file:///tmp/edited.sv")},
                            .contentChanges = {
                                lsp::TextDocumentContentChangeWholeDocument{.text = "new"}}})};
                auto editContext = server.track(change);
                CHECK(oldContext.isCancelled() == enabled);
                CHECK(oldContext.isContentModified() == enabled);
                CHECK_FALSE(otherContext.isCancelled());
                auto latest = query(22, "file:///tmp/edited.sv");
                auto latestContext = server.track(latest);
                CHECK_FALSE(latestContext.isCancelled());
                auto result = server.processMessage(previous, oldContext);
                if (enabled) {
                    REQUIRE(std::holds_alternative<lsp::RpcError>(result));
                    CHECK(std::get<lsp::RpcError>(result).code ==
                          static_cast<int>(lsp::LSPErrorCodes::ContentModified));
                    CHECK(server.contextRequestCalls == 0);
                }
                else
                    CHECK(std::holds_alternative<rfl::Generic>(result));
                CHECK(std::holds_alternative<rfl::Generic>(
                    server.processMessage(latest, latestContext)));
                server.untrack(previous, oldContext);
                server.untrack(other, otherContext);
                server.untrack(latest, latestContext);
                server.untrack(change, editContext);
                CHECK(server.documentQueryCount() == 0);
            }
        }
    }
}
