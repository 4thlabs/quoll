# The runtime of the build: the adapter providing quoll::runtime, the loop a program's tasks run on. Each adapter able
# to run a loop in the calling thread provides <adapter>::runtime; QUOLL_RUNTIME picks one. A browser runs its own
# loop, so there is no runtime under Emscripten yet.
if(EMSCRIPTEN)
  return()
endif()

set(QUOLL_RUNTIME asio CACHE STRING "The adapter providing quoll::runtime")
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
