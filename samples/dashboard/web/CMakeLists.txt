# The products in a browser: the same contract, called from C++ compiled to WASM, with a chart.js widget. Built by the
# Vite plugin of tools/vite-plugin, which also serves it during development: see README.md.

# REST and JSON, in a library of their own: a change in the page does not compile them again.
add_library(quoll_dashboard_web_client OBJECT src/client/products_client.cpp)
target_include_directories(quoll_dashboard_web_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(quoll_dashboard_web_client PUBLIC quoll_dashboard_api quoll::emscripten)

add_executable(quoll_dashboard_web src/main.cpp)
target_link_libraries(quoll_dashboard_web PRIVATE quoll_dashboard_web_client quoll_dashboard_web_components
                                                  quoll::emscripten)
target_link_options(quoll_dashboard_web PRIVATE -sALLOW_MEMORY_GROWTH)

# What dangles is an error in a sample too.
if(TARGET quoll_warnings)
  target_link_libraries(quoll_dashboard_web PRIVATE quoll_warnings)
endif()

# Quoll, precompiled, as parsing it takes most of the rest: it stays a header, and this is the build's own business.
target_precompile_headers(quoll_dashboard_web PRIVATE <quoll/emscripten/mount.hpp> <quoll/ui.hpp> <string> <vector>)
