# ~~~
# SPDX-FileCopyrightText: Hudson River Trading
# SPDX-License-Identifier: MIT
# ~~~

file(GLOB SERVER_TESTS "*.cpp")

file(GLOB TEST_UTILS "utils/*.cpp")

add_executable(server_unittests ${TEST_UTILS} main.cpp ${SERVER_TESTS})
stageBundledUvm(server_unittests)

target_link_libraries(server_unittests PRIVATE slang_server_obj_lib)

target_include_directories(
  server_unittests SYSTEM
  PRIVATE ${PROJECT_SOURCE_DIR}/external/reflect-cpp/include)
target_link_libraries(server_unittests PRIVATE reflectcpp)

target_link_libraries(server_unittests PRIVATE slang::slang Catch2::Catch2)
target_include_directories(
  server_unittests PRIVATE ${PROJECT_SOURCE_DIR}/external/slang/tests/unittests)

include(Catch)
catch_discover_tests(server_unittests)

add_custom_command(
  TARGET server_unittests
  POST_BUILD
  COMMAND
    ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../data
    ${CMAKE_CURRENT_BINARY_DIR}/../data)

add_custom_command(
  TARGET server_unittests
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/golden
          ${CMAKE_CURRENT_BINARY_DIR}/golden)

if(CMAKE_SYSTEM_NAME MATCHES "Windows")
  target_sources(server_unittests
                 PRIVATE ${PROJECT_SOURCE_DIR}/scripts/win32.manifest)
endif()

# Same LSP entry point with a test-only held-child hook. Never installed.
add_executable(diagnostic_test_server ${PROJECT_SOURCE_DIR}/src/server_main.cpp)
stageBundledUvm(diagnostic_test_server)
target_compile_definitions(diagnostic_test_server PRIVATE SLANG_SERVER_TEST_DIAGNOSTIC_GATE)
target_link_libraries(diagnostic_test_server PRIVATE slang_server_obj_lib reflectcpp)

# Count parser entry points only in this test executable. Production and timing
# executables share the ordinary objects and never link the wrapper.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_executable(slang_server_counted ${PROJECT_SOURCE_DIR}/src/server_main.cpp
                 ${PROJECT_SOURCE_DIR}/tests/instrumentation/ParserCounts.cpp)
  target_link_libraries(slang_server_counted PRIVATE slang_server_obj_lib reflectcpp)
  target_link_options(slang_server_counted PRIVATE
    "-Wl,--wrap=_ZN5slang7parsing6Parser20parseCompilationUnitEv")
elseif(APPLE AND NOT BUILD_SHARED_LIBS)
  set(counted_parser "${CMAKE_CURRENT_BINARY_DIR}/CountedParserMembers.cpp")
  add_custom_command(
    OUTPUT "${counted_parser}"
    COMMAND "${CMAKE_COMMAND}"
      "-DINPUT=${PROJECT_SOURCE_DIR}/external/slang/source/parsing/Parser_members.cpp"
      "-DOUTPUT=${counted_parser}"
      -P "${PROJECT_SOURCE_DIR}/tests/instrumentation/prepare-counted-parser.cmake"
    DEPENDS
      "${PROJECT_SOURCE_DIR}/external/slang/source/parsing/Parser_members.cpp"
      "${PROJECT_SOURCE_DIR}/tests/instrumentation/prepare-counted-parser.cmake"
    VERBATIM)
  add_executable(slang_server_counted ${PROJECT_SOURCE_DIR}/src/server_main.cpp
    ${PROJECT_SOURCE_DIR}/tests/instrumentation/ParserCounts.cpp "${counted_parser}")
  target_compile_definitions(slang_server_counted PRIVATE
    SLANG_SERVER_PARSER_SOURCE_INSTRUMENTATION)
  target_include_directories(slang_server_counted PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/instrumentation")
  target_link_libraries(slang_server_counted PRIVATE slang_server_obj_lib reflectcpp)
endif()
if(TARGET slang_server_counted)
  stageBundledUvm(slang_server_counted)
endif()
