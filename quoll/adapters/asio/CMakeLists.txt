# Native only: asio has no sockets to offer in the browser.
if(EMSCRIPTEN)
  return()
endif()

find_package(Threads REQUIRED)

# The runtime is compiled against this asio. A program using only quoll::runtime never sees it. Without an asio::asio target
# defined before Quoll, Quoll fetches its own copy, under names of its own (CPM package quoll_asio, target
# quoll_asio_headers). A program using the streams and acceptors, or runtime::context(), compiles asio too: the same version,
# as asio has no versioned namespace to tell two copies apart. Only the build interface links it: an installed quoll::asio
# leaves that to the application.
if(TARGET asio::asio)
  set(quoll_asio_dependency asio::asio)
else()
  CPMAddPackage(
    NAME quoll_asio
    VERSION 1.38.2
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-38-2.tar.gz
    URL_HASH SHA256=9f2648fa483e58a6bf848d970ee0ea650ca19ed7769dfa520ed4f7b8d27af1db
    DOWNLOAD_ONLY YES
  )

  add_library(quoll_asio_headers INTERFACE)
  target_include_directories(quoll_asio_headers SYSTEM INTERFACE ${quoll_asio_SOURCE_DIR}/include)
  set(quoll_asio_dependency quoll_asio_headers)
endif()

quoll_declare_module(
  NAME asio
  SOURCES
    src/runtime.cpp
  DEPS
    quoll::io
    Threads::Threads
    $<BUILD_INTERFACE:${quoll_asio_dependency}>
)

# TLS needs OpenSSL. AUTO uses it when it is there: without it, https:// and a server's certificate fail at run time,
# saying so. ON makes it required; OFF goes without.
set(QUOLL_TLS AUTO CACHE STRING "TLS through OpenSSL: AUTO uses it when found, ON requires it, OFF goes without")
set_property(CACHE QUOLL_TLS PROPERTY STRINGS AUTO ON OFF)

if(QUOLL_TLS STREQUAL "ON")
  find_package(OpenSSL REQUIRED)
elseif(QUOLL_TLS STREQUAL "AUTO")
  find_package(OpenSSL QUIET)
endif()

if(OpenSSL_FOUND AND NOT QUOLL_TLS STREQUAL "OFF")
  message(STATUS "Quoll: TLS through OpenSSL ${OPENSSL_VERSION}")
  target_link_libraries(quoll_asio PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  target_compile_definitions(quoll_asio PUBLIC QUOLL_ASIO_TLS)
  # For the package's quoll-config.cmake: its users need OpenSSL too.
  set(QUOLL_PACKAGE_NEEDS_OPENSSL ON CACHE INTERNAL "")
else()
  message(STATUS "Quoll: no TLS: https:// and a server's certificate fail at run time")
  set(QUOLL_PACKAGE_NEEDS_OPENSSL OFF CACHE INTERNAL "")
endif()

# What a program using the runtime compiles has no asio in it, the point of compiling the runtime once: without its include
# directory, including an asio header there stops the build.
if(QUOLL_BUILD_TESTS AND QUOLL_HEADER_CHECKS)
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/runtime_without_asio.cpp" CONTENT "#include <quoll/asio/runtime.hpp>\n")
  add_library(quoll_asio_runtime_without_asio OBJECT "${CMAKE_CURRENT_BINARY_DIR}/runtime_without_asio.cpp")
  target_compile_features(quoll_asio_runtime_without_asio PRIVATE cxx_std_23)
  target_include_directories(quoll_asio_runtime_without_asio PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/quoll/async/include"
    "${PROJECT_SOURCE_DIR}/quoll/io/include"
  )
endif()

# The certificates of the TLS tests.
add_library(quoll_asio_test_tls INTERFACE)
target_compile_definitions(quoll_asio_test_tls INTERFACE
  "QUOLL_TEST_TLS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/tls\""
)

quoll_declare_tests(
  SOURCES
    tests/asio_test.cpp
    tests/tls_test.cpp
  DEPS
    quoll_asio_test_tls
    quoll::asio
    quoll::client
    quoll::http
    quoll::rest
    quoll::server
    quoll::threads
)
