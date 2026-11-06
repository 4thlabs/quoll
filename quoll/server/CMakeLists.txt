quoll_declare_module(
  NAME server
  SOURCES
    src/connection.cpp
    src/server.cpp
    src/static_files.cpp
  DEPS
    quoll::http
)

quoll_declare_tests(
  SOURCES
    tests/serve_connection_test.cpp
    tests/server_test.cpp
    tests/static_files_test.cpp
  DEPS
    quoll::server
)

# What Clang refuses, now that it is told what a task refers to. Only Clang can tell.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  quoll_declare_compile_fail_tests(
    SOURCES
      tests/compile_fail/dangling_listener.cpp
    DEPS
      quoll::server
  )
endif()
