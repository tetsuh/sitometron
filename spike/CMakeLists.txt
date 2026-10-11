# Non-normative walking skeleton. See spike/README.md and Issue #48.
# Built only when SITOMETRON_BUILD_SPIKE=ON; never part of the Phase 0A qualification set.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "sitometron_spike is Linux-only (Issue #48); disable SITOMETRON_BUILD_SPIKE")
endif()

option(SITOMETRON_BUILD_SPIKE_SITOS
       "Build the opt-in Issue #98 Sitos Session spike"
       OFF)

if(SITOMETRON_BUILD_SPIKE_SITOS)
  set(SITOMETRON_SPIKE_PYTHON "python3" CACHE STRING
      "Python interpreter used by the opt-in Sitos spike smoke test")
  find_package(sitos CONFIG REQUIRED)
endif()

add_executable(
  sitometron_spike
  src/main.cpp
  src/http_server.cpp
  src/job_driver.cpp
  src/process_runner.cpp
  src/rest_v1.cpp
  src/sitos_runtime.cpp
  src/system_ports.cpp)
target_include_directories(sitometron_spike PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                                    ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(sitometron_spike PRIVATE sitometron::core sitometron::journal
                                               nlohmann_json::nlohmann_json Threads::Threads)
if(SITOMETRON_BUILD_SPIKE_SITOS)
  target_compile_definitions(sitometron_spike PRIVATE SITOMETRON_SPIKE_SITOS=1)
  target_link_libraries(sitometron_spike PRIVATE sitos::sitos)
endif()
sitometron_enable_project_warnings(sitometron_spike)
sitometron_enable_sanitizers(sitometron_spike)

if(SITOMETRON_BUILD_TESTS)
  add_test(
    NAME spike_walking_skeleton_smoke
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.sh $<TARGET_FILE:sitometron_spike>
            ${CMAKE_CURRENT_BINARY_DIR}/smoke-scratch)
  set_tests_properties(spike_walking_skeleton_smoke PROPERTIES TIMEOUT 120 LABELS spike)
  if(SITOMETRON_BUILD_SPIKE_SITOS)
    add_test(
      NAME spike_sitos_session_smoke
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/sitos_smoke.sh $<TARGET_FILE:sitometron_spike>
              ${CMAKE_CURRENT_BINARY_DIR}/sitos-smoke-scratch ${SITOMETRON_SPIKE_PYTHON}
              ${CMAKE_CURRENT_SOURCE_DIR}/scripts/session_worker.py
              ${CMAKE_CURRENT_SOURCE_DIR}/scripts/buffer_reader.py)
    set_tests_properties(spike_sitos_session_smoke PROPERTIES TIMEOUT 180 LABELS "spike;sitos")
  endif()
endif()
