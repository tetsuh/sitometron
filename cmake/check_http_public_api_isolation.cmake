cmake_minimum_required(VERSION 3.28)

if(DEFINED SITOMETRON_HTTP_PUBLIC_SCAN_FILES)
  set(_files ${SITOMETRON_HTTP_PUBLIC_SCAN_FILES})
else()
  file(GLOB_RECURSE _files
    "${CMAKE_CURRENT_LIST_DIR}/../include/sitometron/http/*.hpp")
endif()

foreach(_file IN LISTS _files)
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "HttpPublicApiIsolation setup failure: missing scan file ${_file}")
  endif()
  file(READ "${_file}" _contents)
  string(REPLACE "\r\n" "\n" _contents "${_contents}")
  string(REPLACE "\r" "\n" _contents "${_contents}")
  string(REPLACE ";" "\\;" _line_contents "${_contents}")
  string(REPLACE "\n" ";" _lines "${_line_contents}")
  set(_line_number 0)
  foreach(_line IN LISTS _lines)
    math(EXPR _line_number "${_line_number} + 1")
    if(_line MATCHES [[^[ \t]*#[ \t]*include]])
      if(_line MATCHES [[^[ \t]*#[ \t]*include[ \t]*<([^>]+)>[ \t]*(//.*)?$]])
        set(_header_name "${CMAKE_MATCH_1}")
      elseif(_line MATCHES [[^[ \t]*#[ \t]*include[ \t]*"([^"]+)"[ \t]*(//.*)?$]])
        set(_header_name "${CMAKE_MATCH_1}")
      else()
        message(FATAL_ERROR
          "HttpPublicApiIsolation unparseable HTTP public include directive: "
          "${_file}:${_line_number}: ${_line}")
      endif()
      if(_header_name MATCHES "^(boost/|nlohmann/)")
        message(FATAL_ERROR
          "HttpPublicApiIsolation dependency-owned public include: "
          "${_file}:${_line_number}: ${_line}")
      endif()
    endif()
  endforeach()
  file(READ "${_file}" _contents)
  if(_contents MATCHES "(^|[^A-Za-z0-9_])(boost::|beast::|asio::|nlohmann::)")
    message(FATAL_ERROR
      "HttpPublicApiIsolation dependency-owned public type: ${_file}")
  endif()
endforeach()

message(STATUS "HttpPublicApiIsolation public header boundary passed")
