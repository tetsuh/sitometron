cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED SITOMETRON_SOURCE_DIR OR "${SITOMETRON_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "SITOMETRON_SOURCE_DIR is required")
endif()

file(READ "${SITOMETRON_SOURCE_DIR}/vcpkg.json" _manifest)
foreach(_port IN ITEMS nlohmann-json boost-uuid boost-hash2)
  string(FIND "${_manifest}" "\"${_port}\"" _port_index)
  if(_port_index EQUAL -1)
    message(FATAL_ERROR
      "CoreDependencyAllowlist RED: approved manifest port '${_port}' is not declared")
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

message(STATUS "CoreDependencyAllowlist approved manifest and CMake integration are present")
