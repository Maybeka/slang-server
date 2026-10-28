// SPDX-License-Identifier: MIT
#include "util/DirectoryProofPlan.h"
#include "util/SourcePathProof.h"
#include "slang/util/ScopeGuard.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace server {
namespace {
struct Group {
    std::string parent;
    std::vector<uint32_t> names;
};
// A hash match can only reject acceleration. If the scanned directory has
// no entry with any planned hash, every planned name (and collision) is absent.
// No file content or successful physical witness is represented by this hash.
[[maybe_unused]] bool fingerprint(std::string_view text, uint32_t& result) {
    result = 2166136261u;
    for (unsigned char c : text) {
        if (!c || c >= 128)
            return false;
        const auto folded = c >= 'A' && c <= 'Z' ? unsigned(c + 'a' - 'A') : unsigned(c);
        result = (result ^ folded) * 16777619u;
    }
    return true;
}
#if defined(__APPLE__)
void checkResources() {
    if (errno == EMFILE || errno == ENFILE)
        throw DirectoryProofUnavailable();
}
bool same(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec &&
           a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
           a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec &&
           a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
}
constexpr int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK;
struct Lease {
    int fd = -1;
    struct stat version{};
    std::string resolved;
    bool current(const Group& group) const {
        struct stat after, absolute;
        char path[MAXPATHLEN];
        return fd >= 0 && ::fstat(fd, &after) == 0 &&
               ::stat(group.parent.c_str(), &absolute) == 0 && S_ISDIR(absolute.st_mode) &&
               same(version, after) && same(after, absolute) &&
               ::fcntl(fd, F_GETPATH, path) == 0 && resolved == path;
    }
};
#endif
} // namespace
struct DirectoryProofPlan::Impl {
    std::vector<Group> groups;
    size_t charged = 0;
};
DirectoryProofPlan::DirectoryProofPlan(std::unique_ptr<Impl> value) : impl(std::move(value)) {}
DirectoryProofPlan::~DirectoryProofPlan() = default;
size_t DirectoryProofPlan::chargedBytes() const { return impl->charged; }

std::unique_ptr<DirectoryProofPlan> DirectoryProofPlan::build(
    std::span<const std::string_view> missing, size_t budgetBytes) {
#if defined(__APPLE__)
    if (!budgetBytes)
        return {};
    std::map<std::string, std::vector<uint32_t>, std::less<>> groups;
    size_t charge = sizeof(Impl);
    for (auto path : missing) {
        if (path.find('\0') != std::string_view::npos ||
            !exactNormalizedPosixPath(path, path))
            continue;
        auto slash = path.rfind('/');
        auto parent = path.substr(0, slash ? slash : 1);
        auto leaf = path.substr(slash + 1);
        uint32_t name;
        if (leaf.size() > NAME_MAX || !fingerprint(leaf, name))
            continue;
        // Charge construction as well as the retained representation. Both are
        // bounded even when most groups are later rejected as too small.
        charge += 2 * sizeof(uint32_t);
        if (!groups.contains(parent))
            charge += 2 * (sizeof(Group) + parent.size() + 65);
        if (charge > budgetBytes)
            return {};
        groups[std::string(parent)].push_back(name);
    }
    auto result = std::make_unique<Impl>();
    for (auto& [parent, names] : groups) {
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        if (names.size() < 8)
            continue;
        result->groups.push_back({parent, std::move(names)});
        if (result->groups.size() == 64)
            break;
    }
    if (result->groups.empty())
        return {};
    result->charged = charge;
    return std::unique_ptr<DirectoryProofPlan>(new DirectoryProofPlan(std::move(result)));
#else
    (void)missing;
    (void)budgetBytes;
    return {};
#endif
}

struct DirectoryProofPlan::Session::State {
    const Impl& plan;
    const DirectoryProofPlan& owner;
#if defined(__APPLE__)
    std::vector<Lease> leases;
    explicit State(const Impl& plan, const DirectoryProofPlan& owner) :
        plan(plan), owner(owner), leases(plan.groups.size()) {}
    ~State() {
        for (auto& lease : leases)
            if (lease.fd >= 0)
                ::close(lease.fd);
    }
#else
    explicit State(const Impl& plan, const DirectoryProofPlan& owner) : plan(plan), owner(owner) {}
#endif
};
DirectoryProofPlan::Session::Session(const DirectoryProofPlan& plan,
                                    const lsp::RequestContext& ctx) :
    state(std::make_unique<State>(*plan.impl, plan)) {
#if defined(__APPLE__)
    for (size_t i = 0; i < state->plan.groups.size(); ++i) {
        ctx.checkpoint("before missing input directory scan");
        const auto& group = state->plan.groups[i];
        int fd = ::open(group.parent.c_str(), flags);
        if (fd < 0) {
            checkResources();
            continue;
        }
        DIR* stream = nullptr;
        auto cleanup = slang::ScopeGuard([&] {
            if (stream) ::closedir(stream);
            else ::close(fd);
        });
        Lease candidate;
        struct statfs filesystem;
        char resolved[MAXPATHLEN];
        if (::fstatfs(fd, &filesystem) || !(filesystem.f_flags & MNT_LOCAL) ||
            ::fstat(fd, &candidate.version) || !S_ISDIR(candidate.version.st_mode) ||
            ::fcntl(fd, F_GETPATH, resolved))
            continue;
        candidate.resolved = resolved;
        if (candidate.resolved != "/" &&
            !exactNormalizedPosixPath(candidate.resolved, candidate.resolved))
            continue;
        ctx.checkpoint(group.parent);
        stream = ::fdopendir(fd);
        if (!stream) {
            checkResources();
            continue;
        }
        bool absent = true;
        for (;;) {
            ctx.checkpoint("during missing input directory scan");
            errno = 0;
            const auto* entry = ::readdir(stream);
            if (!entry) {
                absent = errno == 0;
                checkResources();
                break;
            }
            uint32_t name;
            if (entry->d_namlen > NAME_MAX ||
                !fingerprint(std::string_view(entry->d_name, entry->d_namlen), name)) {
                absent = false;
                break;
            }
            if (std::binary_search(group.names.begin(), group.names.end(), name)) {
                absent = false; // Includes collisions, dangling links and case ambiguity.
                break;
            }
        }
        if (!absent)
            continue;
        candidate.fd = fd; // Borrowed only until the CLOEXEC duplicate succeeds.
        ctx.checkpoint("after missing input directory scan");
        if (!candidate.current(group))
            throw std::runtime_error("missing input directory changed");
        candidate.fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (candidate.fd < 0) {
            checkResources();
            continue;
        }
        // Move assignment of a string with the default allocator cannot throw.
        state->leases[i] = std::move(candidate);
    }
#else
    (void)ctx;
#endif
}
DirectoryProofPlan::Session::~Session() = default;

size_t DirectoryProofPlan::group(std::string_view path) const {
#if defined(__APPLE__)
    auto slash = path.rfind('/');
    if (slash == std::string_view::npos)
        return size_t(-1);
    auto parent = path.substr(0, slash ? slash : 1);
    const auto& groups = impl->groups;
    auto group = std::lower_bound(groups.begin(), groups.end(), parent,
                                 [](const auto& a, auto b) { return a.parent < b; });
    if (group == groups.end() || group->parent != parent)
        return size_t(-1);
    auto leaf = path.substr(slash + 1);
    uint32_t name;
    if (leaf.size() > NAME_MAX || !fingerprint(leaf, name))
        return size_t(-1);
    return std::binary_search(group->names.begin(), group->names.end(), name)
               ? size_t(group - groups.begin()) : size_t(-1);
#else
    (void)path;
#endif
    return size_t(-1);
}
bool DirectoryProofPlan::Session::covers(size_t group) const {
#if defined(__APPLE__)
    return group < state->leases.size() && state->leases[group].fd >= 0;
#else
    (void)group;
    return false;
#endif
}
bool DirectoryProofPlan::Session::covers(std::string_view path) const {
    return covers(state->owner.group(path));
}
std::optional<int> DirectoryProofPlan::Session::directory(std::string_view parent) const {
#if defined(__APPLE__)
    const auto& groups = state->plan.groups;
    auto group = std::lower_bound(groups.begin(), groups.end(), parent,
                                 [](const auto& a, auto b) { return a.parent < b; });
    if (group != groups.end() && group->parent == parent) {
        const auto& lease = state->leases[size_t(group - groups.begin())];
        if (lease.fd >= 0 && lease.resolved == parent)
            return lease.fd;
    }
#else
    (void)parent;
#endif
    return {};
}
bool DirectoryProofPlan::Session::valid(const lsp::RequestContext& ctx) const {
#if defined(__APPLE__)
    for (size_t i = 0; i < state->leases.size(); ++i) {
        ctx.checkpoint("before missing input directory recheck");
        if (state->leases[i].fd >= 0 && !state->leases[i].current(state->plan.groups[i]))
            return false;
    }
#else
    (void)ctx;
#endif
    return true;
}
} // namespace server
