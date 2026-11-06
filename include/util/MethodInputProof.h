// SPDX-License-Identifier: MIT
#pragma once
#include "util/DirectoryProofPlan.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace server::method_input {
struct ReadResult {
    std::optional<std::string> contents;
    bool matches = false;
};

/// Invocation-local access to one directory; never a successful input witness.
class Directories {
public:
    explicit Directories(const DirectoryProofPlan::Session* session = nullptr) : session(session) {}
    Directories(const Directories&) = delete;
    Directories& operator=(const Directories&) = delete;
    ~Directories();

private:
    friend ReadResult readCertificateFile(const std::string&, bool, const lsp::RequestContext&,
                                          const std::optional<std::string>*, Directories*);
#if defined(__APPLE__)
    std::optional<int> openInput(const std::string& path);
#endif
    void clear();
#if defined(__APPLE__)
    void releaseOnExhaustion();
    std::string parent;
    int descriptor = -1;
    bool enabled = true;
    bool owned = true;
#endif
    [[maybe_unused]] const DirectoryProofPlan::Session* session;
};

ReadResult readCertificateFile(const std::string&, bool fastPath, const lsp::RequestContext&,
                               const std::optional<std::string>* expected = nullptr,
                               Directories* directories = nullptr);
std::vector<std::string> directoryEntries(const std::filesystem::path&);
} // namespace server::method_input
