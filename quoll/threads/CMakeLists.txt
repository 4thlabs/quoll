find_package(Threads REQUIRED)

quoll_declare_module(NAME threads DEPS quoll::async Threads::Threads)

# The browser has no threads unless the whole program is built with pthreads: the pool is not tested under WASM.
if(NOT EMSCRIPTEN)
  quoll_declare_tests(
    SOURCES
      tests/thread_pool_test.cpp
    DEPS
      quoll::threads
  )
endif()
