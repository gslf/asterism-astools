# tests/CMakeLists.txt — astools unit + integration tests.
#
# Every test_*.c is a standalone executable over astools_static (+xcdn).
# test_json additionally compiles mcp/json.c; test_regex compiles
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
  test_sandbox
  test_invoke
  test_escape
)

# Tests that use the fakes.c fixture writers.
set(ASTOOLS_TESTS_WITH_FAKES
  test_policy
  test_lockfile
  test_catalog
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
  if(t STREQUAL "test_json")
    list(APPEND t_sources ${CMAKE_CURRENT_SOURCE_DIR}/../mcp/json.c)
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

target_include_directories(test_json PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../mcp)
target_include_directories(test_regex PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../std)

# The invocation suites spawn the fake tool by absolute path.
foreach(t test_invoke test_escape)
  target_compile_definitions(${t} PRIVATE
    "ASTOOLS_TEST_TOOL_PATH=\"$<TARGET_FILE:tool_fake>\"")
  add_dependencies(${t} tool_fake)
endforeach()

set_tests_properties(${ASTOOLS_TESTS} PROPERTIES
  TIMEOUT 60
  ENVIRONMENT "ASTOOLS_TEST_TOOL=$<TARGET_FILE:tool_fake>;ASTOOLS_JAIL=$<TARGET_FILE:astools-jail>")

# ── std-tool containment regressions (need the real astools-std packages) ────
if(ASTOOLS_BUILD_STD)
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
