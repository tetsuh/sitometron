cmake_minimum_required(VERSION 3.28)
if(NOT SITOMETRON_SOURCE_DIR OR NOT SITOMETRON_IDENTITY_SCAN_CONFIG OR NOT IDENTITY_NEGATIVE_MODE)
  message(FATAL_ERROR "IdentityNegativeIsolation setup failure: source/config/mode are required")
endif()
set(_fixtures "${SITOMETRON_SOURCE_DIR}/tests/fixtures")
set(_approved_targets "sitometron::core,Boost::uuid")

function(identity_probe _expected _script _diagnostic _fixture _line)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" ${ARGN} -P "${SITOMETRON_SOURCE_DIR}/cmake/${_script}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
  string(CONCAT _combined "${_output}" "${_error}")
  if(NOT _result EQUAL _expected OR NOT _combined MATCHES "${_diagnostic}")
    message(FATAL_ERROR "IdentityNegativeIsolation unexpected probe result ${_result}: ${_combined}")
  endif()
  if(NOT "${_fixture}" STREQUAL "")
    string(FIND "${_combined}" "${_fixture}:${_line}:" _location)
    if(_location EQUAL -1)
      message(FATAL_ERROR "IdentityNegativeIsolation missing physical diagnostic location: ${_combined}")
    endif()
  endif()
  if(_expected EQUAL 0)
    message(STATUS "IdentityValidation accepted ${_script} ${_fixture}")
  else()
    message(STATUS "IdentityValidation rejected ${_fixture}: ${_diagnostic}")
  endif()
endfunction()

function(identity_include_probe _fixture _line _diagnostic)
  identity_probe(1 check_identity_dependency_allowlist.cmake "${_diagnostic}"
    "${_fixtures}/${_fixture}" "${_line}"
    "-DSITOMETRON_IDENTITY_LINK_TARGETS=${_approved_targets}"
    "-DSITOMETRON_IDENTITY_SCAN_FILES=${_fixtures}/${_fixture}")
endfunction()

function(identity_public_probe _fixture _line _diagnostic)
  identity_probe(1 check_identity_public_api_isolation.cmake "${_diagnostic}"
    "${_fixtures}/${_fixture}" "${_line}"
    "-DSITOMETRON_IDENTITY_PUBLIC_SCAN_FILES=${_fixtures}/${_fixture}")
endfunction()

if(IDENTITY_NEGATIVE_MODE STREQUAL "allowlist")
  identity_probe(0 check_identity_dependency_allowlist.cmake "passed" "" 0
    "-DSITOMETRON_IDENTITY_SCAN_CONFIG=${SITOMETRON_IDENTITY_SCAN_CONFIG}")
  foreach(_fixture IN ITEMS identity_dependency_approved.hpp identity_public_approved.hpp
      dependency_scanner_approved_brackets.hpp)
    identity_probe(0 check_identity_dependency_allowlist.cmake "passed" "" 0
      "-DSITOMETRON_IDENTITY_LINK_TARGETS=${_approved_targets}"
      "-DSITOMETRON_IDENTITY_SCAN_FILES=${_fixtures}/${_fixture}")
  endforeach()
  identity_probe(0 check_identity_dependency_allowlist.cmake "passed" "" 0
    "-DSITOMETRON_IDENTITY_LINK_TARGETS=${_approved_targets}"
    "-DSITOMETRON_IDENTITY_SCAN_FILES=${_fixtures}/identity_clock_approved.hpp"
    "-DSITOMETRON_IDENTITY_CLOCK_FILES=${_fixtures}/identity_clock_approved.hpp")
  identity_include_probe(identity_clock_approved.hpp 3 "chrono outside clock implementation")
  identity_include_probe(identity_source_forbidden_chrono_include.hpp 3
    "chrono outside clock implementation")
  foreach(_fixture IN ITEMS identity_source_forbidden_random_include.hpp
      identity_forbidden_unapproved_uuid.hpp identity_forbidden_json.hpp identity_forbidden_os.hpp
      identity_forbidden_other_adapter.hpp)
    identity_include_probe(${_fixture} 3 "unapproved identity include")
  endforeach()
  foreach(_fixture IN ITEMS dependency_scanner_forbidden_range_open.hpp
      dependency_scanner_forbidden_range_close.hpp dependency_scanner_forbidden_trailing_bracket.hpp
      dependency_scanner_forbidden_string_bracket.hpp dependency_scanner_forbidden_tabs.hpp)
    identity_include_probe(${_fixture} 5 "unapproved identity include")
  endforeach()
  identity_include_probe(dependency_scanner_forbidden_bom.hpp 1 "unapproved identity include")
  identity_include_probe(http_dependency_forbidden_macro.hpp 2 "unparseable identity include")
  foreach(_target IN ITEMS Boost::hash2 Boost::beast nlohmann_json::nlohmann_json
      identity_local_forbidden bcrypt)
    identity_probe(1 check_identity_dependency_allowlist.cmake "unapproved identity link target" "" 0
      "-DSITOMETRON_IDENTITY_LINK_TARGETS=${_approved_targets},${_target}"
      "-DSITOMETRON_IDENTITY_SCAN_FILES=${_fixtures}/identity_dependency_approved.hpp")
  endforeach()
  foreach(_targets IN ITEMS "sitometron::core" "sitometron::core,Boost::uuid,Boost::uuid")
    identity_probe(1 check_identity_dependency_allowlist.cmake "exact target set mismatch" "" 0
      "-DSITOMETRON_IDENTITY_LINK_TARGETS=${_targets}"
      "-DSITOMETRON_IDENTITY_SCAN_FILES=${_fixtures}/identity_dependency_approved.hpp")
  endforeach()
  identity_probe(1 check_identity_dependency_allowlist.cmake "public dependency leakage" "" 0
    "-DSITOMETRON_IDENTITY_LINK_TARGETS=${_approved_targets}"
    "-DSITOMETRON_IDENTITY_INTERFACE_LINK_TARGETS=${_approved_targets}"
    "-DSITOMETRON_IDENTITY_SCAN_FILES=${_fixtures}/identity_dependency_approved.hpp")
  foreach(_fixture IN ITEMS identity_forbidden_v7_in_core.hpp identity_forbidden_v4_in_core.hpp)
    identity_probe(1 check_core_dependency_isolation.cmake "unapproved core include"
      "${_fixtures}/${_fixture}" 3 "-DSITOMETRON_CORE_SCAN_FILES=${_fixtures}/${_fixture}")
  endforeach()
elseif(IDENTITY_NEGATIVE_MODE STREQUAL "public")
  identity_probe(0 check_identity_public_api_isolation.cmake "passed" "" 0
    "-DSITOMETRON_IDENTITY_SCAN_CONFIG=${SITOMETRON_IDENTITY_SCAN_CONFIG}")
  foreach(_fixture IN ITEMS identity_public_approved.hpp dependency_scanner_approved_brackets.hpp)
    identity_probe(0 check_identity_public_api_isolation.cmake "passed" "" 0
      "-DSITOMETRON_IDENTITY_PUBLIC_SCAN_FILES=${_fixtures}/${_fixture}")
  endforeach()
  foreach(_fixture IN ITEMS identity_forbidden_v7_in_core.hpp identity_forbidden_v4_in_core.hpp
      identity_forbidden_json.hpp identity_forbidden_os.hpp identity_clock_approved.hpp
      identity_source_forbidden_random_include.hpp)
    identity_public_probe(${_fixture} 3 "dependency-owned or clock public include")
  endforeach()
  foreach(_fixture IN ITEMS dependency_scanner_forbidden_range_open.hpp
      dependency_scanner_forbidden_range_close.hpp dependency_scanner_forbidden_trailing_bracket.hpp
      dependency_scanner_forbidden_string_bracket.hpp dependency_scanner_forbidden_tabs.hpp)
    identity_public_probe(${_fixture} 5 "dependency-owned or clock public include")
  endforeach()
  identity_public_probe(dependency_scanner_forbidden_bom.hpp 1
    "dependency-owned or clock public include")
  identity_public_probe(http_public_api_forbidden_macro.hpp 2 "unparseable identity include")
  foreach(_fixture IN ITEMS http_public_api_forbidden_boost_spaced.hpp
      http_public_api_forbidden_boost_tab.hpp http_public_api_forbidden_boost_comment.hpp
      http_public_api_forbidden_boost_alias.hpp http_public_api_forbidden_boost_using.hpp
      http_public_api_forbidden_nlohmann_spaced.hpp http_public_api_forbidden_nlohmann_tab.hpp
      http_public_api_forbidden_nlohmann_comment.hpp http_public_api_forbidden_nlohmann_alias.hpp
      http_public_api_forbidden_nlohmann_using.hpp)
    identity_public_probe(${_fixture} 5 "dependency or clock root word")
  endforeach()
  foreach(_fixture IN ITEMS identity_source_forbidden_spaced.hpp identity_source_forbidden_tab.hpp)
    identity_public_probe(${_fixture} 3 "dependency or clock root word")
  endforeach()
elseif(IDENTITY_NEGATIVE_MODE STREQUAL "source")
  identity_probe(0 check_identity_source_reads_no_other_clock_or_entropy.cmake "passed" "" 0
    "-DSITOMETRON_IDENTITY_SCAN_CONFIG=${SITOMETRON_IDENTITY_SCAN_CONFIG}")
  identity_probe(0 check_identity_source_reads_no_other_clock_or_entropy.cmake "passed" "" 0
    "-DSITOMETRON_IDENTITY_SOURCE_SCAN_FILES=${_fixtures}/identity_source_approved.hpp")
  file(GLOB _source_fixtures "${_fixtures}/identity_source_forbidden_*.hpp")
  list(LENGTH _source_fixtures _source_fixture_count)
  if(_source_fixture_count LESS 40)
    message(FATAL_ERROR "IdentityNegativeIsolation source token fixture coverage is incomplete")
  endif()
  foreach(_fixture IN LISTS _source_fixtures)
    if(_fixture MATCHES "_include.hpp$")
      set(_diagnostic "forbidden source include")
    else()
      set(_diagnostic "forbidden clock or entropy token")
    endif()
    identity_probe(1 check_identity_source_reads_no_other_clock_or_entropy.cmake "${_diagnostic}"
      "${_fixture}" 3 "-DSITOMETRON_IDENTITY_SOURCE_SCAN_FILES=${_fixture}")
  endforeach()
else()
  message(FATAL_ERROR "IdentityNegativeIsolation setup failure: unknown mode ${IDENTITY_NEGATIVE_MODE}")
endif()
message(STATUS "IdentityNegativeIsolation ${IDENTITY_NEGATIVE_MODE} validation passed")
