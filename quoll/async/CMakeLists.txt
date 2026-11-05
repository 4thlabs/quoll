quoll_declare_module(NAME async)

quoll_declare_tests(
  SOURCES
    tests/any_executor_test.cpp
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

# What synchronized refuses, with what the compiler says.
quoll_declare_compile_fail_tests(
  SOURCES
    tests/compile_fail/synchronized_await.cpp
    tests/compile_fail/synchronized_reference.cpp
  DEPS
    quoll::async
)
