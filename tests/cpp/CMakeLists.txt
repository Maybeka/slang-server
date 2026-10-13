# ~~~
# SPDX-FileCopyrightText: Hudson River Trading
# SPDX-License-Identifier: MIT
# ~~~

file(GLOB SERVER_TESTS "*.cpp")

file(GLOB TEST_UTILS "utils/*.cpp")

add_executable(server_unittests ${TEST_UTILS} main.cpp ${SERVER_TESTS})

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
target_compile_definitions(diagnostic_test_server PRIVATE SLANG_SERVER_TEST_DIAGNOSTIC_GATE)
target_link_libraries(diagnostic_test_server PRIVATE slang_server_obj_lib reflectcpp)
