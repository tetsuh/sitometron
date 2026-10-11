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
  file(STRINGS "${_file}" _dependency_include_lines REGEX
    "^[ \t]*#[ \t]*include[ \t]*[<\"](boost/beast/[^>\"]+|boost/asio/[^>\"]+|nlohmann/[^>\"]+)[>\"]")
  if(_dependency_include_lines)
    message(FATAL_ERROR
      "HttpPublicApiIsolation dependency-owned public include: ${_file}")
  endif()
  file(READ "${_file}" _contents)
  if(_contents MATCHES "(^|[^A-Za-z0-9_])(boost::beast|boost::asio|beast|asio|nlohmann)::")
    message(FATAL_ERROR
      "HttpPublicApiIsolation dependency-owned public type: ${_file}")
  endif()
endforeach()

message(STATUS "HttpPublicApiIsolation public header boundary passed")
