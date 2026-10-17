//------------------------------------------------------------------------------
// server_main.cpp
// Generates C++ headers for SystemVerilog types
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#ifdef _WIN32
#    include <fcntl.h>
#    include <io.h>
#endif

#include "SlangServer.h"
#include "diagnostics/DiagnosticProcess.h"
#include "diagnostics/DiagnosticWorker.h"
#include <cstdio>
#ifdef SLANG_SERVER_TEST_DIAGNOSTIC_GATE
#    include <chrono>
#    include <cstdlib>
#    include <fstream>
#    include <thread>
#endif
#include <fmt/format.h>
#include <rfl/DefaultIfMissing.hpp>

#include "slang/util/CommandLine.h"
#include "slang/util/VersionInfo.h"

using namespace slang;
using namespace server;

int main(int argc, char** argv) {
    try {
        isolated_diagnostics::initializeDiagnosticProcessPlatform();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Cannot initialize diagnostic process safety: %s\n", error.what());
        return 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--diagnostic-worker") {
#ifdef SLANG_SERVER_TEST_DIAGNOSTIC_GATE
        runDiagnosticWorker([] {
            if (const char* gate = std::getenv("SLANG_DIAGNOSTIC_TEST_GATE")) {
                std::ofstream(std::string(gate) + ".ready") << "private semantic ready";
                while (!std::filesystem::exists(std::string(gate) + ".release"))
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
#else
        runDiagnosticWorker();
#endif
    }

#ifdef _WIN32
    // By default windows accesses streams in text mode. This mostly means that
    // line feeds are converted to carriage return-line feed (CRLF), this messes
    // up the language server protocol. The combat this to solution is to explicitly
    // set the IO line feed to use binary mode instead of text mode.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    OS::setupConsole();

    CommandLine cmdline;

    std::optional<bool> showHelp;
    cmdline.add("-h,--help", showHelp, "Display available options");

    std::optional<bool> showVersion;
    cmdline.add("--version", showVersion, "Display version information and exit");

    std::optional<bool> configSchema;
    cmdline.add("--config-schema", configSchema, "Print json schema of config file and exit");

    cmdline.parse(argc, argv);

    if (showHelp == true) {
        OS::print(cmdline.getHelpText("Slang Language Server"));
        return 0;
    }

    if (showVersion == true) {
        OS::print(fmt::format("slang-server version {}.{}.{}+{}\n", VersionInfo::getMajor(),
                              VersionInfo::getMinor(), VersionInfo::getPatch(),
                              VersionInfo::getHash()));
        return 0;
    }

    if (configSchema == true) {
        try {
            const std::string schema = rfl::json::to_schema<Config, rfl::DefaultIfMissing>(
                rfl::json::pretty | YYJSON_WRITE_PRETTY_TWO_SPACES
                // Add this when comment support is added
                // , "@generated from `include/Config.h`"
            );
            OS::print(schema);
            OS::print("\n");
        }
        catch (const std::exception& e) {
            OS::print(fmt::format("Error generating config schema: {}\n", e.what()));
            return 1;
        }
        return 0;
    }
    SlangLspClient client;
    SlangServer server(client);
#if defined(__linux__) || defined(__APPLE__)
    try {
        server.enableIsolatedDiagnostics(isolated_diagnostics::currentDiagnosticExecutable());
    }
    catch (const std::exception& error) {
        // LSP stdout is protocol-only even before initialization.
        std::fprintf(stderr, "Isolated diagnostics unavailable: %s\n", error.what());
    }
#endif
    server.run();

    return 0;
}
