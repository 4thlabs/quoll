# Fuzz targets, with Clang's libFuzzer, under ASan and UBSan: see QUOLL_FUZZ in the root CMakeLists.txt.
function(quoll_add_fuzzer name)
  add_executable(quoll_fuzz_${name} ${name}.cpp)
  target_include_directories(quoll_fuzz_${name} PRIVATE support ${PROJECT_SOURCE_DIR}/tests/support)
  target_link_libraries(quoll_fuzz_${name} PRIVATE quoll::client quoll::server quoll::rest)
  target_link_options(quoll_fuzz_${name} PRIVATE -fsanitize=fuzzer)
  add_dependencies(quoll_fuzzers quoll_fuzz_${name})
endfunction()

add_custom_target(quoll_fuzzers)

foreach(target IN ITEMS client event_parser request_parser response_parser rest_client rest_server router serve_connection unfolding_client
                       unfolding_server)
  quoll_add_fuzzer(${target})
endforeach()
