cmake_minimum_required(VERSION 3.28)
if(NOT SITOMETRON_SOURCE_DIR OR NOT SITOMETRON_BINARY_DIR OR NOT PROCESS_TEST_MODE)
  message(FATAL_ERROR "process isolation fixture requires source, binary and mode")
endif()
if(PROCESS_TEST_MODE STREQUAL "allowlist")
  set(_scanner check_process_dependency_allowlist.cmake)
  set(_missing "process dependency boundary has no enforced target/header allowlist")
elseif(PROCESS_TEST_MODE STREQUAL "public")
  set(_scanner check_process_public_api_isolation.cmake)
  set(_missing "process public API boundary has no owned-type isolation scanner")
else()
  message(FATAL_ERROR "unknown process isolation mode")
endif()
if(NOT EXISTS "${SITOMETRON_SOURCE_DIR}/cmake/${_scanner}")
  message(FATAL_ERROR "${_missing}")
endif()
set(_config "${SITOMETRON_BINARY_DIR}/process-scan-inputs.cmake")
if(NOT EXISTS "${_config}")
  message(FATAL_ERROR "process isolation production target scan metadata missing")
endif()
set(_scratch "${SITOMETRON_BINARY_DIR}/process-${PROCESS_TEST_MODE}-controls")
file(MAKE_DIRECTORY "${_scratch}")
function(probe _expected _diagnostic)
  execute_process(COMMAND "${CMAKE_COMMAND}" ${ARGN}
    -P "${SITOMETRON_SOURCE_DIR}/cmake/${_scanner}"
    RESULT_VARIABLE _status OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  string(CONCAT _text "${_out}" "${_err}")
  string(REGEX REPLACE "[ \t\r\n]" "" _actual "${_text}")
  string(REGEX REPLACE "[ \t\r\n]" "" _wanted "${_diagnostic}")
  string(FIND "${_actual}" "${_wanted}" _found)
  if(NOT _status EQUAL _expected OR _found EQUAL -1)
    message(FATAL_ERROR "process isolation control expected ${_expected}: ${_diagnostic}; got ${_status}: ${_text}")
  endif()
  message(STATUS "process isolation expected ${_status}: ${_diagnostic}")
endfunction()
function(source_probe _expected _diagnostic _file)
  if(PROCESS_TEST_MODE STREQUAL "allowlist")
    probe(${_expected} "${_diagnostic}"
      "-DSITOMETRON_PROCESS_LINK_TARGETS=Threads::Threads"
      "-DSITOMETRON_PROCESS_SCAN_FILES=${_file}")
  else()
    probe(${_expected} "${_diagnostic}" "-DSITOMETRON_PROCESS_PUBLIC_SCAN_FILES=${_file}")
  endif()
endfunction()
probe(0 "passed" "-DSITOMETRON_PROCESS_SCAN_CONFIG=${_config}")
set(_approved "${_scratch}/approved.hpp")
file(WRITE "${_approved}" "#include <string>\n#include <vector>\n#include <sitometron/core/job_ports.hpp>\n#include <sitometron/core/job_reducer.hpp>\nstruct Owned { std::string value; };\n")
source_probe(0 "passed" "${_approved}")
if(PROCESS_TEST_MODE STREQUAL "allowlist")
  foreach(_header IN ITEMS boost/asio.hpp nlohmann/json.hpp random job_orchestrator.hpp
      ../core/job_orchestrator.hpp sitometron/http/server.hpp sitometron/registry/registry.hpp
      sitometron/identity/identity_source.hpp sitometron/journal/segment_journal.hpp)
    set(_bad "${_scratch}/bad.hpp")
    file(WRITE "${_bad}" "// first line\n#include <${_header}>\n")
    source_probe(1 "bad.hpp:2:" "${_bad}")
  endforeach()
  foreach(_extra IN ITEMS Boost::asio nlohmann_json::nlohmann_json sitometron::core unapproved)
    probe(1 "unapproved" "-DSITOMETRON_PROCESS_LINK_TARGETS=Threads::Threads,${_extra}"
      "-DSITOMETRON_PROCESS_SCAN_FILES=${_approved}")
  endforeach()
  probe(1 "public" "-DSITOMETRON_PROCESS_LINK_TARGETS=Threads::Threads"
    "-DSITOMETRON_PROCESS_INTERFACE_LINK_TARGETS=Threads::Threads"
    "-DSITOMETRON_PROCESS_SCAN_FILES=${_approved}")
else()
  foreach(_line IN ITEMS "#include <boost/asio.hpp>" "#include <nlohmann/json.hpp>"
      "#include <windows.h>" "#include <sys/wait.h>" "using Value = nlohmann :: json;"
      "namespace dependency = boost;" "HANDLE handle;" "pid_t child;"
      "nlohmann/*gap*/::json value;")
    set(_bad "${_scratch}/public-bad.hpp")
    file(WRITE "${_bad}" "// first line\n${_line}\n")
    source_probe(1 "public-bad.hpp:2:" "${_bad}")
  endforeach()
endif()
set(_digraph "${_scratch}/digraph.hpp")
file(WRITE "${_digraph}" "// first line\n%:include <unapproved_process_header.hpp>\n")
source_probe(1 "digraph.hpp:2:" "${_digraph}")
set(_macro "${_scratch}/macro.hpp")
file(WRITE "${_macro}" "#define HEADER <vector>\n#include HEADER\n")
source_probe(1 "macro.hpp:2:" "${_macro}")
foreach(_ending IN ITEMS lf crlf)
  if(_ending STREQUAL "crlf")
    set(_eol "\r\n")
  else()
    set(_eol "\n")
  endif()
  foreach(_split IN ITEMS before_include inside_include inside_header)
    set(_file "${_scratch}/continuation-${_split}-${_ending}.hpp")
    set(_header "boo\\${_eol}st/asio.hpp")
    if(_split STREQUAL "before_include")
      set(_directive "#\\${_eol}include <${_header}>")
    elseif(_split STREQUAL "inside_include")
      set(_directive "#inc\\${_eol}lude <${_header}>")
    else()
      set(_directive "#include <${_header}>")
    endif()
    file(WRITE "${_file}" "// first line${_eol}${_directive}${_eol}")
    source_probe(1 "line continuation: ${_file}:2:" "${_file}")
  endforeach()
endforeach()
foreach(_fixture IN ITEMS dependency_scanner_forbidden_range_open.hpp
    dependency_scanner_forbidden_range_close.hpp dependency_scanner_forbidden_trailing_bracket.hpp
    dependency_scanner_forbidden_string_bracket.hpp dependency_scanner_forbidden_tabs.hpp
    dependency_scanner_forbidden_bom.hpp)
  if(_fixture STREQUAL "dependency_scanner_forbidden_bom.hpp")
    set(_line 1)
  else()
    set(_line 5)
  endif()
  source_probe(1 "${_fixture}:${_line}:" "${SITOMETRON_SOURCE_DIR}/tests/fixtures/${_fixture}")
endforeach()
# Compiler verdicts are checked separately; literal-nested is intentionally invalid C++.
# Generate the whole matrix before running probes, so RED leaves every specimen available.
set(_family_cases)
function(family_case _name _text _line _expected)
  set(_path "${_scratch}/family-${_name}.cpp")
  file(WRITE "${_path}" "${_text}")
  set(_family_cases ${_family_cases} "${_path}|${_line}|${_expected}" PARENT_SCOPE)
endfunction()
string(ASCII 239 187 191 _bom)
foreach(_ending IN ITEMS lf crlf)
  if(_ending STREQUAL "lf")
    set(_eol "\n")
  else()
    set(_eol "\r\n")
  endif()
  foreach(_encoding IN ITEMS plain bom)
    set(_prefix "")
    if(_encoding STREQUAL "bom")
      set(_prefix "${_bom}")
    endif()
    foreach(_introducer IN ITEMS hash digraph)
      if(_introducer STREQUAL "hash")
        set(_intro "#")
      else()
        set(_intro "%:")
      endif()
      foreach(_form IN ITEMS angle quoted)
        if(_form STREQUAL "angle")
          set(_header "<map>")
        else()
          set(_header "\"map\"")
        endif()
        set(_tag "${_ending}-${_encoding}-${_introducer}-${_form}")
        foreach(_position IN ITEMS cross_intro after_intro before_intro cross_before
            before_header cross_header after_header cross_after line_after)
          set(_line 2)
          if(_position STREQUAL "after_intro")
            set(_text "${_intro}/*gap*/include ${_header}")
          elseif(_position STREQUAL "cross_intro")
            set(_text "${_intro} /*${_eol}*/ include ${_header}")
          elseif(_position STREQUAL "before_intro")
            set(_text "/*gap*/ ${_intro}include ${_header}")
          elseif(_position STREQUAL "cross_before")
            set(_text "/*gap${_eol}*/ ${_intro}include ${_header}")
            set(_line 3)
          elseif(_position STREQUAL "before_header")
            set(_text "${_intro}include /*gap*/ ${_header}")
          elseif(_position STREQUAL "cross_header")
            set(_text "${_intro}include /*${_eol}*/ ${_header}")
          elseif(_position STREQUAL "after_header")
            set(_text "${_intro}include ${_header} /*gap*/")
          elseif(_position STREQUAL "cross_after")
            set(_text "${_intro}include ${_header} /*${_eol}*/")
          else()
            set(_text "${_intro}include ${_header} //gap")
          endif()
          family_case("${_tag}-${_position}" "${_prefix}// first${_eol}${_text}${_eol}" ${_line} 1)
        endforeach()
        foreach(_directive IN ITEMS include_next import)
          family_case("${_tag}-${_directive}"
            "${_prefix}// first${_eol}${_intro}${_directive} ${_header}${_eol}" 2 1)
        endforeach()
        family_case("${_tag}-has_include"
          "${_prefix}// first${_eol}${_intro}if __has_include(${_header})${_eol}${_intro}include ${_header}${_eol}${_intro}endif${_eol}" 3 1)
        family_case("${_tag}-inactive"
          "${_prefix}// first${_eol}${_intro}if 0${_eol}${_intro}include ${_header}${_eol}${_intro}endif${_eol}" 3 1)
      endforeach()
    endforeach()
    foreach(_declaration IN ITEMS implementation interface partition exported_partition global
        private split keyword_macro name_macro function_macro empty_macro paste_keyword paste_name)
      set(_line 2)
      if(_declaration STREQUAL "implementation")
        set(_text "module process_scan_module;")
      elseif(_declaration STREQUAL "interface")
        set(_text "export module process_scan_interface;")
      elseif(_declaration STREQUAL "partition")
        set(_text "module process_scan_module:part;")
      elseif(_declaration STREQUAL "exported_partition")
        set(_text "export module process_scan_module:part;")
      elseif(_declaration STREQUAL "global")
        set(_text "module;${_eol}export module process_scan_global;")
      elseif(_declaration STREQUAL "private")
        # A private fragment is legal only after its primary interface declaration.
        # The first forbidden declaration is therefore physical line 2, not 3.
        set(_text "export module process_scan_private;${_eol}module :private;")
      elseif(_declaration STREQUAL "split")
        set(_text "module${_eol}process_scan_module;")
      elseif(_declaration STREQUAL "keyword_macro")
        set(_text "#define MOD module${_eol}MOD process_scan_module;")
      elseif(_declaration STREQUAL "name_macro")
        set(_text "#define NAME process_scan_module${_eol}module NAME;")
        set(_line 3)
      elseif(_declaration STREQUAL "function_macro")
        set(_text "#define ID(x) x${_eol}ID(module) process_scan_module;")
      elseif(_declaration STREQUAL "empty_macro")
        set(_text "#define EMPTY${_eol}EMPTY module process_scan_module;")
        set(_line 3)
      elseif(_declaration STREQUAL "paste_keyword")
        set(_text "#define MOD mo##dule${_eol}MOD process_scan_module;")
      else()
        set(_text "#define NAME process_scan_##module${_eol}module NAME;")
      endif()
      family_case("${_ending}-${_encoding}-decl-${_declaration}"
        "${_prefix}// first${_eol}${_text}${_eol}" ${_line} 1)
    endforeach()
    foreach(_module IN ITEMS angle approved_angle quoted named exported same_line
        macro_keyword macro_paste macro_digraph)
      if(_module STREQUAL "angle")
        set(_text "import <map>;")
      elseif(_module STREQUAL "approved_angle")
        set(_text "import <string>;")
      elseif(_module STREQUAL "quoted")
        set(_text "import \"x.hpp\";")
      elseif(_module STREQUAL "named")
        set(_text "import process_scan_module;")
      elseif(_module STREQUAL "exported")
        set(_text "export module process_scan_export; export import process_scan_module;")
      elseif(_module STREQUAL "same_line")
        set(_text "export module process_scan_same; import process_scan_module;")
      elseif(_module STREQUAL "macro_keyword")
        set(_text "#define IMP import${_eol}IMP process_scan_module;")
      elseif(_module STREQUAL "macro_paste")
        set(_text "#define IMP im##port${_eol}IMP process_scan_module;")
      else()
        set(_text "%:define IMP im%:%:port${_eol}IMP process_scan_module;")
      endif()
      family_case("${_ending}-${_encoding}-module-${_module}"
        "${_prefix}// first${_eol}${_text}${_eol}" 2 1)
    endforeach()
    set(_literals [=[constexpr auto ordinary = "# /* not a comment */ include <map> import boost HANDLE";
constexpr char quote = '\'';
constexpr auto escaped = "\" /* #include <map> */";
constexpr auto comment_delimiter = R"/*( #include <map> )/*";
constexpr auto raw = u8R"tag(
#include <map>
%:/* raw */include "map"
import <map>; HANDLE nlohmann
)tag";
]=])
    string(REPLACE "\n" "${_eol}" _literals "${_literals}")
    family_case("${_ending}-${_encoding}-literals" "${_prefix}${_literals}" 1 0)
    # An identifier ending in R is NOT a raw-literal prefix (phase-3 maximal munch).
    set(_not_raw [=[#define identifierR constexpr auto first =
identifierR"(not raw";
#include <map>
constexpr auto last = ")";
]=])
    string(REPLACE "\n" "${_eol}" _not_raw "${_not_raw}")
    family_case("${_ending}-${_encoding}-not-raw-prefix" "${_prefix}${_not_raw}" 3 1)
    family_case("${_ending}-${_encoding}-bare-escape"
      "${_prefix}// first${_eol}constexpr int \\u00E9R = 0;${_eol}#include <map>${_eol}" 2 1)
    string(ASCII 11 _vt)
    string(ASCII 12 _ff)
    foreach(_space IN ITEMS space tab vt ff)
      if(_space STREQUAL "space")
        set(_gap " ")
      elseif(_space STREQUAL "tab")
        set(_gap "\t")
      elseif(_space STREQUAL "vt")
        set(_gap "${_vt}")
      else()
        set(_gap "${_ff}")
      endif()
      foreach(_intro IN ITEMS "#" "%:")
        if(_intro STREQUAL "#")
          set(_kind hash)
        else()
          set(_kind digraph)
        endif()
        family_case("${_ending}-${_encoding}-${_space}-${_kind}-whitespace"
          "${_prefix}// first${_eol}${_gap}${_intro}${_gap}include${_gap}<map>${_eol}" 2 1)
      endforeach()
      family_case("${_ending}-${_encoding}-${_space}-splice"
        "${_prefix}// first${_eol}#\\${_gap}${_eol}include <map>${_eol}" 2 1)
    endforeach()
    family_case("${_ending}-${_encoding}-line-remap"
      "${_prefix}// first${_eol}#line 800 \"platform_linux.cpp\"${_eol}#include <map>${_eol}" 3 1)
    family_case("${_ending}-${_encoding}-has-include-next"
      "${_prefix}// first${_eol}#if __has_include_next(<map>)${_eol}#include_next <map>${_eol}#endif${_eol}" 3 1)
    family_case("${_ending}-${_encoding}-inactive-brace-import"
      "${_prefix}// first${_eol}#if 0${_eol}void hidden() {${_eol}#endif${_eol}import <map>;${_eol}" 5 1)
    # Word policy is independent of the compiler's brace depth.
    family_case("${_ending}-${_encoding}-r6-macro-close"
      "${_prefix}#define CLOSE }${_eol}namespace scan_probe { CLOSE${_eol}import <map>;${_eol}" 3 1)
    family_case("${_ending}-${_encoding}-r6-literal-nested"
      "${_prefix}namespace scan_probe {${_eol}import <map>;${_eol}}${_eol}" 2 1)
    foreach(_word IN ITEMS import module)
      family_case("${_ending}-${_encoding}-r6-${_word}-replacement"
        "${_prefix}// first${_eol}#define WORD ${_word}${_eol}" 2 1)
      family_case("${_ending}-${_encoding}-r6-${_word}-macro"
        "${_prefix}// first${_eol}#define ${_word} 1${_eol}" 2 1)
      family_case("${_ending}-${_encoding}-r6-${_word}-variable"
        "${_prefix}// first${_eol}int ${_word} = 0;${_eol}" 2 1)
      family_case("${_ending}-${_encoding}-r6-${_word}-function"
        "${_prefix}// first${_eol}int ${_word}() { return 0; }${_eol}" 2 1)
      family_case("${_ending}-${_encoding}-r6-${_word}-member"
        "${_prefix}// first${_eol}struct Fields { int ${_word}; };${_eol}" 2 1)
      family_case("${_ending}-${_encoding}-r6-${_word}-namespace"
        "${_prefix}// first${_eol}namespace ${_word} {}${_eol}" 2 1)
      family_case("${_ending}-${_encoding}-r6-${_word}-label"
        "${_prefix}// first${_eol}int f() { goto ${_word}; ${_word}: return 0; }${_eol}" 2 1)
      family_case("${_ending}-${_encoding}-r6-${_word}-data"
        "${_prefix}// ${_word}${_eol}/* ${_word} */${_eol}constexpr auto ordinary = \"${_word}\";${_eol}constexpr auto raw = R\"tag(${_eol}${_word}${_eol})tag\";${_eol}constexpr int chars = '${_word}';${_eol}" 1 0)
    endforeach()
    family_case("${_ending}-${_encoding}-r6-near-misses"
      "${_prefix}int imported, modules, my_module, importer, éimport, importé, émodule, moduleé;${_eol}" 1 0)
    set(_identifiers [=[int module = 1;
int import = 2;
int émodule = 3;
int importé = 4;
struct Fields { int module = 3; int import = 4; };
namespace names { struct module final {}; namespace import { int module = 5; } }
template<class T> int import_value(T value) { return value; }
int identifiers() {
  int module = 1, import = 2;
  Fields fields; Fields* pointer = &fields;
  module; import;
  import < 3;
#if 1
  module; import;
#else
  module; import;
#endif
  ++module; import += 1;
  fields.module += pointer->import;
  return module + import + ::module + ::import + names::import::module;
}
constexpr auto words = "module process_scan_module; import <map>; _Pragma __pragma #pragma #embed";
]=])
    string(REPLACE "\n" "${_eol}" _identifiers "${_identifiers}")
    family_case("${_ending}-${_encoding}-ordinary-identifiers" "${_prefix}${_identifiers}" 1 1)
    foreach(_asm IN ITEMS asm __asm __asm__)
      family_case("${_ending}-${_encoding}-asm-${_asm}"
        "${_prefix}// first${_eol}${_asm}(\".include \\\"process-scan.s\\\"\");${_eol}" 2 1)
    endforeach()
    foreach(_pragma IN ITEMS alias pch once intrinsic msvc_intrinsic macro_intrinsic)
      if(_pragma STREQUAL "alias")
        set(_text "#pragma include_alias(<string>, <map>)")
      elseif(_pragma STREQUAL "pch")
        set(_text "#pragma clang module import process_scan_module")
      elseif(_pragma STREQUAL "once")
        set(_text "#pragma once")
      elseif(_pragma STREQUAL "intrinsic")
        set(_text "_Pragma(\"once\")")
      elseif(_pragma STREQUAL "msvc_intrinsic")
        set(_text "__pragma(include_alias(<string>, <map>))")
      else()
        set(_text "#define P _Pragma${_eol}P(\"once\")")
      endif()
      family_case("${_ending}-${_encoding}-pragma-${_pragma}"
        "${_prefix}// first${_eol}${_text}${_eol}" 2 1)
    endforeach()
    foreach(_gap IN ITEMS " " "\t")
      if(_gap STREQUAL "\t")
        set(_whitespace tabs)
      else()
        set(_whitespace spaces)
      endif()
      family_case("${_ending}-${_encoding}-r5-${_whitespace}-pragma"
        "${_prefix}// first${_eol}${_gap}#${_gap}pragma once${_eol}" 2 1)
      family_case("${_ending}-${_encoding}-r5-${_whitespace}-include"
        "${_prefix}// first${_eol}${_gap}#${_gap}include${_gap}<string>${_eol}" 2 0)
    endforeach()
    set(_names "struct caféHANDLE {}; struct HANDLEé {}; struct caféDWORD {}; struct DWORDé {};")
    family_case("${_ending}-${_encoding}-r5-whole-names" "${_prefix}${_names}${_eol}" 1 0)
    set(_arms [=[int f(int import) {
#if ARM
  return import
#else
  return 1
#endif
  ;
}
int nested(int import) {
  return import
#if ARM
#if INNER
  + 1
#else
  - 1
#endif
#elif OUTER
  + 2
#endif
  ;
}
]=])
    string(REPLACE "\n" "${_eol}" _arms "${_arms}")
    family_case("${_ending}-${_encoding}-r5-ordinary-arms" "${_prefix}${_arms}" 1 1)
    foreach(_case IN ITEMS true false before nested omitted)
      if(_case STREQUAL "true")
        # The second guard activates the tail only for this alternative.
        set(_body "#if ARM\nimport\n#else\n// no import\n#endif\n#if ARM\n<map>;\n#endif\n")
        set(_control_line 2)
      elseif(_case STREQUAL "false")
        set(_body "#if ARM\n// no import\n#else\nimport\n#endif\n#if !ARM\n<map>;\n#endif\n")
        set(_control_line 4)
      elseif(_case STREQUAL "before")
        set(_body "import\n#if ARM\n<map>;\n#else\n<string>;\n#endif\n")
        set(_control_line 1)
      elseif(_case STREQUAL "omitted")
        set(_body "import\n#if ARM\n<map>;\n#endif\n#if !ARM\n<string>;\n#endif\n")
        set(_control_line 1)
      else()
        set(_body "#if ARM\n#if INNER\nimport\n#else\nimport\n#endif\n#else\nimport\n#endif\n<map>;\n")
        set(_control_line 3)
      endif()
      string(REPLACE "\n" "${_eol}" _body "${_body}")
      family_case("${_ending}-${_encoding}-r5-import-arm-${_case}" "${_prefix}${_body}" ${_control_line} 1)
    endforeach()
    set(_ordinary [=[constexpr int decimal = 1'000;
constexpr auto hexa = 0xCA'FE;
constexpr auto binary = 0b1010'0101;
constexpr auto floating = .1'25e+0'2;
constexpr auto hex_float = 0xA'B.C'Dp-0'2;
constexpr unsigned long long operator""_counter(unsigned long long value) { return value; }
constexpr auto custom = 1'024_counter;
constexpr auto utf8_char = u8'x';
constexpr auto wide = L"import HANDLE";
constexpr auto raw_quote = R"x(" import HANDLE)x";
constexpr auto quote = '\'';
constexpr auto slash = "\\";
constexpr int array<:1:> = <% 1 %>;
struct Thing <% int operator<(const Thing&) const <% return 1; %> %>;
template<int N> constexpr int import() <% return N; %>
template<> constexpr int import<1>() <% return 1; %>
[[maybe_unused]] constexpr auto use = import<1>();
int compare(int import) <%
  import < 3;
  return import;
%>
constexpr int modules = 0, imported = 0;
#define HASH #
]=])
    string(REPLACE "\n" "${_eol}" _ordinary "${_ordinary}")
    family_case("${_ending}-${_encoding}-r5-ordinary-tokens" "${_prefix}${_ordinary}" 15 1)
    # Keep a passing lexer control for the number/literal/digraph spellings
    # in the preceding specimen, without its newly forbidden identifiers.
    string(REPLACE "import" "importer" _near_misses "${_ordinary}")
    family_case("${_ending}-${_encoding}-r6-ordinary-tokens" "${_prefix}${_near_misses}" 1 0)
    family_case("${_ending}-${_encoding}-r5-number-then-import"
      "${_prefix}// first${_eol}constexpr int n = 1'000; import process_scan_module;${_eol}" 2 1)
    family_case("${_ending}-${_encoding}-r5-number-then-include"
      "${_prefix}// first${_eol}#if 1'000${_eol}#include <map>${_eol}#endif${_eol}" 3 1)
    family_case("${_ending}-${_encoding}-r5-sameline-comment-include"
      "${_prefix}// first${_eol}/* 1'000 */ #include <map>${_eol}" 2 1)
    family_case("${_ending}-${_encoding}-r5-stringize"
      "${_prefix}// first${_eol}#define STR(arg) #arg${_eol}static_assert(sizeof(STR(text)) == 5);${_eol}" 2 1)
    family_case("${_ending}-${_encoding}-r5-comment-splice"
      "${_prefix}// first${_eol}// splice \\${_eol}#include <map>${_eol}int retained;${_eol}" 2 1)
    family_case("${_ending}-${_encoding}-approved-guard"
      "${_prefix}#if __has_include(<string>)${_eol}#include <string> // allowed${_eol}#endif${_eol}" 2 0)
  endforeach()
endforeach()
file(WRITE "${_scratch}/family-inventory.txt" "")
foreach(_case IN LISTS _family_cases)
  file(APPEND "${_scratch}/family-inventory.txt" "${_case}\n")
endforeach()
foreach(_case IN LISTS _family_cases)
  string(REPLACE "|" ";" _parts "${_case}")
  list(GET _parts 0 _path)
  list(GET _parts 1 _line)
  list(GET _parts 2 _expected)
  if(_expected EQUAL 0)
    source_probe(0 "passed" "${_path}")
  else()
    source_probe(1 "${_path}:${_line}:" "${_path}")
  endif()
endforeach()
message(STATUS "process ${PROCESS_TEST_MODE} positive and planted-rejection controls passed")
