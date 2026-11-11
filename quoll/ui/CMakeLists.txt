quoll_declare_module(
  NAME ui
  SOURCES
    src/app_core.cpp
    src/effect.cpp
    src/element.cpp
    src/expand.cpp
    src/hot.cpp
    src/instance.cpp
    src/ready.cpp
    src/reconciler.cpp
    src/renderer_ref.cpp
    src/run_error.cpp
    src/run_slot.cpp
    src/to_html.cpp
    src/view_context.cpp
  DEPS
    quoll::async
    $<BUILD_INTERFACE:${quoll_glaze_dependency}>
    $<INSTALL_INTERFACE:glaze::glaze>
)

# For development, with its server: a text or a class list changed in the source shows without compiling.
option(QUOLL_UI_HOT "quoll::ui: literals can be replaced while the program runs, for hot reload" OFF)

if(QUOLL_UI_HOT)
  target_compile_definitions(quoll_ui INTERFACE QUOLL_UI_HOT)
endif()

quoll_declare_tests(
  SOURCES
    tests/action_test.cpp
    tests/app_test.cpp
    tests/callback_test.cpp
    tests/nested_test.cpp
    tests/skip_test.cpp
    tests/effect_test.cpp
    tests/element_test.cpp
    tests/to_html_test.cpp
    tests/widget_test.cpp
  DEPS
    quoll::ui
)

# What a component must be, with the message the user gets.
quoll_declare_compile_fail_tests(
  SOURCES
    tests/compile_fail/action_optional_view_argument.cpp
    tests/compile_fail/action_reference_argument.cpp
    tests/compile_fail/action_throwing_move.cpp
    tests/compile_fail/callback_optional_view_argument.cpp
    tests/compile_fail/callback_optional_view_result.cpp
    tests/compile_fail/callback_reference_argument.cpp
    tests/compile_fail/callback_reference_result.cpp
    tests/compile_fail/callback_reference_wrapper_argument.cpp
    tests/compile_fail/callback_transform_view_argument.cpp
    tests/compile_fail/callback_view_argument.cpp
    tests/compile_fail/callback_view_result.cpp
    tests/compile_fail/child_component_not_aggregate.cpp
    tests/compile_fail/child_component_without_props_not_aggregate.cpp
    tests/compile_fail/component_not_aggregate.cpp
    tests/compile_fail/event_function_signature.cpp
    tests/compile_fail/effect_optional_view_dependency.cpp
    tests/compile_fail/effect_pair_span_value.cpp
    tests/compile_fail/effect_values_without_target.cpp
    tests/compile_fail/effect_view_value.cpp
    tests/compile_fail/target_bool_value.cpp
    tests/compile_fail/target_two_flags.cpp
    tests/compile_fail/target_value_count.cpp
    tests/compile_fail/target_view_value.cpp
  DEPS
    quoll::ui
)

# What a debug build asserts of an effect or an action: destroyed while a run refers to it, which its component, going
# once its tasks have ended, never is.
quoll_declare_assert_tests(
  NAME action_destroyed_under_way
  CASE "action: destroyed while a run is under way is asserted"
  EXPECT "quoll::ui::action: destroyed while a run is under way"
)
quoll_declare_assert_tests(
  NAME effect_destroyed_under_way
  CASE "effect: destroyed while a run is under way is asserted"
  EXPECT "quoll::ui::effect: destroyed while a run is under way"
)

# The hot reload switch changes signatures: a program of its own, native only, rather than one more test source.
if(QUOLL_BUILD_TESTS AND NOT EMSCRIPTEN AND NOT QUOLL_UI_HOT)
  add_executable(quoll_ui_hot_test tests/hot_test.cpp)
  target_compile_definitions(quoll_ui_hot_test PRIVATE QUOLL_UI_HOT)
  target_include_directories(quoll_ui_hot_test PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_link_libraries(quoll_ui_hot_test PRIVATE quoll::ui quoll_warnings)
  set_property(GLOBAL APPEND PROPERTY QUOLL_UI_HOT_TEST quoll_ui_hot_test)
endif()
