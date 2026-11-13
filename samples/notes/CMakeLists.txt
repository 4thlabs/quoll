# The notes: one REST contract (api/), served by server/, called by cli/ natively and by the page in web/ in a browser.

# The contract, which every face includes.
add_library(quoll_notes_api INTERFACE)
target_include_directories(quoll_notes_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/api)
target_link_libraries(quoll_notes_api INTERFACE quoll::rest)

# The components of the page, a header and a source for each, whose sources are those of web/src/components/: one made
# there is compiled at the next build, a change of one compiles again only it. The test of the page runs them natively,
# in place of the calls of web/src/client/notes_client.cpp, which go through fetch.
file(GLOB_RECURSE quoll_notes_components CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/src/components/*.cpp)
add_library(quoll_notes_web_components INTERFACE)
target_sources(quoll_notes_web_components INTERFACE ${quoll_notes_components})
target_include_directories(quoll_notes_web_components INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/web/src)
target_link_libraries(quoll_notes_web_components INTERFACE quoll_notes_api quoll::ui)

quoll_declare_tests(SOURCES tests/notes_page_test.cpp DEPS quoll_notes_web_components)

if(EMSCRIPTEN)
  add_subdirectory(web)
  return()
endif()

add_executable(quoll_notes_server server/main.cpp)
target_link_libraries(quoll_notes_server PRIVATE quoll_notes_api quoll::runtime quoll::server)

add_executable(quoll_notes_cli cli/main.cpp)
target_link_libraries(quoll_notes_cli PRIVATE quoll_notes_api quoll::runtime)

# What dangles is an error in a sample too.
if(TARGET quoll_warnings)
  target_link_libraries(quoll_notes_server PRIVATE quoll_warnings)
  target_link_libraries(quoll_notes_cli PRIVATE quoll_warnings)
endif()

# The whole session of the command line against the server, over loopback TCP.
if(QUOLL_BUILD_TESTS AND UNIX)
  add_test(
    NAME notes_round_trip
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/round_trip.sh $<TARGET_FILE:quoll_notes_server>
            $<TARGET_FILE:quoll_notes_cli>
  )
  set_tests_properties(notes_round_trip PROPERTIES RESOURCE_LOCK quoll_ports)
endif()
