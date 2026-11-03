cmake_minimum_required(VERSION 3.23)

project(quoll VERSION 0.1.0 LANGUAGES C CXX)

# Dependencies built by CPM (Catch2) get the same standard as Quoll: Catch2 compiled as C++14 lacks the string_view
# support its C++23 users expect.
if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(QUOLL_BUILD_TESTS "Build the Quoll tests" ${PROJECT_IS_TOP_LEVEL})
option(QUOLL_INSTALL "Generate the Quoll install rules" ${PROJECT_IS_TOP_LEVEL})
option(QUOLL_BUILD_SAMPLES "Build the Quoll samples" ${PROJECT_IS_TOP_LEVEL})

# Emscripten disables exceptions by default. Set before any dependency is added, so that Quoll and everything CPM
# builds (Catch2, llhttp) agree on the exception model.
if(EMSCRIPTEN)
  add_compile_options(-fwasm-exceptions)
  add_link_options(-fwasm-exceptions)
endif()

include(cmake/QuollHelpers.cmake)
include(cmake/CPM.cmake)

# Modules, in dependency order: the core, then the adapters.
add_subdirectory(quoll/async)
add_subdirectory(quoll/io)
add_subdirectory(quoll/http)
add_subdirectory(quoll/server)
add_subdirectory(quoll/client)
add_subdirectory(quoll/rest)
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

if(QUOLL_INSTALL)
  quoll_install()
endif()
