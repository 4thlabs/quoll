# A command-line program calling GitHub's REST API: natively, and under Emscripten in Node, through fetch.
add_executable(quoll_github github.cpp)
target_link_libraries(quoll_github PRIVATE quoll::rest quoll::runtime)

if(TARGET quoll_warnings)
  target_link_libraries(quoll_github PRIVATE quoll_warnings)
endif()

# In Node, through fetch: the program reads GITHUB_TOKEN and QUOLL_GITHUB_URL, which node_env.js hands over, as
# Emscripten keeps an environment of its own.
if(EMSCRIPTEN)
  target_link_options(quoll_github PRIVATE -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH
                      "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/node_env.js")

  # Run in Node through fetch against a fake GitHub Node serves, as against_fake.sh does natively.
  if(QUOLL_BUILD_TESTS)
    add_test(NAME github_through_fetch COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
                                               ${CMAKE_CURRENT_SOURCE_DIR}/against_fake.mjs $<TARGET_FILE:quoll_github>)
  endif()

  return()
endif()

# The fake GitHub its test runs against, rather than GitHub, whose limits a CI would meet.
add_executable(quoll_fake_github fake_github.cpp)
target_link_libraries(quoll_fake_github PRIVATE quoll::rest quoll::runtime quoll::server)

if(TARGET quoll_warnings)
  target_link_libraries(quoll_fake_github PRIVATE quoll_warnings)
endif()

if(QUOLL_BUILD_TESTS AND UNIX)
  add_test(NAME github_against_fake COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/against_fake.sh
                                            $<TARGET_FILE:quoll_fake_github> $<TARGET_FILE:quoll_github>)
  set_tests_properties(github_against_fake PROPERTIES RESOURCE_LOCK quoll_ports)
endif()
