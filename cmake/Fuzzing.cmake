# Instrument production parsers, including xCDN; no copied parser implementation.
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR WIN32)
  message(FATAL_ERROR "Parser fuzzing requires Clang and a POSIX host")
endif()
foreach(target astools_obj xcdn)
  target_compile_options(${target} PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
endforeach()
target_compile_definitions(astools_obj PRIVATE ASTOOLS_SANITIZERS_ACTIVE)
target_link_options(xcdn INTERFACE -fsanitize=address,undefined)
foreach(name json manifest schema)
  add_executable(astools-fuzz-${name} fuzz/${name}.c)
  target_include_directories(astools-fuzz-${name} PRIVATE src)
  target_link_libraries(astools-fuzz-${name} PRIVATE astools_static)
  target_compile_options(astools-fuzz-${name} PRIVATE ${ASTOOLS_C_FLAGS}
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  target_link_options(astools-fuzz-${name} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
