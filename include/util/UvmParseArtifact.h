// SPDX-License-Identifier: MIT
#pragma once
#include "UvmArchiveIdentity.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#include "slang/util/Hash.h"
#include "slang/util/OS.h"
namespace server::uvm_artifact {
inline uint64_t checksum(std::string_view bytes) {
    return slang::hash<std::string_view>{}(bytes);
}
struct Data {
    std::string syntax;
    std::set<std::string> macroDependencies;
    std::string configuration;
};
inline Data read(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 128 * 1024 * 1024)
        throw std::runtime_error("UVM parse archive exceeds size limit");
    std::ifstream in(path, std::ios::binary);
    std::string identity, hash;
    std::getline(in, identity);
    std::getline(in, hash);
    if (identity != SLANG_UVM_ARCHIVE_IDENTITY)
        throw std::runtime_error(
            "UVM parse archive is incompatible with this server; reinstall matching resources");
    const auto start = in.tellg();
    const auto size = std::filesystem::file_size(path);
    if (start < 0 || uint64_t(start) > size)
        throw std::runtime_error("truncated UVM parse archive");
    std::string payload(size - uint64_t(start), '\0');
    in.read(payload.data(), std::streamsize(payload.size()));
    if (!in || hash != std::to_string(checksum(payload)))
        throw std::runtime_error("UVM parse archive is corrupt; reinstall matching resources");
    auto end = payload.find('\n');
    if (end == std::string::npos)
        throw std::runtime_error("missing UVM archive profile");
    Data result;
    std::istringstream names(payload.substr(0, end));
    std::string name;
    while (names >> name)
        result.macroDependencies.insert(name);
    payload.erase(0, end + 1);
    end = payload.find('\n');
    if (end == std::string::npos)
        throw std::runtime_error("missing UVM archive configuration");
    result.configuration = payload.substr(0, end);
    payload.erase(0, end + 1);
    result.syntax = std::move(payload);
    return result;
}
inline void write(const std::filesystem::path& path, std::string_view syntax,
                  const std::set<std::string>& dependencies, std::string_view configuration = {}) {
    std::string payload;
    for (auto& name : dependencies) {
        payload += name;
        payload += ' ';
    }
    payload += '\n';
    payload += configuration;
    payload += '\n';
    payload += syntax;
    auto temporary = path;
    static std::atomic<uint64_t> serial = 0;
    temporary += ".tmp-" + std::to_string(slang::OS::getpid()) + "-" + std::to_string(serial++);
    struct RemoveTemporary {
        std::filesystem::path path;
        ~RemoveTemporary() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{temporary};
    {
        std::ofstream out(temporary, std::ios::binary);
        out << SLANG_UVM_ARCHIVE_IDENTITY << '\n' << checksum(payload) << '\n';
        out.write(payload.data(), std::streamsize(payload.size()));
        if (!out)
            throw std::runtime_error("cannot write UVM parse archive");
    }
#ifdef _WIN32
    std::filesystem::remove(path);
#endif
    std::filesystem::rename(temporary, path);
}
} // namespace server::uvm_artifact
