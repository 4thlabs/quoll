cmake_minimum_required(VERSION 3.23)

project(quoll VERSION 0.1.0 LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(QUOLL_BUILD_TESTS "Build the Quoll tests" ${PROJECT_IS_TOP_LEVEL})
option(QUOLL_INSTALL "Generate the Quoll install rules" ${PROJECT_IS_TOP_LEVEL})

# Emscripten disables exceptions by default. Set before any dependency is added, so that Quoll and everything CPM
# builds (Catch2, llhttp) agree on the exception model.
if(EMSCRIPTEN)
  add_compile_options(-fwasm-exceptions)
  add_link_options(-fwasm-exceptions)
endif()

include(cmake/QuollHelpers.cmake)
include(cmake/CPM.cmake)

# Modules, in dependency order: add_subdirectory(quoll/<module>)

if(QUOLL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(QUOLL_INSTALL)
  quoll_install()
endif()
