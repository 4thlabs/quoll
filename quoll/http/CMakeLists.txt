# llhttp is compiled from its release sources: its own CMakeLists requires CMake 3.25 and forces CMAKE_BUILD_TYPE
# into the cache. The target is exported with Quoll as quoll::llhttp.
CPMAddPackage(
  NAME llhttp
  VERSION 9.4.3
  URL https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.4.3.tar.gz
  URL_HASH SHA256=1eb813c7437b31a87496a1cd3ed79f00746720f5e7e29c79b42c02cb69f36c39
  DOWNLOAD_ONLY YES
)

add_library(quoll_llhttp STATIC
  ${llhttp_SOURCE_DIR}/src/api.c
  ${llhttp_SOURCE_DIR}/src/http.c
  ${llhttp_SOURCE_DIR}/src/llhttp.c
)
add_library(quoll::llhttp ALIAS quoll_llhttp)
set_target_properties(quoll_llhttp PROPERTIES EXPORT_NAME llhttp POSITION_INDEPENDENT_CODE ON)
target_include_directories(quoll_llhttp PRIVATE ${llhttp_SOURCE_DIR}/include)

# Users see the header as <quoll/third_party/llhttp.h>, so that it never clashes with a system llhttp.
set(quoll_llhttp_include ${CMAKE_CURRENT_BINARY_DIR}/third_party/include)
configure_file(${llhttp_SOURCE_DIR}/include/llhttp.h ${quoll_llhttp_include}/quoll/third_party/llhttp.h COPYONLY)
target_sources(quoll_llhttp
  PUBLIC FILE_SET HEADERS BASE_DIRS ${quoll_llhttp_include} FILES ${quoll_llhttp_include}/quoll/third_party/llhttp.h
)

if(QUOLL_INSTALL)
  install(TARGETS quoll_llhttp EXPORT quoll-targets FILE_SET HEADERS)
endif()

quoll_declare_module(
  NAME http
  SOURCES
    src/body_reader.cpp
    src/connection_field.cpp
    src/error.cpp
    src/event_parser.cpp
    src/event_reader.cpp
    src/event_writer.cpp
    src/fields.cpp
    src/format_event.cpp
    src/local_transport.cpp
    src/parser.cpp
    src/percent.cpp
    src/response_writer.cpp
    src/router.cpp
    src/serializer.cpp
    src/utf8.cpp
  DEPS
    quoll::io
  PRIVATE_DEPS
    quoll::llhttp
)

quoll_declare_tests(
  SOURCES
    tests/event_parser_test.cpp
    tests/event_reader_test.cpp
    tests/fields_test.cpp
    tests/format_event_test.cpp
    tests/parser_test.cpp
    tests/percent_test.cpp
    tests/router_test.cpp
    tests/serializer_test.cpp
    tests/stream_test.cpp
    tests/streamed_response_test.cpp
  DEPS
    quoll::http
)

# What a route's path and its handler must agree on, with what the compiler says.
quoll_declare_compile_fail_tests(
  SOURCES
    tests/compile_fail/router_body_reader_not_last.cpp
    tests/compile_fail/router_dot_segment.cpp
    tests/compile_fail/body_stream_wrong_head.cpp
    tests/compile_fail/body_stream_overloaded_head.cpp
    tests/compile_fail/router_two_body_readers.cpp
    tests/compile_fail/router_too_few_parameters.cpp
    tests/compile_fail/router_unknown_type.cpp
    tests/compile_fail/router_too_many_parameters.cpp
    tests/compile_fail/router_invalid_template.cpp
    tests/compile_fail/router_auto_parameter.cpp
    tests/compile_fail/router_parameter_type.cpp
    tests/compile_fail/router_handler_result.cpp
    tests/compile_fail/router_response_writer_not_last.cpp
    tests/compile_fail/router_response_writer_result.cpp
    tests/compile_fail/dangling_dispatch.cpp
  DEPS
    quoll::http
)

# What Clang refuses, now that it is told what refers to what. Only Clang can tell.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  quoll_declare_compile_fail_tests(
    SOURCES
      tests/compile_fail/dangling_view.cpp
    DEPS
      quoll::http
  )
endif()
