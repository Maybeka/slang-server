//------------------------------------------------------------------------------
// JsonRpc.h
// JSON-RPC 2.0 protocol types and message handling utilities
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#pragma once

#include "JsonTypes.h"
#include "rfl/Generic.hpp"
#include <algorithm>
#include <charconv>
#include <iostream>
#include <optional>
#include <rfl/json.hpp> // IWYU pragma: keep
#include <string>
#include <string_view>
#include <variant>

namespace lsp {

using ID_t = std::variant<int, std::string>;
using Params_t = std::optional<rfl::Generic>;

struct RpcRequest {
    /// includes notifications
    std::string jsonrpc;
    std::optional<ID_t> id;
    std::string method;
    Params_t params;
};

struct RpcNotification {
    std::string jsonrpc;
    std::string method;
    Params_t params;
};

struct RpcResponse {
    std::string jsonrpc;
    std::optional<ID_t> id;
    Params_t result;
};

struct RpcError {
    /// A number indicating the error type that occurred.
    int32_t code;
    /// A string providing a short description of the error.
    std::string message;
};
struct RpcErrorResponse {
    std::string jsonrpc;
    std::optional<ID_t> id;
    RpcError error;
};

template<typename T>
void sendMessage(const T& message) {
    auto message_str = rfl::json::write<rfl::UnderlyingEnums>(message);
    std::cout << "Content-Length: " << message_str.length() << "\r\n\r\n";
    std::cout << message_str;
    std::cout.flush();
}

namespace detail {
// The envelope exists only during synchronous serialization. Reflection writes
// the pointed-to parameters directly; no parameter tree or owner is retained.
template<typename Params>
struct RpcNotificationView {
    std::string_view jsonrpc;
    std::string_view method;
    const Params* params;
};
} // namespace detail

template<typename Params>
void sendTypedNotification(std::string_view method, const Params& params) {
    sendMessage(detail::RpcNotificationView<Params>{
        .jsonrpc = "2.0",
        .method = method,
        .params = &params,
    });
    std::cerr << "---> " << method;
    // std::cerr << ": " << rfl::json::write(params) << std::endl;
    std::cerr << std::endl;
}

inline void sendNotification(const std::string& method, const rfl::Generic& params) {
    sendTypedNotification(method, params);
}

inline void sendRequest(const std::string& method, const rfl::Generic& params) {
    sendMessage(RpcRequest{
        .jsonrpc = "2.0",
        .id = 0,
        .method = method,
        .params = params,
    });
    std::cerr << "---> " << method;
    // std::cerr << ": " << rfl::json::write(params) << std::endl;
    std::cerr << std::endl;
}

template<typename T>
std::optional<T> readJson(std::string& line, std::string& content) {
    while (std::getline(std::cin, line)) {
        if (!line.starts_with("Content-Length: ")) {
            std::cerr << "<-/- " << "Invalid Line: " << line << std::endl;
            continue;
        }

        auto field = std::string_view(line).substr(16);
        if (field.ends_with('\r'))
            field.remove_suffix(1);
        size_t remaining = 0;
        auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), remaining);
        if (error != std::errc{} || end != field.data() + field.size() || remaining == 0 ||
            remaining > content.max_size())
            continue;

        while (std::getline(std::cin, line) && !line.empty() && line != "\r") {
        }
        if (!std::cin)
            return std::nullopt;

        // Grow with bytes actually received, including when a client closes mid-frame.
        content.clear();
        while (remaining) {
            size_t count = std::min<size_t>(remaining, 16384);
            auto offset = content.size();
            content.resize(offset + count);
            if (!std::cin.read(content.data() + offset, count))
                return std::nullopt;
            remaining -= count;
        }

        rfl::Result<T> request = rfl::json::read<T>(content);
        if (!request) {
            auto response = rfl::json::read<RpcResponse>(content);
            if (response) {
                continue;
            }
            std::cerr << "Error parsing JSON: " << content << std::endl;
            std::cerr << "Rfl Error: " << request.error().what() << std::endl;
            sendMessage(
                RpcErrorResponse{.jsonrpc = "2.0",
                                 .id = 0,
                                 .error = RpcError{
                                     .code = 1,
                                     .message = "Error parsing JSON: " + request.error().what() +
                                                " for content: " + content,
                                 }});
            continue;
        }

        return std::move(request.value());
    }
    return std::nullopt;
}

} // namespace lsp
