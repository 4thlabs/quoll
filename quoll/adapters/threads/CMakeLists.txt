find_package(Threads REQUIRED)

# No threads in the browser: its headers still compile there, with nothing to link.
if(EMSCRIPTEN)
  quoll_declare_module(NAME threads DEPS quoll::async Threads::Threads)
else()
  quoll_declare_module(
    NAME threads
    SOURCES
      src/thread_pool.cpp
    DEPS
      quoll::async
      Threads::Threads
  )
endif()

# The browser has no threads unless the whole program is built with pthreads: the pool is not tested under WASM.
if(NOT EMSCRIPTEN)
  quoll_declare_tests(
    SOURCES
      tests/thread_pool_test.cpp
    DEPS
      quoll::threads
  )
endif()
