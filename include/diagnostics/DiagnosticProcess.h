#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace isolated_diagnostics {

struct Job {
    uint64_t generation = 0;
    uint64_t jobId = 0;
    std::string input;
    // Optional encoder runs on the I/O thread; captures must own immutable DTOs.
    // If set, replaces input. Cancellation is checked before and after encoding.
    // Keep encoding bounded: shutdown join must wait for an executing encoder.
    std::function<std::string()> encode;
};

struct Result {
    uint64_t generation = 0;
    uint64_t jobId = 0;
    std::string output;
    // Empty on success. Cancelled or replaced jobs produce no result.
    std::string error;
};

struct Limits {
    uint64_t maxInputBytes = 64 * 1024 * 1024;
    uint64_t maxOutputBytes = 64 * 1024 * 1024;
    std::chrono::milliseconds timeout{120000};
};

// Shared Linux/macOS transport; unsupported platforms throw an explicit error.
// Owns exactly one child at a time. No automatic retries; the caller decides
// whether/when to resubmit failures. The executable must be an absolute path
// to this server, not a command line. argv is fixed to --diagnostic-worker.
class DiagnosticProcess {
public:
    // Wake runs on the I/O thread, outside internal locks. It must be short,
    // noexcept in effect, and must not destroy/join this object. It may submit
    // or takeResult. It only schedules main-thread processing of the result.
    DiagnosticProcess(std::string executable, Limits limits, std::function<void()> wake);
    ~DiagnosticProcess();
    DiagnosticProcess(const DiagnosticProcess&) = delete;
    DiagnosticProcess& operator=(const DiagnosticProcess&) = delete;

    // Thread safe. Returns false after stop. Jobs are immutable after transfer.
    // Replaces pending work, discards queued results, and cancels the old child.
    bool submit(Job job);
    void cancel();
    void requestStop(); // Nonblocking; actual kill/reap occurs on I/O thread.
    void join();        // Shutdown path only; never call from wake callback.
    std::optional<Result> takeResult();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Call once at process entry, before threads or concurrent fork calls. macOS
// registers atfork synchronization for non-atomic close-on-exec socket creation.
void initializeDiagnosticProcessPlatform();

// Linux returns an inode-pinned /proc path. macOS returns a resolved path and
// authenticates the live child against the signed running image before sending
// a job; executable replacement or unsigned images fail explicitly.
std::string currentDiagnosticExecutable();

// Worker lifetime is bound to the exact current parent. On macOS a kqueue
// watcher exits the entire worker when its parent exits; registration failures
// fail closed. Construct before potentially expensive snapshot/semantic work.
class DiagnosticParentGuard {
public:
    explicit DiagnosticParentGuard(int64_t owner);
    ~DiagnosticParentGuard();
    DiagnosticParentGuard(const DiagnosticParentGuard&) = delete;
    DiagnosticParentGuard& operator=(const DiagnosticParentGuard&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Worker-mode helpers. Call before starting the normal server or its threads.
// Transport is one 8-byte unsigned big-endian length followed by exact bytes.
// Only one request and one response are allowed. stdout is protocol-only;
// logs go to stderr. Helpers throw std::runtime_error on failure/oversize.
std::string readDiagnosticRequest(uint64_t maxBytes = 64 * 1024 * 1024);
void writeDiagnosticResponse(const std::string& bytes, uint64_t maxBytes = 64 * 1024 * 1024);

} // namespace isolated_diagnostics
