# The test suite. Included by the top-level CMakeLists.txt when QL_BUILD_TESTS
# is on (the default).
#
# Each test is a plain executable built from <name>.cpp with the harness in
# test_main.h, run by CTest from the top of the build tree. ql_tests builds
# them all.
set(QL_ROOT_DEFINE QL_PROJECT_ROOT="${PROJECT_SOURCE_DIR}/..")

add_custom_target(ql_tests)

# ---- SingleStepTests/m68000 support ----------------------------------------------
# Reading the suite's files and deriving what each test means for a 68008. A
# library of its own because both the CPU's runner and the tests of the
# derivation itself use it.
add_library(ql_m68000_suite STATIC
  m68000_file.cpp
  m68008_timing.cpp
)
target_include_directories(ql_m68000_suite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ql_m68000_suite PRIVATE ${QL_WARNINGS})

function(ql_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ql_m68008 ql_m68000_suite)
  target_compile_options(${name} PRIVATE ${QL_WARNINGS})
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  add_dependencies(ql_tests ${name})
endfunction()

ql_add_test(m68000_file_tests)
ql_add_test(m68008_timing_tests)

# The CPU gate, registered twice: a fixed sample of each file in the fast
# suite, and every test as a slow one. Exit code 77 means the suite has not
# been fetched (scripts/fetch-m68000-tests.ps1), which CTest reports as
# skipped rather than passed or failed.
add_executable(m68000_suite m68000_suite.cpp)
target_link_libraries(m68000_suite PRIVATE ql_m68008 ql_m68000_suite)
target_compile_options(m68000_suite PRIVATE ${QL_WARNINGS})
target_compile_definitions(m68000_suite PRIVATE ${QL_ROOT_DEFINE})
add_dependencies(ql_tests m68000_suite)
add_test(NAME m68000_sample COMMAND m68000_suite WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME m68000_full COMMAND m68000_suite --full WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(m68000_sample m68000_full PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(m68000_full PROPERTIES LABELS slow)
