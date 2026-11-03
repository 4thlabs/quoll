# Run by the compile_fail.<case> ctests (see quoll_compile_fail_tests in QuollHelpers.cmake):
#
#   cmake -DQUOLL_BUILD_DIR=<dir> -DQUOLL_TARGET=<target> -DQUOLL_CONFIG=<config> -DQUOLL_EXPECT=<text> -P <this file>
#
# Builds the target of a source that must not compile, and succeeds only if the build fails with <text>, taken
# literally, in its output: a build that succeeds, or fails for another reason, is an error.
execute_process(
  COMMAND ${CMAKE_COMMAND} --build "${QUOLL_BUILD_DIR}" --target "${QUOLL_TARGET}" --config "${QUOLL_CONFIG}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output
)

if(result EQUAL 0)
  message(FATAL_ERROR "${QUOLL_TARGET} compiled, but must not:\n${output}")
endif()

string(FIND "${output}" "${QUOLL_EXPECT}" position)

if(position EQUAL -1)
  message(FATAL_ERROR "${QUOLL_TARGET} did not compile, but without the message '${QUOLL_EXPECT}':\n${output}")
endif()
