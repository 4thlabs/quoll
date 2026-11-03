include_guard(GLOBAL)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(QUOLL_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

set_property(GLOBAL PROPERTY QUOLL_MODULES "")
set_property(GLOBAL PROPERTY QUOLL_TEST_SOURCES "")
set_property(GLOBAL PROPERTY QUOLL_TEST_DEPS "")

# Warnings for Quoll's own translation units (tests, header checks). Never exported to users.
if(NOT TARGET quoll_warnings)
  add_library(quoll_warnings INTERFACE)
  target_compile_options(quoll_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
  )
endif()

#
# Declare a header-only module from its CMakeLists.txt (quoll/<module>/, or quoll/adapters/<module>/ for an adapter):
#
#   quoll_declare_module(NAME <module> [DEPS <targets>...])
#
# Creates the INTERFACE target quoll_<module> with alias quoll::<module>, publishing every header under the include/
# directory next to that CMakeLists.txt. With QUOLL_BUILD_TESTS, each header is also compiled alone in its own
# translation unit, which enforces the self-contained header rule.
#
function(quoll_declare_module)
  cmake_parse_arguments(PARSE_ARGV 0 QUOLL_MODULE "" "NAME" "DEPS")

  if(NOT QUOLL_MODULE_NAME)
    message(FATAL_ERROR "quoll_declare_module: NAME is required")
  endif()

  set(target "quoll_${QUOLL_MODULE_NAME}")
  set(include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

  file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${include_dir}/*.hpp")

  add_library(${target} INTERFACE)
  add_library(quoll::${QUOLL_MODULE_NAME} ALIAS ${target})
  set_target_properties(${target} PROPERTIES EXPORT_NAME ${QUOLL_MODULE_NAME})

  target_sources(${target} INTERFACE FILE_SET HEADERS BASE_DIRS "${include_dir}" FILES ${headers})
  target_compile_features(${target} INTERFACE cxx_std_20)
  target_link_libraries(${target} INTERFACE ${QUOLL_MODULE_DEPS})

  set_property(GLOBAL APPEND PROPERTY QUOLL_MODULES ${QUOLL_MODULE_NAME})

  if(QUOLL_INSTALL)
    install(TARGETS ${target} EXPORT quoll-targets FILE_SET HEADERS)
  endif()

  if(QUOLL_BUILD_TESTS AND headers)
    quoll_detail_add_header_check(${target} "${include_dir}" ${headers})
  endif()
endfunction()

#
# Add test sources to the single test executable (built by quoll_main_tests):
#
#   quoll_declare_tests(SOURCES <files>... [DEPS <targets>...])
#
# Paths are relative to the calling CMakeLists.txt. Does nothing unless QUOLL_BUILD_TESTS is ON.
#
function(quoll_declare_tests)
  if(NOT QUOLL_BUILD_TESTS)
    return()
  endif()

  cmake_parse_arguments(PARSE_ARGV 0 QUOLL_TESTS "" "" "SOURCES;DEPS")

  foreach(source IN LISTS QUOLL_TESTS_SOURCES)
    set_property(GLOBAL APPEND PROPERTY QUOLL_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
  endforeach()

  set_property(GLOBAL APPEND PROPERTY QUOLL_TEST_DEPS ${QUOLL_TESTS_DEPS})
endfunction()

#
# Build the test executable from every source declared with quoll_declare_tests. Called once from
# tests/CMakeLists.txt, after every module has been added.
#
function(quoll_main_tests)
  get_property(sources GLOBAL PROPERTY QUOLL_TEST_SOURCES)
  get_property(deps GLOBAL PROPERTY QUOLL_TEST_DEPS)

  if(NOT sources)
    message(STATUS "Quoll: no test declared, skipping the test executable")
    return()
  endif()

  list(REMOVE_DUPLICATES deps)

  CPMAddPackage("gh:catchorg/Catch2@3.8.1")

  add_executable(quoll_tests ${sources})
  target_link_libraries(quoll_tests PRIVATE Catch2::Catch2WithMain quoll_warnings ${deps})
  # Helpers shared by the tests of every module (quoll/test/...).
  target_include_directories(quoll_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)

  # Under Emscripten, CTest runs the target through CMAKE_CROSSCOMPILING_EMULATOR (node). Tests may wait for the
  # JavaScript event loop (fetch, timers) by suspending there with JSPI, which node 24 still hides behind a flag.
  if(EMSCRIPTEN)
    target_link_options(quoll_tests PRIVATE -sJSPI)
    set_property(TARGET quoll_tests PROPERTY CROSSCOMPILING_EMULATOR ${CMAKE_CROSSCOMPILING_EMULATOR}
                                                                     --experimental-wasm-jspi)
  endif()

  add_test(NAME quoll COMMAND quoll_tests)
endfunction()

#
# Generate the CMake package (find_package(quoll)) for every declared module. Called once from the root
# CMakeLists.txt, after every module has been added.
#
function(quoll_install)
  get_property(modules GLOBAL PROPERTY QUOLL_MODULES)

  if(NOT modules)
    return()
  endif()

  set(config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/quoll")

  # Expanded in quoll-config.cmake.in: each module is a component of find_package(quoll COMPONENTS ...).
  set(QUOLL_PACKAGE_MODULES ${modules})

  install(EXPORT quoll-targets NAMESPACE quoll:: DESTINATION "${config_dir}")

  configure_package_config_file(
    "${QUOLL_CMAKE_DIR}/quoll-config.cmake.in"
    "${PROJECT_BINARY_DIR}/quoll-config.cmake"
    INSTALL_DESTINATION "${config_dir}"
  )

  write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/quoll-config-version.cmake"
    COMPATIBILITY SameMinorVersion
    ARCH_INDEPENDENT
  )

  install(
    FILES "${PROJECT_BINARY_DIR}/quoll-config.cmake" "${PROJECT_BINARY_DIR}/quoll-config-version.cmake"
    DESTINATION "${config_dir}"
  )
endfunction()

# One translation unit per header, each including only that header.
function(quoll_detail_add_header_check target include_dir)
  set(sources "")

  foreach(header IN LISTS ARGN)
    file(RELATIVE_PATH relative "${include_dir}" "${header}")
    # Mirror the header path: distinct headers always get distinct sources.
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_check/${relative}.cpp")
    file(CONFIGURE OUTPUT "${source}" CONTENT "#include <${relative}>\n")
    list(APPEND sources "${source}")
  endforeach()

  add_library(${target}_header_check OBJECT ${sources})
  target_link_libraries(${target}_header_check PRIVATE ${target} quoll_warnings)
endfunction()
