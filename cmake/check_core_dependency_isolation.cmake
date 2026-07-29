if(DEFINED SITOMETRON_CORE_SCAN_FILES)
  set(CORE_FILES ${SITOMETRON_CORE_SCAN_FILES})
else()
  file(GLOB_RECURSE CORE_FILES
    "${CMAKE_CURRENT_LIST_DIR}/../include/sitometron/core/*.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/core/*.cpp")
endif()

set(ALLOWED_STANDARD_HEADERS
  string_view)

foreach(FILE_PATH IN LISTS CORE_FILES)
  file(STRINGS "${FILE_PATH}" INCLUDE_LINES REGEX "^[ \t]*#include")
  foreach(INCLUDE_LINE IN LISTS INCLUDE_LINES)
    if(INCLUDE_LINE MATCHES "#include[ \t]*<([^>]+)>")
      set(HEADER_NAME "${CMAKE_MATCH_1}")
      list(FIND ALLOWED_STANDARD_HEADERS "${HEADER_NAME}" HEADER_INDEX)
      if(HEADER_INDEX EQUAL -1)
        message(FATAL_ERROR "Non-allowlisted core include in ${FILE_PATH}: ${INCLUDE_LINE}")
      endif()
      continue()
    endif()
    if(INCLUDE_LINE MATCHES "#include[ \t]*\"sitometron/core/[A-Za-z0-9_./-]+\"")
      continue()
    endif()
    message(FATAL_ERROR "Forbidden core include in ${FILE_PATH}: ${INCLUDE_LINE}")
  endforeach()
endforeach()

message(STATUS "sitometron_core dependency isolation check passed")
