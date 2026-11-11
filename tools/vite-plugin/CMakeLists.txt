# The registry of widgets the plugin makes, tested by Node, which the build for Emscripten runs.
add_test(NAME vite_plugin_widget_registry COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} --test
                                                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/widgets.test.mjs)
