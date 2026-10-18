// SPDX-License-Identifier: MIT
#pragma once

namespace slang::syntax {
class CompilationUnitSyntax;
}

void slang_server_parser_enter();
slang::syntax::CompilationUnitSyntax& slang_server_record_parse(
    slang::syntax::CompilationUnitSyntax&);
