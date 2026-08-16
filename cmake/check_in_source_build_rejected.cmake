cmake_minimum_required(VERSION 3.28)

foreach(_required IN ITEMS
    SITOMETRON_SOURCE_DIR
    SITOMETRON_BINARY_DIR
    SITOMETRON_POLICY_SCRATCH_ROOT
    SITOMETRON_POLICY_GENERATOR)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "in_source_build_rejected setup failure: ${_required} is required")
  endif()
endforeach()

file(REAL_PATH "${SITOMETRON_SOURCE_DIR}" _source_dir)
file(REAL_PATH "${SITOMETRON_BINARY_DIR}" _binary_dir)
cmake_path(ABSOLUTE_PATH SITOMETRON_POLICY_SCRATCH_ROOT NORMALIZE OUTPUT_VARIABLE _scratch_root)
cmake_path(IS_PREFIX _binary_dir "${_scratch_root}" NORMALIZE _root_is_in_binary)
if(NOT _root_is_in_binary OR _scratch_root STREQUAL _binary_dir)
  message(FATAL_ERROR
    "in_source_build_rejected setup failure: scratch root must be a child of the build tree")
endif()
if(EXISTS "${_scratch_root}" AND (NOT IS_DIRECTORY "${_scratch_root}" OR IS_SYMLINK "${_scratch_root}"))
  message(FATAL_ERROR "in_source_build_rejected setup failure: scratch root is not a real directory")
endif()
file(MAKE_DIRECTORY "${_scratch_root}")
file(REAL_PATH "${_scratch_root}" _scratch_root_real)
cmake_path(IS_PREFIX _binary_dir "${_scratch_root_real}" NORMALIZE _real_root_is_in_binary)
if(NOT _real_root_is_in_binary OR _scratch_root_real STREQUAL _binary_dir)
  message(FATAL_ERROR "in_source_build_rejected setup failure: scratch root escapes the build tree")
endif()
if(NOT EXISTS "${_source_dir}/CMakeLists.txt")
  message(FATAL_ERROR "in_source_build_rejected setup failure: top-level CMakeLists.txt is missing")
endif()

set(_source_artifacts
    CMakeCache.txt
    CMakeFiles
    build.ninja
    cmake_install.cmake
    rules.ninja
    .ninja_deps
    .ninja_log)
foreach(_artifact IN LISTS _source_artifacts)
  if(EXISTS "${_source_dir}/${_artifact}")
    message(FATAL_ERROR
      "in_source_build_rejected setup failure: source checkout already contains ${_artifact}")
  endif()
endforeach()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef _nonce)
set(_fixture_root "${_scratch_root_real}/in-source-build-${_nonce}")
if(EXISTS "${_fixture_root}" OR IS_SYMLINK "${_fixture_root}")
  message(FATAL_ERROR "in_source_build_rejected setup failure: invocation leaf already exists")
endif()
file(MAKE_DIRECTORY "${_fixture_root}")
file(REAL_PATH "${_fixture_root}" _fixture_root_real)
cmake_path(IS_PREFIX _scratch_root_real "${_fixture_root_real}" NORMALIZE _leaf_is_in_root)
if(NOT _leaf_is_in_root OR _fixture_root_real STREQUAL _scratch_root_real)
  message(FATAL_ERROR "in_source_build_rejected setup failure: invocation leaf escapes scratch root")
endif()

set(_control_dir "${_fixture_root_real}/control")
set(_probe_dir "${_fixture_root_real}/project")
file(MAKE_DIRECTORY "${_control_dir}" "${_probe_dir}")
file(WRITE "${_control_dir}/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.28)\nproject(in_source_control NONE)\n")
file(COPY_FILE
  "${_source_dir}/CMakeLists.txt"
  "${_probe_dir}/CMakeLists.txt"
  ONLY_IF_DIFFERENT)

execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -G "${SITOMETRON_POLICY_GENERATOR}"
    -S "${_control_dir}"
    -B "${_control_dir}"
  RESULT_VARIABLE _control_result
  OUTPUT_VARIABLE _control_output
  ERROR_VARIABLE _control_error)
execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -G "${SITOMETRON_POLICY_GENERATOR}"
    -S "${_probe_dir}"
    -B "${_probe_dir}"
  RESULT_VARIABLE _probe_result
  OUTPUT_VARIABLE _probe_output
  ERROR_VARIABLE _probe_error)

string(CONCAT _probe_log "${_probe_output}\n${_probe_error}")
string(REGEX REPLACE "[ \t\r\n]+" " " _probe_error_normalized "${_probe_error}")
string(STRIP "${_probe_error_normalized}" _probe_error_normalized)
string(REGEX MATCHALL "CMake Error at " _probe_error_headers "${_probe_error}")
list(LENGTH _probe_error_headers _probe_error_count)

set(_failure "")
if(NOT _control_result EQUAL 0)
  string(CONCAT _failure
    "control configure failed unexpectedly: ${_control_output}\n${_control_error}")
elseif(_probe_result EQUAL 0)
  set(_failure "copied project unexpectedly allowed an in-source configure")
elseif(NOT _probe_error_count EQUAL 1
    OR NOT _probe_error_normalized MATCHES
      "^CMake Error at CMakeLists\\.txt:[0-9]+ \\(message\\): In-source builds are not supported\\. Use a CMake preset\\.")
  set(_failure "required diagnostic was not the probe's fatal CMake error: ${_probe_log}")
elseif(_probe_log MATCHES
    "CXX compiler identification|nlohmann_json|Could NOT find|Could not find a package")
  set(_failure "project continued beyond the top-level in-source guard: ${_probe_log}")
endif()

file(REMOVE_RECURSE "${_fixture_root_real}")
foreach(_artifact IN LISTS _source_artifacts)
  if(EXISTS "${_source_dir}/${_artifact}")
    string(APPEND _failure " source checkout gained ${_artifact}")
  endif()
endforeach()

if(NOT _failure STREQUAL "")
  message(FATAL_ERROR "in_source_build_rejected: ${_failure}")
endif()
message(STATUS "in_source_build_rejected: intended guard and benign control passed")
