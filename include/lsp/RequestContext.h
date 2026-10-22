//------------------------------------------------------------------------------
// RequestContext.h
// Per-request logging context for JSON-RPC messages.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "JsonRpc.h"
#include "util/Log.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace lsp {

class RequestCancelled : public std::runtime_error {
public:
    explicit RequestCancelled(std::string_view checkpoint) :
        std::runtime_error(std::string(checkpoint)) {}
};

class RequestFailed : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RequestContext {
    struct State;

public:
    /// Protocol-worker-only lifetime for evidence obtained during this dispatch.
    /// Cancellation still uses its existing independent atomic state.
    class InputWitnessScope {
    public:
        explicit InputWitnessScope(std::shared_ptr<State> state) noexcept;
        ~InputWitnessScope();
        InputWitnessScope(const InputWitnessScope&) = delete;
        InputWitnessScope& operator=(const InputWitnessScope&) = delete;

    private:
        std::shared_ptr<State> m_state;
    };
    InputWitnessScope inputWitnessScope() const { return InputWitnessScope(m_state); }
    void recordInputWitness(const std::shared_ptr<const void>& owner, uint64_t revision) const {
        if (owner && m_state && m_rpcId && m_state->inputWitnessActive) {
            m_state->inputWitnessOwner = owner;
            m_state->inputWitnessRevision = revision;
        }
    }
    bool hasInputWitness(const std::shared_ptr<const void>& owner, uint64_t revision) const {
        return owner && m_state && m_rpcId && m_state->inputWitnessActive &&
               m_state->inputWitnessRevision == revision &&
               m_state->inputWitnessOwner.lock() == owner;
    }
    RequestContext() = default;

    RequestContext(std::string_view method, std::optional<ID_t> rpcId,
                   bool supportsCancellation = true) :
        m_id(s_nextId.fetch_add(1, std::memory_order_relaxed)), m_method(method),
        m_rpcId(std::move(rpcId)), m_logId(makeLogId(m_id, m_rpcId)),
        m_timestamp(std::chrono::system_clock::now()), m_start(std::chrono::steady_clock::now()),
        m_supportsCancellation(supportsCancellation),
        m_state((m_rpcId || supportsCancellation) ? std::make_shared<State>() : nullptr) {}

    explicit operator bool() const { return m_id != 0; }

    uint64_t id() const { return m_id; }
    std::string_view method() const { return m_method; }
    const std::optional<ID_t>& rpcId() const { return m_rpcId; }
    bool supportsCancellation() const { return m_supportsCancellation; }

    void cancel() const {
        if (m_state)
            m_state->cancelled.store(true, std::memory_order_relaxed);
    }

    void cancelForContentChange() const {
        if (m_state) {
            m_state->contentModified.store(true, std::memory_order_relaxed);
            m_state->cancelled.store(true, std::memory_order_release);
        }
    }
    bool isContentModified() const {
        return m_state && m_state->contentModified.load(std::memory_order_acquire);
    }

    bool isCancelled() const {
        return m_state && m_state->cancelled.load(std::memory_order_acquire);
    }

    void markStarted() const {
        if (m_state)
            m_state->started.store(true, std::memory_order_relaxed);
    }

    bool hasStarted() const { return m_state && m_state->started.load(std::memory_order_relaxed); }

    void throwIfCancelled(std::string_view checkpoint) const {
        if (isCancelled())
            throw RequestCancelled(checkpoint);
    }

    // Explicit safe boundaries only: never interrupt a parser, binder, or visitor in flight.
    void checkpoint(std::string_view name) const {
        throwIfCancelled(name);
        if (m_checkpointObserver)
            (*m_checkpointObserver)(name);
        throwIfCancelled(name);
    }

    // Install before dispatch. Only deterministic tests use this per-request observer;
    // normal requests carry no observer and no global test state is involved.
    void setCheckpointObserverForTesting(std::function<void(std::string_view)> observer) {
        m_checkpointObserver = std::make_shared<const std::function<void(std::string_view)>>(
            std::move(observer));
    }

    std::string elapsedContext() const {
        const auto elapsed = std::chrono::round<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - m_start)
                                 .count();
        if (elapsed < 1000)
            return fmt::format("[{}        +.{:03}]", m_logId, elapsed);
        return fmt::format("[{} {:+12.3f}]", m_logId, elapsed / 1000.0);
    }

    std::string timestamp() const {
        const auto seconds = std::chrono::floor<std::chrono::seconds>(m_timestamp);
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            m_timestamp - seconds);
        const auto time = std::chrono::system_clock::to_time_t(seconds);
        std::tm localTime{};
#ifdef _WIN32
        localtime_s(&localTime, &time);
#else
        localtime_r(&time, &localTime);
#endif
        return fmt::format("{:%H:%M:%S}.{:03}", localTime, milliseconds.count());
    }

    template<typename... Args>
    void startInfo(fmt::format_string<Args...> format, Args&&... args) const {
        if (*this) {
            server::logging::infoWithContext(fmt::format("[{} {}]", m_logId, timestamp()), format,
                                             std::forward<Args>(args)...);
        }
        else {
            server::logging::info(format, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    void info(fmt::format_string<Args...> format, Args&&... args) const {
        if (*this) {
            server::logging::infoWithContext(elapsedContext(), format, std::forward<Args>(args)...);
        }
        else {
            server::logging::info(format, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    void error(fmt::format_string<Args...> format, Args&&... args) const {
        if (*this) {
            server::logging::errorWithContext(elapsedContext(), format,
                                              std::forward<Args>(args)...);
        }
        else {
            server::logging::error(format, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    void warn(fmt::format_string<Args...> format, Args&&... args) const {
        if (*this) {
            server::logging::warnWithContext(elapsedContext(), format, std::forward<Args>(args)...);
        }
        else {
            server::logging::warn(format, std::forward<Args>(args)...);
        }
    }

private:
    struct State {
        std::atomic_bool cancelled = false;
        std::atomic_bool started = false;
        std::atomic_bool contentModified = false;
        // Accessed only on the protocol worker, never by the cancellation reader.
        bool inputWitnessActive = false;
        std::weak_ptr<const void> inputWitnessOwner;
        uint64_t inputWitnessRevision = 0;
    };

    static std::string makeLogId(uint64_t contextId, const std::optional<ID_t>& rpcId) {
        if (rpcId) {
            return std::visit([](const auto& id) { return fmt::format("#{}", id); }, *rpcId);
        }
        return fmt::format("n{}", contextId);
    }

    inline static std::atomic<uint64_t> s_nextId = 1;

    uint64_t m_id = 0;
    std::string m_method;
    std::optional<ID_t> m_rpcId;
    std::string m_logId;
    std::chrono::system_clock::time_point m_timestamp;
    std::chrono::steady_clock::time_point m_start;
    bool m_supportsCancellation = false;
    std::shared_ptr<State> m_state;
    std::shared_ptr<const std::function<void(std::string_view)>> m_checkpointObserver;
};

inline RequestContext::InputWitnessScope::InputWitnessScope(std::shared_ptr<State> state) noexcept :
    m_state(std::move(state)) {
    if (m_state) {
        m_state->inputWitnessActive = true;
        m_state->inputWitnessOwner.reset();
    }
}
inline RequestContext::InputWitnessScope::~InputWitnessScope() {
    if (m_state) {
        m_state->inputWitnessActive = false;
        m_state->inputWitnessOwner.reset();
    }
}
} // namespace lsp
