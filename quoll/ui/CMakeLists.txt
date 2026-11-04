quoll_declare_module(NAME ui DEPS quoll::async $<BUILD_INTERFACE:${quoll_glaze_dependency}> $<INSTALL_INTERFACE:glaze::glaze>)

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
