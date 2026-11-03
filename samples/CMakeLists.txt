# Native only: the samples talk over TCP through quoll::asio.
if(EMSCRIPTEN)
  return()
endif()

add_subdirectory(notes)
