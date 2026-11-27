#include "lsp/URI.h"

#include <algorithm>
#include <cctype>
#include <ctre.hpp>

namespace {
constexpr auto uriPattern = ctll::fixed_string{
    R"(^(([^:/?#]+):)?(//([^/?#]*))?([^?#]*)(\?([^#]*))?(#(.*))?$)"};

int hexValue(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

std::string decode(std::string_view input) {
    if (input.find('%') == input.npos)
        return std::string(input);
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '%') {
            result += input[i];
            continue;
        }
        if (i + 2 >= input.size())
            throw std::invalid_argument("Incomplete URI escape");
        int high = hexValue(input[i + 1]);
        int low = hexValue(input[i + 2]);
        if (high < 0 || low < 0 || (high == 0 && low == 0))
            throw std::invalid_argument("Invalid URI escape");
        result += static_cast<char>(high * 16 + low);
        i += 2;
    }
    return result;
}

constexpr bool isUnreserved(unsigned char c) {
    const unsigned lower = c | 0x20;
    const unsigned alphanumeric = (lower >= 'a' && lower <= 'z') || (c >= '0' && c <= '9');
    const unsigned punctuation = c == '-' || c == '.' || c == '_' || c == '~';
    return (alphanumeric | punctuation) != 0;
}

bool filePathNeedsEscaping(std::string_view path) {
    // A reduction lets the compiler check ordinary source paths several bytes at a time.
    unsigned needsEscaping = 0;
    for (unsigned char c : path)
        needsEscaping |= !(isUnreserved(c) || c == '/' || c == ':');
    return needsEscaping != 0;
}

void appendEncoded(std::string& uri, std::string_view text, std::string_view delimiters = {}) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    for (unsigned char c : text) {
        if (isUnreserved(c) || delimiters.find(c) != text.npos) {
            uri += static_cast<char>(c);
        }
        else {
            uri += '%';
            uri += digits[c >> 4];
            uri += digits[c & 15];
        }
    }
}
} // namespace

URI::URI(const std::string& input) {
    if (input.empty())
        return;
    auto m = ctre::match<uriPattern>(input);
    if (!m)
        throw std::invalid_argument("Invalid URI");
    auto path = decode(m.get<5>().to_view());
    if (m.get<2>().to_view() == "file") {
        setFilePath(decode(m.get<4>().to_view()), std::move(path));
        underlying_ += m.get<6>().to_view();
        underlying_ += m.get<8>().to_view();
    }
    else {
        underlying_ = input;
        fsPath_ = std::move(path);
    }
}

void URI::setFilePath(std::string_view authority, std::string path) {
    if (path.empty() || path.front() != '/')
        path.insert(path.begin(), '/');
#ifdef _WIN32
    if (path.size() >= 3 && path[2] == ':')
        path[1] = static_cast<char>(std::toupper(static_cast<unsigned char>(path[1])));
#endif
    underlying_.reserve(7 + authority.size() + path.size());
    underlying_ = "file://";
    appendEncoded(underlying_, authority, "[]:");
    if (filePathNeedsEscaping(path))
        appendEncoded(underlying_, path, "/:");
    else
        underlying_ += path;
#ifdef _WIN32
    if (path.size() >= 3 && path[2] == ':')
        path.erase(0, 1);
    std::replace(path.begin(), path.end(), '/', '\\');
    if (!authority.empty())
        path = "\\\\" + std::string(authority) + path;
#endif
    fsPath_ = std::move(path);
}

URI::URI(std::string scheme, std::string authority, std::string path, std::string query,
         std::string fragment) {
    if (scheme == "file") {
        setFilePath(authority, std::move(path));
    }
    else {
        if (!scheme.empty())
            underlying_ = scheme + ":";
        if (!authority.empty()) {
            underlying_ += "//";
            appendEncoded(underlying_, authority, "[]:@");
        }
        appendEncoded(underlying_, path, "/:");
        fsPath_ = std::move(path);
    }
    if (!query.empty()) {
        underlying_ += '?';
        appendEncoded(underlying_, query, "/?:@&=+");
    }
    if (!fragment.empty()) {
        underlying_ += '#';
        appendEncoded(underlying_, fragment, "/?:@");
    }
}

URI URI::fromFile(const std::filesystem::path& file) {
    if (file.empty())
        return {};
    auto path = (file.is_absolute() ? file : std::filesystem::absolute(file)).generic_string();
    std::string authority;
#ifdef _WIN32
    if (path.starts_with("//")) {
        auto end = path.find('/', 2);
        authority = path.substr(2, end == path.npos ? end : end - 2);
        path = end == path.npos ? "/" : path.substr(end);
    }
#endif
    return URI("file", std::move(authority), std::move(path));
}

URI URI::fromWeb(const std::string& url) {
    return URI("https://" + url);
}

std::string_view URI::getPath() const {
    return fsPath_;
}
URI::ReflectionType URI::reflection() const {
    return underlying_;
}
std::string URI::str() const {
    return underlying_;
}
bool URI::operator==(const URI& other) const {
    return underlying_ == other.underlying_;
}
