# SPDX-License-Identifier: MIT
# Darwin's linker has no GNU --wrap. Compile a generated copy of the one
# translation unit into the counted executable, ahead of the static SDK archive.
# Its definitions satisfy all Parser_members.cpp symbols, so the original archive
# member is not extracted. Never mutate the SDK or ordinary/timing objects.
file(READ "${INPUT}" parser)
set(entry "CompilationUnitSyntax& Parser::parseCompilationUnit() {")
set(normal "return factory.compilationUnit(members, meta.eofToken);")
set(recovery "return factory.compilationUnit(nullptr, meta.eofToken);")
foreach(anchor IN ITEMS "${entry}" "${normal}" "${recovery}")
  string(FIND "${parser}" "${anchor}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Pinned SDK parser changed; refusing partial instrumentation")
  endif()
endforeach()
string(REPLACE "${entry}" "${entry}\n    ::slang_server_parser_enter();" parser "${parser}")
string(REPLACE "${normal}"
  "return ::slang_server_record_parse(factory.compilationUnit(members, meta.eofToken));"
  parser "${parser}")
string(REPLACE "${recovery}"
  "return ::slang_server_record_parse(factory.compilationUnit(nullptr, meta.eofToken));"
  parser "${parser}")
file(WRITE "${OUTPUT}" "#include \"ParserCounts.h\"\n${parser}")
