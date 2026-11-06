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
    tests/response_writer_test.cpp
    tests/serve_connection_test.cpp
    tests/server_test.cpp
    tests/static_files_test.cpp
  DEPS
    quoll::server
)

quoll_declare_compile_fail_tests(
  SOURCES
    tests/compile_fail/serve_listener_not_moved.cpp
    tests/compile_fail/server_route_handler.cpp
  DEPS
    quoll::server
)
