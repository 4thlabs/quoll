# The notes of samples/notes in a browser: the same contract, called from C++ compiled to WASM. Built by Vite's plugin
# (quoll-vite-plugin.js), which also serves it during development: see README.md.
add_executable(quoll_ui_notes main.cpp)
target_include_directories(quoll_ui_notes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../notes)
target_link_libraries(quoll_ui_notes PRIVATE quoll::emscripten quoll::rest quoll::ui)
target_link_options(quoll_ui_notes PRIVATE -sALLOW_MEMORY_GROWTH)
