quoll_declare_module(
  NAME server
  SOURCES
    src/connection.cpp
    src/listen.cpp
    src/serve.cpp
  DEPS
    quoll::http
)

quoll_declare_tests(
  SOURCES
    tests/serve_connection_test.cpp
    tests/serve_test.cpp
  DEPS
    quoll::server
)
