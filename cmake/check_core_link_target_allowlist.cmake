cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED SITOMETRON_CORE_LINK_TARGETS)
  message(FATAL_ERROR "CoreDependencyTargetAllowlist setup failure: SITOMETRON_CORE_LINK_TARGETS is required")
endif()

set(_allowed_targets
  nlohmann_json::nlohmann_json
  Boost::uuid
  Boost::hash2)
foreach(_target IN LISTS SITOMETRON_CORE_LINK_TARGETS)
  set(_candidate "${_target}")
  if(_candidate MATCHES "^\\$<LINK_ONLY:(.*)>$")
    set(_candidate "${CMAKE_MATCH_1}")
  endif()
  list(FIND _allowed_targets "${_candidate}" _index)
  if(_index EQUAL -1)
    message(FATAL_ERROR
      "CoreDependencyTargetAllowlist unapproved core link target: ${_target}")
  endif()
endforeach()
message(STATUS "CoreDependencyTargetAllowlist approved direct targets passed")
