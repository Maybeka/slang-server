#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif
#include "diagnostics/DiagnosticProcess.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#if defined(__linux__)
#    include <features.h>
#endif
#if defined(__linux__) && defined(__GLIBC__)
#    if __GLIBC_PREREQ(2, 34)
#        define DIAGNOSTIC_PROCESS_SUPPORTED 1
#    endif
#endif
#ifdef __APPLE__
#    define DIAGNOSTIC_PROCESS_SUPPORTED 1
#endif
#ifdef DIAGNOSTIC_PROCESS_SUPPORTED
#    include <fcntl.h>
#    include <poll.h>
#    include <signal.h>
#    include <spawn.h>
#    ifdef __linux__
#        include <sys/eventfd.h>
#        include <sys/prctl.h>
#    else
#        include <CoreFoundation/CoreFoundation.h>
#        include <Security/Security.h>
#        include <mach-o/dyld.h>
#        include <mach/machine.h>
#        include <pthread.h>
#        include <sys/event.h>
#    endif
#    include <sys/socket.h>
#    include <sys/wait.h>
#    include <unistd.h>
extern char** environ;
#endif

namespace isolated_diagnostics {
namespace {
[[maybe_unused, noreturn]] void unsupported() {
    throw std::runtime_error(
        "Isolated diagnostics require macOS or Linux with glibc 2.34 or newer");
}
#ifdef DIAGNOSTIC_PROCESS_SUPPORTED
std::runtime_error sysError(const char* what, int code = errno) {
    return std::runtime_error(std::string(what) + ": " + std::strerror(code));
}
struct Fd {
    int value = -1;
    Fd() = default;
    explicit Fd(int value) : value(value) {}
    ~Fd() {
        if (value >= 0)
            ::close(value);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
std::array<char, 8> header(uint64_t length) {
    std::array<char, 8> result{};
    for (size_t i = 0; i < 8; ++i)
        result[7 - i] = char(length >> (i * 8));
    return result;
}
uint64_t lengthOf(const std::array<char, 8>& bytes) {
    uint64_t length = 0;
    for (unsigned char byte : bytes)
        length = (length << 8) | byte;
    return length;
}
struct Child {
    pid_t pid = -1;
    int status = 0;
    ~Child() {
        if (pid <= 0)
            return;
        // This unreaped PID belongs exclusively to us and cannot be reused.
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    bool reap() {
        if (pid <= 0)
            return true;
        pid_t got;
        do {
            got = ::waitpid(pid, &status, WNOHANG);
        } while (got < 0 && errno == EINTR);
        if (got < 0) {
            // Do not signal a PID that an external reaper may already have released.
            pid = -1;
            throw sysError("waitpid");
        }
        if (got == 0)
            return false;
        pid = -1;
        return true;
    }
};
#    ifdef __APPLE__
// Darwin has no atomic SOCK_CLOEXEC. Synchronize descriptor creation with fork
// (including the waveform viewer) so no child can copy a not-yet-CLOEXEC end.
// posix_spawn uses CLOEXEC_DEFAULT below and does not need atfork handlers.
pthread_mutex_t descriptorCreationMutex = PTHREAD_MUTEX_INITIALIZER;
void descriptorForkPrepare() {
    if (::pthread_mutex_lock(&descriptorCreationMutex) != 0)
        std::abort();
}
void descriptorForkDone() {
    if (::pthread_mutex_unlock(&descriptorCreationMutex) != 0)
        std::abort();
}
void registerDescriptorForkGuard() {
    static const int registration = ::pthread_atfork(descriptorForkPrepare, descriptorForkDone,
                                                     descriptorForkDone);
    if (registration)
        throw sysError("register descriptor fork guard", registration);
}
struct DescriptorCreationGuard {
    DescriptorCreationGuard() {
        registerDescriptorForkGuard();
        descriptorForkPrepare();
    }
    ~DescriptorCreationGuard() { descriptorForkDone(); }
};

template<typename T>
struct CFObject {
    T value = nullptr;
    ~CFObject() {
        if (value)
            ::CFRelease(value);
    }
    CFObject() = default;
    CFObject(const CFObject&) = delete;
    CFObject& operator=(const CFObject&) = delete;
};
void securityCheck(OSStatus status, const char* action) {
    if (status != errSecSuccess)
        throw std::runtime_error(std::string(action) + " (Security status " +
                                 std::to_string(status) + ")");
}
struct MacImageIdentity {
    CFObject<SecRequirementRef> requirement;

    MacImageIdentity() {
        CFObject<SecCodeRef> self;
        securityCheck(::SecCodeCopySelf(kSecCSDefaultFlags, &self.value),
                      "identify running server");
        CFObject<CFDictionaryRef> info;
        securityCheck(::SecCodeCopySigningInformation(self.value, kSecCSSigningInformation,
                                                      &info.value),
                      "read running server signature (signed macOS build required)");
        auto value = ::CFDictionaryGetValue(info.value, kSecCodeInfoUnique);
        if (!value || ::CFGetTypeID(value) != ::CFDataGetTypeID())
            throw std::runtime_error("diagnostic server has no signed code identity");
        auto hash = static_cast<CFDataRef>(value);
        auto length = ::CFDataGetLength(hash);
        if (length <= 0 || length > 64)
            throw std::runtime_error("invalid diagnostic server code identity");
        const auto* bytes = ::CFDataGetBytePtr(hash);
        constexpr char hex[] = "0123456789abcdef";
        std::string text = "cdhash H\"";
        for (CFIndex i = 0; i < length; ++i) {
            text += hex[bytes[i] >> 4];
            text += hex[bytes[i] & 15];
        }
        text += '\"';
        CFObject<CFStringRef> expression;
        expression.value = ::CFStringCreateWithCString(nullptr, text.c_str(),
                                                       kCFStringEncodingASCII);
        if (!expression.value)
            throw std::bad_alloc();
        securityCheck(::SecRequirementCreateWithString(expression.value, kSecCSDefaultFlags,
                                                       &requirement.value),
                      "create exact diagnostic image requirement");
        // SigningInformation alone reads disk and is not proof of the live image.
        // Dynamic validation also compares the kernel-hosted cdhash, so replacement
        // between process startup and this capture fails instead of selecting new code.
        securityCheck(::SecCodeCheckValidity(self.value,
                                             SecCSFlags(kSecCSStrictValidate) |
                                                 SecCSFlags(kSecCSNoNetworkAccess),
                                             requirement.value),
                      "validate running diagnostic image");
    }
    void validateChild(pid_t pid) const {
        CFObject<CFNumberRef> number;
        int32_t child = int32_t(pid);
        number.value = ::CFNumberCreate(nullptr, kCFNumberSInt32Type, &child);
        if (!number.value)
            throw std::bad_alloc();
        const void* key = kSecGuestAttributePid;
        const void* value = number.value;
        CFObject<CFDictionaryRef> attributes;
        attributes.value = ::CFDictionaryCreate(nullptr, &key, &value, 1,
                                                &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
        if (!attributes.value)
            throw std::bad_alloc();
        CFObject<SecCodeRef> guest;
        securityCheck(::SecCodeCopyGuestWithAttributes(nullptr, attributes.value,
                                                       kSecCSDefaultFlags, &guest.value),
                      "identify owned diagnostic child");
        securityCheck(::SecCodeCheckValidity(guest.value,
                                             SecCSFlags(kSecCSStrictValidate) |
                                                 SecCSFlags(kSecCSNoNetworkAccess),
                                             requirement.value),
                      "diagnostic child executable differs from running server");
    }
};
#    endif
// macOS uses a per-socket option; never change the process-wide SIGPIPE policy.
constexpr int sendFlags() {
#    ifdef __APPLE__
    return 0;
#    else
    return MSG_NOSIGNAL;
#    endif
}
void configureSocket(int fd) {
#    ifdef __APPLE__
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0 ||
        ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        throw sysError("configure diagnostic socket");
#    else
    (void)fd;
#    endif
}
void socketPair(int (&pair)[2]) {
#    ifdef __APPLE__
    DescriptorCreationGuard guard;
    constexpr int flags = SOCK_STREAM;
#    else
    constexpr int flags = SOCK_STREAM | SOCK_CLOEXEC;
#    endif
    if (::socketpair(AF_UNIX, flags, 0, pair) < 0)
        throw sysError("socketpair");
    try {
        configureSocket(pair[0]);
        configureSocket(pair[1]);
    }
    catch (...) {
        ::close(pair[0]);
        ::close(pair[1]);
        throw;
    }
}
void moveAboveStdio(int& fd) {
    if (fd > STDERR_FILENO)
        return;
    int copy = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (copy < 0)
        throw sysError("fcntl(F_DUPFD_CLOEXEC)");
    ::close(fd);
    fd = copy;
}
#endif
} // namespace

#ifdef DIAGNOSTIC_PROCESS_SUPPORTED
struct DiagnosticProcess::Impl {
    std::string executable;
    Limits limits;
    std::function<void()> wake;
    std::mutex mutex;
    std::optional<Job> pending;
    std::optional<Result> result;
    uint64_t epoch = 0;
    uint64_t sessionInvalidation = 0, observedSessionInvalidation = 0;
    bool stopping = false;
    Fd notification;
#    ifdef __APPLE__
    std::unique_ptr<MacImageIdentity> imageIdentity;
    Fd notificationWrite;
#    endif
    std::thread thread;
    std::unique_ptr<Child> sessionChild;
    Fd sessionSocket;
    uint64_t sessionKey = 0, sessionToken = 0;

    void resetSession() noexcept {
        sessionChild.reset(); // Kill/reap before EOF can look like a malformed request.
        if (sessionSocket.value >= 0) {
            ::close(sessionSocket.value);
            sessionSocket.value = -1;
        }
        sessionKey = sessionToken = 0;
    }

    Impl(std::string path, Limits config, std::function<void()> callback) :
        executable(std::move(path)), limits(config), wake(std::move(callback)) {
#    ifdef __APPLE__
        int pair[2];
        socketPair(pair);
        notification.value = pair[0];
        notificationWrite.value = pair[1];
        moveAboveStdio(notification.value);
        moveAboveStdio(notificationWrite.value);
        if (::fcntl(notification.value, F_SETFL, O_NONBLOCK) < 0 ||
            ::fcntl(notificationWrite.value, F_SETFL, O_NONBLOCK) < 0)
            throw sysError("notification nonblocking");
#    else
        notification.value = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
#    endif
        if (executable.empty() || executable[0] != '/' ||
            executable.find('\0') != std::string::npos)
            throw std::invalid_argument("Diagnostic executable must be an absolute path");
        if (limits.timeout.count() <= 0 || limits.maxInputBytes > std::string().max_size() ||
            limits.maxOutputBytes > std::string().max_size())
            throw std::invalid_argument("Invalid diagnostic limits");
        if (notification.value < 0)
            throw sysError("eventfd");
        thread = std::thread([this] { run(); });
    }
    void notify() noexcept {
#    ifdef __APPLE__
        char value = 1;
        while (::send(notificationWrite.value, &value, sizeof(value), sendFlags()) < 0 &&
               errno == EINTR) {
        }
#    else
        uint64_t value = 1;
        while (::write(notification.value, &value, sizeof(value)) < 0 && errno == EINTR) {
        }
#    endif
        // EAGAIN means an already pending notification will wake the thread.
    }
    void drain() noexcept {
#    ifdef __APPLE__
        std::array<char, 256> value;
        for (;;) {
            ssize_t count = ::read(notification.value, value.data(), value.size());
            if (count > 0 || (count < 0 && errno == EINTR))
                continue;
            break;
        }
#    else
        uint64_t value;
        while (::read(notification.value, &value, sizeof(value)) < 0 && errno == EINTR) {
        }
#    endif
    }
    bool obsolete(uint64_t token) {
        std::lock_guard lock(mutex);
        return stopping || epoch != token;
    }
    void run() noexcept {
        struct SessionCleanup {
            Impl& owner;
            ~SessionCleanup() { owner.resetSession(); }
        } cleanup{*this};
        for (;;) {
            std::optional<Job> job;
            uint64_t token;
            uint64_t invalidation;
            {
                std::lock_guard lock(mutex);
                if (stopping)
                    return;
                token = epoch;
                invalidation = sessionInvalidation;
                job.swap(pending);
            }
            if (observedSessionInvalidation != invalidation) {
                resetSession();
                observedSessionInvalidation = invalidation;
            }
            if (!job) {
                if (sessionChild && token != sessionToken)
                    resetSession();
                pollfd fd{notification.value, POLLIN, 0};
                int rc;
                do {
                    // A warm diagnostic library is useful during typing, but need
                    // not retain an extra compiler process through long idle periods.
                    rc = ::poll(&fd, 1, sessionChild ? 5000 : -1);
                } while (rc < 0 && errno == EINTR);
                if (rc < 0)
                    return; // Only possible for invalid internal descriptors.
                if (rc == 0)
                    resetSession();
                drain();
                continue;
            }
            Result completed{job->generation, job->jobId, {}, {}};
            try {
                if (!obsolete(token)) {
                    if (job->encode)
                        job->input = job->encode();
                    if (!obsolete(token))
                        completed.output = exchange(*job, token);
                }
            }
            catch (const std::exception& error) {
                resetSession();
                completed.error = error.what();
            }
            catch (...) {
                resetSession();
                completed.error = "Unknown diagnostic process failure";
            }
            bool publish = false;
            {
                std::lock_guard lock(mutex);
                if (!stopping && epoch == token) {
                    result = std::move(completed);
                    publish = true;
                }
            }
            if (publish && wake) {
                try {
                    wake();
                }
                catch (...) { /* Scheduler callback must not kill I/O thread. */
                }
            }
        }
    }
    std::string exchange(const Job& job, uint64_t token) {
        if (job.input.size() > limits.maxInputBytes)
            throw std::runtime_error("Diagnostic input exceeds limit");
        if (obsolete(token))
            return {};
#    ifdef __APPLE__
        // Security framework work belongs to the I/O thread, never the LSP
        // foreground thread that constructs DiagnosticProcess.
        if (!imageIdentity)
            imageIdentity = std::make_unique<MacImageIdentity>();
        if (obsolete(token))
            return {};
#    endif
        const bool persistent = job.sessionKey != 0;
        Fd localSocket;
        Child localChild;
        if (!persistent || !sessionChild || sessionKey != job.sessionKey) {
            resetSession();
            int pair[2];
            socketPair(pair);
            Fd parent(pair[0]), childSocket(pair[1]);
            moveAboveStdio(parent.value);
            moveAboveStdio(childSocket.value);
            if (::fcntl(parent.value, F_SETFL, O_NONBLOCK) < 0)
                throw sysError("fcntl(O_NONBLOCK)");
            posix_spawn_file_actions_t actions;
            int rc = ::posix_spawn_file_actions_init(&actions);
            if (rc)
                throw sysError("spawn actions init", rc);
            struct Cleanup {
                posix_spawn_file_actions_t* a;
                ~Cleanup() { ::posix_spawn_file_actions_destroy(a); }
            } cleanup{&actions};
            auto check = [](int error) {
                if (error)
                    throw sysError("spawn actions", error);
            };
            check(::posix_spawn_file_actions_adddup2(&actions, childSocket.value, STDIN_FILENO));
            check(::posix_spawn_file_actions_adddup2(&actions, childSocket.value, STDOUT_FILENO));
#    ifdef __APPLE__
            // CLOEXEC_DEFAULT closes all descriptors not explicitly used by an action,
            // even descriptors concurrently opened without FD_CLOEXEC elsewhere.
            if (::fcntl(STDERR_FILENO, F_GETFD) >= 0)
                check(::posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO));
            else if (errno != EBADF)
                throw sysError("inspect stderr");
#    else
            check(::posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1));
#    endif
            posix_spawnattr_t attributes;
            check(::posix_spawnattr_init(&attributes));
            struct AttrCleanup {
                posix_spawnattr_t* a;
                ~AttrCleanup() { ::posix_spawnattr_destroy(a); }
            } attrCleanup{&attributes};
            sigset_t empty;
            sigemptyset(&empty);
            check(::posix_spawnattr_setsigmask(&attributes, &empty));
            short spawnFlags = POSIX_SPAWN_SETSIGMASK;
#    ifdef __APPLE__
            spawnFlags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
            // Universal executables must launch the same slice as this process,
            // including an x86_64 process running under Rosetta.
#        if defined(__arm64__) || defined(__aarch64__)
            cpu_type_t architecture = CPU_TYPE_ARM64;
#        elif defined(__x86_64__)
            cpu_type_t architecture = CPU_TYPE_X86_64;
#        else
#            error Unsupported macOS diagnostic worker architecture
#        endif
            size_t selected = 0;
            check(::posix_spawnattr_setbinpref_np(&attributes, 1, &architecture, &selected));
            if (selected != 1)
                throw std::runtime_error("cannot select diagnostic worker architecture");
#    endif
            check(::posix_spawnattr_setflags(&attributes, spawnFlags));
            char onceMode[] = "--diagnostic-worker";
            char sessionMode[] = "--diagnostic-session-worker";
            char* mode = persistent ? sessionMode : onceMode;
            char* argv[] = {executable.data(), mode, nullptr};
            Child child;
            rc = ::posix_spawn(&child.pid, executable.c_str(), &actions, &attributes, argv,
                               environ);
            if (rc) {
                child.pid = -1;
                throw sysError("posix_spawn", rc);
            }
            ::close(childSocket.value);
            childSocket.value = -1;
#    ifdef __APPLE__
            // The PID is still our unreaped child. No snapshot bytes are sent until
            // public Security APIs authenticate its live image against our own.
            // A replaced path may bootstrap, but it cannot receive a diagnostic job.
            imageIdentity->validateChild(child.pid);
#    endif
            if (persistent) {
                sessionChild = std::make_unique<Child>();
                sessionChild->pid = child.pid;
                sessionSocket.value = parent.value;
                sessionKey = job.sessionKey;
            }
            else {
                localChild.pid = child.pid;
                localSocket.value = parent.value;
            }
            child.pid = -1;
            parent.value = -1;
        }
        Child& child = persistent ? *sessionChild : localChild;
        Fd& parent = persistent ? sessionSocket : localSocket;
        int rc = 0;
        const auto deadline = std::chrono::steady_clock::now() + limits.timeout;
        auto inputHeader = header(job.input.size());
        size_t sentHeader = 0, sentInput = 0, gotHeader = 0;
        std::array<char, 8> outputHeader{};
        std::string output;
        uint64_t expected = 0;
        bool inputClosed = false, eof = false;
        for (;;) {
            if (obsolete(token)) {
                resetSession();
                return {}; // Child RAII cancels and reaps off main thread.
            }
            bool exited = child.reap();
            if (persistent && exited)
                throw std::runtime_error("Diagnostic session worker exited");
            if (persistent && sentHeader == 8 && sentInput == job.input.size() && gotHeader == 8 &&
                output.size() == expected) {
                sessionToken = token;
                return output;
            }
            if (exited && (!WIFEXITED(child.status) || WEXITSTATUS(child.status) != 0))
                throw std::runtime_error("Diagnostic worker exited unsuccessfully (wait status " +
                                         std::to_string(child.status) + ")");
            if (exited && eof) {
                if (sentHeader != 8 || sentInput != job.input.size())
                    throw std::runtime_error("Worker exited before receiving request");
                if (gotHeader != 8 || output.size() != expected)
                    throw std::runtime_error("Truncated diagnostic response");
                return output;
            }
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                throw std::runtime_error("Diagnostic worker timed out");
            if (!inputClosed && sentHeader == 8 && sentInput == job.input.size()) {
                if (!persistent)
                    ::shutdown(parent.value, SHUT_WR);
                inputClosed = true;
            }
            pollfd fds[2] = {{notification.value, POLLIN, 0},
                             {(eof && inputClosed) ? -1 : parent.value,
                              short((eof ? 0 : POLLIN) | (inputClosed ? 0 : POLLOUT)), 0}};
            int timeout = int(std::min<int64_t>(
                50,
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1));
            do {
                rc = ::poll(fds, 2, timeout);
            } while (rc < 0 && errno == EINTR);
            if (rc < 0)
                throw sysError("poll");
            if (fds[0].revents) {
                drain();
                continue;
            }
            if (fds[1].revents & POLLNVAL)
                throw std::runtime_error("Invalid diagnostic socket");
            if (!eof && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                std::array<char, 65536> buffer;
                ssize_t count = ::recv(parent.value, buffer.data(), buffer.size(), 0);
                if (count == 0 && persistent)
                    throw std::runtime_error("Diagnostic session socket closed unexpectedly");
                if (count == 0)
                    eof = true;
                else if (count < 0) {
                    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                        throw sysError("recv");
                }
                else {
                    size_t offset = 0;
                    while (gotHeader < 8 && offset < size_t(count))
                        outputHeader[gotHeader++] = buffer[offset++];
                    if (gotHeader == 8) {
                        expected = lengthOf(outputHeader);
                        if (expected > limits.maxOutputBytes)
                            throw std::runtime_error("Diagnostic response exceeds limit");
                        if (uint64_t(count - offset) > expected - output.size())
                            throw std::runtime_error("Extra bytes in diagnostic response");
                        output.append(buffer.data() + offset, size_t(count) - offset);
                    }
                }
            }
            if (!inputClosed && (fds[1].revents & POLLOUT)) {
                const char* bytes = sentHeader < 8 ? inputHeader.data() + sentHeader
                                                   : job.input.data() + sentInput;
                size_t remaining = sentHeader < 8 ? 8 - sentHeader : job.input.size() - sentInput;
                if (remaining) {
                    ssize_t count = ::send(parent.value, bytes, std::min<size_t>(remaining, 65536),
                                           sendFlags());
                    if (count < 0) {
                        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                            throw sysError("send");
                    }
                    else if (sentHeader < 8)
                        sentHeader += size_t(count);
                    else
                        sentInput += size_t(count);
                }
            }
        }
    }
};
#else
struct DiagnosticProcess::Impl {};
#endif

#ifdef __APPLE__
struct DiagnosticParentGuard::Impl {
    Fd queue;
    std::atomic<bool> stopping{false};
    std::thread thread;

    explicit Impl(int64_t owner) : queue(::kqueue()) {
        if (owner <= 1 || int64_t(::getppid()) != owner)
            throw std::runtime_error("diagnostic worker lost its owning parent");
        if (queue.value < 0 || ::fcntl(queue.value, F_SETFD, FD_CLOEXEC) < 0)
            throw sysError("parent-watch kqueue");
        struct kevent changes[2];
        EV_SET(&changes[0], uintptr_t(owner), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0,
               nullptr);
        EV_SET(&changes[1], 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        // NOTE_EXIT observes only future exits. Checking the real parent on both
        // sides closes the registration window and prevents watching a reused PID.
        if (::kevent(queue.value, changes, 2, nullptr, 0, nullptr) < 0)
            throw sysError("register parent exit watch");
        if (int64_t(::getppid()) != owner)
            throw std::runtime_error("diagnostic worker lost its owning parent");
        thread = std::thread([this] {
            for (;;) {
                struct kevent event;
                int count = ::kevent(queue.value, nullptr, 0, &event, 1, nullptr);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count != 1 || (event.flags & EV_ERROR))
                    ::_exit(125); // Losing the lifetime watch must fail closed.
                if (event.filter == EVFILT_PROC && (event.fflags & NOTE_EXIT))
                    ::_exit(125); // Terminate all worker threads, even hung semantics.
                if (event.filter == EVFILT_USER && stopping.load())
                    return;
            }
        });
    }
    ~Impl() {
        stopping.store(true);
        struct kevent event;
        EV_SET(&event, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        int rc;
        do {
            rc = ::kevent(queue.value, &event, 1, nullptr, 0, nullptr);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0)
            ::_exit(125); // Never hang a worker while destroying a broken watch.
        thread.join();
    }
};
#else
struct DiagnosticParentGuard::Impl {};
#endif

DiagnosticParentGuard::DiagnosticParentGuard(int64_t owner) {
#ifdef __APPLE__
    impl = std::make_unique<Impl>(owner);
#elif defined(DIAGNOSTIC_PROCESS_SUPPORTED)
    if (owner <= 1 || int64_t(::getppid()) != owner || ::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
        int64_t(::getppid()) != owner)
        throw std::runtime_error("diagnostic worker lost its owning parent");
#else
    (void)owner;
    unsupported();
#endif
}
DiagnosticParentGuard::~DiagnosticParentGuard() = default;

DiagnosticProcess::DiagnosticProcess(std::string executable, Limits limits,
                                     std::function<void()> wake) {
#ifdef DIAGNOSTIC_PROCESS_SUPPORTED
    impl = std::make_unique<Impl>(std::move(executable), limits, std::move(wake));
#else
    (void)executable;
    (void)limits;
    (void)wake;
    unsupported();
#endif
}
DiagnosticProcess::~DiagnosticProcess() {
    requestStop();
    join();
}
bool DiagnosticProcess::submit(Job job) {
#ifdef DIAGNOSTIC_PROCESS_SUPPORTED
    {
        std::lock_guard lock(impl->mutex);
        if (impl->stopping)
            return false;
        ++impl->epoch;
        impl->pending = std::move(job);
        impl->result.reset();
    }
    impl->notify();
    return true;
#else
    (void)job;
    unsupported();
#endif
}
void DiagnosticProcess::cancel() {
#ifdef DIAGNOSTIC_PROCESS_SUPPORTED
    {
        std::lock_guard lock(impl->mutex);
        ++impl->epoch;
        ++impl->sessionInvalidation;
        impl->pending.reset();
        impl->result.reset();
    }
    impl->notify();
#endif
}
void DiagnosticProcess::requestStop() {
#ifdef DIAGNOSTIC_PROCESS_SUPPORTED
    {
        std::lock_guard lock(impl->mutex);
        impl->stopping = true;
        impl->pending.reset();
        impl->result.reset();
    }
    impl->notify();
#endif
}
void DiagnosticProcess::join() {
#ifdef DIAGNOSTIC_PROCESS_SUPPORTED
    requestStop();
    if (impl->thread.joinable())
        impl->thread.join();
#endif
}
std::optional<Result> DiagnosticProcess::takeResult() {
#ifdef DIAGNOSTIC_PROCESS_SUPPORTED
    std::lock_guard lock(impl->mutex);
    std::optional<Result> value;
    value.swap(impl->result);
    return value;
#else
    unsupported();
#endif
}

#ifdef DIAGNOSTIC_PROCESS_SUPPORTED
namespace {
void transfer(int fd, char* bytes, size_t length, bool writing) {
    while (length) {
        ssize_t count = writing ? ::send(fd, bytes, std::min<size_t>(length, 65536), sendFlags())
                                : ::read(fd, bytes, std::min<size_t>(length, 65536));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw sysError(writing ? "worker send" : "worker read");
        }
        if (count == 0)
            throw std::runtime_error("Unexpected end of diagnostic frame");
        bytes += count;
        length -= size_t(count);
    }
}
} // namespace
#endif
void initializeDiagnosticProcessPlatform() {
#ifdef __APPLE__
    // Call from main before creating any threads or permitting a concurrent fork.
    registerDescriptorForkGuard();
#endif
}

std::string currentDiagnosticExecutable() {
#ifdef __APPLE__
    uint32_t size = 0;
    ::_NSGetExecutablePath(nullptr, &size);
    if (!size)
        throw std::runtime_error("cannot determine diagnostic executable path");
    std::string path(size, '\0');
    if (::_NSGetExecutablePath(path.data(), &size) != 0)
        throw std::runtime_error("cannot determine diagnostic executable path");
    path.resize(std::strlen(path.c_str()));
    return std::filesystem::canonical(path).string();
#elif defined(DIAGNOSTIC_PROCESS_SUPPORTED)
    return "/proc/" + std::to_string(::getpid()) + "/exe";
#else
    unsupported();
#endif
}

std::string readDiagnosticRequest(uint64_t maxBytes) {
#ifdef DIAGNOSTIC_PROCESS_SUPPORTED
    std::array<char, 8> bytes;
    transfer(STDIN_FILENO, bytes.data(), bytes.size(), false);
    uint64_t length = lengthOf(bytes);
    if (length > maxBytes || length > std::string().max_size())
        throw std::runtime_error("Diagnostic request exceeds limit");
    std::string input(size_t(length), '\0');
    transfer(STDIN_FILENO, input.data(), input.size(), false);
    return input;
#else
    (void)maxBytes;
    unsupported();
#endif
}
void writeDiagnosticResponse(const std::string& bytes, uint64_t maxBytes) {
#ifdef DIAGNOSTIC_PROCESS_SUPPORTED
    if (bytes.size() > maxBytes)
        throw std::runtime_error("Diagnostic response exceeds limit");
    auto prefix = header(bytes.size());
    transfer(STDOUT_FILENO, prefix.data(), prefix.size(), true);
    transfer(STDOUT_FILENO, const_cast<char*>(bytes.data()), bytes.size(), true);
#else
    (void)bytes;
    (void)maxBytes;
    unsupported();
#endif
}
} // namespace isolated_diagnostics
