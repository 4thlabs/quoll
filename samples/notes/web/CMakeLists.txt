# The notes in a browser: the same contract, called from C++ compiled to WASM. Built by the Vite plugin of
# tools/vite-plugin, which also serves it during development: see README.md.

# REST and JSON, in a library of their own: a change in the page does not compile them again.
add_library(quoll_notes_web_client OBJECT notes_client.cpp)
target_link_libraries(quoll_notes_web_client PUBLIC quoll_notes_api quoll::emscripten)

add_executable(quoll_notes_web main.cpp)
target_link_libraries(quoll_notes_web PRIVATE quoll_notes_web_client quoll_notes_web_components quoll::emscripten)
target_link_options(quoll_notes_web PRIVATE -sALLOW_MEMORY_GROWTH)

# What dangles is an error in a sample too.
if(TARGET quoll_warnings)
  target_link_libraries(quoll_notes_web PRIVATE quoll_warnings)
endif()

# Quoll, precompiled, as parsing it takes most of the rest: it stays a header, and this is the build's own business.
target_precompile_headers(quoll_notes_web PRIVATE <quoll/emscripten/mount.hpp> <quoll/ui.hpp> <string> <vector>)
