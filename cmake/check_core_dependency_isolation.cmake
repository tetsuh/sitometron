cmake_minimum_required(VERSION 3.28)

if(DEFINED SITOMETRON_CORE_SCAN_FILES)
  set(CORE_FILES ${SITOMETRON_CORE_SCAN_FILES})
else()
  file(GLOB_RECURSE CORE_FILES
    "${CMAKE_CURRENT_LIST_DIR}/../include/sitometron/core/*.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/core/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/core/*.hpp")
endif()

set(ALLOWED_STANDARD_HEADERS
  algorithm
  array
  cctype
  cstdint
  condition_variable
  memory
  mutex
  thread
  initializer_list
  limits
  optional
  string
  string_view
  utility
  variant
  vector)
set(ALLOWED_DEPENDENCY_HEADERS
  nlohmann/json.hpp
  boost/uuid/uuid.hpp
  boost/uuid/string_generator.hpp
  boost/uuid/uuid_io.hpp
  boost/hash2/sha2.hpp)

foreach(FILE_PATH IN LISTS CORE_FILES)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "CoreDependencyAllowlist setup failure: missing scan file ${FILE_PATH}")
  endif()
  file(READ "${FILE_PATH}" _contents)
  string(ASCII 239 187 191 _utf8_bom)
  string(LENGTH "${_contents}" _contents_length)
  if(_contents_length GREATER_EQUAL 3)
    string(SUBSTRING "${_contents}" 0 3 _contents_prefix)
    if("${_contents_prefix}" STREQUAL "${_utf8_bom}")
      string(SUBSTRING "${_contents}" 3 -1 _contents)
    endif()
  endif()
  string(REPLACE "\r\n" "\n" _contents "${_contents}")
  string(REPLACE "\r" "\n" _contents "${_contents}")
  set(_line_number 0)
  while(TRUE)
    string(FIND "${_contents}" "\n" _newline_index)
    if(_newline_index EQUAL -1)
      set(_line "${_contents}")
      set(_contents "")
      set(_has_next_line FALSE)
    else()
      string(SUBSTRING "${_contents}" 0 ${_newline_index} _line)
      math(EXPR _next_line_start "${_newline_index} + 1")
      string(SUBSTRING "${_contents}" ${_next_line_start} -1 _contents)
      set(_has_next_line TRUE)
    endif()
    math(EXPR _line_number "${_line_number} + 1")
    if("${_line}" MATCHES [[^[ 	]*#[ 	]*include]])
      if("${_line}" MATCHES [[^[ 	]*#[ 	]*include[ 	]*<([^>]+)>[ 	]*(//.*)?$]])
        set(HEADER_NAME "${CMAKE_MATCH_1}")
        list(FIND ALLOWED_STANDARD_HEADERS "${HEADER_NAME}" HEADER_INDEX)
        if(NOT HEADER_INDEX EQUAL -1)
          continue()
        endif()
        list(FIND ALLOWED_DEPENDENCY_HEADERS "${HEADER_NAME}" HEADER_INDEX)
        if(NOT HEADER_INDEX EQUAL -1)
          continue()
        endif()
      elseif("${_line}" MATCHES [[^[ 	]*#[ 	]*include[ 	]*"([^"]+)"[ 	]*(//.*)?$]])
        set(HEADER_NAME "${CMAKE_MATCH_1}")
        if(HEADER_NAME MATCHES [[^sitometron/core/[A-Za-z0-9_./-]+$]] OR
           HEADER_NAME STREQUAL "job_orchestrator.hpp")
          continue()
        endif()
      else()
        message(FATAL_ERROR
          "CoreDependencyAllowlist unparseable core include directive: "
          "${FILE_PATH}:${_line_number}: ${_line}")
      endif()
      message(FATAL_ERROR
        "CoreDependencyAllowlist unapproved core include: "
        "${FILE_PATH}:${_line_number}: ${_line}")
    endif()
    if(NOT _has_next_line)
      break()
    endif()
  endwhile()
endforeach()

message(STATUS "CoreDependencyAllowlist standard and approved dependency headers passed")
