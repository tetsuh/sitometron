cmake_minimum_required(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/check_identity_scan_lines.cmake")
if(DEFINED SITOMETRON_IDENTITY_SCAN_CONFIG)
  include("${SITOMETRON_IDENTITY_SCAN_CONFIG}")
endif()

if(NOT SITOMETRON_IDENTITY_LINK_TARGETS OR NOT SITOMETRON_IDENTITY_SCAN_FILES)
  message(FATAL_ERROR "IdentityDependencyAllowlist setup failure: targets and scan files are required")
endif()

set(_identity_expected_targets sitometron::core Boost::uuid)
function(identity_require_targets _label _private)
  set(_actual)
  string(REPLACE "," ";" _targets "${ARGN}")
  foreach(_target IN LISTS _targets)
    set(_candidate "${_target}")
    if(_private)
      if(NOT _target MATCHES "^\\$<LINK_ONLY:(.*)>$")
        message(FATAL_ERROR "IdentityDependencyAllowlist public dependency leakage: ${_target}")
      endif()
      set(_candidate "${CMAKE_MATCH_1}")
    endif()
    if(NOT _candidate IN_LIST _identity_expected_targets)
      message(FATAL_ERROR "IdentityDependencyAllowlist unapproved identity link target: ${_target}")
    endif()
    list(APPEND _actual "${_candidate}")
  endforeach()
  list(SORT _actual)
  set(_expected ${_identity_expected_targets})
  list(SORT _expected)
  if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR "IdentityDependencyAllowlist exact target set mismatch in ${_label}")
  endif()
endfunction()
identity_require_targets(LINK_LIBRARIES FALSE ${SITOMETRON_IDENTITY_LINK_TARGETS})
if(DEFINED SITOMETRON_IDENTITY_INTERFACE_LINK_TARGETS)
  identity_require_targets(INTERFACE_LINK_LIBRARIES TRUE
    ${SITOMETRON_IDENTITY_INTERFACE_LINK_TARGETS})
endif()

set(_identity_standard_headers
  algorithm array charconv chrono cstddef cstdint limits memory numeric stdexcept string
  string_view system_error unordered_set utility variant)
set(_identity_dependency_headers
  boost/uuid/uuid.hpp boost/uuid/uuid_io.hpp boost/uuid/time_generator_v7.hpp
  boost/uuid/random_generator.hpp)

function(identity_check_include _file _number _line)
  if(NOT "${_line}" MATCHES [[^[ 	]*#[ 	]*include]])
    return()
  endif()
  identity_parse_include(IdentityDependencyAllowlist "${_file}" "${_number}" "${_line}")
  if(IDENTITY_HEADER STREQUAL "chrono" AND NOT _file IN_LIST SITOMETRON_IDENTITY_CLOCK_FILES)
    message(FATAL_ERROR
      "IdentityDependencyAllowlist chrono outside clock implementation: ${_file}:${_number}: ${_line}")
  endif()
  if(IDENTITY_HEADER IN_LIST _identity_standard_headers OR
     IDENTITY_HEADER IN_LIST _identity_dependency_headers)
    return()
  endif()
  if(IDENTITY_HEADER MATCHES "^sitometron/(core|identity)/[A-Za-z0-9_./-]+$" AND
     NOT IDENTITY_HEADER MATCHES "(^|/)\\.\\.(/|$)")
    return()
  endif()
  message(FATAL_ERROR
    "IdentityDependencyAllowlist unapproved identity include: ${_file}:${_number}: ${_line}")
endfunction()

foreach(_file IN LISTS SITOMETRON_IDENTITY_SCAN_FILES)
  identity_scan_lines("${_file}" identity_check_include)
endforeach()
message(STATUS "IdentityDependencyAllowlist exact private targets and direct includes passed")
