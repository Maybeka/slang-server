#include "diagnostics/DiagnosticProcess.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;
using namespace std::chrono_literals;
using isolated_diagnostics::DiagnosticProcess;
using isolated_diagnostics::Job;
using isolated_diagnostics::Limits;
using isolated_diagnostics::Result;
using Clock = std::chrono::steady_clock;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::string systemError(const char* label) {
    return std::string(label) + ": " + std::strerror(errno);
}
struct Descriptor {
    int value = -1;
    explicit Descriptor(int fd = -1) : value(fd) {}
    ~Descriptor() {
        if (value >= 0)
            ::close(value);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};
std::array<char, 8> frameHeader(uint64_t length) {
    std::array<char, 8> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[7 - i] = static_cast<char>(length >> (8 * i));
    return bytes;
}
void writeAll(int fd, const char* bytes, size_t size) {
    while (size) {
        ssize_t count = ::write(fd, bytes, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error(systemError("write"));
        bytes += count;
        size -= static_cast<size_t>(count);
    }
}
void rawFrame(uint64_t declared, const std::string& bytes) {
    auto prefix = frameHeader(declared);
    writeAll(STDOUT_FILENO, prefix.data(), prefix.size());
    writeAll(STDOUT_FILENO, bytes.data(), bytes.size());
}
void publishPid(const std::string& path) {
    Descriptor fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600));
    require(fd.value >= 0, systemError("open worker marker"));
    auto text = std::to_string(::getpid()) + "\n";
    writeAll(fd.value, text.data(), text.size());
}
int worker() {
    const std::string input = isolated_diagnostics::readDiagnosticRequest();
    auto separator = input.find('\n');
    const auto mode = input.substr(0, separator);
    const auto payload = separator == std::string::npos ? std::string()
                                                        : input.substr(separator + 1);
    if (mode == "echo") {
        isolated_diagnostics::writeDiagnosticResponse(payload);
    }
    else if (mode == "fragmented") {
        auto prefix = frameHeader(payload.size());
        for (char byte : prefix) {
            writeAll(STDOUT_FILENO, &byte, 1);
            std::this_thread::sleep_for(1ms);
        }
        for (size_t i = 0; i < payload.size(); i += 7)
            writeAll(STDOUT_FILENO, payload.data() + i, std::min<size_t>(7, payload.size() - i));
    }
    else if (mode == "oversize") {
        rawFrame(std::numeric_limits<uint64_t>::max(), "");
    }
    else if (mode == "truncated-header") {
        auto prefix = frameHeader(5);
        writeAll(STDOUT_FILENO, prefix.data(), 4);
    }
    else if (mode == "truncated-body") {
        rawFrame(8, "short");
    }
    else if (mode == "extra") {
        rawFrame(2, "abc");
    }
    else if (mode == "double-frame") {
        isolated_diagnostics::writeDiagnosticResponse("first");
        isolated_diagnostics::writeDiagnosticResponse("second");
    }
    else if (mode == "crash") {
        ::raise(SIGKILL); // No core file, no dependency on a shell or another executable.
        return 91;
    }
    else if (mode == "hold") {
        publishPid(payload);
        for (;;)
            ::pause();
    }
    else if (mode == "fd-check") {
        const int fd = std::stoi(payload);
        errno = 0;
        const int flags = ::fcntl(fd, F_GETFD);
        isolated_diagnostics::writeDiagnosticResponse(flags == -1 && errno == EBADF ? "closed"
                                                                                    : "leaked");
    }
    else if (mode == "helper-output-limit") {
        try {
            isolated_diagnostics::writeDiagnosticResponse("12345", 4);
            return 92;
        }
        catch (const std::runtime_error&) {
            isolated_diagnostics::writeDiagnosticResponse("rejected");
        }
    }
    else {
        throw std::runtime_error("Unknown test worker mode: " + mode);
    }
    return 0;
}

struct TemporaryDirectory {
    std::string path;
    std::vector<std::string> files;
    explicit TemporaryDirectory(const std::string& root) {
        std::string pattern = root + "/diagnostic-transport-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char* made = ::mkdtemp(buffer.data());
        require(made != nullptr, systemError("mkdtemp"));
        path = made;
    }
    std::string file(const std::string& name) {
        files.push_back(path + "/" + name);
        return files.back();
    }
    ~TemporaryDirectory() {
        for (const auto& file : files)
            ::unlink(file.c_str());
        ::rmdir(path.c_str());
    }
};
void copyExecutable(const std::string& from, const std::string& to) {
    Descriptor input(::open(from.c_str(), O_RDONLY));
    require(input.value >= 0, systemError("open fixture source"));
    Descriptor output(::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0700));
    require(output.value >= 0, systemError("open fixture copy"));
    std::array<char, 65536> buffer;
    for (;;) {
        ssize_t count = ::read(input.value, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        require(count >= 0, systemError("read fixture"));
        if (!count)
            break;
        writeAll(output.value, buffer.data(), static_cast<size_t>(count));
    }
    require(::fchmod(output.value, 0700) == 0, systemError("chmod fixture"));
}
#ifdef __APPLE__
struct MarkerEnvironment {
    const std::string name = "DIAGNOSTIC_TRANSPORT_IDENTITY_MARKER";
    std::optional<std::string> previous;
    explicit MarkerEnvironment(const std::string& path) {
        if (const char* old = ::getenv(name.c_str()))
            previous = old;
        require(::setenv(name.c_str(), path.c_str(), 1) == 0,
                systemError("set marker environment"));
    }
    ~MarkerEnvironment() {
        if (previous)
            ::setenv(name.c_str(), previous->c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
};
void expectNoSnapshot(const std::string& marker) {
    std::ifstream input(marker);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    require(contents.find("INPUT") == std::string::npos,
            "Unauthenticated replacement executable received request bytes");
}
#endif
Limits limits(std::chrono::milliseconds timeout = 5000ms) {
    Limits config;
    config.timeout = timeout;
    config.maxInputBytes = 4 * 1024 * 1024;
    config.maxOutputBytes = 4 * 1024 * 1024;
    return config;
}
struct Session {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<unsigned> wakeCount{0};
    DiagnosticProcess process;
    Session(const std::string& executable, Limits config = limits()) :
        process(executable, config, [this] {
            ++wakeCount;
            changed.notify_all();
        }) {}
    // process is declared last, so its real destructor joins before callback state dies.
    Result await(std::chrono::milliseconds timeout = 7000ms) {
        auto deadline = Clock::now() + timeout;
        for (;;) {
            if (auto result = process.takeResult())
                return std::move(*result);
            require(Clock::now() < deadline, "No diagnostic result before test deadline");
            std::unique_lock lock(mutex);
            changed.wait_for(lock, 5ms);
        }
    }
    void submit(std::string input, uint64_t generation = 17, uint64_t id = 31) {
        require(process.submit(Job{generation, id, std::move(input), {}}),
                "submit unexpectedly rejected");
    }
};
void expectSuccess(const Result& result, const std::string& output, uint64_t generation = 17,
                   uint64_t id = 31) {
    require(result.generation == generation && result.jobId == id, "Result identity mismatch");
    require(result.error.empty(), "Unexpected transport error: " + result.error);
    require(result.output == output, "Response payload mismatch");
}
void expectError(const Result& result, const std::string& substring) {
    require(result.generation == 17 && result.jobId == 31, "Failure result identity mismatch");
    require(!result.error.empty(), "Malformed worker unexpectedly succeeded");
    require(result.error.find(substring) != std::string::npos,
            "Expected error containing '" + substring + "'; got '" + result.error + "'");
    require(result.output.empty(), "Failed transport published partial payload");
}
pid_t awaitPid(const std::string& path) {
    auto deadline = Clock::now() + 5000ms;
    do {
        std::ifstream input(path);
        long pid = 0;
        if (input >> pid) {
            require(pid > 1, "Invalid worker PID marker");
            require(::kill(static_cast<pid_t>(pid), 0) == 0, "Held worker is not alive");
            return static_cast<pid_t>(pid);
        }
        std::this_thread::sleep_for(5ms);
    } while (Clock::now() < deadline);
    throw std::runtime_error("Worker readiness marker not published");
}
void expectReaped(pid_t pid) {
    auto deadline = Clock::now() + 2000ms;
    do {
        errno = 0;
        if (::kill(pid, 0) == -1 && errno == ESRCH) {
            int status = 0;
            errno = 0;
            require(::waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD,
                    "Worker disappeared but was not reaped by DiagnosticProcess");
            return;
        }
        std::this_thread::sleep_for(5ms);
    } while (Clock::now() < deadline);
    throw std::runtime_error("Worker remains live or a zombie after transport cleanup");
}
struct GateState {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};
struct EncoderGate {
    std::shared_ptr<GateState> state = std::make_shared<GateState>();
    ~EncoderGate() { release(); }
    void release() {
        std::lock_guard lock(state->mutex);
        state->released = true;
        state->changed.notify_all();
    }
    void awaitEntered() {
        std::unique_lock lock(state->mutex);
        require(state->changed.wait_for(lock, 5000ms, [&] { return state->entered; }),
                "Encoder did not enter gate");
    }
    std::function<std::string()> encoder() {
        return [state = state] {
            std::unique_lock lock(state->mutex);
            state->entered = true;
            state->changed.notify_all();
            state->changed.wait(lock, [&] { return state->released; });
            return std::string("echo\nobsolete encoder");
        };
    }
};

int stdioCase(const std::string& executable) {
    ::alarm(12);
    Session session(executable);
    session.submit("echo\nclosed-standard-descriptors");
    expectSuccess(session.await(), "closed-standard-descriptors");
    session.process.join();
    return 0;
}
int helperCase(const std::string& mode) {
    ::alarm(12);
    if (mode == "wrong-parent") {
        for (int64_t owner : {int64_t(-1), int64_t(0), int64_t(1), int64_t(::getpid())}) {
            bool rejected = false;
            try {
                isolated_diagnostics::DiagnosticParentGuard guard(owner);
            }
            catch (const std::runtime_error&) {
                rejected = true;
            }
            require(rejected, "Parent guard accepted an invalid or non-parent PID");
        }
        return 0;
    }
    int sockets[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, systemError("socketpair"));
    Descriptor local(sockets[0]), peer(sockets[1]);
    if (mode == "broken-output") {
#ifdef __APPLE__
        // Production transport establishes this per-socket policy before spawn.
        // The raw helper fixture must supply the same worker socket contract.
        int one = 1;
        require(::setsockopt(local.value, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == 0,
                systemError("SO_NOSIGPIPE fixture"));
#endif
        require(::dup2(local.value, STDOUT_FILENO) == STDOUT_FILENO, systemError("dup2"));
        ::close(peer.value);
        peer.value = -1;
        try {
            isolated_diagnostics::writeDiagnosticResponse("must throw without SIGPIPE");
        }
        catch (const std::runtime_error&) {
            return 0;
        }
        throw std::runtime_error("Write to closed peer did not fail");
    }
    require(::dup2(local.value, STDIN_FILENO) == STDIN_FILENO, systemError("dup2"));
    auto prefix = frameHeader(mode == "oversize-request" ? 1024 : 5);
    const size_t prefixSize = mode == "short-header" ? 3 : prefix.size();
    writeAll(peer.value, prefix.data(), prefixSize);
    if (mode == "short-body")
        writeAll(peer.value, "ab", 2);
    ::shutdown(peer.value, SHUT_WR);
    try {
        (void)isolated_diagnostics::readDiagnosticRequest(32);
    }
    catch (const std::runtime_error&) {
        return 0;
    }
    throw std::runtime_error("Malformed request was accepted by worker helper");
}
ssize_t timedRead(int fd, char* bytes, size_t size, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                               Clock::now());
        require(remaining.count() > 0, "Worker lifetime observation timed out");
        pollfd item{fd, POLLIN, 0};
        int rc = ::poll(&item, 1, static_cast<int>(remaining.count()));
        if (rc < 0 && errno == EINTR)
            continue;
        require(rc > 0, "Worker lifetime observation timed out");
        ssize_t count = ::read(fd, bytes, size);
        if (count < 0 && errno == EINTR)
            continue;
        require(count >= 0, systemError("read worker lifetime"));
        return count;
    }
}
void readExact(int fd, char* bytes, size_t size) {
    while (size) {
        ssize_t count = timedRead(fd, bytes, size);
        require(count > 0, "Unexpected EOF while waiting for worker readiness");
        bytes += count;
        size -= static_cast<size_t>(count);
    }
}
pid_t spawnLifetimeHelper(const std::string& executable, std::vector<std::string> arguments,
                          const std::vector<int>& closeDescriptors, int stdinFd = -1) {
    posix_spawn_file_actions_t actions;
    require(::posix_spawn_file_actions_init(&actions) == 0, "lifetime spawn actions init failed");
    struct ActionCleanup {
        posix_spawn_file_actions_t* actions;
        ~ActionCleanup() { ::posix_spawn_file_actions_destroy(actions); }
    } cleanup{&actions};
    if (stdinFd >= 0)
        require(::posix_spawn_file_actions_adddup2(&actions, stdinFd, STDIN_FILENO) == 0,
                "lifetime stdin duplication action failed");
    for (int fd : closeDescriptors)
        require(::posix_spawn_file_actions_addclose(&actions, fd) == 0,
                "lifetime close action failed");
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t child = -1;
    const int rc = ::posix_spawn(&child, executable.c_str(), &actions, nullptr, argv.data(),
                                 environ);
    require(rc == 0, std::string("spawn lifetime helper: ") + std::strerror(rc));
    return child;
}
int parentWatchCase(const std::string& stage, int64_t ownerPid, int reporting) {
    ::alarm(12);
    const int64_t pid = ::getpid();
    writeAll(reporting, reinterpret_cast<const char*>(&pid), sizeof(pid));
    if (stage == "registered-busy") {
        isolated_diagnostics::DiagnosticParentGuard guard(ownerPid);
        writeAll(reporting, "R", 1);
        uint64_t value = 1;
        for (;;) {
            value = value * 1664525 + 1013904223;
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }
    writeAll(reporting, "R", 1);
    if (stage == "before-frame") {
        try {
            (void)isolated_diagnostics::readDiagnosticRequest();
        }
        catch (const std::runtime_error&) {
            writeAll(reporting, "X", 1);
            return 0;
        }
    }
    else if (stage == "before-registration") {
        char proceed = 0;
        readExact(reporting, &proceed, 1);
        require(proceed == 'G', "Unexpected guard registration command");
        try {
            isolated_diagnostics::DiagnosticParentGuard guard(ownerPid);
        }
        catch (const std::runtime_error&) {
            writeAll(reporting, "X", 1);
            return 0;
        }
    }
    writeAll(reporting, "U", 1); // Unexpected success after owner death.
    return 96;
}
int parentOwnerCase(const std::string& executable, const std::string& stage, int reporting,
                    int commands) {
    ::alarm(12);
    int request[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, request) == 0,
            systemError("owner request socketpair"));
    Descriptor parentRequest(request[0]), childRequest(request[1]);
    (void)spawnLifetimeHelper(
        executable,
        {"--parent-watch-case", stage, std::to_string(::getpid()), std::to_string(reporting)},
        {commands, parentRequest.value, childRequest.value}, childRequest.value);
    ::close(childRequest.value);
    childRequest.value = -1;
    ::close(reporting);
    char command;
    while (::read(commands, &command, 1) < 0 && errno == EINTR) {
    }
    ::_exit(0); // Abrupt owner exit, without normal worker shutdown.
}
void parentDeathCase(const std::string& executable, const std::string& stage) {
    // Every topology node starts through posix_spawn/exec. No Security APIs or
    // C++ thread construction run in a raw post-fork child, even on macOS.
    int observations[2], commands[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, observations) == 0,
            systemError("lifetime socketpair"));
    Descriptor observed(observations[0]), reporting(observations[1]);
    require(::pipe(commands) == 0, systemError("owner command pipe"));
    Descriptor commandRead(commands[0]), commandWrite(commands[1]);
    const pid_t owner = spawnLifetimeHelper(executable,
                                            {"--parent-owner-case", stage,
                                             std::to_string(reporting.value),
                                             std::to_string(commandRead.value)},
                                            {observed.value, commandWrite.value});
    ::close(reporting.value);
    reporting.value = -1;
    ::close(commandRead.value);
    commandRead.value = -1;
    struct FamilyCleanup {
        pid_t owner;
        pid_t worker = -1;
        bool eof = false;
        ~FamilyCleanup() {
            if (owner > 0) {
                ::kill(owner, SIGKILL);
                int status;
                while (::waitpid(owner, &status, 0) < 0 && errno == EINTR) {
                }
            }
            // Failure-only escape hatch for the known test child. EOF proves
            // exit and prevents signaling an already-reaped/reused worker PID.
            if (worker > 0 && !eof)
                ::kill(worker, SIGKILL);
        }
    } cleanup{owner};
    int64_t workerPid = -1;
    readExact(observed.value, reinterpret_cast<char*>(&workerPid), sizeof(workerPid));
    require(workerPid > 1, "Invalid guarded-worker PID");
    cleanup.worker = static_cast<pid_t>(workerPid);
    char ready = 0;
    readExact(observed.value, &ready, 1);
    require(ready == 'R', "Worker did not reach requested parent-death boundary");
    ::close(commandWrite.value);
    commandWrite.value = -1;
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(owner, &status, 0);
    } while (waited < 0 && errno == EINTR);
    require(waited == owner, "Failed to reap temporary owner");
    cleanup.owner = -1;
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "Temporary owner did not exit cleanly");
    if (stage == "before-registration")
        writeAll(observed.value, "G", 1);
    std::string response;
    for (;;) {
        char bytes[16];
        const ssize_t count = timedRead(observed.value, bytes, sizeof(bytes));
        if (count == 0) {
            cleanup.eof = true;
            break;
        }
        response.append(bytes, static_cast<size_t>(count));
    }
    require(response == (stage == "registered-busy" ? "" : "X"),
            "Worker did not reject/exit at the requested parent-death boundary");
}

void spawnCase(const std::string& executable, const std::string& mode, const std::string& argument,
               unsigned closeMask = 0) {
    posix_spawn_file_actions_t actions;
    require(::posix_spawn_file_actions_init(&actions) == 0, "spawn actions init failed");
    struct ActionCleanup {
        posix_spawn_file_actions_t* actions;
        ~ActionCleanup() { ::posix_spawn_file_actions_destroy(actions); }
    } cleanup{&actions};
    for (unsigned fd = 0; fd < 3; ++fd)
        if (closeMask & (1u << fd))
            require(::posix_spawn_file_actions_addclose(&actions, static_cast<int>(fd)) == 0,
                    "spawn close action failed");
    std::array<char*, 4> args{const_cast<char*>(executable.c_str()),
                              const_cast<char*>(mode.c_str()), const_cast<char*>(argument.c_str()),
                              nullptr};
    pid_t child = -1;
    int rc = ::posix_spawn(&child, executable.c_str(), &actions, nullptr, args.data(), environ);
    require(rc == 0, std::string("spawn test case: ") + std::strerror(rc));
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    require(waited == child, "waitpid test case failed");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            mode + " (" + argument + ") failed with wait status " + std::to_string(status));
}
} // namespace

int main(int argc, char** argv) {
    try {
        isolated_diagnostics::initializeDiagnosticProcessPlatform();
        if (argc == 5 && std::string(argv[1]) == "--parent-owner-case")
            return parentOwnerCase(argv[0], argv[2], std::stoi(argv[3]), std::stoi(argv[4]));
        if (argc == 5 && std::string(argv[1]) == "--parent-watch-case")
            return parentWatchCase(argv[2], std::stoll(argv[3]), std::stoi(argv[4]));
        if (argc == 2 && std::string(argv[1]) == "--diagnostic-worker")
            return worker();
        if (argc == 3 && std::string(argv[1]) == "--stdio-case")
            return stdioCase(argv[2]);
        if (argc == 3 && std::string(argv[1]) == "--helper-case")
            return helperCase(argv[2]);
        if (argc == 3 && std::string(argv[1]) == "--parent-death-case") {
            ::alarm(12);
            parentDeathCase(argv[0], argv[2]);
            return 0;
        }
        require(argc == 1, "Usage: diagnostic_transport_tests (worker argv is fixed)");
        ::alarm(90); // Broken cleanup or notification paths must fail instead of hanging CI.
        std::array<char, 4096> resolved{};
        require(::realpath(argv[0], resolved.data()) != nullptr,
                systemError("realpath executable"));
        const std::string executable(resolved.data());
        const std::string directory = executable.substr(0, executable.find_last_of('/'));
        TemporaryDirectory temporary(directory);
        unsigned passed = 0, failed = 0;
        auto test = [&](const std::string& name, const std::function<void()>& body) {
            const auto start = Clock::now();
            try {
                body();
                ++passed;
                std::cout << "PASS " << name;
            }
            catch (const std::exception& error) {
                ++failed;
                std::cout << "FAIL " << name << ": " << error.what();
            }
            std::cout << " ("
                      << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start)
                             .count()
                      << " ms)" << std::endl;
        };
        test("binary large frame and identity", [&] {
            std::string payload(1024 * 1024 + 17, '\0');
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(i % 256);
            Session session(executable);
            session.submit("echo\n" + payload);
            expectSuccess(session.await(), payload);
            require(!session.process.takeResult(), "takeResult returned the result twice");
        });
        test("zero-length response", [&] {
            Session session(executable);
            session.submit("echo\n");
            expectSuccess(session.await(), "");
        });
        test("fragmented response header and payload", [&] {
            std::string payload(65539, 'x');
            payload[31] = '\0';
            Session session(executable);
            session.submit("fragmented\n" + payload);
            expectSuccess(session.await(), payload);
        });
        for (const auto& item :
             std::vector<std::pair<std::string, std::string>>{{"oversize", "exceeds limit"},
                                                              {"truncated-header", "Truncated"},
                                                              {"truncated-body", "Truncated"},
                                                              {"extra", "Extra bytes"},
                                                              {"double-frame", "Extra bytes"},
                                                              {"crash", "unsuccessfully"}}) {
            test("reject " + item.first, [&] {
                Session session(executable);
                session.submit(item.first + "\n");
                expectError(session.await(), item.second);
                session.submit("echo\nrecovered", 18, 32);
                expectSuccess(session.await(), "recovered", 18, 32);
            });
        }
        test("input size preflight", [&] {
            auto config = limits();
            config.maxInputBytes = 8;
            Session session(executable, config);
            session.submit("echo\n12345");
            expectError(session.await(), "input exceeds limit");
        });
        test("encoder exceptions remain recoverable", [&] {
            Session session(executable);
            require(session.process.submit(
                        Job{17, 31, "",
                            []() -> std::string { throw std::runtime_error("encoder sentinel"); }}),
                    "encoder submit failed");
            expectError(session.await(), "encoder sentinel");
            session.submit("echo\nafter encoder", 18, 32);
            expectSuccess(session.await(), "after encoder", 18, 32);
        });
        test("timeout kills and reaps held worker", [&] {
            Session session(executable, limits(300ms));
            auto marker = temporary.file("timeout.pid");
            session.submit("hold\n" + marker);
            auto pid = awaitPid(marker);
            expectError(session.await(), "timed out");
            expectReaped(pid);
            session.submit("echo\nafter timeout", 18, 32);
            expectSuccess(session.await(), "after timeout", 18, 32);
        });
        test("cancel is nonblocking, suppresses result, and reaps", [&] {
            Session session(executable, limits(30000ms));
            auto marker = temporary.file("cancel.pid");
            session.submit("hold\n" + marker);
            auto pid = awaitPid(marker);
            auto start = Clock::now();
            session.process.cancel();
            require(Clock::now() - start < 500ms, "cancel blocked the caller");
            expectReaped(pid);
            require(!session.process.takeResult(), "Cancelled job published a result");
            require(session.wakeCount == 0, "Cancelled job invoked result callback");
            session.submit("echo\nafter cancel", 18, 32);
            expectSuccess(session.await(), "after cancel", 18, 32);
        });
        test("replacement kills held child and only newest pending runs", [&] {
            Session session(executable, limits(30000ms));
            auto marker = temporary.file("replace.pid");
            session.submit("hold\n" + marker, 1, 1);
            auto pid = awaitPid(marker);
            EncoderGate gate; // Released before session cleanup, including assertion failures.
            require(session.process.submit(Job{2, 2, "", gate.encoder()}), "gated submit failed");
            gate.awaitEntered();
            expectReaped(pid);
            auto obsoleteRan = std::make_shared<std::atomic<bool>>(false);
            require(session.process.submit(Job{3, 3, "",
                                               [obsoleteRan] {
                                                   *obsoleteRan = true;
                                                   return "echo\nobsolete pending";
                                               }}),
                    "obsolete pending submit failed");
            session.submit("echo\nnewest", 4, 4);
            gate.release();
            expectSuccess(session.await(), "newest", 4, 4);
            require(!*obsoleteRan, "Replaced pending encoder ran");
            session.process.join();
            require(session.wakeCount == 1, "Obsolete job invoked result callback");
            require(!session.process.takeResult(), "Obsolete job left another result");
        });
        test("notification burst remains nonblocking and newest wins", [&] {
            Session session(executable);
            EncoderGate gate;
            require(session.process.submit(Job{1, 1, "", gate.encoder()}), "gated submit failed");
            gate.awaitEntered();
            const auto start = Clock::now();
            for (uint64_t i = 0; i < 20000; ++i) {
                if (i % 3 == 0)
                    session.process.cancel();
                else
                    session.submit("echo\nreplaced", i, i);
            }
            session.submit("echo\nlatest after burst", 30000, 30000);
            require(Clock::now() - start < 3000ms, "Notification burst blocked the caller");
            gate.release();
            expectSuccess(session.await(), "latest after burst", 30000, 30000);
        });
        test("requestStop and join kill and reap held worker", [&] {
            Session session(executable, limits(30000ms));
            auto marker = temporary.file("stop.pid");
            session.submit("hold\n" + marker);
            auto pid = awaitPid(marker);
            const auto start = Clock::now();
            session.process.requestStop();
            require(Clock::now() - start < 500ms, "requestStop blocked the caller");
            session.process.join();
            require(Clock::now() - start < 2000ms, "join waited for worker timeout");
            expectReaped(pid);
            require(!session.process.submit(Job{19, 33, "echo\nrejected", {}}),
                    "submit accepted after stop");
            require(!session.process.takeResult(), "Stopped job published a result");
            require(session.wakeCount == 0, "Stopped job invoked callback");
            session.process.join(); // Idempotent cleanup.
        });
        test("destructor kills and reaps held worker", [&] {
            pid_t pid;
            const auto start = Clock::now();
            {
                Session session(executable, limits(30000ms));
                auto marker = temporary.file("destructor.pid");
                session.submit("hold\n" + marker);
                pid = awaitPid(marker);
            }
            require(Clock::now() - start < 2000ms, "Destruction waited for worker timeout");
            expectReaped(pid);
        });
        test("unrelated non-CLOEXEC descriptor does not reach worker", [&] {
            Descriptor original(::open("/dev/null", O_RDONLY));
            require(original.value >= 0, systemError("open /dev/null"));
            Descriptor unrelated(::fcntl(original.value, F_DUPFD, 64));
            require(unrelated.value >= 64, systemError("F_DUPFD"));
            require((::fcntl(unrelated.value, F_GETFD) & FD_CLOEXEC) == 0,
                    "Test descriptor must deliberately lack CLOEXEC");
            Session session(executable);
            session.submit("fd-check\n" + std::to_string(unrelated.value));
            expectSuccess(session.await(), "closed");
            require(::fcntl(unrelated.value, F_GETFD) >= 0,
                    "Parent's unrelated descriptor was closed");
        });
        test("same binary copied to another path succeeds", [&] {
            auto copied = temporary.file("same-image");
            copyExecutable(executable, copied);
            Session session(copied);
            session.submit("echo\nsame image");
            expectSuccess(session.await(), "same image");
        });
#ifdef __APPLE__
        test("different signed worker receives no request bytes", [&] {
            auto marker = temporary.file("different-image.marker");
            MarkerEnvironment environment(marker);
            Session session(directory + "/replacement_worker");
            session.submit("echo\nprivate snapshot sentinel");
            expectError(session.await(), "differs from running server");
            session.process.join();
            expectNoSnapshot(marker);
        });
        test("same-path replacement receives no request bytes", [&] {
            auto configured = temporary.file("replaceable-image");
            auto next = temporary.file("replacement-next");
            auto marker = temporary.file("same-path.marker");
            copyExecutable(executable, configured);
            MarkerEnvironment environment(marker);
            Session session(configured);
            session.submit("echo\nbefore replacement", 18, 32);
            expectSuccess(session.await(), "before replacement", 18, 32);
            copyExecutable(directory + "/replacement_worker", next);
            require(::rename(next.c_str(), configured.c_str()) == 0,
                    systemError("atomic fixture replacement"));
            session.submit("echo\nprivate snapshot sentinel");
            expectError(session.await(), "differs from running server");
            session.process.join();
            expectNoSnapshot(marker);
        });
        test("unsigned worker fails closed", [&] {
            Session session(directory + "/diagnostic_transport_unsigned");
            session.submit("echo\nunsigned image must not execute a job");
            const auto result = session.await();
            require(result.generation == 17 && result.jobId == 31,
                    "Unsigned failure identity mismatch");
            require(!result.error.empty() && result.output.empty(),
                    "Unsigned worker accepted a job");
            std::cout << "[" << result.error << "] ";
        });
#else
        std::cout << "SKIP macOS signed image, same-path replacement, and unsigned image tests "
                     "(requires native macOS)"
                  << std::endl;
#endif
        for (unsigned mask : {1u, 2u, 3u, 7u}) {
            test("closed standard descriptors mask " + std::to_string(mask),
                 [&] { spawnCase(executable, "--stdio-case", executable, mask); });
        }
        for (const auto& mode :
             {"oversize-request", "short-header", "short-body", "broken-output", "wrong-parent"}) {
            test(std::string("worker helper ") + mode,
                 [&] { spawnCase(executable, "--helper-case", mode); });
        }
        for (const auto& stage : {"before-frame", "before-registration", "registered-busy"}) {
            test(std::string("parent death ") + stage,
                 [&] { spawnCase(executable, "--parent-death-case", stage); });
        }
        test("worker helper output size limit", [&] {
            Session session(executable);
            session.submit("helper-output-limit\n");
            expectSuccess(session.await(), "rejected");
        });
        ::alarm(0);
        std::cout << "RESULT " << passed << " passed, " << failed << " failed" << std::endl;
        return failed ? 1 : 0;
    }
    catch (const std::exception& error) {
        std::cerr << "Harness error: " << error.what() << std::endl;
        return 2;
    }
}
