#include "lsp/URI.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("URI Decode") {

    URI u("file:///x/%41%42%43/%20y.z");

#ifndef _WIN32
    CHECK(u.getPath() == "/x/ABC/ y.z");
#else
    CHECK(u.getPath() == R"(\x\ABC\ y.z)");
#endif
}

TEST_CASE("URI EmptyInput") {
    URI u("");

    CHECK(u.str().empty());
    CHECK(u.getPath().empty());
}

#ifdef _WIN32

TEST_CASE("URI WindowsDriveLetter") {
    SECTION("HexDecoded") {
        URI u("file:///c:/temp/file.txt");

        CHECK(u.str() == "file:///C:/temp/file.txt");
        CHECK(u.getPath() == "C:\\temp\\file.txt");
    }

    SECTION("HexEncoded") {
        URI u("file:///c%3A/temp/file.txt");

        CHECK(u.str() == "file:///C:/temp/file.txt");
        CHECK(u.getPath() == "C:\\temp\\file.txt");
    }
}

TEST_CASE("URI WindowsUNCPath") {
    SECTION("UNC Basic") {
        URI u("file://server/share/file.txt");

        CHECK(u.str() == "file://server/share/file.txt");
        CHECK(u.getPath() == R"(\\server\share\file.txt)");
    }

    SECTION("UNC FromFile") {
        std::filesystem::path p(R"(\\server\share\file.txt)");
        URI u = URI::fromFile(p);

        CHECK(u.str() == "file://server/share/file.txt");
        CHECK(u.getPath() == R"(\\server\share\file.txt)");
    }
}

#endif

TEST_CASE("File URI identity survives escaped path serialization") {
    const auto path = std::filesystem::absolute(std::filesystem::path(u8"源 %23#.sv"));
    auto uri = URI::fromFile(path);
    REQUIRE(uri.str().ends_with("%E6%BA%90%20%2523%23.sv"));
    CHECK(URI(uri.reflection()) == uri);
    auto alternate = uri.str();
    alternate.replace(alternate.find("%E6"), 3, "%e6");
    CHECK(std::hash<URI>{}(URI(alternate)) == std::hash<URI>{}(uri));
    CHECK(std::filesystem::path(URI(alternate).getPath()) == path);
    CHECK(URI(uri.str() + "?view=1#symbol").getPath() == uri.getPath());
}

TEST_CASE("File URI paths preserve every allowed byte and escape every other filename byte") {
    constexpr std::string_view literal =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/:";
    constexpr std::string_view hex = "0123456789ABCDEF";
    const std::string prefix = "/src/" + std::string(96, 'x') + "/";
    const std::string suffix = "/rtl/top.sv";
    for (unsigned byte = 1; byte <= 255; ++byte) {
        CAPTURE(byte);
        const char c = static_cast<char>(byte);
        std::string expected = "file://" + prefix;
        if (literal.find(c) != literal.npos)
            expected += c;
        else {
            expected += '%';
            expected += hex[byte >> 4];
            expected += hex[byte & 15];
        }
        CHECK(URI("file", "", prefix + c + suffix).str() == expected + suffix);
    }
}
