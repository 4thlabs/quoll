quoll_declare_module(NAME ui DEPS quoll::async $<BUILD_INTERFACE:${quoll_glaze_dependency}> $<INSTALL_INTERFACE:glaze::glaze>)

# For development, with its server: a text or a class list changed in the source shows without compiling.
option(QUOLL_UI_HOT "quoll::ui: literals can be replaced while the program runs, for hot reload" OFF)

if(QUOLL_UI_HOT)
  target_compile_definitions(quoll_ui INTERFACE QUOLL_UI_HOT)
endif()

quoll_declare_tests(
  SOURCES
    tests/app_test.cpp
    tests/element_test.cpp
  DEPS
    quoll::ui
)

# What a component must be, with the message the user gets.
quoll_declare_compile_fail_tests(
  SOURCES
    tests/compile_fail/component_not_aggregate.cpp
  DEPS
    quoll::ui
)

# The hot reload switch changes signatures: a program of its own, native only, rather than one more test source.
if(QUOLL_BUILD_TESTS AND NOT EMSCRIPTEN AND NOT QUOLL_UI_HOT)
  add_executable(quoll_ui_hot_test tests/hot_test.cpp)
  target_compile_definitions(quoll_ui_hot_test PRIVATE QUOLL_UI_HOT)
  target_include_directories(quoll_ui_hot_test PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_link_libraries(quoll_ui_hot_test PRIVATE quoll::ui quoll_warnings)
  set_property(GLOBAL APPEND PROPERTY QUOLL_UI_HOT_TEST quoll_ui_hot_test)
endif()
