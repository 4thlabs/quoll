# The notes of samples/notes in a browser: the same contract, called from C++ compiled to WASM. Built by Vite's plugin
# (quoll-vite-plugin.js), which also serves it during development: see README.md.

# REST and JSON, in a library of their own: a change in the interface does not compile them again.
add_library(quoll_ui_notes_client OBJECT notes_client.cpp)
target_include_directories(quoll_ui_notes_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../notes)
target_link_libraries(quoll_ui_notes_client PUBLIC quoll::emscripten quoll::rest)

add_executable(quoll_ui_notes main.cpp)
target_link_libraries(quoll_ui_notes PRIVATE quoll_ui_notes_client quoll::emscripten quoll::ui)
target_link_options(quoll_ui_notes PRIVATE -sALLOW_MEMORY_GROWTH)

# What dangles is an error in a sample too.
if(TARGET quoll_warnings)
  target_link_libraries(quoll_ui_notes PRIVATE quoll_warnings)
endif()

# Quoll, precompiled, as parsing it takes most of the rest: it stays a header, and this is the build's own business.
target_precompile_headers(quoll_ui_notes PRIVATE <quoll/emscripten/mount.hpp> <quoll/ui.hpp> <string> <vector>)
