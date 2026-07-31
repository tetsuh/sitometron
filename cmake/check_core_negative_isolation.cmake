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
    message(FATAL_ERROR "unapproved private include did not report the include violation: ${RUN_CHECK_OUTPUT}")
  endif()
elseif(CORE_NEGATIVE_MODE STREQUAL "public")
  run_check(
    "approved public API control" 0
    check_core_public_api_isolation.cmake
    "-DSITOMETRON_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/core_public_api_approved.hpp")
  run_check(
    "public dependency include" 1
    check_core_public_api_isolation.cmake
    "-DSITOMETRON_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/core_public_api_forbidden_include.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "dependency-owned public include")
    message(FATAL_ERROR "public dependency include did not report the include violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_check(
    "public dependency type" 1
    check_core_public_api_isolation.cmake
    "-DSITOMETRON_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/core_public_api_forbidden_type.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "dependency-owned public type")
    message(FATAL_ERROR "public dependency type did not report the type violation: ${RUN_CHECK_OUTPUT}")
  endif()
elseif(CORE_NEGATIVE_MODE STREQUAL "target")
  run_check(
    "approved target control" 0
    check_core_link_target_allowlist.cmake
    "-DSITOMETRON_CORE_LINK_TARGETS=nlohmann_json::nlohmann_json\\;Boost::uuid\\;Boost::hash2")
  run_check(
    "unapproved target" 1
    check_core_link_target_allowlist.cmake
    "-DSITOMETRON_CORE_LINK_TARGETS=nlohmann_json::nlohmann_json\\;sitometron_local_forbidden")
  if(NOT RUN_CHECK_OUTPUT MATCHES "unapproved core link target")
    message(FATAL_ERROR "unapproved target did not report the target violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_check(
    "LINK_ONLY target control" 0
    check_core_link_target_allowlist.cmake
    "-DSITOMETRON_CORE_LINK_TARGETS=$<LINK_ONLY:nlohmann_json::nlohmann_json>")
else()
  message(FATAL_ERROR "Unknown CORE_NEGATIVE_MODE: ${CORE_NEGATIVE_MODE}")
endif()

message(STATUS "CoreDependencyNegativeIsolation ${CORE_NEGATIVE_MODE} violation-specific controls passed")
