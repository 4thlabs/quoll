# Measures Quoll, natively: see QUOLL_BENCH in the root CMakeLists.txt, and docs/BENCHMARKS.md for its figures.
if(EMSCRIPTEN)
  message(FATAL_ERROR "QUOLL_BENCH is native only")
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(WARNING "quoll_bench measures an optimized build: its figures mean little in a ${CMAKE_BUILD_TYPE} one")
endif()

add_executable(quoll_bench bench.cpp)
target_include_directories(quoll_bench PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(quoll_bench PRIVATE quoll::client quoll::rest quoll::runtime quoll::server quoll_warnings)
target_compile_definitions(quoll_bench PRIVATE QUOLL_BENCH_TLS_DIR="${PROJECT_SOURCE_DIR}/quoll/adapters/asio/tests/tls")
