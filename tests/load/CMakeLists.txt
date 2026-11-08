# A mixed load under the sanitizers: see QUOLL_LOAD in the root CMakeLists.txt, and load.yml, which runs it every night.
if(EMSCRIPTEN)
  message(FATAL_ERROR "QUOLL_LOAD is native only")
endif()

add_executable(quoll_load load.cpp)
target_include_directories(quoll_load PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(quoll_load PRIVATE quoll::client quoll::runtime quoll::server quoll_warnings)
