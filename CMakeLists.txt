cmake_minimum_required(VERSION 3.16)
project(astools C)

# ── Options (§13.3) ──────────────────────────────────────────────────────────
option(ASTOOLS_BUILD_MCP    "Build the astools-mcp server"        ON)
option(ASTOOLS_BUILD_STD    "Build the astools-std tool suite"    ON)
option(ASTOOLS_BUILD_NET    "Build the net tool (links libcurl)"  OFF)
option(ASTOOLS_BUILD_CHECK  "Build the astools-check CLI"         ON)
option(ASTOOLS_BUILD_TESTS  "Build unit/integration tests"        ON)
option(ASTOOLS_BUILD_PLUGIN "Emit the Agent Plugin package"       ON)
option(ASTOOLS_NO_THREADS   "Single-threaded build (astools_tick)" OFF)
option(ASTOOLS_SANITIZERS   "Enable ASan/UBSan"                   OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Strict flags apply to astools translation units only (deps use their own).
if(MSVC)
  set(ASTOOLS_C_FLAGS /W4 /WX)
else()
  set(ASTOOLS_C_FLAGS -Wall -Wextra -Wpedantic -Werror)
endif()

if(ASTOOLS_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Package layout slug for the current platform (registry entry selection).
if(WIN32)
  set(ASTOOLS_PLAT_OS windows)
elseif(APPLE)
  set(ASTOOLS_PLAT_OS macos)
else()
  set(ASTOOLS_PLAT_OS linux)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(ASTOOLS_PLAT_ARCH arm64)
else()
  set(ASTOOLS_PLAT_ARCH x86_64)
endif()
set(ASTOOLS_PLAT "${ASTOOLS_PLAT_OS}-${ASTOOLS_PLAT_ARCH}")

# ── Dependencies (§13.1): xCDN-C only ────────────────────────────────────────
# xcdn-c registers its own add_test entries unconditionally, so its targets
# must be buildable whenever our tests run (same approach as Asper).
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/xcdn-c/CMakeLists.txt)
  message(FATAL_ERROR "deps/xcdn-c missing — run: git submodule update --init")
endif()
# Local fix for two upstream xcdn-c defects (string-escape decode asymmetry,
# unchecked realloc growth) until they land upstream; applied once, detected
# by the buggy pattern so a fixed upstream skips it cleanly.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/deps/xcdn-c/src/lexer.c ASTOOLS_XCDN_LEXER)
string(FIND "${ASTOOLS_XCDN_LEXER}" "strbuf_push(&sb, 'n')" ASTOOLS_XCDN_BUGGY)
if(NOT ASTOOLS_XCDN_BUGGY EQUAL -1)
  execute_process(
    COMMAND git apply ${CMAKE_CURRENT_SOURCE_DIR}/deps/patches/0001-xcdn-c-string-escapes-and-oom.patch
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/deps/xcdn-c
    RESULT_VARIABLE ASTOOLS_PATCH_RC)
  if(NOT ASTOOLS_PATCH_RC EQUAL 0)
    message(FATAL_ERROR "failed to apply deps/patches/0001-xcdn-c-*.patch")
  endif()
  message(STATUS "applied local xcdn-c fixes (deps/patches)")
endif()
unset(ASTOOLS_XCDN_LEXER)
if(ASTOOLS_BUILD_TESTS)
  add_subdirectory(deps/xcdn-c)
else()
  add_subdirectory(deps/xcdn-c EXCLUDE_FROM_ALL)
endif()

# ── libastools ───────────────────────────────────────────────────────────────
set(ASTOOLS_SOURCES
  src/util.c
  src/time.c
  src/uuid.c
  src/sha256.c
  src/log.c
  src/types.c
  src/manifest.c
  src/semver.c
  src/registry.c
  src/lockfile.c
  src/config.c
  src/path.c
  src/policy.c
  src/proto.c
  src/invoke.c
  src/worker.c
  src/catalog.c
  src/gbnf.c
  src/callline.c
  src/jscm.c
  src/api.c
  src/os_common.c
)
if(WIN32)
  list(APPEND ASTOOLS_SOURCES src/os_win32.c src/os_proc_stub.c
       src/sandbox_stub.c)
else()
  list(APPEND ASTOOLS_SOURCES src/os_posix.c src/os_proc_posix.c
       src/sandbox_posix.c)
endif()

add_library(astools_obj OBJECT ${ASTOOLS_SOURCES})
target_include_directories(astools_obj
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/deps/xcdn-c/src)
target_compile_options(astools_obj PRIVATE ${ASTOOLS_C_FLAGS})
target_link_libraries(astools_obj PUBLIC xcdn)
if(ASTOOLS_NO_THREADS)
  target_compile_definitions(astools_obj PUBLIC ASTOOLS_NO_THREADS)
endif()

add_library(astools_static STATIC $<TARGET_OBJECTS:astools_obj>)
set_target_properties(astools_static PROPERTIES OUTPUT_NAME astools)
add_library(astools_shared SHARED $<TARGET_OBJECTS:astools_obj>)
set_target_properties(astools_shared PROPERTIES OUTPUT_NAME astools)

foreach(tgt astools_static astools_shared)
  target_include_directories(${tgt} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(${tgt} PUBLIC xcdn)
  if(NOT WIN32 AND NOT ASTOOLS_NO_THREADS)
    find_package(Threads REQUIRED)
    target_link_libraries(${tgt} PUBLIC Threads::Threads)
  endif()
  if(NOT WIN32)
    target_link_libraries(${tgt} PUBLIC ${CMAKE_DL_LIBS} m)
  endif()
endforeach()

# ── astools-jail (§6.2; POSIX helper, ships next to libastools) ─────────────
if(NOT WIN32)
  add_executable(astools-jail jail/main.c)
  target_compile_options(astools-jail PRIVATE ${ASTOOLS_C_FLAGS})
endif()

# ── astools-std (§8): one busybox-style binary, links xcdn only ──────────────
if(ASTOOLS_BUILD_STD)
  set(ASTOOLS_STD_SOURCES
    std/main.c
    std/sdk.c
    std/regex.c
    std/tool_fs.c
    std/tool_grep.c
    std/tool_edit.c
    std/tool_sys.c
    std/tool_env.c
    std/tool_proc.c
    std/tool_git.c
  )
  if(ASTOOLS_BUILD_NET)
    list(APPEND ASTOOLS_STD_SOURCES std/tool_net.c)
  endif()
  add_executable(astools-std ${ASTOOLS_STD_SOURCES})
  target_include_directories(astools-std PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/xcdn-c/src
    ${CMAKE_CURRENT_SOURCE_DIR}/std)
  target_compile_options(astools-std PRIVATE ${ASTOOLS_C_FLAGS})
  target_link_libraries(astools-std PRIVATE xcdn)
  if(ASTOOLS_BUILD_NET)
    find_package(CURL REQUIRED)
    target_link_libraries(astools-std PRIVATE CURL::libcurl)
    target_compile_definitions(astools-std PRIVATE ASTOOLS_BUILD_NET)
  endif()

  # Assemble ready-to-copy packages: manifest + per-platform binary (§3.1).
  set(ASTOOLS_STD_TOOLS fs grep edit sys env proc git)
  if(ASTOOLS_BUILD_NET)
    list(APPEND ASTOOLS_STD_TOOLS net)
  endif()
  set(ASTOOLS_PKG_DIR ${CMAKE_BINARY_DIR}/packages)
  set(ASTOOLS_PKG_STAMPS)
  foreach(tool ${ASTOOLS_STD_TOOLS})
    add_custom_command(
      OUTPUT ${ASTOOLS_PKG_DIR}/${tool}/manifest.xcdn
      COMMAND ${CMAKE_COMMAND} -E make_directory
              ${ASTOOLS_PKG_DIR}/${tool}/bin/${ASTOOLS_PLAT}
      COMMAND ${CMAKE_COMMAND} -E copy
              ${CMAKE_CURRENT_SOURCE_DIR}/packages/${tool}/manifest.xcdn
              ${ASTOOLS_PKG_DIR}/${tool}/manifest.xcdn
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:astools-std>
              ${ASTOOLS_PKG_DIR}/${tool}/bin/${ASTOOLS_PLAT}/
      DEPENDS astools-std
              ${CMAKE_CURRENT_SOURCE_DIR}/packages/${tool}/manifest.xcdn
      COMMENT "Assembling package ${tool}")
    list(APPEND ASTOOLS_PKG_STAMPS ${ASTOOLS_PKG_DIR}/${tool}/manifest.xcdn)
  endforeach()
  add_custom_target(astools_packages ALL DEPENDS ${ASTOOLS_PKG_STAMPS})
endif()

# ── astools-mcp (§11) ────────────────────────────────────────────────────────
if(ASTOOLS_BUILD_MCP)
  add_executable(astools-mcp mcp/main.c mcp/json.c)
  target_include_directories(astools-mcp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/xcdn-c/src
    ${CMAKE_CURRENT_SOURCE_DIR}/mcp)
  target_compile_options(astools-mcp PRIVATE ${ASTOOLS_C_FLAGS})
  target_link_libraries(astools-mcp PRIVATE astools_static)
endif()

# ── astools-check (§1.2, §A2.4) ──────────────────────────────────────────────
if(ASTOOLS_BUILD_CHECK)
  add_executable(astools-check check/main.c)
  target_include_directories(astools-check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/xcdn-c/src)
  target_compile_options(astools-check PRIVATE ${ASTOOLS_C_FLAGS})
  target_link_libraries(astools-check PRIVATE astools_static)
endif()

# ── Agent Plugin package (§A2.3) ─────────────────────────────────────────────
if(ASTOOLS_BUILD_PLUGIN)
  set(ASTOOLS_PLUGIN_DIR ${CMAKE_BINARY_DIR}/astools-plugin)
  add_custom_target(astools_plugin ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${ASTOOLS_PLUGIN_DIR}/skills/astools
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/plugin/plugin.json
            ${ASTOOLS_PLUGIN_DIR}/plugin.json
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/plugin/mcp.json
            ${ASTOOLS_PLUGIN_DIR}/mcp.json
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/plugin/skills/astools/SKILL.md
            ${ASTOOLS_PLUGIN_DIR}/skills/astools/SKILL.md
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
            ${ASTOOLS_PLUGIN_DIR}/LICENSE
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/plugin/CHANGELOG.md
            ${ASTOOLS_PLUGIN_DIR}/CHANGELOG.md
    COMMENT "Assembling astools-plugin")
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(ASTOOLS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
