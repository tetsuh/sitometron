cmake_minimum_required(VERSION 3.28)

if(DEFINED SITOMETRON_PUBLIC_SCAN_FILES)
  set(_files ${SITOMETRON_PUBLIC_SCAN_FILES})
else()
  file(GLOB_RECURSE _files "${CMAKE_CURRENT_LIST_DIR}/../include/sitometron/core/*.hpp")
endif()

foreach(_file IN LISTS _files)
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "CorePublicApiIsolation setup failure: missing scan file ${_file}")
  endif()
  file(READ "${_file}" _contents)
  file(STRINGS "${_file}" _dependency_include_lines
    REGEX "^[ \\t]*#[ \\t]*include[ \\t]*[<\"](nlohmann/[^>\"]+|boost/[^>\"]+)[>\"]")
  if(_dependency_include_lines)
    message(FATAL_ERROR
      "CorePublicApiIsolation dependency-owned public include: ${_file}")
  endif()
  if(_contents MATCHES "(^|[^A-Za-z0-9_])(nlohmann|boost)::")
    message(FATAL_ERROR
      "CorePublicApiIsolation dependency-owned public type: ${_file}")
  endif()
  file(STRINGS "${_file}" _private_include_lines
    REGEX "^[ \t]*#[ \t]*include[ \t]*<(atomic|condition_variable|future|memory|mutex|thread)>")
  if(_private_include_lines)
    message(FATAL_ERROR "CorePublicApiIsolation private-only public include: ${_file}")
  endif()
  if(_contents MATCHES "(^|[^A-Za-z0-9_])std::(atomic|future|thread|mutex|condition_variable|(unique|shared|weak)_ptr)")
    message(FATAL_ERROR "CorePublicApiIsolation private-only public type: ${_file}")
  endif()
endforeach()
message(STATUS "CorePublicApiIsolation public header boundary passed")
