cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED SITOMETRON_SOURCE_DIR OR "${SITOMETRON_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "HttpDependencyNegativeIsolation setup failure: SITOMETRON_SOURCE_DIR is required")
endif()
if(NOT DEFINED HTTP_NEGATIVE_MODE)
  message(FATAL_ERROR "HttpDependencyNegativeIsolation setup failure: HTTP_NEGATIVE_MODE is required")
endif()

set(_allowed_targets_arg
  "sitometron::core,Boost::beast,Boost::asio,nlohmann_json::nlohmann_json")
set(_approved_fixture
  "${SITOMETRON_SOURCE_DIR}/tests/fixtures/http_dependency_approved.hpp")
set(_approved_public_fixture
  "${SITOMETRON_SOURCE_DIR}/tests/fixtures/http_public_api_approved.hpp")

function(run_check _label _expected_result _script)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" ${ARGN}
      -P "${SITOMETRON_SOURCE_DIR}/cmake/${_script}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error)
  string(CONCAT _combined "${_output}" "${_error}")
  if(NOT _result EQUAL ${_expected_result})
    message(FATAL_ERROR "${_label} unexpected result ${_result}: ${_combined}")
  endif()
  set(RUN_CHECK_OUTPUT "${_combined}" PARENT_SCOPE)
endfunction()

function(assert_http_include_input_gap _fixture _line)
  set(_path "${SITOMETRON_SOURCE_DIR}/tests/fixtures/${_fixture}")
  run_check(
    "HTTP physical-line input ${_fixture}" 1
    check_http_dependency_allowlist.cmake
    "-DSITOMETRON_HTTP_LINK_TARGETS=${_allowed_targets_arg}"
    "-DSITOMETRON_HTTP_SCAN_FILES=${_path}")
  if(NOT RUN_CHECK_OUTPUT MATCHES "unapproved HTTP include")
    message(FATAL_ERROR
      "${_fixture} did not report the HTTP include violation: ${RUN_CHECK_OUTPUT}")
  endif()
  string(FIND "${RUN_CHECK_OUTPUT}" "${_path}:${_line}:" _location)
  if(_location EQUAL -1)
    message(FATAL_ERROR
      "${_fixture} did not report the physical file and line ${_path}:${_line}: "
      "${RUN_CHECK_OUTPUT}")
  endif()
endfunction()

function(assert_http_public_input_gap _fixture _line)
  set(_path "${SITOMETRON_SOURCE_DIR}/tests/fixtures/${_fixture}")
  run_check(
    "HTTP public physical-line input ${_fixture}" 1
    check_http_public_api_isolation.cmake
    "-DSITOMETRON_HTTP_PUBLIC_SCAN_FILES=${_path}")
  if(NOT RUN_CHECK_OUTPUT MATCHES "dependency-owned public include")
    message(FATAL_ERROR
      "${_fixture} did not report the public include violation: ${RUN_CHECK_OUTPUT}")
  endif()
  string(FIND "${RUN_CHECK_OUTPUT}" "${_path}:${_line}:" _location)
  if(_location EQUAL -1)
    message(FATAL_ERROR
      "${_fixture} did not report the physical file and line ${_path}:${_line}: "
      "${RUN_CHECK_OUTPUT}")
  endif()
endfunction()

if(HTTP_NEGATIVE_MODE STREQUAL "allowlist")
  run_check(
    "approved HTTP dependency control" 0
    check_http_dependency_allowlist.cmake
    "-DSITOMETRON_HTTP_LINK_TARGETS=${_allowed_targets_arg}"
    "-DSITOMETRON_HTTP_SCAN_FILES=${_approved_fixture}")
elseif(HTTP_NEGATIVE_MODE STREQUAL "include")
  run_check(
    "approved HTTP include control" 0
    check_http_dependency_allowlist.cmake
    "-DSITOMETRON_HTTP_LINK_TARGETS=${_allowed_targets_arg}"
    "-DSITOMETRON_HTTP_SCAN_FILES=${_approved_fixture}")
  run_check(
    "approved HTTP physical-line input control" 0
    check_http_dependency_allowlist.cmake
    "-DSITOMETRON_HTTP_LINK_TARGETS=${_allowed_targets_arg}"
    "-DSITOMETRON_HTTP_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/dependency_scanner_approved_brackets.hpp")
  assert_http_include_input_gap(dependency_scanner_forbidden_range_open.hpp 5)
  assert_http_include_input_gap(dependency_scanner_forbidden_range_close.hpp 5)
  assert_http_include_input_gap(dependency_scanner_forbidden_trailing_bracket.hpp 5)
  assert_http_include_input_gap(dependency_scanner_forbidden_string_bracket.hpp 5)
  assert_http_include_input_gap(dependency_scanner_forbidden_tabs.hpp 5)
  assert_http_include_input_gap(dependency_scanner_forbidden_bom.hpp 1)
  foreach(_fixture IN ITEMS
      http_dependency_forbidden_whitespace.hpp
      http_dependency_forbidden_beast_websocket.hpp
      http_dependency_forbidden_asio_spawn.hpp
      http_dependency_forbidden_asio_ssl.hpp
      http_dependency_forbidden_beast_tcp_stream.hpp)
    run_check(
      "unapproved HTTP include ${_fixture}" 1
      check_http_dependency_allowlist.cmake
      "-DSITOMETRON_HTTP_LINK_TARGETS=${_allowed_targets_arg}"
      "-DSITOMETRON_HTTP_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/${_fixture}")
    if(NOT RUN_CHECK_OUTPUT MATCHES "unapproved HTTP include")
      message(FATAL_ERROR
        "${_fixture} did not report the include violation: ${RUN_CHECK_OUTPUT}")
    endif()
  endforeach()
  run_check(
    "unparseable HTTP include macro" 1
    check_http_dependency_allowlist.cmake
    "-DSITOMETRON_HTTP_LINK_TARGETS=${_allowed_targets_arg}"
    "-DSITOMETRON_HTTP_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/http_dependency_forbidden_macro.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "unparseable HTTP include directive")
    message(FATAL_ERROR
      "HTTP include macro did not report the directive violation: ${RUN_CHECK_OUTPUT}")
  endif()
elseif(HTTP_NEGATIVE_MODE STREQUAL "target")
  run_check(
    "approved HTTP target control" 0
    check_http_dependency_allowlist.cmake
    "-DSITOMETRON_HTTP_LINK_TARGETS=${_allowed_targets_arg}"
    "-DSITOMETRON_HTTP_SCAN_FILES=${_approved_fixture}")
  foreach(_forbidden_target IN ITEMS
      sitometron_http_local_forbidden
      Boost::context)
    run_check(
      "unapproved HTTP target ${_forbidden_target}" 1
      check_http_dependency_allowlist.cmake
      "-DSITOMETRON_HTTP_LINK_TARGETS=${_allowed_targets_arg},${_forbidden_target}"
      "-DSITOMETRON_HTTP_SCAN_FILES=${_approved_fixture}")
    if(NOT RUN_CHECK_OUTPUT MATCHES "unapproved HTTP link target")
      message(FATAL_ERROR
        "${_forbidden_target} did not report the target violation: ${RUN_CHECK_OUTPUT}")
    endif()
  endforeach()
elseif(HTTP_NEGATIVE_MODE STREQUAL "public")
  run_check(
    "approved HTTP public API control" 0
    check_http_public_api_isolation.cmake
    "-DSITOMETRON_HTTP_PUBLIC_SCAN_FILES=${_approved_public_fixture}")
  run_check(
    "approved HTTP public physical-line input control" 0
    check_http_public_api_isolation.cmake
    "-DSITOMETRON_HTTP_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/dependency_scanner_approved_brackets.hpp")
  assert_http_public_input_gap(dependency_scanner_forbidden_range_open.hpp 5)
  assert_http_public_input_gap(dependency_scanner_forbidden_range_close.hpp 5)
  assert_http_public_input_gap(dependency_scanner_forbidden_trailing_bracket.hpp 5)
  assert_http_public_input_gap(dependency_scanner_forbidden_string_bracket.hpp 5)
  assert_http_public_input_gap(dependency_scanner_forbidden_tabs.hpp 5)
  assert_http_public_input_gap(dependency_scanner_forbidden_bom.hpp 1)
  run_check(
    "HTTP public dependency include" 1
    check_http_public_api_isolation.cmake
    "-DSITOMETRON_HTTP_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/http_public_api_forbidden_include.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "dependency-owned public include")
    message(FATAL_ERROR
      "HTTP public dependency include did not report the violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_check(
    "HTTP public dependency type" 1
    check_http_public_api_isolation.cmake
    "-DSITOMETRON_HTTP_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/http_public_api_forbidden_type.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "dependency-owned public type")
    message(FATAL_ERROR
      "HTTP public dependency type did not report the violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_check(
    "HTTP public Boost.System dependency" 1
    check_http_public_api_isolation.cmake
    "-DSITOMETRON_HTTP_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/http_public_api_forbidden_boost_system.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "dependency-owned public include")
    message(FATAL_ERROR
      "HTTP public Boost.System dependency did not report the include violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_check(
    "HTTP public Asio include" 1
    check_http_public_api_isolation.cmake
    "-DSITOMETRON_HTTP_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/http_public_api_forbidden_asio_include.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "dependency-owned public include")
    message(FATAL_ERROR
      "HTTP public Asio include did not report the violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_check(
    "HTTP public Asio type" 1
    check_http_public_api_isolation.cmake
    "-DSITOMETRON_HTTP_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/http_public_api_forbidden_asio_type.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "dependency-owned public type")
    message(FATAL_ERROR
      "HTTP public Asio type did not report the violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_check(
    "HTTP public nlohmann include" 1
    check_http_public_api_isolation.cmake
    "-DSITOMETRON_HTTP_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/http_public_api_forbidden_nlohmann_include.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "dependency-owned public include")
    message(FATAL_ERROR
      "HTTP public nlohmann include did not report the violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_check(
    "HTTP public nlohmann type" 1
    check_http_public_api_isolation.cmake
    "-DSITOMETRON_HTTP_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/http_public_api_forbidden_nlohmann_type.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "dependency-owned public type")
    message(FATAL_ERROR
      "HTTP public nlohmann type did not report the violation: ${RUN_CHECK_OUTPUT}")
  endif()
  run_check(
    "HTTP public macro include" 1
    check_http_public_api_isolation.cmake
    "-DSITOMETRON_HTTP_PUBLIC_SCAN_FILES=${SITOMETRON_SOURCE_DIR}/tests/fixtures/http_public_api_forbidden_macro.hpp")
  if(NOT RUN_CHECK_OUTPUT MATCHES "unparseable HTTP public include directive")
    message(FATAL_ERROR
      "HTTP public macro include did not report the directive violation: ${RUN_CHECK_OUTPUT}")
  endif()
else()
  message(FATAL_ERROR "Unknown HTTP_NEGATIVE_MODE: ${HTTP_NEGATIVE_MODE}")
endif()

message(STATUS
  "HttpDependencyNegativeIsolation ${HTTP_NEGATIVE_MODE} violation-specific controls passed")
