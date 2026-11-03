# The runtime of the build: the adapter providing quoll::sync_wait, the entry point of a program. Each adapter able to
# run a loop in the calling thread provides <adapter>::sync_wait; QUOLL_RUNTIME picks one. Nothing blocks in a browser,
# so there is no runtime under Emscripten yet.
if(EMSCRIPTEN)
  return()
endif()

set(QUOLL_RUNTIME asio CACHE STRING "The adapter providing quoll::sync_wait")
set_property(CACHE QUOLL_RUNTIME PROPERTY STRINGS asio)

quoll_declare_module(NAME runtime DEPS quoll::${QUOLL_RUNTIME})

string(TOUPPER "${QUOLL_RUNTIME}" quoll_runtime_upper)
target_compile_definitions(quoll_runtime INTERFACE QUOLL_RUNTIME_${quoll_runtime_upper})

quoll_declare_tests(
  SOURCES
    tests/runtime_test.cpp
  DEPS
    quoll::runtime
)
