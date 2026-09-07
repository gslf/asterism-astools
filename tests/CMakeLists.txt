# tests/CMakeLists.txt — ⁂ astools unit + integration tests.
#
# Every test_*.c is a standalone executable over astools_static (+xcdn).
# test_json uses the shared transport codec; test_regex compiles
# std/regex.c. tool_fake is the scripted protocol tool driven by
# test_invoke / test_escape via the ASTOOLS_TEST_TOOL_PATH definition.

set(ASTOOLS_TESTS
  test_util
  test_time
  test_semver
  test_types
  test_manifest
  test_path
  test_policy
  test_lockfile
  test_callline
  test_json
  test_regex
  test_jscm
  test_catalog
  test_discovery
  test_project_receipt
  test_process_frame
  test_lsp_files
  test_mcp_contract
  test_sandbox
  test_invoke
  test_escape
)

# Tests that use the fakes.c fixture writers.
set(ASTOOLS_TESTS_WITH_FAKES
  test_policy
  test_lockfile
  test_catalog
  test_discovery
  test_invoke
  test_escape
)

# ── tool_fake: scripted protocol tool (xcdn only, like std) ─────────────────
add_executable(tool_fake tools/tool_fake.c)
target_include_directories(tool_fake PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../deps/xcdn-c/src)
target_compile_options(tool_fake PRIVATE ${ASTOOLS_C_FLAGS})
target_link_libraries(tool_fake PRIVATE xcdn)

# ── test executables ────────────────────────────────────────────────────────
foreach(t ${ASTOOLS_TESTS})
  set(t_sources ${t}.c)
  if(t IN_LIST ASTOOLS_TESTS_WITH_FAKES)
    list(APPEND t_sources fakes.c)
  endif()
  if(t STREQUAL "test_regex")
    list(APPEND t_sources ${CMAKE_CURRENT_SOURCE_DIR}/../std/regex.c)
  endif()

  add_executable(${t} ${t_sources})
  target_include_directories(${t} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/xcdn-c/src)
  target_compile_options(${t} PRIVATE ${ASTOOLS_C_FLAGS})
  target_link_libraries(${t} PRIVATE astools_static xcdn)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

target_include_directories(test_regex PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../std)

# The invocation suites spawn the fake tool by absolute path.
foreach(t test_invoke test_escape)
  target_compile_definitions(${t} PRIVATE
    "ASTOOLS_TEST_TOOL_PATH=\"$<TARGET_FILE:tool_fake>\"")
  add_dependencies(${t} tool_fake)
endforeach()

set(ASTOOLS_TEST_ENV "ASTOOLS_TEST_TOOL=$<TARGET_FILE:tool_fake>")
if(TARGET astools-jail)
  list(APPEND ASTOOLS_TEST_ENV "ASTOOLS_JAIL=$<TARGET_FILE:astools-jail>")
endif()



if(TARGET astools-mcp)
  add_test(NAME test_mcp_protocol
    COMMAND ${CMAKE_COMMAND}
      "-DMCP_EXE=$<TARGET_FILE:astools-mcp>"
      "-DMCP_ROOT_FLAG=--workspace"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/mcp_smoke.cmake")
  set_tests_properties(test_mcp_protocol PROPERTIES TIMEOUT 60)
endif()
set_tests_properties(${ASTOOLS_TESTS} PROPERTIES
  TIMEOUT 60
  ENVIRONMENT "${ASTOOLS_TEST_ENV}")

# ── std-tool containment regressions (need the real astools-std packages) ────
if(ASTOOLS_BUILD_STD)
  # The child environment does not inherit PATH; explicitly admit the test toolchain.
  get_filename_component(ASTOOLS_CMAKE_BIN "${CMAKE_COMMAND}" DIRECTORY)
  set(ASTOOLS_TEST_CMAKE_BIN "${ASTOOLS_CMAKE_BIN}" CACHE PATH "CMake/CTest directory for project fixtures")
  add_executable(test_std_semantic test_std_semantic.c)
  target_include_directories(test_std_semantic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/xcdn-c/src)
  target_compile_options(test_std_semantic PRIVATE ${ASTOOLS_C_FLAGS})
  target_compile_definitions(test_std_semantic PRIVATE
    "ASTOOLS_STD_PACKAGES=\"${CMAKE_BINARY_DIR}/packages\""
    "ASTOOLS_TEST_CMAKE_BIN=\"${ASTOOLS_TEST_CMAKE_BIN}\"")
  target_link_libraries(test_std_semantic PRIVATE astools_static xcdn)
  add_dependencies(test_std_semantic astools_packages)
  add_test(NAME test_std_semantic COMMAND test_std_semantic)
  set_tests_properties(test_std_semantic PROPERTIES TIMEOUT 60)
endif()

if(ASTOOLS_BUILD_STD AND NOT WIN32)
  add_executable(test_std_containment test_std_containment.c)
  target_include_directories(test_std_containment PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/xcdn-c/src)
  target_compile_options(test_std_containment PRIVATE ${ASTOOLS_C_FLAGS})
  target_compile_definitions(test_std_containment PRIVATE
    "ASTOOLS_STD_PACKAGES=\"${CMAKE_BINARY_DIR}/packages\"")
  target_link_libraries(test_std_containment PRIVATE astools_static xcdn)
  add_dependencies(test_std_containment astools_packages)
  add_test(NAME test_std_containment COMMAND test_std_containment)
  set_tests_properties(test_std_containment PROPERTIES TIMEOUT 60)
endif()

target_sources(test_project_receipt PRIVATE ../std/project_receipt.c)

if(NOT WIN32 AND NOT ASTOOLS_NO_THREADS)
  add_executable(tool_persistent tools/tool_persistent.c)
  target_include_directories(tool_persistent PRIVATE ../deps/xcdn-c/src)
  target_compile_options(tool_persistent PRIVATE ${ASTOOLS_C_FLAGS})
  target_link_libraries(tool_persistent PRIVATE xcdn)
  add_executable(test_persistent test_persistent.c fakes.c)
  target_include_directories(test_persistent PRIVATE . ../include ../src ../deps/xcdn-c/src)
  target_compile_options(test_persistent PRIVATE ${ASTOOLS_C_FLAGS})
  target_compile_definitions(test_persistent PRIVATE
    "ASTOOLS_TEST_PERSISTENT_PATH=\"$<TARGET_FILE:tool_persistent>\"")
  target_link_libraries(test_persistent PRIVATE astools_static xcdn)
  add_dependencies(test_persistent tool_persistent)
  add_test(NAME test_persistent COMMAND test_persistent)
  set_tests_properties(test_persistent PROPERTIES TIMEOUT 30 ENVIRONMENT "${ASTOOLS_TEST_ENV}")
endif()

if(NOT WIN32)
  add_library(tool_library SHARED tools/tool_library.c)
  target_include_directories(tool_library PRIVATE ../include)
  target_compile_options(tool_library PRIVATE ${ASTOOLS_C_FLAGS})
  add_executable(test_library test_library.c fakes.c)
  target_include_directories(test_library PRIVATE . ../include ../src ../deps/xcdn-c/src)
  target_compile_options(test_library PRIVATE ${ASTOOLS_C_FLAGS})
  target_compile_definitions(test_library PRIVATE
    "ASTOOLS_TEST_LIBRARY_PATH=\"$<TARGET_FILE:tool_library>\"")
  target_link_libraries(test_library PRIVATE astools_static xcdn)
  add_dependencies(test_library tool_library)
  add_test(NAME test_library COMMAND test_library)
  set_tests_properties(test_library PROPERTIES TIMEOUT 20 ENVIRONMENT "${ASTOOLS_TEST_ENV}")
endif()

if(NOT WIN32 AND NOT ASTOOLS_NO_THREADS)
  add_executable(tool_lsp tools/tool_lsp.c)
  target_include_directories(tool_lsp PRIVATE ../src)
  target_compile_options(tool_lsp PRIVATE ${ASTOOLS_C_FLAGS})
  target_link_libraries(tool_lsp PRIVATE astools_static)
  add_executable(test_lsp test_lsp.c fakes.c)
  target_include_directories(test_lsp PRIVATE . ../include ../src ../deps/xcdn-c/src)
  target_compile_options(test_lsp PRIVATE ${ASTOOLS_C_FLAGS})
  target_compile_definitions(test_lsp PRIVATE "ASTOOLS_TEST_LSP_PATH=\"$<TARGET_FILE:tool_lsp>\"")
  target_link_libraries(test_lsp PRIVATE astools_static xcdn)
  add_dependencies(test_lsp tool_lsp)
  add_test(NAME test_lsp COMMAND test_lsp)
  set_tests_properties(test_lsp PROPERTIES TIMEOUT 30 ENVIRONMENT "${ASTOOLS_TEST_ENV}")
  add_executable(tool_mcp tools/tool_mcp.c)
  target_include_directories(tool_mcp PRIVATE ../src)
  target_compile_options(tool_mcp PRIVATE ${ASTOOLS_C_FLAGS})
  target_link_libraries(tool_mcp PRIVATE astools_static)
  add_executable(test_mcp_client test_mcp_client.c fakes.c)
  target_include_directories(test_mcp_client PRIVATE . ../include ../src ../deps/xcdn-c/src)
  target_compile_options(test_mcp_client PRIVATE ${ASTOOLS_C_FLAGS})
  target_compile_definitions(test_mcp_client PRIVATE "ASTOOLS_TEST_MCP_PATH=\"$<TARGET_FILE:tool_mcp>\"")
  target_link_libraries(test_mcp_client PRIVATE astools_static xcdn)
  add_dependencies(test_mcp_client tool_mcp)
  add_test(NAME test_mcp_client COMMAND test_mcp_client)
  set_tests_properties(test_mcp_client PROPERTIES TIMEOUT 30 ENVIRONMENT "${ASTOOLS_TEST_ENV}")
endif()

if(NOT WIN32 AND NOT ASTOOLS_NO_THREADS)
  find_program(ASTOOLS_CLANGD_TEST_EXECUTABLE NAMES clangd)
  if(APPLE AND ASTOOLS_CLANGD_TEST_EXECUTABLE STREQUAL "/usr/bin/clangd")
    # /usr/bin contains Xcode dispatch shims, not the provider itself. Resolve
    # before entering Seatbelt: only the selected entry binary is admitted.
    execute_process(COMMAND xcrun --find clangd
      RESULT_VARIABLE ASTOOLS_CLANGD_LOOKUP_STATUS
      OUTPUT_VARIABLE ASTOOLS_CLANGD_RESOLVED
      ERROR_VARIABLE ASTOOLS_CLANGD_LOOKUP_ERROR
      TIMEOUT 15
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT ASTOOLS_CLANGD_LOOKUP_STATUS STREQUAL "0" OR
       NOT IS_ABSOLUTE "${ASTOOLS_CLANGD_RESOLVED}" OR
       NOT EXISTS "${ASTOOLS_CLANGD_RESOLVED}" OR
       ASTOOLS_CLANGD_RESOLVED STREQUAL "/usr/bin/clangd")
      message(FATAL_ERROR "Cannot resolve the Xcode clangd provider: ${ASTOOLS_CLANGD_LOOKUP_ERROR}")
    endif()
    set(ASTOOLS_CLANGD_TEST_EXECUTABLE "${ASTOOLS_CLANGD_RESOLVED}")
  endif()
  if(ASTOOLS_CLANGD_TEST_EXECUTABLE)
    message(STATUS "LSP conformance provider: ${ASTOOLS_CLANGD_TEST_EXECUTABLE}")
    add_executable(test_lsp_clangd test_lsp_clangd.c fakes.c)
    target_include_directories(test_lsp_clangd PRIVATE . ../include ../src ../deps/xcdn-c/src)
    target_compile_options(test_lsp_clangd PRIVATE ${ASTOOLS_C_FLAGS})
    target_compile_definitions(test_lsp_clangd PRIVATE "ASTOOLS_CLANGD_PATH=\"${ASTOOLS_CLANGD_TEST_EXECUTABLE}\"")
    target_link_libraries(test_lsp_clangd PRIVATE astools_static xcdn)
    add_test(NAME test_lsp_clangd COMMAND test_lsp_clangd)
    set_tests_properties(test_lsp_clangd PROPERTIES TIMEOUT 45 ENVIRONMENT "${ASTOOLS_TEST_ENV}")
  else()
    message(STATUS "clangd unavailable: real LSP conformance test skipped")
  endif()
endif()

if(TARGET test_lsp_clangd AND TARGET astools-check AND TARGET astools-mcp)
  find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME test_lsp_package COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_lsp_package.py
      --checker $<TARGET_FILE:astools-check> --mcp $<TARGET_FILE:astools-mcp> --clangd ${ASTOOLS_CLANGD_TEST_EXECUTABLE})
    set_tests_properties(test_lsp_package PROPERTIES TIMEOUT 45 ENVIRONMENT "${ASTOOLS_TEST_ENV}")
  else()
    message(STATUS "Python 3.11 unavailable: optional clangd package smoke test skipped")
  endif()
endif()

if(TARGET tool_mcp AND TARGET astools-check AND TARGET astools-mcp)
  find_package(Python3 3.11 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME test_mcp_package COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_package.py
      --checker $<TARGET_FILE:astools-check> --mcp $<TARGET_FILE:astools-mcp> --server $<TARGET_FILE:tool_mcp>)
    set_tests_properties(test_mcp_package PROPERTIES TIMEOUT 60 ENVIRONMENT "${ASTOOLS_TEST_ENV}")
  endif()
endif()
