cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED SITOMETRON_SOURCE_DIR OR "${SITOMETRON_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "SITOMETRON_SOURCE_DIR is required")
endif()
if(NOT DEFINED CORE_NEGATIVE_MODE)
  message(FATAL_ERROR "CORE_NEGATIVE_MODE is required")
endif()

function(run_check _label _expected_result _script)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" ${ARGN} -P "${SITOMETRON_SOURCE_DIR}/cmake/${_script}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error)
  string(CONCAT _combined "${_output}" "${_error}")
  if(NOT _result EQUAL ${_expected_result})
    message(FATAL_ERROR "${_label} unexpected result ${_result}: ${_combined}")
  endif()
  set(RUN_CHECK_OUTPUT "${_combined}" PARENT_SCOPE)
endfunction()

if(CORE_NEGATIVE_MODE STREQUAL "private")
  run_check(
    "approved private include control" 0
    check_core_dependency_isolation.cmake
    "-DSITOMETRON_CORE_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/core_dependency_approved.hpp")
  run_check(
    "unapproved private include" 1
    check_core_dependency_isolation.cmake
    "-DSITOMETRON_CORE_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/core_dependency_forbidden.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "unapproved core include")
    message(FATAL_ERROR
      "unapproved private include did not report the include violation: ${RUN_CHECK_OUTPUT}")
  endif()
elseif(CORE_NEGATIVE_MODE STREQUAL "public")
  run_check(
    "actual public API boundary" 0
    check_core_public_api_isolation.cmake)
  run_check(
    "approved public API control" 0
    check_core_public_api_isolation.cmake
    "-DSITOMETRON_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/core_public_api_approved.hpp")
  run_check(
    "public dependency include" 1
    check_core_public_api_isolation.cmake
    "-DSITOMETRON_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/core_public_api_forbidden_include.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "dependency-owned public include")
    message(FATAL_ERROR
      "public dependency include did not report the include violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_check(
    "public dependency type" 1
    check_core_public_api_isolation.cmake
    "-DSITOMETRON_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/core_public_api_forbidden_type.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "dependency-owned public type")
    message(FATAL_ERROR
      "public dependency type did not report the type violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_check(
    "public concurrency include" 1
    check_core_public_api_isolation.cmake
    "-DSITOMETRON_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/core_public_api_forbidden_concurrency_include.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "private-only public include")
    message(FATAL_ERROR "public concurrency include did not report the violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_check(
    "public concurrency type" 1
    check_core_public_api_isolation.cmake
    "-DSITOMETRON_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/core_public_api_forbidden_concurrency_type.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "private-only public type")
    message(FATAL_ERROR "public concurrency type did not report the violation: ${RUN_CHECK_OUTPUT}")
  endif()
elseif(CORE_NEGATIVE_MODE STREQUAL "target")
  if(NOT DEFINED SITOMETRON_NEGATIVE_SCRATCH_DIR OR
     "${SITOMETRON_NEGATIVE_SCRATCH_DIR}" STREQUAL "")
    message(FATAL_ERROR
      "SITOMETRON_NEGATIVE_SCRATCH_DIR is required for the target isolation probe")
  endif()

  function(run_target_probe _label _expected_result _interface_mode)
    set(_probe_dir "${SITOMETRON_NEGATIVE_SCRATCH_DIR}")
    file(REMOVE_RECURSE "${_probe_dir}")
    file(MAKE_DIRECTORY "${_probe_dir}")
    string(JOIN ";" _target_list ${ARGN})
    set(_probe_cmake [=[
cmake_minimum_required(VERSION 3.28)
project(core_target_probe NONE)
add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
add_library(Boost::uuid INTERFACE IMPORTED)
add_library(Boost::hash2 INTERFACE IMPORTED)
add_library(Threads::Threads INTERFACE IMPORTED)
add_library(sitometron_local_forbidden INTERFACE)
set(SITOMETRON_CORE_LINK_TARGETS
  nlohmann_json::nlohmann_json Boost::uuid Boost::hash2 Threads::Threads)
if(@INTERFACE_MODE@)
  set(SITOMETRON_CORE_INTERFACE_LINK_TARGETS @TARGETS@)
else()
  set(SITOMETRON_CORE_LINK_TARGETS @TARGETS@)
endif()
include("@HELPER@")
]=])
    set(TARGETS "${_target_list}")
    set(INTERFACE_MODE "${_interface_mode}")
    set(HELPER "${SITOMETRON_SOURCE_DIR}/cmake/check_core_link_target_allowlist.cmake")
    string(CONFIGURE "${_probe_cmake}" _probe_cmake @ONLY)
    file(WRITE "${_probe_dir}/CMakeLists.txt" "${_probe_cmake}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -S "${_probe_dir}" -B "${_probe_dir}/build" -G Ninja
      RESULT_VARIABLE _result
      OUTPUT_VARIABLE _output
      ERROR_VARIABLE _error)
    string(CONCAT _combined "${_output}" "${_error}")
    if(NOT _result EQUAL ${_expected_result})
      message(FATAL_ERROR "${_label} unexpected result ${_result}: ${_combined}")
    endif()
    set(RUN_CHECK_OUTPUT "${_combined}" PARENT_SCOPE)
  endfunction()

  run_target_probe(
    "approved target control" 0 FALSE
    nlohmann_json::nlohmann_json Boost::uuid Boost::hash2 Threads::Threads)
  run_target_probe(
    "unapproved target" 1 FALSE
    nlohmann_json::nlohmann_json Boost::uuid Threads::Threads sitometron_local_forbidden)
  if(NOT RUN_CHECK_OUTPUT MATCHES "unapproved core link target")
    message(FATAL_ERROR
      "unapproved target did not report the target violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_target_probe(
    "missing approved target" 1 FALSE
    nlohmann_json::nlohmann_json Boost::uuid Threads::Threads)
  if(NOT RUN_CHECK_OUTPUT MATCHES "exact target set mismatch")
    message(FATAL_ERROR
      "missing target did not report the exact-set violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_target_probe(
    "LINK_ONLY target control" 0 TRUE
    "\\$<LINK_ONLY:nlohmann_json::nlohmann_json>"
    "\\$<LINK_ONLY:Boost::uuid>"
    "\\$<LINK_ONLY:Boost::hash2>"
    "\\$<LINK_ONLY:Threads::Threads>")
  run_target_probe(
    "public target leakage" 1 TRUE
    nlohmann_json::nlohmann_json Boost::uuid Boost::hash2 Threads::Threads)
  if(NOT RUN_CHECK_OUTPUT MATCHES "public dependency leakage")
    message(FATAL_ERROR
      "public target did not report the interface violation: ${RUN_CHECK_OUTPUT}")
  endif()
else()
  message(FATAL_ERROR "Unknown CORE_NEGATIVE_MODE: ${CORE_NEGATIVE_MODE}")
endif()

message(STATUS
  "CoreDependencyNegativeIsolation ${CORE_NEGATIVE_MODE} violation-specific controls passed")
