cmake_minimum_required(VERSION 3.23)

project(quoll VERSION 0.1.0 LANGUAGES C CXX)

# Dependencies built by CPM (Catch2) get the same standard as Quoll: Catch2 compiled as C++14 lacks the string_view
# support its C++23 users expect.
if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  # For clang-tidy.
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(QUOLL_BUILD_TESTS "Build the Quoll tests" ${PROJECT_IS_TOP_LEVEL})
option(QUOLL_HEADER_CHECKS "With the tests, compile every header alone" ON)
option(QUOLL_INSTALL "Generate the Quoll install rules" ${PROJECT_IS_TOP_LEVEL})
option(QUOLL_BUILD_SAMPLES "Build the Quoll samples" ${PROJECT_IS_TOP_LEVEL})

# Emscripten disables exceptions by default. Set before any dependency is added, so that Quoll and everything CPM
# builds (Catch2, llhttp) agree on the exception model.
if(EMSCRIPTEN)
  add_compile_options(-fwasm-exceptions)
  add_link_options(-fwasm-exceptions)
endif()

# The benchmark program (tests/bench), with the standard library's assertions off, as an application builds it: see
# cmake/QuollHelpers.cmake. The tests rely on those assertions.
option(QUOLL_BENCH "Build the benchmark program, tests/bench" OFF)

if(QUOLL_BENCH AND QUOLL_BUILD_TESTS)
  message(FATAL_ERROR "QUOLL_BENCH goes without the tests, which need the assertions it turns off: -DQUOLL_BUILD_TESTS=OFF")
endif()

# Fuzz targets (tests/fuzz), with Clang: everything is instrumented, so that libFuzzer sees Quoll's branches and its dependencies'.
option(QUOLL_FUZZ "Build the fuzz targets, with Clang's libFuzzer under ASan and UBSan" OFF)

if(QUOLL_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR EMSCRIPTEN)
    message(FATAL_ERROR "QUOLL_FUZZ needs Clang as the C and C++ compiler, for libFuzzer")
  endif()

  # With the library's assertions on, whatever the build type: a broken invariant is what a fuzzer looks for.
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
                      -UNDEBUG -D_GLIBCXX_ASSERTIONS)
  add_link_options(-fsanitize=address,undefined)
endif()

include(cmake/QuollHelpers.cmake)
include(cmake/CPM.cmake)

include(cmake/QuollGlaze.cmake)

# Modules, in dependency order: the core, then the adapters.
add_subdirectory(quoll/async)
add_subdirectory(quoll/io)
add_subdirectory(quoll/http)
add_subdirectory(quoll/server)
add_subdirectory(quoll/client)
add_subdirectory(quoll/rest)
add_subdirectory(quoll/ui)
add_subdirectory(quoll/adapters)

# Before the samples: they declare a test of their own.
if(QUOLL_BUILD_TESTS)
  enable_testing()
endif()

if(QUOLL_BUILD_SAMPLES)
  add_subdirectory(samples)
endif()

if(QUOLL_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(QUOLL_FUZZ)
  add_subdirectory(tests/fuzz)
endif()

if(QUOLL_BENCH)
  add_subdirectory(tests/bench)
endif()

if(QUOLL_INSTALL)
  quoll_install()
endif()
