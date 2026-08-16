cmake_minimum_required(VERSION 3.28)

function(_sleep_family_for_path _path _out_family)
  set(_family "")
  if(_path MATCHES "^tests/unit/.+\\.cpp$")
    set(_family cpp)
  elseif(_path MATCHES "^tests/support/.+\\.(cpp|hpp)$")
    set(_family cpp)
  elseif(_path MATCHES "^tests/tooling/.+\\.sh$")
    set(_family shell)
  elseif(_path MATCHES "^tests/tooling/.+\\.ps1$")
    set(_family powershell)
  elseif(_path MATCHES "^tests/tooling/.+\\.py$")
    set(_family python)
  endif()
  set(${_out_family} "${_family}" PARENT_SCOPE)
endfunction()

function(_sleep_path_shape _path _out_valid _out_family)
  set(_valid TRUE)
  if(_path STREQUAL ""
      OR IS_ABSOLUTE "${_path}"
      OR _path MATCHES "^[A-Za-z]:"
      OR _path MATCHES "^//"
      OR _path MATCHES "\\\\"
      OR _path MATCHES "(^|/)(\\.|\\.\\.)(/|$)"
      OR _path MATCHES "//")
    set(_valid FALSE)
  endif()
  _sleep_family_for_path("${_path}" _family)
  if(_family STREQUAL "")
    set(_valid FALSE)
  endif()
  set(${_out_valid} "${_valid}" PARENT_SCOPE)
  set(${_out_family} "${_family}" PARENT_SCOPE)
endfunction()

function(_sleep_selection_shape _paths_var _out_valid)
  set(_valid TRUE)
  list(LENGTH ${_paths_var} _count)
  if(_count EQUAL 0)
    set(_valid FALSE)
  else()
    foreach(_path IN LISTS ${_paths_var})
      _sleep_path_shape("${_path}" _path_valid _unused_family)
      if(NOT _path_valid)
        set(_valid FALSE)
        break()
      endif()
    endforeach()
  endif()
  set(${_out_valid} "${_valid}" PARENT_SCOPE)
endfunction()

function(_sleep_scan_content _path _content _out_found _out_token)
  string(REPLACE "\r\n" "\n" _normalized "${_content}")
  string(REPLACE "\r" "\n" _normalized "${_normalized}")
  _sleep_family_for_path("${_path}" _family)
  set(_found FALSE)
  set(_token "")

  if(_family STREQUAL cpp)
    foreach(_candidate IN ITEMS
        "std::this_thread::sleep_for"
        "std::this_thread::sleep_until"
        "::Sleep("
        "usleep("
        "nanosleep("
        "sleep(")
      string(FIND "${_normalized}" "${_candidate}" _position)
      if(NOT _position EQUAL -1)
        set(_found TRUE)
        set(_token "${_candidate}")
        break()
      endif()
    endforeach()
  elseif(_family STREQUAL powershell)
    string(TOLOWER "${_normalized}" _lower)
    if(_lower MATCHES "(^|[^a-z0-9_-])start-sleep([^a-z0-9_-]|$)")
      set(_found TRUE)
      set(_token "Start-Sleep")
    endif()
  elseif(_family STREQUAL shell)
    if(_normalized MATCHES "(^|[^A-Za-z0-9_-])sleep([^A-Za-z0-9_-]|$)")
      set(_found TRUE)
      set(_token "sleep")
    endif()
  elseif(_family STREQUAL python)
    string(FIND "${_normalized}" "time.sleep(" _position)
    if(NOT _position EQUAL -1)
      set(_found TRUE)
      set(_token "time.sleep(")
    endif()
  endif()

  set(${_out_found} "${_found}" PARENT_SCOPE)
  set(${_out_token} "${_token}" PARENT_SCOPE)
endfunction()

function(_sleep_scan_repository
    _scan_source_input
    _scan_file_root_input
    _git_executable
    _out_ok
    _out_message
    _out_count)
  set(_ok FALSE)
  set(_message "")
  set(_count 0)
  if(NOT EXISTS "${_scan_source_input}/.git")
    set(_message "source is not a Git worktree")
  else()
    file(REAL_PATH "${_scan_source_input}" _scan_source)
    file(REAL_PATH "${_scan_file_root_input}" _scan_file_root)
    execute_process(
      COMMAND "${_git_executable}" -C "${_scan_source}" rev-parse --show-toplevel
      RESULT_VARIABLE _root_result
      OUTPUT_VARIABLE _git_root
      ERROR_VARIABLE _root_error
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _root_result EQUAL 0)
      set(_message "Git root lookup failed: ${_root_error}")
    else()
      file(REAL_PATH "${_git_root}" _git_root)
      if(NOT _git_root STREQUAL _scan_source)
        set(_message "Git root does not match source")
      endif()
    endif()
  endif()

  if(_message STREQUAL "")
    execute_process(
      COMMAND
        "${_git_executable}"
        -c core.quotePath=false
        -C "${_scan_source}"
        ls-files
        --
        ":(glob)tests/unit/**/*.cpp"
        ":(glob)tests/support/**/*.cpp"
        ":(glob)tests/support/**/*.hpp"
        ":(glob)tests/tooling/**/*.sh"
        ":(glob)tests/tooling/**/*.ps1"
        ":(glob)tests/tooling/**/*.py"
      RESULT_VARIABLE _list_result
      OUTPUT_VARIABLE _tracked_output
      ERROR_VARIABLE _list_error)
    if(NOT _list_result EQUAL 0)
      set(_message "Git enumeration failed: ${_list_error}")
    else()
      string(REPLACE "\r\n" "\n" _tracked_output "${_tracked_output}")
      string(REGEX REPLACE "\n+$" "" _tracked_output "${_tracked_output}")
      if(_tracked_output MATCHES ";")
        set(_message "selected Git path contains an unsupported semicolon")
      elseif(_tracked_output STREQUAL "")
        set(_tracked_paths "")
      else()
        string(REPLACE "\n" ";" _tracked_paths "${_tracked_output}")
      endif()
    endif()
  endif()

  set(_selected_paths "")
  if(_message STREQUAL "")
    foreach(_path IN LISTS _tracked_paths)
      if(_path MATCHES "^\"")
        set(_message "quoted Git path is unsupported")
        break()
      endif()
      _sleep_path_shape("${_path}" _valid_path _family)
      if(NOT _valid_path)
        set(_message "invalid selected path ${_path}")
        break()
      endif()
      list(FIND _selected_paths "${_path}" _duplicate_index)
      if(NOT _duplicate_index EQUAL -1)
        set(_message "duplicate selected path ${_path}")
        break()
      endif()
      set(_full_path "${_scan_file_root}/${_path}")
      if(NOT EXISTS "${_full_path}" OR IS_DIRECTORY "${_full_path}" OR IS_SYMLINK "${_full_path}")
        set(_message "selected path is not a regular file: ${_path}")
        break()
      endif()
      file(REAL_PATH "${_full_path}" _resolved_path)
      cmake_path(IS_PREFIX _scan_source "${_resolved_path}" NORMALIZE _resolved_is_in_source)
      if(NOT _resolved_is_in_source OR _resolved_path STREQUAL _scan_source)
        set(_message "selected path escapes the repository: ${_path}")
        break()
      endif()
      list(APPEND _selected_paths "${_path}")
    endforeach()
  endif()

  if(_message STREQUAL "")
    _sleep_selection_shape(_selected_paths _selection_valid)
    if(NOT _selection_valid)
      set(_message "selected tracked set is invalid")
    endif()
  endif()
  if(_message STREQUAL "")
    foreach(_path IN LISTS _selected_paths)
      file(READ "${_scan_source}/${_path}" _content)
      _sleep_scan_content("${_path}" "${_content}" _found _token)
      if(_found)
        set(_message "forbidden token ${_token} in ${_path}")
        break()
      endif()
    endforeach()
  endif()

  if(_message STREQUAL "")
    list(LENGTH _selected_paths _count)
    set(_ok TRUE)
  endif()
  set(${_out_ok} "${_ok}" PARENT_SCOPE)
  set(${_out_message} "${_message}" PARENT_SCOPE)
  set(${_out_count} "${_count}" PARENT_SCOPE)
endfunction()

find_program(_git_executable NAMES git NO_CACHE)
if(NOT _git_executable)
  message(FATAL_ERROR "unit_tests_reject_real_sleep setup failure: Git is required")
endif()

if(DEFINED SITOMETRON_POLICY_SELF_TEST_MODE AND SITOMETRON_POLICY_SELF_TEST_MODE)
  if(NOT DEFINED SITOMETRON_POLICY_SELF_TEST_DIR
      OR SITOMETRON_POLICY_SELF_TEST_DIR STREQUAL "")
    message(FATAL_ERROR "unit_tests_reject_real_sleep self-test setup failure: directory is required")
  endif()
  set(_control_root "${SITOMETRON_POLICY_SELF_TEST_DIR}/control")
  file(MAKE_DIRECTORY
    "${_control_root}/tests/unit"
    "${_control_root}/tests/support"
    "${_control_root}/tests/tooling")
  set(_cpp_clean "int main() { return 0; }\n")
  set(_hpp_clean "#pragma once\n")
  set(_shell_clean "sleep_mode=1\n")
  set(_powershell_clean "Write-Output Start-Sleeper\n")
  set(_python_clean "time_sleeper(1)\n")
  file(WRITE "${_control_root}/tests/unit/sample.cpp" "${_cpp_clean}")
  file(WRITE "${_control_root}/tests/support/sample.hpp" "${_hpp_clean}")
  file(WRITE "${_control_root}/tests/tooling/sample.sh" "${_shell_clean}")
  file(WRITE "${_control_root}/tests/tooling/sample.ps1" "${_powershell_clean}")
  file(WRITE "${_control_root}/tests/tooling/sample.py" "${_python_clean}")
  execute_process(
    COMMAND "${_git_executable}" -C "${_control_root}" init --quiet
    RESULT_VARIABLE _init_result
    ERROR_VARIABLE _init_error)
  execute_process(
    COMMAND "${_git_executable}" -C "${_control_root}" add -- tests
    RESULT_VARIABLE _add_result
    ERROR_VARIABLE _add_error)
  if(NOT _init_result EQUAL 0 OR NOT _add_result EQUAL 0)
    message(FATAL_ERROR
      "unit_tests_reject_real_sleep self-test setup failure: ${_init_error}${_add_error}")
  endif()

  function(_sleep_run_child _root _out_result _out_log)
    execute_process(
      COMMAND
        "${CMAKE_COMMAND}"
        "-DSITOMETRON_SOURCE_DIR:PATH=${_root}"
        -DSITOMETRON_POLICY_SKIP_SELF_TEST:BOOL=ON
        -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE _result
      OUTPUT_VARIABLE _output
      ERROR_VARIABLE _error)
    string(CONCAT _log "${_output}\n${_error}")
    set(${_out_result} "${_result}" PARENT_SCOPE)
    set(${_out_log} "${_log}" PARENT_SCOPE)
  endfunction()

  function(_sleep_expect_child_finding _label _path _content _clean_content)
    file(WRITE "${_control_root}/${_path}" "${_content}")
    _sleep_run_child("${_control_root}" _result _log)
    file(WRITE "${_control_root}/${_path}" "${_clean_content}")
    if(_result EQUAL 0
        OR NOT _log MATCHES "forbidden token"
        OR NOT _log MATCHES "${_path}")
      message(FATAL_ERROR
        "unit_tests_reject_real_sleep self-test failed for ${_label}: ${_log}")
    endif()
  endfunction()

  _sleep_run_child("${_control_root}" _clean_result _clean_log)
  if(NOT _clean_result EQUAL 0)
    message(FATAL_ERROR "unit_tests_reject_real_sleep clean control failed: ${_clean_log}")
  endif()
  foreach(_token IN ITEMS
      "std::this_thread::sleep_for"
      "std::this_thread::sleep_until"
      "::Sleep("
      "usleep("
      "nanosleep("
      "sleep(")
    _sleep_expect_child_finding(
      "C++ token ${_token}" "tests/unit/sample.cpp" "${_token}" "${_cpp_clean}")
  endforeach()
  _sleep_expect_child_finding(
    "C++ comment" "tests/unit/sample.cpp" "// sleep(1)" "${_cpp_clean}")
  _sleep_expect_child_finding(
    "C++ string"
    "tests/unit/sample.cpp"
    "const char* value = \"sleep(\";"
    "${_cpp_clean}")
  _sleep_expect_child_finding(
    "PowerShell command"
    "tests/tooling/sample.ps1"
    "Start-Sleep -Seconds 1"
    "${_powershell_clean}")
  _sleep_expect_child_finding(
    "PowerShell comment" "tests/tooling/sample.ps1" "# Start-Sleep" "${_powershell_clean}")
  _sleep_expect_child_finding(
    "PowerShell string"
    "tests/tooling/sample.ps1"
    "Write-Output 'Start-Sleep'"
    "${_powershell_clean}")
  _sleep_expect_child_finding(
    "shell command" "tests/tooling/sample.sh" "sleep 1" "${_shell_clean}")
  _sleep_expect_child_finding(
    "shell comment" "tests/tooling/sample.sh" "# sleep 1" "${_shell_clean}")
  _sleep_expect_child_finding(
    "shell string" "tests/tooling/sample.sh" "printf '%s' 'sleep'" "${_shell_clean}")
  _sleep_expect_child_finding(
    "Python call" "tests/tooling/sample.py" "time.sleep(1)" "${_python_clean}")
  _sleep_expect_child_finding(
    "Python comment" "tests/tooling/sample.py" "# time.sleep(1)" "${_python_clean}")
  _sleep_expect_child_finding(
    "Python string" "tests/tooling/sample.py" "value = 'time.sleep('" "${_python_clean}")
  _sleep_expect_child_finding(
    "LF normalization"
    "tests/tooling/sample.ps1"
    "Write-Output ok\nStart-Sleep"
    "${_powershell_clean}")
  _sleep_expect_child_finding(
    "CRLF normalization"
    "tests/tooling/sample.ps1"
    "Write-Output ok\r\nStart-Sleep"
    "${_powershell_clean}")

  file(WRITE "${SITOMETRON_POLICY_SELF_TEST_DIR}/escape.cpp" "sleep(1)\n")
  _sleep_path_shape("../escape.cpp" _escape_valid _unused_family)
  if(_escape_valid)
    message(FATAL_ERROR "unit_tests_reject_real_sleep self-test failed: path escape was accepted")
  endif()
  set(_outside_root "${SITOMETRON_POLICY_SELF_TEST_DIR}/outside")
  file(MAKE_DIRECTORY
    "${_outside_root}/tests/unit"
    "${_outside_root}/tests/support"
    "${_outside_root}/tests/tooling")
  file(WRITE "${_outside_root}/tests/unit/sample.cpp" "${_cpp_clean}")
  file(WRITE "${_outside_root}/tests/support/sample.hpp" "${_hpp_clean}")
  file(WRITE "${_outside_root}/tests/tooling/sample.sh" "${_shell_clean}")
  file(WRITE "${_outside_root}/tests/tooling/sample.ps1" "${_powershell_clean}")
  file(WRITE "${_outside_root}/tests/tooling/sample.py" "${_python_clean}")
  _sleep_scan_repository(
    "${_control_root}"
    "${_outside_root}"
    "${_git_executable}"
    _escape_scan_ok
    _escape_scan_message
    _escape_scan_count)
  if(_escape_scan_ok OR NOT _escape_scan_message MATCHES "selected path escapes the repository")
    message(FATAL_ERROR
      "unit_tests_reject_real_sleep containment self-test failed: ${_escape_scan_message}")
  endif()
  set(_empty_root "${SITOMETRON_POLICY_SELF_TEST_DIR}/empty")
  file(MAKE_DIRECTORY "${_empty_root}")
  file(WRITE "${_empty_root}/README.md" "empty selection\n")
  execute_process(COMMAND "${_git_executable}" -C "${_empty_root}" init --quiet)
  execute_process(COMMAND "${_git_executable}" -C "${_empty_root}" add -- README.md)
  _sleep_run_child("${_empty_root}" _empty_result _empty_log)
  if(_empty_result EQUAL 0 OR NOT _empty_log MATCHES "selected tracked set is invalid")
    message(FATAL_ERROR "unit_tests_reject_real_sleep empty-selection self-test failed: ${_empty_log}")
  endif()
  return()
endif()

if(NOT DEFINED SITOMETRON_SOURCE_DIR OR SITOMETRON_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "unit_tests_reject_real_sleep setup failure: SITOMETRON_SOURCE_DIR is required")
endif()

if(NOT DEFINED SITOMETRON_POLICY_SKIP_SELF_TEST OR NOT SITOMETRON_POLICY_SKIP_SELF_TEST)
  foreach(_required IN ITEMS SITOMETRON_BINARY_DIR SITOMETRON_POLICY_SCRATCH_ROOT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
      message(FATAL_ERROR "unit_tests_reject_real_sleep setup failure: ${_required} is required")
    endif()
  endforeach()
  file(REAL_PATH "${SITOMETRON_BINARY_DIR}" _binary_dir)
  cmake_path(ABSOLUTE_PATH SITOMETRON_POLICY_SCRATCH_ROOT NORMALIZE OUTPUT_VARIABLE _scratch_root)
  cmake_path(IS_PREFIX _binary_dir "${_scratch_root}" NORMALIZE _root_is_in_binary)
  if(NOT _root_is_in_binary OR _scratch_root STREQUAL _binary_dir)
    message(FATAL_ERROR
      "unit_tests_reject_real_sleep setup failure: scratch root must be below the build tree")
  endif()
  if(EXISTS "${_scratch_root}"
      AND (NOT IS_DIRECTORY "${_scratch_root}" OR IS_SYMLINK "${_scratch_root}"))
    message(FATAL_ERROR "unit_tests_reject_real_sleep setup failure: scratch root is not real")
  endif()
  file(MAKE_DIRECTORY "${_scratch_root}")
  file(REAL_PATH "${_scratch_root}" _scratch_root_real)
  cmake_path(IS_PREFIX _binary_dir "${_scratch_root_real}" NORMALIZE _real_root_is_in_binary)
  if(NOT _real_root_is_in_binary OR _scratch_root_real STREQUAL _binary_dir)
    message(FATAL_ERROR "unit_tests_reject_real_sleep setup failure: scratch root escapes build tree")
  endif()
  string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef _nonce)
  set(_self_test_dir "${_scratch_root_real}/real-sleep-${_nonce}")
  if(EXISTS "${_self_test_dir}" OR IS_SYMLINK "${_self_test_dir}")
    message(FATAL_ERROR "unit_tests_reject_real_sleep setup failure: invocation leaf exists")
  endif()
  file(MAKE_DIRECTORY "${_self_test_dir}")
  file(REAL_PATH "${_self_test_dir}" _self_test_dir_real)
  cmake_path(IS_PREFIX _scratch_root_real "${_self_test_dir_real}" NORMALIZE _leaf_is_in_root)
  if(NOT _leaf_is_in_root OR _self_test_dir_real STREQUAL _scratch_root_real)
    message(FATAL_ERROR "unit_tests_reject_real_sleep setup failure: invocation leaf escapes")
  endif()
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}"
      -DSITOMETRON_POLICY_SELF_TEST_MODE:BOOL=ON
      "-DSITOMETRON_POLICY_SELF_TEST_DIR:PATH=${_self_test_dir_real}"
      -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _self_result
    OUTPUT_VARIABLE _self_output
    ERROR_VARIABLE _self_error)
  file(REMOVE_RECURSE "${_self_test_dir_real}")
  if(NOT _self_result EQUAL 0)
    message(FATAL_ERROR
      "unit_tests_reject_real_sleep self-qualification failed: ${_self_output}${_self_error}")
  endif()
endif()

_sleep_scan_repository(
  "${SITOMETRON_SOURCE_DIR}"
  "${SITOMETRON_SOURCE_DIR}"
  "${_git_executable}"
  _scan_ok
  _scan_message
  _scan_count)
if(NOT _scan_ok)
  message(FATAL_ERROR "unit_tests_reject_real_sleep: ${_scan_message}")
endif()
message(STATUS "unit_tests_reject_real_sleep: ${_scan_count} tracked files passed")
