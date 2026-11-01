cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not supported. Use a CMake preset.")
endif()

project(sitometron VERSION 0.1.0 LANGUAGES CXX)

option(SITOMETRON_BUILD_TESTS "Build sitometron tests" ON)
option(SITOMETRON_ENABLE_HTTP "Build the HTTP adapter" ON)
option(SITOMETRON_ENABLE_SITOS "Build the sitos adapter" OFF)
option(SITOMETRON_ENABLE_HWLOC "Build the hwloc topology adapter" OFF)
option(SITOMETRON_ENABLE_QUILL "Build the Quill logging adapter" OFF)
option(SITOMETRON_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SITOMETRON_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(SITOMETRON_BUILD_SPIKE "Build the non-normative walking skeleton (Issue #48)" OFF)

if(SITOMETRON_ENABLE_ASAN_UBSAN AND SITOMETRON_ENABLE_TSAN)
  message(FATAL_ERROR "ASan/UBSan and TSan cannot be enabled together")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(sitometron_enable_project_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /EHsc)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

function(sitometron_enable_sanitizers target)
  if(MSVC)
    if(SITOMETRON_ENABLE_ASAN_UBSAN OR SITOMETRON_ENABLE_TSAN)
      message(FATAL_ERROR "The selected sanitizer preset is not supported by the MSVC baseline")
    endif()
    return()
  endif()

  if(SITOMETRON_ENABLE_ASAN_UBSAN)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  elseif(SITOMETRON_ENABLE_TSAN)
    target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=thread)
  endif()
endfunction()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS uuid hash2)
find_package(Threads REQUIRED)

add_library(sitometron_core STATIC src/core/version.cpp src/core/job_reducer.cpp src/core/job_orchestrator.cpp)
add_library(sitometron::core ALIAS sitometron_core)
target_include_directories(
  sitometron_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(sitometron_core PUBLIC cxx_std_20)
target_link_libraries(
  sitometron_core
  PRIVATE
    nlohmann_json::nlohmann_json
    Boost::uuid
    Boost::hash2
    Threads::Threads)
sitometron_enable_project_warnings(sitometron_core)
sitometron_enable_sanitizers(sitometron_core)

get_target_property(SITOMETRON_CORE_LINK_LIBRARIES sitometron_core LINK_LIBRARIES)
get_target_property(
  SITOMETRON_CORE_INTERFACE_LINK_LIBRARIES sitometron_core INTERFACE_LINK_LIBRARIES)
if(NOT SITOMETRON_CORE_LINK_LIBRARIES OR NOT SITOMETRON_CORE_INTERFACE_LINK_LIBRARIES)
  message(FATAL_ERROR "sitometron_core must link the approved private dependency targets")
endif()
set(SITOMETRON_CORE_LINK_TARGETS ${SITOMETRON_CORE_LINK_LIBRARIES})
set(SITOMETRON_CORE_INTERFACE_LINK_TARGETS ${SITOMETRON_CORE_INTERFACE_LINK_LIBRARIES})
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_core_link_target_allowlist.cmake)

add_library(sitometron_identity STATIC src/identity/identity_source.cpp src/identity/system_clock.cpp)
add_library(sitometron::identity ALIAS sitometron_identity)
target_include_directories(
  sitometron_identity
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(sitometron_identity PUBLIC cxx_std_20)
target_link_libraries(sitometron_identity PRIVATE sitometron::core Boost::uuid)
sitometron_enable_project_warnings(sitometron_identity)
sitometron_enable_sanitizers(sitometron_identity)
get_target_property(SITOMETRON_IDENTITY_LINK_TARGETS sitometron_identity LINK_LIBRARIES)
get_target_property(
  SITOMETRON_IDENTITY_INTERFACE_LINK_TARGETS sitometron_identity INTERFACE_LINK_LIBRARIES)
file(GLOB_RECURSE SITOMETRON_IDENTITY_SCAN_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/identity/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/identity/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/sitometron/identity/*.hpp")
file(GLOB_RECURSE SITOMETRON_IDENTITY_PUBLIC_SCAN_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/include/sitometron/identity/*.hpp")
set(SITOMETRON_IDENTITY_CLOCK_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/identity/system_clock.cpp")
set(SITOMETRON_IDENTITY_SOURCE_SCAN_FILES ${SITOMETRON_IDENTITY_SCAN_FILES})
list(REMOVE_ITEM SITOMETRON_IDENTITY_SOURCE_SCAN_FILES ${SITOMETRON_IDENTITY_CLOCK_FILES})
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_identity_dependency_allowlist.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_identity_public_api_isolation.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_identity_source_reads_no_other_clock_or_entropy.cmake)
set(SITOMETRON_IDENTITY_SCAN_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/identity-scan-inputs.cmake")
file(WRITE "${SITOMETRON_IDENTITY_SCAN_CONFIG}" "")
foreach(_identity_input IN ITEMS LINK_TARGETS INTERFACE_LINK_TARGETS SCAN_FILES PUBLIC_SCAN_FILES
    CLOCK_FILES SOURCE_SCAN_FILES)
  file(APPEND "${SITOMETRON_IDENTITY_SCAN_CONFIG}"
    "set(SITOMETRON_IDENTITY_${_identity_input} [==[${SITOMETRON_IDENTITY_${_identity_input}}]==])\n")
endforeach()

if(SITOMETRON_ENABLE_HTTP)
  find_package(Boost CONFIG REQUIRED COMPONENTS beast asio)

  add_library(sitometron_http STATIC src/http/http_server.cpp)
  add_library(sitometron::http ALIAS sitometron_http)
  target_include_directories(
    sitometron_http
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>)
  target_compile_features(sitometron_http PUBLIC cxx_std_20)
  target_link_libraries(
    sitometron_http
    PRIVATE
      sitometron::core
      Boost::beast
      Boost::asio
      nlohmann_json::nlohmann_json)
  sitometron_enable_project_warnings(sitometron_http)
  sitometron_enable_sanitizers(sitometron_http)

  get_target_property(SITOMETRON_HTTP_LINK_LIBRARIES sitometron_http LINK_LIBRARIES)
  if(NOT SITOMETRON_HTTP_LINK_LIBRARIES)
    message(FATAL_ERROR "sitometron_http must link the approved private dependency targets")
  endif()
  set(SITOMETRON_HTTP_LINK_TARGETS ${SITOMETRON_HTTP_LINK_LIBRARIES})
  file(GLOB_RECURSE SITOMETRON_HTTP_SCAN_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/http/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/http/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/sitometron/http/*.hpp")
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_http_dependency_allowlist.cmake)

  file(GLOB_RECURSE SITOMETRON_HTTP_PUBLIC_SCAN_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/sitometron/http/*.hpp")
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_http_public_api_isolation.cmake)
endif()

add_library(
  sitometron_journal STATIC
  src/journal/record_codec.cpp
  src/journal/segment_journal.cpp
  src/journal/file_system.cpp
  src/journal/replay.cpp
  src/journal/maintenance.cpp)
add_library(sitometron::journal ALIAS sitometron_journal)
target_include_directories(
  sitometron_journal
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(sitometron_journal PUBLIC cxx_std_20)
target_link_libraries(
  sitometron_journal
  PUBLIC sitometron::core
  PRIVATE nlohmann_json::nlohmann_json)
if(WIN32)
  # ConvertStringSecurityDescriptorToSecurityDescriptorW for the owner-only Journal DACL.
  target_link_libraries(sitometron_journal PRIVATE advapi32)
endif()
sitometron_enable_project_warnings(sitometron_journal)
sitometron_enable_sanitizers(sitometron_journal)

add_executable(sitometrond apps/sitometrond/main.cpp)
target_include_directories(sitometrond PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(SITOMETRON_ENABLE_HTTP)
  add_library(sitometrond_composition STATIC apps/sitometrond/composition.cpp)
  target_include_directories(sitometrond_composition PRIVATE src/core)
  target_link_libraries(
    sitometrond_composition
    PRIVATE
      sitometron::core
      sitometron::journal
      sitometron::http
      sitometron::identity)
  sitometron_enable_project_warnings(sitometrond_composition)
  sitometron_enable_sanitizers(sitometrond_composition)
endif()
sitometron_enable_project_warnings(sitometrond)
sitometron_enable_sanitizers(sitometrond)

add_library(sitometrond_entrypoint STATIC apps/sitometrond/entrypoint.cpp)
target_include_directories(sitometrond_entrypoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sitometrond_entrypoint PRIVATE sitometron::identity)
if(SITOMETRON_ENABLE_HTTP)
  target_compile_definitions(sitometrond_entrypoint PRIVATE SITOMETRON_ENABLE_HTTP=1)
  target_link_libraries(sitometrond_entrypoint PRIVATE sitometrond_composition)
endif()
sitometron_enable_project_warnings(sitometrond_entrypoint)
sitometron_enable_sanitizers(sitometrond_entrypoint)
target_link_libraries(sitometrond PRIVATE sitometrond_entrypoint)

if(SITOMETRON_BUILD_TESTS)
  include(CTest)
  enable_testing()

  add_library(sitometron_test_support INTERFACE)
  add_library(sitometron::test_support ALIAS sitometron_test_support)
  target_include_directories(sitometron_test_support INTERFACE tests/support)

  add_library(sitometron_core_fake_support STATIC tests/support/core_fakes.cpp)
  target_include_directories(sitometron_core_fake_support PUBLIC tests/support)
  target_link_libraries(
    sitometron_core_fake_support
    PRIVATE
      sitometron::core
      nlohmann_json::nlohmann_json
      Boost::hash2)
  sitometron_enable_project_warnings(sitometron_core_fake_support)
  sitometron_enable_sanitizers(sitometron_core_fake_support)

  if(WIN32)
    # Compile the existing entrypoint without HTTP: only identity/core and the default CRT link it.
    add_executable(sitometron_identity_entropy_probe
      apps/sitometrond/main.cpp apps/sitometrond/entrypoint.cpp)
    target_include_directories(sitometron_identity_entropy_probe PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(sitometron_identity_entropy_probe PRIVATE sitometron::identity)
    sitometron_enable_project_warnings(sitometron_identity_entropy_probe)
    sitometron_enable_sanitizers(sitometron_identity_entropy_probe)
    get_target_property(_identity_uuid_type Boost::uuid TYPE)
    get_target_property(_identity_uuid_links Boost::uuid INTERFACE_LINK_LIBRARIES)
    message(STATUS "Identity entropy upstream target: ${_identity_uuid_type}; ${_identity_uuid_links}")
    message(STATUS "Identity entropy probe direct target: sitometron::identity")
    message(STATUS "Identity entropy default system libraries: ${CMAKE_CXX_STANDARD_LIBRARIES}")
  endif()

  add_executable(sitometron_identity_tests
    tests/unit/identity_test.cpp tests/unit/identity_composition_test.cpp)
  target_include_directories(sitometron_identity_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} src/core)
  target_link_libraries(sitometron_identity_tests PRIVATE sitometron::identity
    sitometrond_entrypoint sitometron::test_support sitometron_core_fake_support
    nlohmann_json::nlohmann_json)
  if(SITOMETRON_ENABLE_HTTP)
    target_compile_definitions(sitometron_identity_tests PRIVATE SITOMETRON_IDENTITY_TEST_HTTP=1)
  endif()
  sitometron_enable_project_warnings(sitometron_identity_tests)
  sitometron_enable_sanitizers(sitometron_identity_tests)
  foreach(_identity_check IN ITEMS
      identity_uuidv7_canonical
      identity_uuidv7_monotonic_burst
      identity_uuidv4_canonical
      identity_stable_id_grammar
      identity_never_repeats_within_instance
      identity_source_never_exhausted
      clock_recorded_at_rfc3339
      clock_monotonic_instant
      identity_seed_failure_refuses_startup
      sitometrond_composes_production_identity)
    add_test(NAME ${_identity_check} COMMAND sitometron_identity_tests ${_identity_check})
    set_tests_properties(${_identity_check} PROPERTIES TIMEOUT 300)
  endforeach()
  foreach(_identity_scan IN ITEMS allowlist public source)
    if(_identity_scan STREQUAL "allowlist")
      set(_identity_scan_name identity_dependency_allowlist)
    elseif(_identity_scan STREQUAL "public")
      set(_identity_scan_name identity_public_api_isolation)
    else()
      set(_identity_scan_name identity_source_reads_no_other_clock_or_entropy)
    endif()
    add_test(NAME ${_identity_scan_name} COMMAND ${CMAKE_COMMAND}
      -DSITOMETRON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DSITOMETRON_IDENTITY_SCAN_CONFIG=${SITOMETRON_IDENTITY_SCAN_CONFIG}
      -DIDENTITY_NEGATIVE_MODE=${_identity_scan}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_identity_negative_isolation.cmake)
  endforeach()

  add_executable(sitometron_version_test tests/unit/version_test.cpp)
  target_link_libraries(sitometron_version_test PRIVATE sitometron::core sitometron::test_support)
  sitometron_enable_project_warnings(sitometron_version_test)
  sitometron_enable_sanitizers(sitometron_version_test)
  add_test(NAME sitometron_version_test COMMAND sitometron_version_test)

  add_library(sitometron_reducer_json_support STATIC tests/support/reducer_snapshot_json.cpp)
  target_include_directories(sitometron_reducer_json_support PUBLIC tests/support)
  target_link_libraries(
    sitometron_reducer_json_support
    PUBLIC sitometron::core nlohmann_json::nlohmann_json)
  sitometron_enable_project_warnings(sitometron_reducer_json_support)
  sitometron_enable_sanitizers(sitometron_reducer_json_support)

  add_executable(
    sitometron_core_tests
    tests/unit/job_reducer_test.cpp
    tests/unit/job_reducer_vectors_test.cpp)
  target_link_libraries(
    sitometron_core_tests
    PRIVATE
      sitometron::core
      sitometron::test_support
      sitometron_reducer_json_support
      nlohmann_json::nlohmann_json)
  sitometron_enable_project_warnings(sitometron_core_tests)
  sitometron_enable_sanitizers(sitometron_core_tests)
  add_test(NAME sitometron_core_tests COMMAND sitometron_core_tests)
  foreach(_job_reducer_check IN ITEMS
      job_closed_state_set
      job_state_event_vectors
      job_command_vectors
      job_first_cause_vectors
      job_finalization_vectors
      job_timeout_vectors
      job_late_cleanup_vectors
      job_ordering_vectors
      job_rejected_input_no_append)
    add_test(
      NAME ${_job_reducer_check}
      COMMAND sitometron_core_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/schemas/core/v1/job-reducer-vectors.json
        ${_job_reducer_check})
  endforeach()

  add_executable(sitometron_core_ports_fakes_test tests/unit/core_ports_fakes_test.cpp)
  target_link_libraries(
    sitometron_core_ports_fakes_test
    PRIVATE
      sitometron::core
      sitometron::test_support
      sitometron_core_fake_support)
  sitometron_enable_project_warnings(sitometron_core_ports_fakes_test)
  sitometron_enable_sanitizers(sitometron_core_ports_fakes_test)
  foreach(_core_port_fake_check IN ITEMS
      core_port_fake_contracts
      job_fake_logical_commit_results
      job_fake_effect_observation)
    add_test(
      NAME ${_core_port_fake_check}
      COMMAND sitometron_core_ports_fakes_test ${_core_port_fake_check})
  endforeach()

  add_executable(
    sitometron_job_orchestration_tests
    tests/support/job_orchestrator_harness.cpp
    tests/unit/job_orchestrator_test.cpp)
  target_include_directories(
    sitometron_job_orchestration_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/core
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(
    sitometron_job_orchestration_tests
    PRIVATE
      sitometron::core
      sitometron::test_support
      sitometron_core_fake_support
      nlohmann_json::nlohmann_json)
  target_compile_definitions(sitometron_job_orchestration_tests PRIVATE
    SITOMETRON_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  sitometron_enable_project_warnings(sitometron_job_orchestration_tests)
  sitometron_enable_sanitizers(sitometron_job_orchestration_tests)
  foreach(_job_orchestration_check IN ITEMS
      job_ingress_linearization_order
      job_ingress_single_writer
      job_ingress_source_classification
      job_ingress_capacity_and_reserve
      job_ingress_coalescing
      job_ingress_fail_closed
      job_ingress_shutdown_quiescence
      job_ingress_callback_lifetime
      job_ingress_readiness_failure
      job_journal_envelope_vectors
      job_logical_sequence_exhaustion_fail_closed
      job_logical_commit_order
      job_logical_commit_failure_fail_closed
      job_successful_lifecycle_slice
      job_create_returns_identity
      job_ingress_completion_await
      job_retirement_oldest_closed_first
      job_retirement_requires_closed
      job_retirement_not_under_free_slot
      job_retirement_resets_slot
      job_retirement_stale_timer_rejected
      job_retirement_late_fact_absent
      job_trace_ring_wraps)
    add_test(
      NAME ${_job_orchestration_check}
      COMMAND sitometron_job_orchestration_tests ${_job_orchestration_check})
  endforeach()

  add_test(
    NAME core_job_contract
    COMMAND
      ${CMAKE_COMMAND}
      -DSITOMETRON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_core_job_contract.cmake)

  add_executable(sitometron_journal_tests tests/unit/journal_record_codec_test.cpp)
  target_link_libraries(
    sitometron_journal_tests
    PRIVATE
      sitometron::journal
      sitometron::core
      sitometron::test_support
      nlohmann_json::nlohmann_json)
  sitometron_enable_project_warnings(sitometron_journal_tests)
  sitometron_enable_sanitizers(sitometron_journal_tests)
  foreach(_journal_check IN ITEMS
      journal_record_roundtrip_vectors
      journal_record_size_bound
      journal_record_oversize_schema_valid_rejected
      journal_parser_rejects_noncanonical)
    add_test(
      NAME ${_journal_check}
      COMMAND sitometron_journal_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/schemas/core/v1/job-reducer-vectors.json
        ${_journal_check})
  endforeach()

  add_executable(sitometron_journal_segment_tests tests/unit/journal_segment_writer_test.cpp)
  target_include_directories(sitometron_journal_segment_tests PRIVATE src/core)
  if(WIN32)
    target_link_libraries(sitometron_journal_segment_tests PRIVATE advapi32)
  endif()
  target_link_libraries(
    sitometron_journal_segment_tests
    PRIVATE
      sitometron::journal
      sitometron::core
      sitometron::test_support)
  sitometron_enable_project_warnings(sitometron_journal_segment_tests)
  sitometron_enable_sanitizers(sitometron_journal_segment_tests)
  foreach(_journal_segment_check IN ITEMS
      journal_segment_creation_durable
      job_physical_disk_sync_order
      journal_commit_result_classification
      journal_adapter_poisoned_after_failure
      job_physical_commit_failure_fail_closed
      journal_directory_exclusive_lock
      journal_daemon_never_rewrites
      journal_segment_rotation
      journal_segment_restart_continues)
    add_test(
      NAME ${_journal_segment_check}
      COMMAND sitometron_journal_segment_tests ${_journal_segment_check})
  endforeach()

  add_executable(sitometron_journal_replay_tests tests/unit/journal_replay_test.cpp)
  target_include_directories(sitometron_journal_replay_tests PRIVATE src/core)
  target_link_libraries(
    sitometron_journal_replay_tests
    PRIVATE
      sitometron::journal
      sitometron::core
      sitometron::test_support
      sitometron_reducer_json_support
      sitometron_core_fake_support
      nlohmann_json::nlohmann_json)
  sitometron_enable_project_warnings(sitometron_journal_replay_tests)
  sitometron_enable_sanitizers(sitometron_journal_replay_tests)
  foreach(_journal_replay_check IN ITEMS
      journal_startup_torn_tail_refusal
      journal_startup_corruption_refusal
      journal_startup_segment_name_mismatch
      journal_startup_capacity_refusal
      journal_startup_sequence_exhausted_refusal
      journal_startup_empty_active_segment
      journal_replay_reproduces_vectors
      journal_replay_dispatches_no_effects
      journal_replay_sequence_continuation
      journal_unresolved_jobs_block_admission
      journal_resolved_jobs_seed_writer
      journal_replay_retires_like_writer
      journal_replay_capacity_after_retirement)
    add_test(
      NAME ${_journal_replay_check}
      COMMAND sitometron_journal_replay_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/schemas/core/v1/job-reducer-vectors.json
        ${_journal_replay_check})
  endforeach()

  add_executable(sitometron_journal_maintenance_tests tests/unit/journal_maintenance_test.cpp)
  target_link_libraries(
    sitometron_journal_maintenance_tests
    PRIVATE
      sitometron::journal
      sitometron::core
      sitometron::test_support)
  sitometron_enable_project_warnings(sitometron_journal_maintenance_tests)
  sitometron_enable_sanitizers(sitometron_journal_maintenance_tests)
  foreach(_journal_maintenance_check IN ITEMS
      journal_prune_prefix_only
      journal_prune_refuses_open_jobs
      journal_prune_keeps_last_record
      journal_quarantine_torn_tail
      journal_maintenance_requires_lock
      journal_maintenance_faults
      journal_maintenance_system_file_system)
    add_test(
      NAME ${_journal_maintenance_check}
      COMMAND sitometron_journal_maintenance_tests ${_journal_maintenance_check})
  endforeach()

  add_executable(sitometron_core_dependency_api_smoke tests/unit/core_dependency_api_smoke.cpp)
  target_link_libraries(
    sitometron_core_dependency_api_smoke
    PRIVATE
      sitometron::core
      nlohmann_json::nlohmann_json
      Boost::uuid
      Boost::hash2)
  sitometron_enable_project_warnings(sitometron_core_dependency_api_smoke)
  sitometron_enable_sanitizers(sitometron_core_dependency_api_smoke)
  add_test(NAME core_dependency_api_smoke COMMAND sitometron_core_dependency_api_smoke)

  if(SITOMETRON_ENABLE_HTTP)
    add_executable(
      sitometron_http_dependency_api_smoke
      tests/unit/http_dependency_api_smoke.cpp)
    target_link_libraries(
      sitometron_http_dependency_api_smoke
      PRIVATE
        sitometron::http
        Boost::beast
        Boost::asio
        nlohmann_json::nlohmann_json)
    sitometron_enable_project_warnings(sitometron_http_dependency_api_smoke)
    sitometron_enable_sanitizers(sitometron_http_dependency_api_smoke)
    add_test(
      NAME http_dependency_api_smoke
      COMMAND sitometron_http_dependency_api_smoke)

    add_test(
      NAME http_dependency_allowlist
      COMMAND
        ${CMAKE_COMMAND}
        -DSITOMETRON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DHTTP_NEGATIVE_MODE=allowlist
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_http_negative_isolation.cmake)

    add_test(
      NAME http_dependency_rejects_unapproved_include
      COMMAND
        ${CMAKE_COMMAND}
        -DSITOMETRON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DHTTP_NEGATIVE_MODE=include
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_http_negative_isolation.cmake)

    add_test(
      NAME http_dependency_rejects_unapproved_target
      COMMAND
        ${CMAKE_COMMAND}
        -DSITOMETRON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DHTTP_NEGATIVE_MODE=target
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_http_negative_isolation.cmake)

    add_test(
      NAME http_public_api_dependency_isolation
      COMMAND
        ${CMAKE_COMMAND}
        -DSITOMETRON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DHTTP_NEGATIVE_MODE=public
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_http_negative_isolation.cmake)

    add_executable(sitometron_http_tests tests/unit/http_rest_v1_test.cpp)
    target_include_directories(
      sitometron_http_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_compile_definitions(
      sitometron_http_tests PRIVATE SITOMETRON_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(
      sitometron_http_tests
      PRIVATE
        sitometron::http
        sitometrond_composition
        sitometron::core
        sitometron_core_fake_support
        sitometron_reducer_json_support
        Boost::asio
        nlohmann_json::nlohmann_json)
    sitometron_enable_project_warnings(sitometron_http_tests)
    sitometron_enable_sanitizers(sitometron_http_tests)
    foreach(_rest_v1_check IN ITEMS
        rest_v1_loopback_only
        rest_v1_unknown_route_and_method
        rest_v1_job_routes_not_ready
        rest_v1_health_and_readiness
        rest_v1_malformed_request_refused
        rest_v1_bare_lf_refused
        rest_v1_obs_fold_refused
        rest_v1_http10_request_answered
        rest_v1_absolute_form_target
        rest_v1_targets_without_a_path
        rest_v1_header_receive_timeout
        rest_v1_target_too_long
        rest_v1_headers_too_large
        rest_v1_unknown_transfer_coding
        rest_v1_body_bound_after_routing
        rest_v1_body_receive_timeout
        rest_v1_query_refused
        rest_v1_unlisted_methods_refused
        rest_v1_error_details_empty
        rest_v1_invalid_request_target_refused
        rest_v1_missing_host_refused
        rest_v1_repeated_host_refused
        rest_v1_invalid_host_refused
        rest_v1_nonfinal_transfer_coding_refused
        rest_v1_repeated_chunked_refused
        rest_v1_chunked_parameters_refused
        rest_v1_invalid_transfer_coding_refused
        rest_v1_transfer_encoding_with_content_length_refused
        rest_v1_http10_transfer_encoding_refused
        rest_v1_phase1_ports_not_called
        rest_v1_sitometrond_options
        rest_v1_openapi_contract
        rest_v1_create_commits_before_202
        rest_v1_create_rejects_unknown_application
        rest_v1_create_never_waits
        rest_v1_job_resource_from_committed_state
        rest_v1_list_resident_in_creation_order
        rest_v1_cancel_status_mapping
        rest_v1_cancel_records_configured_principal
        rest_v1_validation_before_submission
        rest_v1_error_envelope_and_codes
        rest_v1_errors_leak_nothing
        rest_v1_not_ready_refuses_writes
        rest_v1_create_serialized
        rest_v1_already_pending_is_busy
        rest_v1_create_without_body
        rest_v1_json_rejects_raw_nul
        rest_v1_json_rejects_invalid_utf8
        rest_v1_json_rejects_trailing_data
        rest_v1_json_rejects_duplicate_keys_at_any_depth
        rest_v1_registered_application_id_form
        rest_v1_cancel_body
        rest_v1_json_rejects_leading_bom
        rest_v1_json_rejects_lone_surrogate
        rest_v1_sitometrond_empty_registry
        rest_v1_retired_job_absent
        rest_v1_daemon_outlives_max_jobs)
      add_test(
        NAME ${_rest_v1_check}
        COMMAND sitometron_http_tests ${_rest_v1_check})
      set_tests_properties(${_rest_v1_check} PROPERTIES TIMEOUT 300)
    endforeach()
    add_test(
      NAME rest_v1_sitometrond_entrypoint
      COMMAND sitometron_http_tests rest_v1_sitometrond_entrypoint $<TARGET_FILE:sitometrond>)
    set_tests_properties(rest_v1_sitometrond_entrypoint PROPERTIES TIMEOUT 300)
  endif()

  add_test(
    NAME core_dependency_allowlist
    COMMAND
      ${CMAKE_COMMAND}
      -DSITOMETRON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_core_dependency_allowlist.cmake)

  add_test(
    NAME core_dependency_rejects_unapproved_target
    COMMAND
      ${CMAKE_COMMAND}
      -DSITOMETRON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DSITOMETRON_NEGATIVE_SCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/core-target-probe
      -DCORE_NEGATIVE_MODE=target
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_core_negative_isolation.cmake)

  add_test(
    NAME core_dependency_rejects_unapproved_private_include
    COMMAND
      ${CMAKE_COMMAND}
      -DSITOMETRON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DCORE_NEGATIVE_MODE=private
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_core_negative_isolation.cmake)

  add_test(
    NAME core_public_api_dependency_isolation
    COMMAND
      ${CMAKE_COMMAND}
      -DSITOMETRON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DCORE_NEGATIVE_MODE=public
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_core_negative_isolation.cmake)

  add_test(
    NAME in_source_build_rejected
    COMMAND
      ${CMAKE_COMMAND}
      "-DSITOMETRON_SOURCE_DIR:PATH=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DSITOMETRON_BINARY_DIR:PATH=${CMAKE_CURRENT_BINARY_DIR}"
      "-DSITOMETRON_POLICY_SCRATCH_ROOT:PATH=${CMAKE_CURRENT_BINARY_DIR}/policy-probes"
      "-DSITOMETRON_POLICY_GENERATOR:STRING=${CMAKE_GENERATOR}"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_in_source_build_rejected.cmake)

  add_test(
    NAME unit_tests_reject_real_sleep
    COMMAND
      ${CMAKE_COMMAND}
      "-DSITOMETRON_SOURCE_DIR:PATH=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DSITOMETRON_BINARY_DIR:PATH=${CMAKE_CURRENT_BINARY_DIR}"
      "-DSITOMETRON_POLICY_SCRATCH_ROOT:PATH=${CMAKE_CURRENT_BINARY_DIR}/policy-probes"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_unit_tests_reject_real_sleep.cmake)
endif()

if(SITOMETRON_BUILD_SPIKE)
  add_subdirectory(spike)
endif()
