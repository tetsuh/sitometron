cmake_minimum_required(VERSION 3.28)
if(NOT SITOMETRON_SOURCE_DIR OR NOT SITOMETRON_BINARY_DIR OR NOT REGISTRY_NEGATIVE_MODE)
  message(FATAL_ERROR "RegistryNegativeIsolation requires source, binary, and mode")
endif()
set(_configuration "${SITOMETRON_BINARY_DIR}/registry-scan-inputs.cmake")
if(NOT EXISTS "${_configuration}")
  message(FATAL_ERROR "RegistryNegativeIsolation setup failure: registry scan configuration is missing")
endif()
set(_targets "nlohmann_json::nlohmann_json,Boost::hash2")
set(_scratch "${SITOMETRON_BINARY_DIR}/registry-${REGISTRY_NEGATIVE_MODE}-controls")
file(MAKE_DIRECTORY "${_scratch}")

function(registry_probe _expected _script _diagnostic)
  execute_process(COMMAND "${CMAKE_COMMAND}" ${ARGN}
    -P "${SITOMETRON_SOURCE_DIR}/cmake/${_script}"
    RESULT_VARIABLE _status OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  string(CONCAT _text "${_out}" "${_err}")
  if(NOT _status EQUAL _expected OR NOT _text MATCHES "${_diagnostic}")
    message(FATAL_ERROR "RegistryNegativeIsolation unexpected status ${_status}: ${_text}")
  endif()
  message(STATUS "RegistryNegativeIsolation expected status ${_status}: ${_diagnostic}")
endfunction()

if(REGISTRY_NEGATIVE_MODE STREQUAL "allowlist")
  set(_script check_registry_dependency_allowlist.cmake)
  registry_probe(0 ${_script} "passed" "-DSITOMETRON_REGISTRY_SCAN_CONFIG=${_configuration}")
  set(_approved "${_scratch}/approved.hpp")
  file(WRITE "${_approved}" "#include <string>\n#include <vector>\n#include <nlohmann/json.hpp>\n#include <boost/hash2/sha2.hpp>\n")
  registry_probe(0 ${_script} "passed"
    "-DSITOMETRON_REGISTRY_LINK_TARGETS=${_targets}"
    "-DSITOMETRON_REGISTRY_SCAN_FILES=${_approved}")
  foreach(_header IN ITEMS random windows.h boost/uuid/uuid.hpp boost/beast.hpp
      boost/hash2/digest.hpp nlohmann/json_fwd.hpp sitometron/http/server.hpp)
    set(_bad "${_scratch}/bad-include.hpp")
    file(WRITE "${_bad}" "// physical line one\n#include <${_header}>\n")
    registry_probe(1 ${_script} "bad-include.hpp:2:"
      "-DSITOMETRON_REGISTRY_LINK_TARGETS=${_targets}"
      "-DSITOMETRON_REGISTRY_SCAN_FILES=${_bad}")
  endforeach()
  set(_macro "${_scratch}/macro.hpp")
  file(WRITE "${_macro}" "#define HEADER <vector>\n#include HEADER\n")
  registry_probe(1 ${_script} "macro.hpp:2:"
    "-DSITOMETRON_REGISTRY_LINK_TARGETS=${_targets}"
    "-DSITOMETRON_REGISTRY_SCAN_FILES=${_macro}")
  foreach(_extra IN ITEMS Boost::uuid Boost::beast sitometron::core registry_unapproved)
    registry_probe(1 ${_script} "unapproved.*target"
      "-DSITOMETRON_REGISTRY_LINK_TARGETS=${_targets},${_extra}"
      "-DSITOMETRON_REGISTRY_SCAN_FILES=${_approved}")
  endforeach()
  foreach(_wrong IN ITEMS "Boost::hash2" "${_targets},Boost::hash2")
    registry_probe(1 ${_script} "exact target set mismatch"
      "-DSITOMETRON_REGISTRY_LINK_TARGETS=${_wrong}"
      "-DSITOMETRON_REGISTRY_SCAN_FILES=${_approved}")
  endforeach()
  registry_probe(1 ${_script} "public dependency leakage"
    "-DSITOMETRON_REGISTRY_LINK_TARGETS=${_targets}"
    "-DSITOMETRON_REGISTRY_INTERFACE_LINK_TARGETS=${_targets}"
    "-DSITOMETRON_REGISTRY_SCAN_FILES=${_approved}")
elseif(REGISTRY_NEGATIVE_MODE STREQUAL "public")
  set(_script check_registry_public_api_isolation.cmake)
  registry_probe(0 ${_script} "passed" "-DSITOMETRON_REGISTRY_SCAN_CONFIG=${_configuration}")
  set(_approved "${_scratch}/approved.hpp")
  file(WRITE "${_approved}" "#include <string>\n#include <vector>\nstruct Owned { std::string value; };\n")
  registry_probe(0 ${_script} "passed" "-DSITOMETRON_REGISTRY_PUBLIC_SCAN_FILES=${_approved}")
  foreach(_line IN ITEMS "#include <nlohmann/json.hpp>" "#include <boost/hash2/sha2.hpp>"
      "namespace dependency = boost;" "using Value = nlohmann :: json;"
      "using boost::hash2::sha2_256;" "boost\t::hash2::sha2_256 value;"
      "nlohmann/*gap*/::json value;")
    set(_bad "${_scratch}/public-bad.hpp")
    file(WRITE "${_bad}" "// physical line one\n${_line}\n")
    registry_probe(1 ${_script} "public-bad.hpp:2:"
      "-DSITOMETRON_REGISTRY_PUBLIC_SCAN_FILES=${_bad}")
  endforeach()
else()
  message(FATAL_ERROR "RegistryNegativeIsolation unknown mode")
endif()

# Shared scanner syntax controls protect physical-line handling (not extra public check names).
foreach(_fixture IN ITEMS dependency_scanner_forbidden_range_open.hpp
    dependency_scanner_forbidden_range_close.hpp dependency_scanner_forbidden_trailing_bracket.hpp
    dependency_scanner_forbidden_string_bracket.hpp dependency_scanner_forbidden_tabs.hpp
    dependency_scanner_forbidden_bom.hpp)
  set(_file "${SITOMETRON_SOURCE_DIR}/tests/fixtures/${_fixture}")
  if(REGISTRY_NEGATIVE_MODE STREQUAL "allowlist")
    registry_probe(1 ${_script} "${_fixture}:[0-9]+:"
      "-DSITOMETRON_REGISTRY_LINK_TARGETS=${_targets}"
      "-DSITOMETRON_REGISTRY_SCAN_FILES=${_file}")
  else()
    registry_probe(1 ${_script} "${_fixture}:[0-9]+:"
      "-DSITOMETRON_REGISTRY_PUBLIC_SCAN_FILES=${_file}")
  endif()
endforeach()
# Reject compiler line splicing before either handler inspects a physical line.
foreach(_ending IN ITEMS lf crlf)
  if(_ending STREQUAL "crlf")
    set(_eol "\r\n")
  else()
    set(_eol "\n")
  endif()
  foreach(_split IN ITEMS before_include inside_include inside_header)
    set(_file "${_scratch}/continuation-${_split}-${_ending}.hpp")
    # Split the dependency root too, so a physical-line keyword check cannot mask the gap.
    set(_header "boo\\${_eol}st/uuid/uuid.hpp")
    if(_split STREQUAL "before_include")
      set(_directive "#\\${_eol}include <${_header}>")
    elseif(_split STREQUAL "inside_include")
      set(_directive "#inc\\${_eol}lude <${_header}>")
    else()
      set(_directive "#include <${_header}>")
    endif()
    file(WRITE "${_file}" "// physical line one${_eol}${_directive}${_eol}")
    set(_diagnostic "RegistryScanner forbidden line continuation:[ \t\r\n]+${_file}:2:")
    if(REGISTRY_NEGATIVE_MODE STREQUAL "allowlist")
      registry_probe(1 ${_script} "${_diagnostic}"
        "-DSITOMETRON_REGISTRY_LINK_TARGETS=${_targets}"
        "-DSITOMETRON_REGISTRY_SCAN_FILES=${_file}")
    else()
      registry_probe(1 ${_script} "${_diagnostic}"
        "-DSITOMETRON_REGISTRY_PUBLIC_SCAN_FILES=${_file}")
    endif()
  endforeach()
endforeach()
message(STATUS "RegistryNegativeIsolation ${REGISTRY_NEGATIVE_MODE} controls passed")
