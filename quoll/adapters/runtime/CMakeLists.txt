# The runtime of the build: the adapter providing quoll::runtime, the loop a program's tasks run on. Each adapter able
# to run a loop in the calling thread provides <adapter>::runtime; QUOLL_RUNTIME picks one. Under Emscripten,
# it is the browser's event loop.
if(EMSCRIPTEN)
  set(quoll_runtime_default emscripten)
else()
  set(quoll_runtime_default asio)
endif()

set(QUOLL_RUNTIME ${quoll_runtime_default} CACHE STRING "The adapter providing quoll::runtime")
set_property(CACHE QUOLL_RUNTIME PROPERTY STRINGS ${quoll_runtime_default})

if(NOT QUOLL_RUNTIME STREQUAL quoll_runtime_default)
  message(FATAL_ERROR "QUOLL_RUNTIME: only ${quoll_runtime_default} can run here, not ${QUOLL_RUNTIME}")
endif()

quoll_declare_module(NAME runtime DEPS quoll::${QUOLL_RUNTIME})

string(TOUPPER "${QUOLL_RUNTIME}" quoll_runtime_upper)
target_compile_definitions(quoll_runtime INTERFACE QUOLL_RUNTIME_${quoll_runtime_upper})

quoll_declare_tests(
  SOURCES
    tests/runtime_test.cpp
  DEPS
    quoll::runtime
)
