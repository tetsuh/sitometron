cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED SITOMETRON_SOURCE_DIR OR "${SITOMETRON_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "SITOMETRON_SOURCE_DIR is required")
endif()

file(READ "${SITOMETRON_SOURCE_DIR}/vcpkg.json" _manifest)
string(JSON _baseline ERROR_VARIABLE _baseline_error GET "${_manifest}" builtin-baseline)
if(_baseline_error OR
   NOT _baseline STREQUAL "40f3c709db80acf154ac4b17a1f83c564ebd022e")
  message(FATAL_ERROR
    "CoreDependencyAllowlist builtin baseline must match the owner-approved vcpkg pin")
endif()
string(JSON _dependency_count ERROR_VARIABLE _json_error LENGTH "${_manifest}" dependencies)
if(_json_error OR NOT _dependency_count EQUAL 3)
  message(FATAL_ERROR
    "CoreDependencyAllowlist manifest must declare exactly three direct dependencies")
endif()
set(_expected_ports nlohmann-json boost-uuid boost-hash2)
set(_expected_versions 3.12.0 1.91.0 1.91.0)
math(EXPR _last_dependency_index "${_dependency_count} - 1")
foreach(_index RANGE ${_last_dependency_index})
  string(JSON _port GET "${_manifest}" dependencies ${_index} name)
  string(JSON _version GET "${_manifest}" dependencies ${_index} "version>=")
  list(GET _expected_ports ${_index} _expected_port)
  list(GET _expected_versions ${_index} _expected_version)
  if(NOT _port STREQUAL _expected_port OR NOT _version STREQUAL _expected_version)
    message(FATAL_ERROR
      "CoreDependencyAllowlist unexpected direct dependency: ${_port} ${_version}")
  endif()
endforeach()

file(READ "${SITOMETRON_SOURCE_DIR}/CMakeLists.txt" _cmake)
foreach(_call IN ITEMS
    "find_package(nlohmann_json CONFIG REQUIRED)"
    "find_package(Boost CONFIG REQUIRED COMPONENTS uuid hash2)"
    "nlohmann_json::nlohmann_json"
    "Boost::uuid"
    "Boost::hash2")
  string(FIND "${_cmake}" "${_call}" _call_index)
  if(_call_index EQUAL -1)
    message(FATAL_ERROR
      "CoreDependencyAllowlist RED: required CMake integration '${_call}' is absent")
  endif()
endforeach()

execute_process(
  COMMAND "${CMAKE_COMMAND}"
    -P "${SITOMETRON_SOURCE_DIR}/cmake/check_core_dependency_isolation.cmake"
  RESULT_VARIABLE _isolation_result
  OUTPUT_VARIABLE _isolation_output
  ERROR_VARIABLE _isolation_error)
if(NOT _isolation_result EQUAL 0)
  message(FATAL_ERROR
    "CoreDependencyAllowlist standard-header/dependency scan failed: ${_isolation_output}${_isolation_error}")
endif()
message(STATUS "CoreDependencyAllowlist approved manifest and CMake integration are present")
