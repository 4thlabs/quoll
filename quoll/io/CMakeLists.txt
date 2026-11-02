quoll_declare_module(NAME io DEPS quoll::async)

quoll_declare_tests(
  SOURCES
    tests/any_stream_test.cpp
    tests/memory_stream_test.cpp
  DEPS
    quoll::io
)
