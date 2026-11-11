# The plugin's tests, run by Node, which the build for Emscripten runs: the registry of widgets it makes, and the options
# of its builds.
add_test(NAME vite_plugin
         COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} --test ${CMAKE_CURRENT_SOURCE_DIR}/tests/widgets.test.mjs
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/options.test.mjs)
