# Run by the assert.<case> ctests (see quoll_main_tests in QuollHelpers.cmake):
#
#   cmake -DQUOLL_TESTS=<quoll_tests> -DQUOLL_CASE=<test case> -DQUOLL_EXPECT=<text> -P <this file>
#
# Runs one hidden test case that breaks what a debug build asserts, and succeeds only if the program ends on an
# assertion, with its text, taken literally, in its output: a case that passes, or fails for another reason, is an
# error. One built without assertions says so, and passes. A script rather than ctest's PASS_REGULAR_EXPRESSION, which
# counts a process ended by a signal as a failure.
execute_process(
  COMMAND "${QUOLL_TESTS}" "${QUOLL_CASE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output
)

string(FIND "${output}" "quoll: assertions are compiled out" compiled_out)

if(NOT compiled_out EQUAL -1)
  message(STATUS "'${QUOLL_CASE}': assertions are compiled out in this build, nothing to check")
  return()
endif()

if(result EQUAL 0)
  message(FATAL_ERROR "'${QUOLL_CASE}' passed, but must end on an assertion:\n${output}")
endif()

# glibc and libc++ both print "Assertion" where an assertion fails, before its text.
string(FIND "${output}" "Assertion" asserted)
string(FIND "${output}" "${QUOLL_EXPECT}" position)

if(asserted EQUAL -1 OR position EQUAL -1)
  message(FATAL_ERROR "'${QUOLL_CASE}' failed, but not on the assertion '${QUOLL_EXPECT}':\n${output}")
endif()
