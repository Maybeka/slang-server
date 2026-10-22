// SPDX-License-Identifier: MIT
#include "ParserCounts.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "slang/parsing/Parser.h"
#include "slang/syntax/AllSyntax.h"

namespace {
std::atomic_uint64_t units{0};
}
void slang_server_parser_enter() {
    units.fetch_add(1, std::memory_order_relaxed);
}
slang::syntax::CompilationUnitSyntax& slang_server_record_parse(
    slang::syntax::CompilationUnitSyntax& result) {
    // A fixture observation, not a production parser heuristic. The full test
    // fixture has three roots; its mini source has exactly one package/class/method.
    bool mini = false;
    if (result.members.size() == 1) {
        mini = result.members.front()->as_if<slang::syntax::FunctionDeclarationSyntax>() != nullptr;
        auto* package = result.members.front()->as_if<slang::syntax::ModuleDeclarationSyntax>();
        if (package && package->kind == slang::syntax::SyntaxKind::PackageDeclaration &&
            package->members.size() == 1) {
            auto* cls = package->members.front()->as_if<slang::syntax::ClassDeclarationSyntax>();
            mini = cls && cls->items.size() == 1 &&
                   cls->items.front()->kind == slang::syntax::SyntaxKind::ClassMethodDeclaration;
        }
    }
    if (const char* path = std::getenv("SLANG_SERVER_PARSE_COUNT_LOG"))
        if (auto* file = std::fopen(path, "a")) {
            std::fprintf(file, "%ld,%s,%zu\n", static_cast<long>(getpid()),
                         mini ? "mini-shape" : "other", result.members.size());
            std::fclose(file);
        }
    return result;
}
#ifndef SLANG_SERVER_PARSER_SOURCE_INSTRUMENTATION
extern "C" slang::syntax::CompilationUnitSyntax&
__real__ZN5slang7parsing6Parser20parseCompilationUnitEv(slang::parsing::Parser*);
extern "C" slang::syntax::CompilationUnitSyntax&
__wrap__ZN5slang7parsing6Parser20parseCompilationUnitEv(slang::parsing::Parser* parser) {
    slang_server_parser_enter();
    return slang_server_record_parse(
        __real__ZN5slang7parsing6Parser20parseCompilationUnitEv(parser));
}
#endif
extern "C" uint64_t slang_server_parser_units() {
    return units.load(std::memory_order_relaxed);
}
