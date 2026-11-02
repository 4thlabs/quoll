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
target_sources(quoll_llhttp
  PUBLIC FILE_SET HEADERS BASE_DIRS ${llhttp_SOURCE_DIR}/include FILES ${llhttp_SOURCE_DIR}/include/llhttp.h
)

if(QUOLL_INSTALL)
  install(TARGETS quoll_llhttp EXPORT quoll-targets FILE_SET HEADERS)
endif()

quoll_declare_module(NAME http DEPS quoll::llhttp)

quoll_declare_tests(
  SOURCES
    tests/fields_test.cpp
    tests/parser_test.cpp
    tests/serializer_test.cpp
  DEPS
    quoll::http
)
