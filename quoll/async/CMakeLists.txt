quoll_declare_module(NAME async SOURCES src/recycled_frame.cpp)

quoll_declare_tests(
  SOURCES
    tests/any_executor_test.cpp
    tests/co_spawn_test.cpp
    tests/out_of_memory_test.cpp
    tests/task_group_test.cpp
    tests/offload_test.cpp
    tests/recycled_frame_test.cpp
    tests/executor_ref_test.cpp
    tests/run_loop_test.cpp
    tests/slots_test.cpp
    tests/stop_token_test.cpp
    tests/synchronized_test.cpp
    tests/task_test.cpp
    tests/timer_test.cpp
    tests/when_all_test.cpp
  DEPS
    quoll::async
)

# What a debug build of the module asserts.
quoll_declare_assert_tests(
  NAME task_destroyed_while_it_runs
  CASE "task: destroying one that runs is asserted"
  EXPECT "still pointing into its frame"
)

# What the module refuses, with what the compiler says.
quoll_declare_compile_fail_tests(
  SOURCES
    tests/compile_fail/co_spawn_converts_to_exception_ptr.cpp
    tests/compile_fail/synchronized_await.cpp
    tests/compile_fail/synchronized_free_co_await.cpp
    tests/compile_fail/synchronized_rvalue_co_await.cpp
    tests/compile_fail/task_awaited_by_name.cpp
    tests/compile_fail/task_explicit_conversion.cpp
    tests/compile_fail/task_over_aligned.cpp
    tests/compile_fail/synchronized_reference.cpp
  DEPS
    quoll::async
)
