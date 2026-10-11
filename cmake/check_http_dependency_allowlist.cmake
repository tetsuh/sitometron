cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED SITOMETRON_HTTP_LINK_TARGETS OR
   "${SITOMETRON_HTTP_LINK_TARGETS}" STREQUAL "")
  message(FATAL_ERROR
    "HttpDependencyAllowlist setup failure: SITOMETRON_HTTP_LINK_TARGETS is required")
endif()
if(NOT DEFINED SITOMETRON_HTTP_SCAN_FILES OR
   "${SITOMETRON_HTTP_SCAN_FILES}" STREQUAL "")
  message(FATAL_ERROR
    "HttpDependencyAllowlist setup failure: SITOMETRON_HTTP_SCAN_FILES is required")
endif()

set(_expected_targets
  sitometron::core
  Boost::beast
  Boost::asio
  nlohmann_json::nlohmann_json)
set(_actual_targets ${SITOMETRON_HTTP_LINK_TARGETS})
list(LENGTH _actual_targets _actual_target_count)
if(_actual_target_count EQUAL 1 AND _actual_targets MATCHES ",")
  string(REPLACE "," ";" _actual_targets "${_actual_targets}")
endif()
foreach(_target IN LISTS _actual_targets)
  list(FIND _expected_targets "${_target}" _target_index)
  if(_target_index EQUAL -1)
    message(FATAL_ERROR
      "HttpDependencyAllowlist unapproved HTTP link target: ${_target}")
  endif()
endforeach()
list(SORT _expected_targets)
list(SORT _actual_targets)
if(NOT _actual_targets STREQUAL _expected_targets)
  message(FATAL_ERROR
    "HttpDependencyAllowlist exact target set mismatch; "
    "actual=${_actual_targets}; expected=${_expected_targets}")
endif()

set(_allowed_standard_headers
  algorithm
  array
  atomic
  cctype
  chrono
  condition_variable
  cstddef
  cstdint
  deque
  exception
  functional
  initializer_list
  limits
  map
  memory
  mutex
  optional
  queue
  set
  span
  string
  string_view
  system_error
  thread
  tuple
  type_traits
  unordered_map
  unordered_set
  utility
  variant
  vector)
set(_allowed_dependency_headers
  boost/beast/core/error.hpp
  boost/beast/http/message.hpp
  boost/beast/http/parser.hpp
  boost/beast/http/string_body.hpp
  boost/beast/http/write.hpp
  boost/beast/http/error.hpp
  boost/beast/http/field.hpp
  boost/beast/http/verb.hpp
  boost/beast/http/status.hpp
  boost/asio/io_context.hpp
  boost/asio/ip/tcp.hpp
  boost/asio/buffer.hpp
  boost/asio/error.hpp
  boost/asio/cancellation_type.hpp
  boost/asio/write.hpp
  boost/asio/awaitable.hpp
  boost/asio/co_spawn.hpp
  boost/asio/detached.hpp
  boost/asio/use_awaitable.hpp
  boost/asio/as_tuple.hpp
  boost/asio/cancel_after.hpp
  nlohmann/json.hpp)

foreach(_file IN LISTS SITOMETRON_HTTP_SCAN_FILES)
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "HttpDependencyAllowlist setup failure: missing scan file ${_file}")
  endif()
  file(STRINGS "${_file}" _include_lines REGEX "^[ \t]*#[ \t]*include")
  foreach(_include_line IN LISTS _include_lines)
    if(_include_line MATCHES "#include[ \t]*[<\"]([^>\"]+)[>\"]")
      set(_header_name "${CMAKE_MATCH_1}")
      list(FIND _allowed_standard_headers "${_header_name}" _standard_index)
      if(NOT _standard_index EQUAL -1)
        continue()
      endif()
      list(FIND _allowed_dependency_headers "${_header_name}" _dependency_index)
      if(NOT _dependency_index EQUAL -1)
        continue()
      endif()
      if(_header_name MATCHES "^sitometron/")
        continue()
      endif()
      message(FATAL_ERROR
        "HttpDependencyAllowlist unapproved HTTP include: ${_file}: ${_include_line}")
    endif()
  endforeach()
endforeach()

message(STATUS "HttpDependencyAllowlist approved targets and direct includes passed")
