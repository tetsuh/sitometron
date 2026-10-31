cmake_minimum_required(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/check_identity_scan_lines.cmake")
if(DEFINED SITOMETRON_IDENTITY_SCAN_CONFIG)
  include("${SITOMETRON_IDENTITY_SCAN_CONFIG}")
endif()
if(NOT SITOMETRON_IDENTITY_PUBLIC_SCAN_FILES)
  message(FATAL_ERROR "IdentityPublicApiIsolation setup failure: public scan files are required")
endif()

function(identity_check_public_include _file _number _line)
  if("${_line}" MATCHES [[^[ 	]*#[ 	]*include]])
    identity_parse_include(IdentityPublicApiIsolation "${_file}" "${_number}" "${_line}")
    if(IDENTITY_HEADER MATCHES "^(boost/|nlohmann/)" OR
       IDENTITY_HEADER MATCHES "^(chrono|random|ctime|time.h|sys/.*|windows.h|Windows.h)$")
      message(FATAL_ERROR
        "IdentityPublicApiIsolation dependency-owned or clock public include: ${_file}:${_number}: ${_line}")
    endif()
  endif()
endfunction()

function(identity_check_public_type _file _number _line)
  if("${_line}" MATCHES [[(^|[^A-Za-z0-9_])(boost|nlohmann|chrono|random_device)([^A-Za-z0-9_]|$)]])
    message(FATAL_ERROR
      "IdentityPublicApiIsolation dependency or clock root word: ${_file}:${_number}: ${_line}")
  endif()
endfunction()

foreach(_file IN LISTS SITOMETRON_IDENTITY_PUBLIC_SCAN_FILES)
  identity_scan_lines("${_file}" identity_check_public_include)
  identity_scan_lines("${_file}" identity_check_public_type)
endforeach()
message(STATUS "IdentityPublicApiIsolation owned public type boundary passed")
