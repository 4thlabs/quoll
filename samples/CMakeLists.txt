# Under Emscripten, the interface in a browser; natively, what talks over TCP through quoll::asio.
if(EMSCRIPTEN)
  add_subdirectory(ui_notes)
  return()
endif()

add_subdirectory(notes)
