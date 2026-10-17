// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "diagnostics/DiagnosticProcess.h"
#include "diagnostics/DiagnosticWorker.h"
#include <catch2/catch_session.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#ifndef _WIN32
#    include <unistd.h>
#endif

#include "slang/diagnostics/Diagnostics.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/BumpAllocator.h"
#include "slang/util/OS.h"

#ifdef _WIN32
#    include <windows.h>
#endif

namespace slang {

BumpAllocator alloc;
Diagnostics diagnostics;

} // namespace slang

// --------------------------------------------------
// Optionally parse a custom flag with Catch2
// --------------------------------------------------

bool g_updateGoldenFlag = false;

// We can parse the flag from the command line here
int main(int argc, char** argv) {
    try {
        isolated_diagnostics::initializeDiagnosticProcessPlatform();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Cannot initialize diagnostic process safety: %s\n", error.what());
        return 1;
    }
    // The test executable can hold a real child after private analysis creation,
    // immediately before semantic collection. Shipping CLI has no gate override.
    if (argc == 2 && std::string_view(argv[1]) == "--diagnostic-worker") {
        server::runDiagnosticWorker([] {
#ifndef _WIN32
            if (const char* gate = std::getenv("SLANG_DIAGNOSTIC_TEST_GATE")) {
                std::ofstream(std::string(gate) + ".ready") << getpid();
                while (!std::filesystem::exists(std::string(gate) + ".release"))
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
#endif
        });
    }
    slang::OS::setupConsole();
    slang::syntax::SyntaxTree::getDefaultSourceManager().setDisableProximatePaths(true);

// Let slang-server know we're running tests
#ifdef _WIN32
    SetEnvironmentVariable(TEXT("SLANG_SERVER_TESTS"), TEXT("YES"));
#else
    setenv("SLANG_SERVER_TESTS", "YES", true);
#endif

    // We create a Catch2 session
    Catch::Session session;

    // Build a custom CLI parser
    using namespace Catch::Clara;
    auto cli = session.cli() | Opt(g_updateGoldenFlag)["--update"](
                                   "Update golden files instead of failing the test on mismatch");

    // Now we update the session's CLI
    session.cli(cli);

    // Let Catch parse the command line
    int returnCode = session.applyCommandLine(argc, argv);
    if (returnCode != 0) {
        return returnCode; // command line error
    }

    // Run tests
    return session.run();
}

// --------------------------------------------------
// Example function under test
// --------------------------------------------------
std::string generateOutput() {
    return "Line 1\nLine 2\nLine 3\n";
    // If we add or remove lines, it will fail unless we run --update
}
