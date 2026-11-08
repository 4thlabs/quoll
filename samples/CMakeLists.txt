# A command-line program calling GitHub's API: natively, and under Emscripten in Node.
add_subdirectory(github)

# Under Emscripten, the interface in a browser; natively, what talks over TCP through quoll::asio.
if(EMSCRIPTEN)
  add_subdirectory(ui_notes)
  return()
endif()

add_subdirectory(notes)
