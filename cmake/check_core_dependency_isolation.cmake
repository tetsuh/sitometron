cmake_minimum_required(VERSION 3.28)

if(DEFINED SITOMETRON_CORE_SCAN_FILES)
  set(CORE_FILES ${SITOMETRON_CORE_SCAN_FILES})
else()
  file(GLOB_RECURSE CORE_FILES
    "${CMAKE_CURRENT_LIST_DIR}/../include/sitometron/core/*.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/core/*.cpp")
endif()

set(ALLOWED_STANDARD_HEADERS
  string_view)
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
  file(STRINGS "${FILE_PATH}" INCLUDE_LINES REGEX "^[ \\t]*#include")
  foreach(INCLUDE_LINE IN LISTS INCLUDE_LINES)
    if(INCLUDE_LINE MATCHES "#include[ \\t]*<([^>]+)>")
      set(HEADER_NAME "${CMAKE_MATCH_1}")
      list(FIND ALLOWED_STANDARD_HEADERS "${HEADER_NAME}" HEADER_INDEX)
      if(NOT HEADER_INDEX EQUAL -1)
        continue()
      endif()
      list(FIND ALLOWED_DEPENDENCY_HEADERS "${HEADER_NAME}" HEADER_INDEX)
      if(NOT HEADER_INDEX EQUAL -1)
        continue()
      endif()
      message(FATAL_ERROR
        "CoreDependencyAllowlist unapproved core include: ${FILE_PATH}: ${INCLUDE_LINE}")
    endif()
    if(INCLUDE_LINE MATCHES "#include[ \\t]*\"sitometron/core/[A-Za-z0-9_./-]+\"")
      continue()
    endif()
    message(FATAL_ERROR
      "CoreDependencyAllowlist unapproved core include: ${FILE_PATH}: ${INCLUDE_LINE}")
  endforeach()
endforeach()

message(STATUS "CoreDependencyAllowlist standard and approved dependency headers passed")
