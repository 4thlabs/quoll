quoll_declare_module(
  NAME ui
  SOURCES
    src/app/app_core.cpp
    src/component/hook_store.cpp
    src/component/instance.cpp
    src/component/ready.cpp
    src/context/context_entry.cpp
    src/context/member_registry.cpp
    src/data/effect.cpp
    src/data/run_error.cpp
    src/data/run_slot.cpp
    src/routing/history.cpp
    src/routing/router.cpp
    src/view/element.cpp
    src/view/expand.cpp
    src/view/hot.cpp
    src/view/reconciler.cpp
    src/view/renderer_ref.cpp
    src/view/to_html.cpp
    src/view/view_context.cpp
  DEPS
    quoll::async
    quoll::http
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
    tests/app/app_test.cpp
    tests/component/callback_test.cpp
    tests/component/function_component_test.cpp
    tests/component/struct_component_test.cpp
    tests/component/nested_test.cpp
    tests/component/skip_test.cpp
    tests/context/context_test.cpp
    tests/data/action_test.cpp
    tests/data/effect_test.cpp
    tests/data/form_state_test.cpp
    tests/routing/router_test.cpp
    tests/view/element_test.cpp
    tests/view/to_html_test.cpp
    tests/widget/widget_test.cpp
  DEPS
    quoll::ui
)

# What a component must be, with the message the user gets.
quoll_declare_compile_fail_tests(
  SOURCES
    tests/compile_fail/action_mutable_reference_argument.cpp
    tests/compile_fail/action_not_a_task_function.cpp
    tests/compile_fail/action_optional_view_argument.cpp
    tests/compile_fail/action_throwing_move.cpp
    tests/compile_fail/action_with_move_only_argument.cpp
    tests/compile_fail/callback_optional_view_argument.cpp
    tests/compile_fail/callback_optional_view_result.cpp
    tests/compile_fail/callback_reference_argument.cpp
    tests/compile_fail/callback_reference_result.cpp
    tests/compile_fail/callback_reference_wrapper_argument.cpp
    tests/compile_fail/callback_transform_view_argument.cpp
    tests/compile_fail/callback_view_argument.cpp
    tests/compile_fail/callback_view_result.cpp
    tests/compile_fail/component_capturing.cpp
    tests/compile_fail/component_generic_lambda.cpp
    tests/compile_fail/component_later_parameter_by_value.cpp
    tests/compile_fail/component_mutable_parameter.cpp
    tests/compile_fail/component_not_a_view.cpp
    tests/compile_fail/component_other_props.cpp
    tests/compile_fail/component_pointer_parameter.cpp
    tests/compile_fail/component_props_by_value.cpp
    tests/compile_fail/component_struct_props_by_value.cpp
    tests/compile_fail/component_view_parameter.cpp
    tests/compile_fail/effect_optional_view_dependency.cpp
    tests/compile_fail/effect_pair_span_value.cpp
    tests/compile_fail/effect_values_without_target.cpp
    tests/compile_fail/effect_view_value.cpp
    tests/compile_fail/event_function_signature.cpp
    tests/compile_fail/form_state_bind_struct_field.cpp
    tests/compile_fail/redirect_trailing_slash.cpp
    tests/compile_fail/redirect_unknown_param.cpp
    tests/compile_fail/route_child_absolute.cpp
    tests/compile_fail/route_field_not_a_member.cpp
    tests/compile_fail/route_field_of_other_props.cpp
    tests/compile_fail/route_field_twice.cpp
    tests/compile_fail/route_fields_without_props.cpp
    tests/compile_fail/route_member_twice.cpp
    tests/compile_fail/route_not_found_path_field.cpp
    tests/compile_fail/route_page_of_parameters.cpp
    tests/compile_fail/route_param_without_field.cpp
    tests/compile_fail/route_parameter_read_twice.cpp
    tests/compile_fail/route_path_name_unknown.cpp
    tests/compile_fail/route_props_not_made.cpp
    tests/compile_fail/route_props_not_routable.cpp
    tests/compile_fail/route_trailing_slash.cpp
    tests/compile_fail/route_typed_param.cpp
    tests/compile_fail/router_provided.cpp
    tests/compile_fail/shared_const_context.cpp
    tests/compile_fail/struct_component_for_its_tag.cpp
    tests/compile_fail/struct_component_not_derived.cpp
    tests/compile_fail/struct_component_view_props.cpp
    tests/compile_fail/struct_component_without_view.cpp
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
  add_executable(quoll_ui_hot_test tests/app/hot_test.cpp)
  target_compile_definitions(quoll_ui_hot_test PRIVATE QUOLL_UI_HOT)
  target_include_directories(quoll_ui_hot_test PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_link_libraries(quoll_ui_hot_test PRIVATE quoll::ui quoll_warnings)
  set_property(GLOBAL APPEND PROPERTY QUOLL_UI_HOT_TEST quoll_ui_hot_test)
endif()
