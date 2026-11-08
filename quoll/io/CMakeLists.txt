quoll_declare_module(
  NAME io
  SOURCES
    src/any_listener.cpp
    src/any_stream.cpp
    src/error.cpp
    src/network.cpp
    src/sound_read.cpp
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

# What the erased wrappers refuse: a reference to a temporary, a stream or a listener to copy.
quoll_declare_compile_fail_tests(
  SOURCES
    tests/compile_fail/any_listener_from_lvalue.cpp
    tests/compile_fail/any_stream_from_lvalue.cpp
    tests/compile_fail/network_ref_from_temporary.cpp
    tests/compile_fail/tls_network_ref_from_temporary.cpp
  DEPS
    quoll::io
)
