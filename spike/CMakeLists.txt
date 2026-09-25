# Non-normative walking skeleton. See spike/README.md and Issue #48.
# Built only when SITOMETRON_BUILD_SPIKE=ON; never part of the Phase 0A qualification set.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "sitometron_spike is Linux-only (Issue #48); disable SITOMETRON_BUILD_SPIKE")
endif()

add_executable(
  sitometron_spike
  src/main.cpp
  src/file_journal.cpp
  src/http_server.cpp
  src/job_driver.cpp
  src/process_runner.cpp
  src/system_ports.cpp)
target_include_directories(sitometron_spike PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                                    ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(sitometron_spike PRIVATE sitometron::core nlohmann_json::nlohmann_json
                                               Threads::Threads)
sitometron_enable_project_warnings(sitometron_spike)
sitometron_enable_sanitizers(sitometron_spike)

if(SITOMETRON_BUILD_TESTS)
  add_test(
    NAME spike_walking_skeleton_smoke
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.sh $<TARGET_FILE:sitometron_spike>
            ${CMAKE_CURRENT_BINARY_DIR}/smoke-scratch)
  set_tests_properties(spike_walking_skeleton_smoke PROPERTIES TIMEOUT 120 LABELS spike)
endif()
