add_executable(quoll_notes_server server.cpp)
target_link_libraries(quoll_notes_server PRIVATE quoll::asio quoll::rest quoll::server)

add_executable(quoll_notes_client client.cpp)
target_link_libraries(quoll_notes_client PRIVATE quoll::asio quoll::client quoll::rest)
