quoll_declare_module(
  NAME client
  SOURCES
    src/client.cpp
    src/connect_websocket.cpp
    src/platform.cpp
    src/url.cpp
  DEPS
    quoll::http
)

quoll_declare_tests(
  SOURCES
    tests/client_test.cpp
    tests/connect_websocket_test.cpp
    tests/platform_test.cpp
  DEPS
    quoll::client
    quoll::server
)
