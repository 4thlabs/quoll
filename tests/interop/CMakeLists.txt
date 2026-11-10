# The two Quoll ends of the interop runs: see QUOLL_INTEROP in the root CMakeLists.txt, and run.sh.
if(EMSCRIPTEN)
  message(FATAL_ERROR "QUOLL_INTEROP is native only")
endif()

add_executable(quoll_interop_server server.cpp)
target_link_libraries(quoll_interop_server PRIVATE quoll::runtime quoll::server quoll_warnings)

add_executable(quoll_interop_client client.cpp)
target_link_libraries(quoll_interop_client PRIVATE quoll::client quoll::runtime quoll_warnings)
