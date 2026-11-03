add_executable(quoll_notes_server server.cpp)
target_link_libraries(quoll_notes_server PRIVATE quoll::asio quoll::rest quoll::server)

add_executable(quoll_notes_client client.cpp)
target_link_libraries(quoll_notes_client PRIVATE quoll::asio quoll::client quoll::rest)

# The whole session of the client against the server, over loopback TCP.
if(QUOLL_BUILD_TESTS AND UNIX)
  add_test(
    NAME notes_round_trip
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/round_trip.sh $<TARGET_FILE:quoll_notes_server>
            $<TARGET_FILE:quoll_notes_client>
  )
endif()
