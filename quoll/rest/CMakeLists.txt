# JSON comes from Glaze (header-only). The application may provide it, as a glaze::glaze target defined before Quoll
# is added; without one, Quoll fetches its own copy under names of its own (CPM package quoll_glaze, target
# quoll_glaze_headers). Glaze's own CMakeLists requires CMake 3.31: Quoll uses its headers directly. An installed
# quoll::rest links glaze::glaze, which the application defines or which quoll-config finds (find_package(glaze)).
if(TARGET glaze::glaze)
  set(quoll_glaze_dependency glaze::glaze)
else()
  CPMAddPackage(
    NAME quoll_glaze
    VERSION 9.1.0
    URL https://github.com/stephenberry/glaze/archive/refs/tags/v9.1.0.tar.gz
    URL_HASH SHA256=55f8a675397c505f9bee6c160e1c26b0085868f185d04d7a7727531fdbff4bd4
    DOWNLOAD_ONLY YES
  )

  add_library(quoll_glaze_headers INTERFACE)
  target_include_directories(quoll_glaze_headers SYSTEM INTERFACE ${quoll_glaze_SOURCE_DIR}/include)
  set(quoll_glaze_dependency quoll_glaze_headers)
endif()

quoll_declare_module(NAME rest DEPS quoll::http $<BUILD_INTERFACE:${quoll_glaze_dependency}> $<INSTALL_INTERFACE:glaze::glaze>)

quoll_declare_tests(
  SOURCES
    tests/client_test.cpp
    tests/contract_test.cpp
    tests/json_codec_test.cpp
    tests/mount_test.cpp
    tests/param_traits_test.cpp
    tests/path_template_test.cpp
  DEPS
    quoll::rest
)

# What the contract, mount and the client refuse at compile time, with the message the user gets.
quoll_declare_compile_fail_tests(
  SOURCES
    tests/compile_fail/bad_escape.cpp
    tests/compile_fail/body_on_get.cpp
    tests/compile_fail/client_unused_scheme.cpp
    tests/compile_fail/endpoint_not_in_api.cpp
    tests/compile_fail/mount_no_service.cpp
    tests/compile_fail/mount_no_services_at_all.cpp
    tests/compile_fail/mount_no_validator.cpp
    tests/compile_fail/mount_two_services.cpp
    tests/compile_fail/mount_two_validators.cpp
    tests/compile_fail/mount_wrong_return.cpp
    tests/compile_fail/not_a_part.cpp
    tests/compile_fail/parameter_repeated_by_prefix.cpp
    tests/compile_fail/path_after_rest_of_path.cpp
    tests/compile_fail/path_parameter_type.cpp
    tests/compile_fail/path_without_slash.cpp
    tests/compile_fail/prefix_trailing_slash.cpp
    tests/compile_fail/query_type.cpp
    tests/compile_fail/same_name.cpp
    tests/compile_fail/same_route.cpp
    tests/compile_fail/same_status.cpp
    tests/compile_fail/with_body_on_get.cpp
    tests/compile_fail/with_same_status.cpp
    tests/compile_fail/with_two_bodies.cpp
  DEPS
    quoll::rest
)
