# A command-line program calling the Docker Engine over its Unix domain socket: natively, where the system has such
# sockets.
if(EMSCRIPTEN OR NOT UNIX)
  return()
endif()

add_executable(quoll_docker docker.cpp)
target_link_libraries(quoll_docker PRIVATE quoll::rest quoll::runtime)

# The fake Docker its test runs against, on a socket of its own, rather than the machine's engine.
add_executable(quoll_fake_docker fake_docker.cpp)
target_link_libraries(quoll_fake_docker PRIVATE quoll::rest quoll::runtime quoll::server)

if(TARGET quoll_warnings)
  target_link_libraries(quoll_docker PRIVATE quoll_warnings)
  target_link_libraries(quoll_fake_docker PRIVATE quoll_warnings)
endif()

if(QUOLL_BUILD_TESTS)
  add_test(NAME docker_against_fake COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/against_fake.sh
                                            $<TARGET_FILE:quoll_fake_docker> $<TARGET_FILE:quoll_docker>)
endif()
