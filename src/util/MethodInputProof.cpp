// SPDX-License-Identifier: MIT
#include "util/MethodInputProof.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "slang/util/ScopeGuard.h"
#if defined(__APPLE__)
#    include "util/SourcePathProof.h"
#    include <cerrno>
#    include <fcntl.h>
#    include <sys/param.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

namespace server::method_input {
namespace fs = std::filesystem;
using slang::ScopeGuard;
namespace {
void need(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
#if defined(__APPLE__)
bool sameFileVersion(const struct stat& before, const struct stat& after) {
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_size == after.st_size &&
           before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
           before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
           before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
           before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
}
#endif
} // namespace

Directories::~Directories() {
    clear();
}
#if defined(__APPLE__)
std::optional<int> Directories::openInput(const std::string& path) {
    if (!enabled)
        return std::nullopt;
    const auto slash = path.rfind('/');
    if (slash == std::string::npos || slash + 1 == path.size())
        return std::nullopt;
    const auto leaf = std::string_view(path).substr(slash + 1);
    if (leaf == "." || leaf == "..")
        return std::nullopt;
    const auto directory = std::string_view(path).substr(0, slash ? slash : 1);
    if (descriptor < 0 || directory != parent) {
        clear();
        if (directory != "/" && !exactNormalizedPosixPath(directory, directory))
            return std::nullopt;
        parent = directory;
        if (session) {
            if (auto borrowed = session->directory(directory)) {
                descriptor = *borrowed;
                owned = false;
            }
        }
        if (descriptor < 0) {
            descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK);
            if (descriptor < 0) {
                releaseOnExhaustion();
                return std::nullopt;
            }
            char resolved[MAXPATHLEN];
            if (::fcntl(descriptor, F_GETPATH, resolved) != 0 ||
                std::string_view(resolved) != parent) {
                clear();
                return std::nullopt;
            }
        }
    }
    const auto fd = ::openat(descriptor, path.c_str() + slash + 1,
                             O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd >= 0)
        return fd;
    releaseOnExhaustion();
    // A pinned directory may have moved. Never certify a missing input
    // through its old directory; retry the original absolute path.
    return std::nullopt;
}
#endif
void Directories::clear() {
#if defined(__APPLE__)
    if (descriptor >= 0 && owned)
        ::close(descriptor);
    descriptor = -1;
    owned = true;
    parent.clear();
#endif
}
#if defined(__APPLE__)
void Directories::releaseOnExhaustion() {
    if (errno == EMFILE || errno == ENFILE) {
        clear();
        enabled = false;
    }
}
#endif

ReadResult readCertificateFile(const std::string& path, [[maybe_unused]] bool fastPath,
                               const lsp::RequestContext& ctx,
                               const std::optional<std::string>* expected,
                               [[maybe_unused]] Directories* directories) {
#if defined(__APPLE__)
    if (fastPath) {
        // The descriptor proves the resolved path of the bytes actually read,
        // avoiding canonical()'s repeated ancestor walks for every project file.
        // This is still a fresh complete content read, not a metadata certificate.
        // A missing leaf cannot be certified through a pinned parent: that
        // parent may have moved. Go straight to the required absolute lookup
        // instead of doing an openat that must retry the absolute path anyway.
        const bool expectedMissing = expected && !expected->has_value();
        auto relative = directories && !expectedMissing ? directories->openInput(path)
                                                        : std::nullopt;
        const int fd = relative ? *relative
                                : ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE)
                throw DirectoryProofUnavailable();
            if (errno == ENOENT)
                return {{}, expected && !expected->has_value()};
            need(false, "unreadable certificate input");
        }
        auto close = ScopeGuard([&] { ::close(fd); });
        struct stat before;
        need(::fstat(fd, &before) == 0 && S_ISREG(before.st_mode), "nonregular certificate input");
        char resolved[MAXPATHLEN];
        if (relative || ::fcntl(fd, F_GETPATH, resolved) == 0) {
            // Avoid constructing and decomposing the same absolute pathname
            // repeatedly. The fast predicate implies the original comparison;
            // all other spellings retain that comparison unchanged.
            const bool exactPath = relative || exactNormalizedPosixPath(path, resolved);
            need(exactPath || (fs::path(resolved) == fs::path(path) &&
                               fs::absolute(path).lexically_normal() == fs::path(path)),
                 "symlink input unsupported");
            need(before.st_size >= 0 && uint64_t(before.st_size) <= std::string().max_size(),
                 "input size unavailable");
            const auto size = size_t(before.st_size);
            std::string bytes;
            if (!expected)
                bytes.resize(size);
            bool matches = expected && expected->has_value() && expected->value().size() == size;
            std::array<char, 64 * 1024> chunk;
            size_t offset = 0;
            while (offset < size) {
                ctx.checkpoint("during method file input read");
                auto destination = expected ? chunk.data() : bytes.data() + offset;
                const auto count = ::read(
                    fd, destination,
                    std::min<size_t>(expected ? chunk.size() : 16 * 1024 * 1024, size - offset));
                if (count < 0 && errno == EINTR)
                    continue;
                need(count > 0, "input changed or read failed");
                if (matches &&
                    std::memcmp(destination, expected->value().data() + offset, size_t(count)) != 0)
                    matches = false;
                offset += size_t(count);
            }
            char extra;
            ssize_t count;
            do {
                count = ::read(fd, &extra, 1);
            } while (count < 0 && errno == EINTR);
            struct stat after, current;
            need(count == 0 && ::fstat(fd, &after) == 0, "input changed or read failed");
            ctx.checkpoint("after method file descriptor metadata");
            ctx.checkpoint(path);
            need(::lstat(path.c_str(), &current) == 0 && S_ISREG(current.st_mode) &&
                     sameFileVersion(before, after) && sameFileVersion(after, current) &&
                     ::fcntl(fd, F_GETPATH, resolved) == 0 &&
                     (exactPath ? std::string_view(resolved) == path
                                : fs::path(resolved) == fs::path(path)),
                 "input changed or read failed");
            return {expected ? std::nullopt : std::optional<std::string>(std::move(bytes)),
                    matches};
        }
        // Unsupported descriptor path queries retain the portable strict proof.
    }
#endif
    std::error_code error;
    auto status = fs::status(path, error);
    if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status)))
        return {{}, expected && !expected->has_value()};
    need(!error && fs::is_regular_file(status), ("nonregular certificate input: " + path).c_str());
    need(fs::canonical(path) == fs::absolute(path).lexically_normal(), "symlink input unsupported");
    // Read into one allocation. Iterator extraction spends most body-query time
    // copying unchanged project bytes a character at a time on large designs.
    // Keep exact byte comparison and reject files that shrink/grow during the read.
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    need(bool(input), "unreadable certificate input");
    const auto size = input.tellg();
    need(size >= 0, "input size unavailable");
    std::string bytes(size_t(size), '\0');
    input.seekg(0);
    ctx.checkpoint("during method file input read");
    input.read(bytes.data(), std::streamsize(bytes.size()));
    need(bool(input) && input.peek() == std::char_traits<char>::eof() && !input.bad(),
         "input changed or read failed");
    const bool matches = expected && expected->has_value() && bytes == expected->value();
    return {std::move(bytes), matches};
}

std::vector<std::string> directoryEntries(const fs::path& path) {
    need(fs::canonical(path) == fs::absolute(path).lexically_normal(),
         "symlink directory input unsupported");
    need(fs::is_directory(path), "directory input changed");
    std::vector<std::string> entries;
    for (const auto& entry : fs::directory_iterator(path))
        entries.push_back(entry.path().filename().string() + ":" +
                          std::to_string(int(entry.symlink_status().type())));
    std::sort(entries.begin(), entries.end());
    return entries;
}

} // namespace server::method_input
