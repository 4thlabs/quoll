# Browser, Worker and Node only. The JavaScript side, js/quoll_emscripten.js, is linked into the application with
# --js-library: from the source tree in Quoll's build, from <prefix>/share/quoll once installed.
if(NOT EMSCRIPTEN)
  return()
endif()

quoll_declare_module(NAME emscripten DEPS quoll::http)

set(quoll_emscripten_library ${CMAKE_CURRENT_SOURCE_DIR}/js/quoll_emscripten.js)
set(quoll_emscripten_library_installed ${CMAKE_INSTALL_DATADIR}/quoll/quoll_emscripten.js)

target_link_options(quoll_emscripten INTERFACE
  "SHELL:--js-library $<BUILD_INTERFACE:${quoll_emscripten_library}>$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${quoll_emscripten_library_installed}>"
)

if(QUOLL_INSTALL)
  install(FILES ${quoll_emscripten_library} DESTINATION ${CMAKE_INSTALL_DATADIR}/quoll)
endif()

if(QUOLL_BUILD_TESTS)
  # A Node HTTP server, started from tests/test_server.js, answers the requests of the tests.
  add_library(quoll_emscripten_test_server INTERFACE)
  target_link_options(quoll_emscripten_test_server INTERFACE
    "SHELL:--js-library ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_server.js"
  )

  quoll_declare_tests(
    SOURCES
      tests/emscripten_test.cpp
    DEPS
      quoll::emscripten
      quoll::rest
      quoll_emscripten_test_server
  )
endif()
