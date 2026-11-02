quoll_declare_module(NAME async)

quoll_declare_tests(
  SOURCES
    tests/executor_ref_test.cpp
    tests/stop_token_test.cpp
    tests/task_test.cpp
    tests/when_all_test.cpp
  DEPS
    quoll::async
)
