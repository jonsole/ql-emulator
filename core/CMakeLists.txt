cmake_minimum_required(VERSION 3.20)
project(ql_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The C and C++ runtime linked in, not loaded from MSVCP140.dll and
# VCRUNTIME140.dll: a released ql_server.exe has to start on a Windows that has
# never had the Visual C++ Redistributable installed. Every target, since they
# all link together and must agree.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Link-time optimisation for optimised builds: the CPU calls through the Bus on
# every byte cycle, and the parts of the machine will each live in their own
# translation unit.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT QL_IPO_OK OUTPUT QL_IPO_WHY)
  if(QL_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  else()
    message(STATUS "Link-time optimisation unavailable: ${QL_IPO_WHY}")
  endif()
endif()

# Every executable goes to the top of the build tree, tests included.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(MSVC)
  add_compile_options(/utf-8)
  set(QL_WARNINGS /W4 /WX)
else()
  set(QL_WARNINGS -Wall -Wextra -Werror)
endif()

# ---- ql_m68008: the CPU, standalone ------------------------------------------
# Stage 1 of docs/plan.md: the 68008 as a library of its own, with no
# dependency on any part of the machine. It reaches the outside world only
# through the Bus interface in m68008.h.
add_library(ql_m68008 STATIC
  src/m68008.cpp
  src/m68008_decode.cpp
  src/m68008_move.cpp
  src/m68008_system.cpp
  src/m68008_flow.cpp
  src/m68008_arithmetic.cpp
  src/m68008_logic.cpp
  src/m68008_shift.cpp
  src/m68008_bcd.cpp
  src/m68008_multiply.cpp
)
target_include_directories(ql_m68008 PUBLIC src)
target_compile_options(ql_m68008 PRIVATE ${QL_WARNINGS})

# ---- ql_machine: the QL around the CPU ----------------------------------------
# Stage 2 of docs/plan.md: the memory map, and in time the ZX8301, the ZX8302
# and the IPC. It is the Bus the 68008 sees.
add_library(ql_machine STATIC
  src/ql.cpp
  src/zx8301.cpp
)
target_include_directories(ql_machine PUBLIC src)
target_link_libraries(ql_machine PUBLIC ql_m68008)
target_compile_options(ql_machine PRIVATE ${QL_WARNINGS})

# ---- tests --------------------------------------------------------------------
option(QL_BUILD_TESTS "Build the test suite" ON)
if(QL_BUILD_TESTS)
  # Here rather than in tests/, so that ctest run from the top of the build
  # tree finds the tests defined down there.
  enable_testing()
  add_subdirectory(tests)
endif()
