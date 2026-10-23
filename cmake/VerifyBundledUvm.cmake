# SPDX-License-Identifier: MIT
# Verify the pinned upstream source inventory before staging or installing it.
set(SLANG_SERVER_BUNDLED_UVM_SOURCE "${PROJECT_SOURCE_DIR}/resources/uvm/1.2")
file(READ "${SLANG_SERVER_BUNDLED_UVM_SOURCE}/MANIFEST.json" bundled_manifest)
string(JSON bundled_count LENGTH "${bundled_manifest}" files)
math(EXPR bundled_last "${bundled_count} - 1")
set(bundled_expected "MANIFEST.json")
foreach(i RANGE 0 ${bundled_last})
  string(JSON bundled_file MEMBER "${bundled_manifest}" files ${i})
  string(JSON bundled_hash GET "${bundled_manifest}" files "${bundled_file}" sha256)
  file(SHA256 "${SLANG_SERVER_BUNDLED_UVM_SOURCE}/${bundled_file}" bundled_actual_hash)
  if(NOT bundled_actual_hash STREQUAL bundled_hash)
    message(FATAL_ERROR "Bundled UVM inventory mismatch: ${bundled_file}")
  endif()
  list(APPEND bundled_expected "${bundled_file}")
endforeach()
file(GLOB_RECURSE bundled_actual LIST_DIRECTORIES false
     RELATIVE "${SLANG_SERVER_BUNDLED_UVM_SOURCE}"
     "${SLANG_SERVER_BUNDLED_UVM_SOURCE}/*")
list(SORT bundled_expected)
list(SORT bundled_actual)
if(NOT bundled_expected STREQUAL bundled_actual)
  message(FATAL_ERROR "Bundled UVM source inventory contains missing or extra files")
endif()

get_filename_component(bundled_install_bin "${CMAKE_INSTALL_BINDIR}" ABSOLUTE
                       BASE_DIR "${CMAKE_INSTALL_PREFIX}")
get_filename_component(bundled_install_data
                       "${CMAKE_INSTALL_DATADIR}/slang-server/uvm/1.2" ABSOLUTE
                       BASE_DIR "${CMAKE_INSTALL_PREFIX}")
file(RELATIVE_PATH SLANG_SERVER_BUNDLED_UVM_RELATIVE_PATH
     "${bundled_install_bin}" "${bundled_install_data}")
# Resolve the actual target directory at build time, including configuration
# expressions and per-target output directories. Run on every requested target
# build so removed or changed resources are staged even when linking is skipped.
function(stageBundledUvm target)
  add_custom_target(
    ${target}_bundled_uvm
    COMMAND
      "${CMAKE_COMMAND}" -E copy_directory "${SLANG_SERVER_BUNDLED_UVM_SOURCE}"
      "$<TARGET_FILE_DIR:${target}>/${SLANG_SERVER_BUNDLED_UVM_RELATIVE_PATH}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
      "$<TARGET_FILE_DIR:${target}>/${SLANG_SERVER_BUNDLED_UVM_RELATIVE_PATH}/preparsed/no-dpi.cst"
      "$<TARGET_FILE_DIR:${target}>/${SLANG_SERVER_BUNDLED_UVM_RELATIVE_PATH}/preparsed/resource-converter.cst"
      "$<TARGET_FILE_DIR:${target}>/${SLANG_SERVER_BUNDLED_UVM_RELATIVE_PATH}/preparsed/no-dpi-resource-converter.cst"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "$<TARGET_FILE_DIR:${target}>/${SLANG_SERVER_BUNDLED_UVM_RELATIVE_PATH}/preparsed"
    COMMAND "${CMAKE_COMMAND}" -E copy "${SLANG_SERVER_UVM_ARCHIVE_DIR}/default.cst"
      "$<TARGET_FILE_DIR:${target}>/${SLANG_SERVER_BUNDLED_UVM_RELATIVE_PATH}/preparsed/default.cst"
    VERBATIM)
  add_dependencies(${target}_bundled_uvm bundled_uvm_preparsed)
  # Test and production executables can share one runtime directory. Serialize
  # staging writers so parallel builds cannot remove/replace the same UVM files.
  get_property(previous_uvm_stage GLOBAL PROPERTY SLANG_SERVER_PREVIOUS_UVM_STAGE)
  if(previous_uvm_stage)
    add_dependencies(${target}_bundled_uvm ${previous_uvm_stage})
  endif()
  set_property(GLOBAL PROPERTY SLANG_SERVER_PREVIOUS_UVM_STAGE ${target}_bundled_uvm)
  add_dependencies(${target} ${target}_bundled_uvm)
endfunction()
