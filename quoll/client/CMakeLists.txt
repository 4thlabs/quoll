quoll_declare_module(NAME client DEPS quoll::http)

quoll_declare_tests(
  SOURCES
    tests/transport_test.cpp
  DEPS
    quoll::client
    quoll::server
)
