# Run by the assert.<case> ctests (see quoll_main_tests in QuollHelpers.cmake):
#
#   cmake -DQUOLL_TESTS=<quoll_tests> -DQUOLL_CASE=<test case> -DQUOLL_EXPECT=<text> -P <this file>
#
# Runs one hidden test case that breaks what a debug build asserts, and succeeds only if the program ends with the
# assertion's text, taken literally, in its output: a case that passes, or fails for another reason, is an error. A
# script rather than ctest's PASS_REGULAR_EXPRESSION, which counts a process ended by a signal as a failure.
execute_process(
  COMMAND "${QUOLL_TESTS}" "${QUOLL_CASE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output
)

if(result EQUAL 0)
  message(FATAL_ERROR "'${QUOLL_CASE}' passed, but must end on an assertion:\n${output}")
endif()

string(FIND "${output}" "${QUOLL_EXPECT}" position)

if(position EQUAL -1)
  message(FATAL_ERROR "'${QUOLL_CASE}' failed, but without the assertion '${QUOLL_EXPECT}':\n${output}")
endif()
