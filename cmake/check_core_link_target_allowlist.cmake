cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED SITOMETRON_CORE_LINK_TARGETS)
  message(FATAL_ERROR
    "CoreDependencyTargetAllowlist setup failure: SITOMETRON_CORE_LINK_TARGETS is required")
endif()

set(_expected_targets
  nlohmann_json::nlohmann_json
  Boost::uuid
  Boost::hash2
  Threads::Threads)

function(require_exact_targets _label _require_link_only)
  set(_actual_targets)
  foreach(_target IN LISTS ARGN)
    set(_candidate "${_target}")
    set(_is_link_only FALSE)
    if(_candidate MATCHES "^\\$<LINK_ONLY:(.*)>$")
      set(_candidate "${CMAKE_MATCH_1}")
      set(_is_link_only TRUE)
    endif()
    if(_require_link_only AND NOT _is_link_only)
      message(FATAL_ERROR
        "CoreDependencyTargetAllowlist public dependency leakage in ${_label}: ${_target}")
    endif()
    if(NOT _require_link_only AND _is_link_only)
      message(FATAL_ERROR
        "CoreDependencyTargetAllowlist unexpected LINK_ONLY target in ${_label}: ${_target}")
    endif()
    list(FIND _expected_targets "${_candidate}" _index)
    if(_index EQUAL -1)
      message(FATAL_ERROR
        "CoreDependencyTargetAllowlist unapproved core link target: ${_target}")
    endif()
    list(APPEND _actual_targets "${_candidate}")
  endforeach()

  list(SORT _actual_targets)
  list(SORT _expected_targets)
  if(NOT _actual_targets STREQUAL _expected_targets)
    message(FATAL_ERROR
      "CoreDependencyTargetAllowlist exact target set mismatch in ${_label}; "
      "actual=${_actual_targets}; expected=${_expected_targets}")
  endif()
endfunction()

require_exact_targets("LINK_LIBRARIES" FALSE ${SITOMETRON_CORE_LINK_TARGETS})

if(DEFINED SITOMETRON_CORE_INTERFACE_LINK_TARGETS)
  require_exact_targets(
    "INTERFACE_LINK_LIBRARIES" TRUE ${SITOMETRON_CORE_INTERFACE_LINK_TARGETS})
endif()

message(STATUS "CoreDependencyTargetAllowlist exact private targets passed")
