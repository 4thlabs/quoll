quoll_declare_module(NAME server DEPS quoll::http)

quoll_declare_tests(
  SOURCES
    tests/serve_connection_test.cpp
    tests/serve_test.cpp
  DEPS
    quoll::server
)
