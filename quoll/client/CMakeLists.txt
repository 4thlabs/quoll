quoll_declare_module(
  NAME client
  SOURCES
    src/transport.cpp
    src/url.cpp
  DEPS
    quoll::http
)

quoll_declare_tests(
  SOURCES
    tests/transport_test.cpp
  DEPS
    quoll::client
    quoll::server
)
