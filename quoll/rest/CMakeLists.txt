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
    tests/contract_test.cpp
    tests/json_codec_test.cpp
    tests/mount_test.cpp
    tests/param_traits_test.cpp
    tests/path_template_test.cpp
  DEPS
    quoll::rest
)
