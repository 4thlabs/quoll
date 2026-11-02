# Native only: asio has no sockets to offer in the browser.
if(EMSCRIPTEN)
  return()
endif()

find_package(Threads REQUIRED)

# The application provides standalone asio (header-only); Quoll's own build fetches it. Only the build interface links
# it: an installed quoll::asio leaves the choice of the asio copy to the application.
if(NOT TARGET asio::asio)
  CPMAddPackage(
    NAME asio
    VERSION 1.38.2
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-38-2.tar.gz
    URL_HASH SHA256=9f2648fa483e58a6bf848d970ee0ea650ca19ed7769dfa520ed4f7b8d27af1db
    DOWNLOAD_ONLY YES
  )

  add_library(quoll_asio_headers INTERFACE)
  target_include_directories(quoll_asio_headers SYSTEM INTERFACE ${asio_SOURCE_DIR}/include)
  add_library(asio::asio ALIAS quoll_asio_headers)
endif()

quoll_declare_module(NAME asio DEPS quoll::io Threads::Threads $<BUILD_INTERFACE:asio::asio>)

quoll_declare_tests(
  SOURCES
    tests/asio_test.cpp
  DEPS
    quoll::asio
    quoll::http
    quoll::threads
)
