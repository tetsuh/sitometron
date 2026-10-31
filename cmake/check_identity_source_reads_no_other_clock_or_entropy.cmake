cmake_minimum_required(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/check_identity_scan_lines.cmake")
if(DEFINED SITOMETRON_IDENTITY_SCAN_CONFIG)
  include("${SITOMETRON_IDENTITY_SCAN_CONFIG}")
endif()
if(NOT SITOMETRON_IDENTITY_SOURCE_SCAN_FILES)
  message(FATAL_ERROR "IdentitySourcePurity setup failure: source scan files are required")
endif()

# Literal-use guards, not a C++ parser or protection against deliberate preprocessing evasion.
set(_identity_forbidden_tokens
  chrono random_device system_clock steady_clock high_resolution_clock
  getrandom getentropy arc4random arc4random_buf rand_s BCryptGenRandom
  CryptAcquireContext CryptAcquireContextA CryptAcquireContextW CryptGenRandom RtlGenRandom
  SystemFunction036 clock_gettime clock_gettime64 gettimeofday gettimeofday64 time timespec_get
  _ftime _ftime32 _ftime64 _ftime_s _ftime32_s _ftime64_s _time32 _time64
  QueryPerformanceCounter QueryPerformanceFrequency GetSystemTime GetSystemTimeAsFileTime
  GetSystemTimePreciseAsFileTime GetLocalTime GetTickCount GetTickCount64 syscall
  rand srand random srandom clock)
list(JOIN _identity_forbidden_tokens "|" _identity_forbidden_pattern)

function(identity_check_source_line _file _number _line)
  if("${_line}" MATCHES [[^[ 	]*#[ 	]*include]])
    identity_parse_include(IdentitySourcePurity "${_file}" "${_number}" "${_line}")
    if(IDENTITY_HEADER STREQUAL "random" OR IDENTITY_HEADER STREQUAL "chrono")
      message(FATAL_ERROR
        "IdentitySourcePurity forbidden source include: ${_file}:${_number}: ${_line}")
    endif()
  endif()
  if("${_line}" MATCHES "(^|[^A-Za-z0-9_])(${_identity_forbidden_pattern})([^A-Za-z0-9_]|$)")
    message(FATAL_ERROR
      "IdentitySourcePurity forbidden clock or entropy token: ${_file}:${_number}: ${_line}")
  endif()
endfunction()

foreach(_file IN LISTS SITOMETRON_IDENTITY_SOURCE_SCAN_FILES)
  identity_scan_lines("${_file}" identity_check_source_line)
endforeach()
message(STATUS "IdentitySourcePurity injected-generators-only boundary passed")
