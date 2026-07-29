cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not supported. Use a CMake preset.")
endif()

project(sitometron VERSION 0.1.0 LANGUAGES CXX)

option(SITOMETRON_BUILD_TESTS "Build sitometron tests" ON)
option(SITOMETRON_ENABLE_HTTP "Build the HTTP adapter" OFF)
option(SITOMETRON_ENABLE_SITOS "Build the sitos adapter" OFF)
option(SITOMETRON_ENABLE_HWLOC "Build the hwloc topology adapter" OFF)
option(SITOMETRON_ENABLE_QUILL "Build the Quill logging adapter" OFF)
option(SITOMETRON_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SITOMETRON_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(SITOMETRON_ENABLE_ASAN_UBSAN AND SITOMETRON_ENABLE_TSAN)
  message(FATAL_ERROR "ASan/UBSan and TSan cannot be enabled together")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(sitometron_enable_project_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /EHsc)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

function(sitometron_enable_sanitizers target)
  if(MSVC)
    if(SITOMETRON_ENABLE_ASAN_UBSAN OR SITOMETRON_ENABLE_TSAN)
      message(FATAL_ERROR "The selected sanitizer preset is not supported by the MSVC baseline")
    endif()
    return()
  endif()

  if(SITOMETRON_ENABLE_ASAN_UBSAN)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  elseif(SITOMETRON_ENABLE_TSAN)
    target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=thread)
  endif()
endfunction()

add_library(sitometron_core STATIC src/core/version.cpp)
add_library(sitometron::core ALIAS sitometron_core)
target_include_directories(
  sitometron_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(sitometron_core PUBLIC cxx_std_20)
sitometron_enable_project_warnings(sitometron_core)
sitometron_enable_sanitizers(sitometron_core)

get_target_property(SITOMETRON_CORE_LINK_LIBRARIES sitometron_core LINK_LIBRARIES)
if(SITOMETRON_CORE_LINK_LIBRARIES)
  message(FATAL_ERROR "sitometron_core must not link non-standard-library targets")
endif()

add_executable(sitometrond apps/sitometrond/main.cpp)
target_link_libraries(sitometrond PRIVATE sitometron::core)
sitometron_enable_project_warnings(sitometrond)
sitometron_enable_sanitizers(sitometrond)

if(SITOMETRON_BUILD_TESTS)
  include(CTest)
  enable_testing()

  add_library(sitometron_test_support INTERFACE)
  add_library(sitometron::test_support ALIAS sitometron_test_support)
  target_include_directories(sitometron_test_support INTERFACE tests/support)

  add_executable(sitometron_core_tests tests/unit/version_test.cpp)
  target_link_libraries(sitometron_core_tests PRIVATE sitometron::core sitometron::test_support)
  sitometron_enable_project_warnings(sitometron_core_tests)
  sitometron_enable_sanitizers(sitometron_core_tests)
  add_test(NAME sitometron_core_tests COMMAND sitometron_core_tests)

  add_test(
    NAME core_dependency_isolation
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_core_dependency_isolation.cmake)
  add_test(
    NAME core_dependency_isolation_rejects_third_party
    COMMAND
      ${CMAKE_COMMAND}
      -DSITOMETRON_CORE_SCAN_FILES=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/core_dependency_forbidden.hpp
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_core_dependency_isolation.cmake)
  set_tests_properties(core_dependency_isolation_rejects_third_party PROPERTIES WILL_FAIL TRUE)
endif()
