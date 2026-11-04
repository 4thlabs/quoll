quoll_declare_module(
  NAME io
  SOURCES
    src/error.cpp
    src/network.cpp
  DEPS
    quoll::async
)

quoll_declare_tests(
  SOURCES
    tests/any_stream_test.cpp
    tests/memory_stream_test.cpp
    tests/network_test.cpp
  DEPS
    quoll::io
)
