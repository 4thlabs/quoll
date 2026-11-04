# Run by the compile_fail.<case> ctests (see quoll_compile_fail_tests in QuollHelpers.cmake):
#
#   cmake -DQUOLL_COMPILER=<compiler> -DQUOLL_FLAGS=<flags> -DQUOLL_INCLUDES=<directories> -DQUOLL_SOURCE=<file>
#         -P <this file>
#
# Compiles a source that must not compile, and succeeds only if the compilation fails with the text of the source's
# `// expect: <text>` line, taken literally, in its output: a compilation that succeeds, or fails for another reason, is
# an error. The text is read here rather than passed: no argument list to split it on a ';'. Checking the syntax is
# enough: templates are instantiated and static_assert evaluated, and no object file is left behind.
file(READ "${QUOLL_SOURCE}" source)
# At the start of a line, without the line end (LF or CRLF), as the configure step finds it.
string(REGEX MATCH "(^|\n)// expect: [^\r\n]*" expect_line "${source}")
string(REGEX REPLACE "^\n?// expect: " "" QUOLL_EXPECT "${expect_line}")

if(QUOLL_EXPECT STREQUAL "")
  message(FATAL_ERROR "${QUOLL_SOURCE}: no '// expect: <text>' line")
endif()

list(TRANSFORM QUOLL_INCLUDES PREPEND "-I")

execute_process(
  COMMAND "${QUOLL_COMPILER}" ${QUOLL_FLAGS} ${QUOLL_INCLUDES} -fsyntax-only "${QUOLL_SOURCE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output
)

if(result EQUAL 0)
  message(FATAL_ERROR "${QUOLL_SOURCE} compiled, but must not:\n${output}")
endif()

string(FIND "${output}" "${QUOLL_EXPECT}" position)

if(position EQUAL -1)
  message(FATAL_ERROR "${QUOLL_SOURCE} did not compile, but without the message '${QUOLL_EXPECT}':\n${output}")
endif()
