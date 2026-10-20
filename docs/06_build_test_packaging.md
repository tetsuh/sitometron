# Build, test, and packaging

## 1. Build baseline

Sitometron requires CMake 3.28 or later, Ninja, and a C++20 compiler. Supported bootstrap compiler
lanes are GCC on Ubuntu 24.04 and MSVC on Windows. In-source builds are rejected.

Developer presets:

```text
dev-linux
dev-windows
release
asan-ubsan
tsan
```

Issue #105 enables and qualifies the production External REST v1 adapter: the `sitometron_http`
target, on by default, builds `src/http/http_server.cpp` and serves the Phase 1 routes that this
document's `API` rows name. The other adapter options remain disabled because their dependencies
are not in the manifest. See [the dependency policy](09_dependency_policy.md).

## 2. C++ style and analysis

Project C++ follows Google C++ Style with a 100-column limit. `.clang-format` is authoritative.
Project code compiles with warnings as errors; third-party code does not inherit project warnings.

Phase 0A PR CI requires:

- Linux configure, build, and CTest;
- Windows configure, build, and CTest;
- pinned development-time Draft 2020-12 schema-tooling tests on Linux and Windows;
- clang-format dry-run;
- clang-tidy 18.1.3 with warnings treated as errors on the frozen tracked translation units;
- Linux ASan/UBSan with the same CTest name set as `dev-linux`;
- pinned Gitleaks 8.30.1 secret scanning of the exact event head on Linux;
- repository-relative link and repository governance validation on Linux and Windows;
- positive and negative core dependency-isolation checks.

The Linux clang-tidy step resolves the absolute `clang-tidy-18` application and runs the
standard-library-only `tools/run_clang_tidy.py` helper against
`build/dev-linux/compile_commands.json`. The helper intersects tracked `apps/**/*.cpp`,
`src/**/*.cpp`, and `tests/**/*.cpp` files with that database, requires the frozen 26-source set and
LLVM 18.1.3, and fails on diagnostics, tool errors, missing inputs, or selection drift.

The sanitizer step reuses the separately provisioned x64-linux dependency installation, configures
the existing `asan-ubsan` preset with manifest and applocal acquisition disabled, and requires exact
CTest-name parity with the 106-test `dev-linux` baseline. It runs without suppressions using
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.

The Linux secret-scan step checks out the exact pull-request head or push head a second time
without persisted credentials and runs the standard-library-only `tools/run_secret_scan.py` helper
against it. The helper asserts that `HEAD` equals the event-selected commit, materializes only
tracked regular-file blobs (modes `100644`/`100755`) into a run-owned tree, and validates the
repository-owned pins `tools/gitleaks-tool-version.txt` and `tools/gitleaks-linux-x64.sha256`
plus `.gitleaks.toml` from that tree. `tools/acquire_gitleaks.py` then downloads the official
Gitleaks 8.30.1 Linux x64 archive through at most two HTTPS redirects on `github.com` and
`release-assets.githubusercontent.com`, verifies the SHA-256 pin, inspects raw tar headers,
extracts only the `gitleaks` executable into the run-owned directory, and requires the reported
version to equal the pin. Every invocation uses `gitleaks dir` with `--no-banner`, `--no-color`,
`--redact=100`, `--ignore-gitleaks-allow`, an explicit empty run-owned ignore file, the
materialized configuration, and `--exit-code 1`. Before the repository scan, ephemeral probes
built outside Git require the synthetic canary to exit `1`, a one-character near-match to exit
`0`, an inline `gitleaks:allow` canary to exit `1`, and a canary whose fingerprint is listed in a
`.gitleaksignore` at the process working directory to exit `1`. Gitleaks 8.30.1 still honors a
`.gitleaksignore` at the scanned tree root even when an explicit ignore path is given, so the helper
fails closed when any tracked path is named `.gitleaksignore`; the materialized tree therefore
never contains one. Repository exit `0` is clean, exit `1` is a finding, and any other exit or
process-start failure is a tool failure; the helper returns `0`, `1`, or `2` respectively, prints
only bounded stage/category lines, writes no report, and removes every run-owned file. The
configuration extends the built-in rules with exactly one canary rule and permits no allowlist or
other suppression; any exception requires an owner re-freeze.

`tools/check_relative_links.py` validates every repository-relative Markdown link and anchor in the
tracked Markdown files without network access. It ignores exactly the `http`, `https`, and `mailto`
schemes and fails closed on every other scheme, on any query in a local target, and on malformed or
ambiguous percent-escapes, encoded separators, control characters, backslashes, drive or UNC forms,
and `.`/`..` escapes. Paths resolve against Git-tracked entries, so repository spelling stays
case-sensitive on Windows; a directory link needs an exact tracked directory, a trailing `/`, and no
fragment, and no implicit README or index resolution occurs. Anchors come only from ATX headings:
the validator removes an optional closing `#` run, replaces links and images with their visible
label, strips HTML tags, unescapes entities, drops backtick and emphasis delimiters, normalizes to
NFC, casefolds, maps each whitespace run to one `-`, keeps Unicode letters, numbers, and marks plus
`-` and `_`, and appends `-1`, `-2`, and so on to later collisions. Setext headings and raw HTML
`id`/`name` anchors are not anchor sources, and fenced-code contents are ignored.

The development-only `docs` dependency group supplies `markdown-it-py` in CommonMark mode for
block and inline structure. Code spans are bounded by their containing block; link titles are not
rescanned as Markdown. Fenced and indented code, and raw HTML blocks, are not Markdown link sources;
HTML attributes are not crawled. Inline links, images, autolinks, and references are checked, including
unused or duplicate definitions. URLs reach repository policy without normalization or scheme
filtering; character escapes in destinations are not silently rewritten into another tracked path.
Undefined full/collapsed references and malformed definition candidates fail closed; undefined
shortcut labels remain text. Labels nest at most eight levels, inline bare destinations at most seven
parenthesis levels, and exceeding the parser's structural nesting limit is a validation error.
The existing ATX slug, path, and governance policies remain repository-owned.

`tools/check_repository_governance.py` validates repository-owned governance invariants only: the
ADR status vocabulary with its core metadata sections and decision date, the independent Contract
Registry maturity and implementation vocabularies, an Accepted ADR authority on every Normative row,
a traceable Issue or design authority on every Planned-maturity row, an authority link in every
Planned-not-normative banner, and the fields the Issue forms and pull-request template must declare
as required. It is a bounded repository-format parser rather than a general YAML implementation: it
requires LF-normalized UTF-8 issue-form text, unique expected `id`/label blocks, and `required: true`
in the correct validation or checkbox block. Actual Issue completeness, clean-room provenance, owner
decisions, current-head authorization, and Gate evidence remain human and provider review evidence;
CI never calls a GitHub API to infer them.

Later Gate Issues own the remaining qualification lanes.

## 3. Test policy

Production behavior follows RED, GREEN, REFACTOR. Intentional RED commits are permitted only in a
feature branch and must contain the expected acceptance or reproduction-test failure. A GREEN commit
must follow before merge.

Unit tests are deterministic. Use fake clocks, deterministic UUID generation, fake Journal and
ApplicationRunner ports, and explicit event injection. Do not use real sleeps to establish ordering.

CTest `in_source_build_rejected` qualifies the top-level CMake guard in a run-owned fixture without
configuring the source checkout. CTest `unit_tests_reject_real_sleep` fail-closed enumerates tracked
`tests/unit/**/*.cpp`, `tests/support/**/*.{cpp,hpp}`, and `tests/tooling/**/*.{sh,ps1,py}` files
through Git. It rejects the frozen C/C++, PowerShell, shell, and Python sleep tokens in normalized
raw text, including comments and strings, and self-qualifies negative and benign fixtures.

Development-time Python checks run with Python 3.12.13 and dependencies frozen by `uv.lock`:

```text
uv sync --frozen --only-group schema --only-group docs
uv run --frozen --only-group schema --only-group docs python -m unittest discover -s tests/tooling -p 'test_*.py' -v
uv run --frozen --only-group schema python tools/validate_core_contract.py
uv run --frozen --no-build --only-group docs python tools/check_relative_links.py
uv run --frozen --no-build --only-group docs python tools/check_repository_governance.py
```

`tools/json_schema_validator.py` checks Draft 2020-12 schema definitions, an explicit local-only
schema registry, local reference resolution, known formats, and instances.
`tools/validate_core_contract.py` applies it to every core schema plus the contract and vector
documents. Contract-specific checks remain in their own validators; generic JSON Schema validation
does not interpret project extension keywords.

The same `unittest` discovery runs the network-free contract tests for `tools/run_clang_tidy.py`,
`tools/acquire_gitleaks.py`, `tools/run_secret_scan.py`, `tools/check_relative_links.py`, and
`tools/check_repository_governance.py` on Linux and native Windows. Those
tests inject HTTP, subprocess, and filesystem seams and never download a tool or scan real
history.

## 4. Requirement-to-test mapping

Each normative Requirement ID maps directly to required test or check names. Test implementations
may iterate internal machine-readable vectors without creating a second stable fixture-ID registry.

| Requirement | Required tests or checks | Status |
|---|---|---|
| `NFR-001` | CI jobs `linux`, `windows` | Active |
| `NFR-002` | Legacy check names retired by Issue #17; standard-header enforcement continues under `NFR-005` | Requirement Superseded |
| `NFR-003` | CI jobs `linux`, `windows`; CTest `in_source_build_rejected` | Active |
| `NFR-004` | CTests `sitometron_core_tests`, `core_port_fake_contracts`, `job_fake_effect_observation`, `unit_tests_reject_real_sleep`; C++ test `job_successful_lifecycle_slice` | Active |
| `NFR-005` | CTests `core_dependency_allowlist` (including the reviewed standard-header allowlist), `core_dependency_rejects_unapproved_target`, `core_dependency_rejects_unapproved_private_include`, `core_public_api_dependency_isolation`, `core_dependency_api_smoke`; CI jobs `linux`, `windows` | Normative under Accepted ADR-0004; implemented by Issue #17 |
| `JOB-001` | CTest `core_job_contract`; C++ test `job_closed_state_set` | Active |
| `JOB-002` | CTest `core_job_contract`; C++ tests `job_state_event_vectors`, `job_command_vectors` | Active |
| `JOB-003` | C++ test `job_first_cause_vectors` | Active |
| `JOB-004` | C++ test `job_finalization_vectors` | Active |
| `JOB-005` | C++ test `job_timeout_vectors` | Active |
| `JOB-006` | C++ test `job_late_cleanup_vectors` | Active |
| `JOB-007` | C++ test `job_ordering_vectors` | Active |
| `JOB-008` | C++ tests `job_ingress_linearization_order`, `job_ingress_single_writer`, `job_ingress_source_classification`, `job_ingress_capacity_and_reserve`, `job_ingress_coalescing`, `job_ingress_fail_closed`, `job_create_returns_identity`, `job_ingress_completion_await` | Normative under Accepted ADR-0003; implementation checks active under Issue #12 and deterministic adverse/race qualification active under Issue #13; creation identity and the completion wait active under Issue #80 |
| `JOB-009` | CTests `core_supervisor_decision_contract`, `core_supervisor_decision_complete`; C++ tests `job_supervisor_drives_to_cleanup`, `job_supervisor_overtakes`, `job_supervisor_rejection_rederives`, `job_supervisor_ingress_results`, `job_supervisor_no_writer_reentry` | Planned under Proposed ADR-0012 under Issue #104; checks activated by the supervisor implementation Issue |
| `OPS-001` | C++ tests `job_ingress_shutdown_quiescence`, `job_ingress_callback_lifetime`, `job_ingress_readiness_failure` | Normative under Accepted ADR-0003; implementation checks active under Issue #12 and deterministic adverse/race qualification active under Issue #13 |
| `JRN-001` | CTests `core_job_contract`, `job_fake_logical_commit_results`; C++ tests `job_journal_envelope_vectors`, `job_logical_sequence_exhaustion_fail_closed` | Contract, logical fake-result, complete writer-envelope construction, and non-wrapping sequence checks active; physical record encoding is active under `JRN-004` (Issue #57) and segment layout under `JRN-005` (Issue #59) |
| `JRN-002` | C++ test `job_logical_commit_order`; CTest `job_fake_effect_observation`; C++ test `job_physical_disk_sync_order` | Logical commit-before-activation/effect ordering active under Issue #12 and deterministically qualified under Issue #13; physical append/data-sync ordering (`job_physical_disk_sync_order`) active under Issue #59 |
| `JRN-003` | C++ tests `job_rejected_input_no_append`, `job_logical_commit_failure_fail_closed`, `job_physical_commit_failure_fail_closed`; CTest `job_fake_logical_commit_results` | Reducer, fake logical-result, and writer fail-closed reaction checks active with Issue #13 adverse-path qualification; physical commit-failure reaction (`job_physical_commit_failure_fail_closed`) active under Issue #59 |
| `JRN-004` | CTests `journal_record_roundtrip_vectors`, `journal_record_size_bound`, `journal_record_oversize_schema_valid_rejected`, `journal_parser_rejects_noncanonical` | Normative under Accepted ADR-0006 under Issue #51; checks active under Issue #57 |
| `JRN-005` | CTests `job_physical_disk_sync_order`, `journal_segment_creation_durable`, `journal_segment_rotation`, `journal_segment_restart_continues` | Normative under Accepted ADR-0006 under Issue #51; checks active under Issue #59 |
| `JRN-006` | CTests `journal_commit_result_classification`, `journal_adapter_poisoned_after_failure`, `job_physical_commit_failure_fail_closed` | Normative under Accepted ADR-0006 under Issue #51; checks active under Issue #59 |
| `JRN-007` | CTests `journal_directory_exclusive_lock`, `journal_daemon_never_rewrites` | Normative under Accepted ADR-0006 under Issue #51; checks active under Issue #59 |
| `OPS-002` | CTests `journal_startup_torn_tail_refusal`, `journal_startup_corruption_refusal`, `journal_startup_segment_name_mismatch`, `journal_startup_capacity_refusal`, `journal_startup_sequence_exhausted_refusal`, `journal_startup_empty_active_segment` | Normative under Accepted ADR-0006 under Issue #51; checks active under Issue #61 |
| `OPS-003` | CTests `journal_replay_reproduces_vectors`, `journal_replay_dispatches_no_effects`, `journal_replay_sequence_continuation` | Normative under Accepted ADR-0006 under Issue #51; checks active under Issue #61 |
| `OPS-004` | CTests `journal_unresolved_jobs_block_admission`, `journal_resolved_jobs_seed_writer` | Normative under Accepted ADR-0006 under Issue #51; checks active under Issue #64 |
| `OPS-005` | CTests `journal_prune_prefix_only`, `journal_prune_refuses_open_jobs`, `journal_prune_keeps_last_record`, `journal_quarantine_torn_tail`, `journal_maintenance_requires_lock`, `journal_maintenance_faults`, `journal_maintenance_system_file_system` | Normative under Accepted ADR-0006 under Issue #51; checks active under Issue #69 |
| `API-001` | CTests `rest_v1_loopback_only`, `rest_v1_openapi_contract`, `rest_v1_unknown_route_and_method` | Normative under Accepted ADR-0008 under Issue #76; #105 activates loopback and unknown-route/method, #106 activates the OpenAPI contract check |
| `API-002` | CTests `rest_v1_create_commits_before_202`, `rest_v1_create_rejects_unknown_application`, `rest_v1_create_never_waits` | Normative under Accepted ADR-0008 under Issue #76; checks Planned, activated by the production adapter Issue |
| `API-003` | CTests `rest_v1_job_resource_from_committed_state`, `rest_v1_list_resident_in_creation_order` | Normative under Accepted ADR-0008 under Issue #76; checks Planned, activated by the production adapter Issue |
| `API-004` | CTests `rest_v1_cancel_status_mapping`, `rest_v1_cancel_records_configured_principal` | Normative under Accepted ADR-0008 under Issue #76; checks Planned, activated by the production adapter Issue |
| `API-005` | CTests `rest_v1_validation_before_submission`, `rest_v1_error_envelope_and_codes`, `rest_v1_errors_leak_nothing` | Normative under Accepted ADR-0008 under Issue #76; checks Planned, activated by the production adapter Issue |
| `API-006` | CTests `rest_v1_health_and_readiness`, `rest_v1_job_routes_not_ready` | Normative under Accepted ADR-0008 under Issue #76; #105 activates health/readiness and transport-gated Job routes, while #106 supplies Job handling |
| `API-007` | CTests `rest_v1_malformed_request_refused`, `rest_v1_bare_lf_refused`, `rest_v1_http10_request_answered`, `rest_v1_absolute_form_target`, `rest_v1_targets_without_a_path`, `rest_v1_header_receive_timeout`, `rest_v1_target_too_long`, `rest_v1_headers_too_large`, `rest_v1_unknown_transfer_coding`, `rest_v1_transfer_encoding_with_content_length_refused` (Section 1); `rest_v1_body_bound_after_routing`, `rest_v1_body_receive_timeout` (Section 2); `rest_v1_create_serialized`, `rest_v1_already_pending_is_busy` (Section 3); `rest_v1_query_refused` (Section 4); `rest_v1_create_without_body` (Section 5); `rest_v1_unlisted_methods_refused` (Section 6); `rest_v1_error_details_empty` (Section 7); `rest_v1_json_rejects_raw_nul`, `rest_v1_json_rejects_invalid_utf8`, `rest_v1_json_rejects_trailing_data`, `rest_v1_json_rejects_duplicate_keys_at_any_depth` (Section 8); `rest_v1_registered_application_id_form` (Section 9); `rest_v1_cancel_body` (Section 10) of ADR-0009 | Normative under Accepted ADR-0009 under Issue #86; Issue #105 activates its Section 1, 2, 4, 6, and 7 names; #106 activates the remaining names |

Issue #105 additionally gives each adapter-owned `400 malformed_request` refusal its own CTest entry:

| Adapter-owned refusal | RFC authority | CTest |
|---|---|---|
| A bare LF line ending in the request line, a field line, or the terminating empty line (the adapter does not exercise the RFC 9112 Section 2.2 option to recognize LF alone) | RFC 9112 Section 2.2 | `rest_v1_bare_lf_refused` |
| Invalid request-target form (not origin-form, absolute-form, asterisk-form, or authority-form) | RFC 9112 Sections 3.1-3.2; RFC 9110 Section 7.1 | `rest_v1_invalid_request_target_refused` |
| CONNECT authority-form with an empty or out-of-range port (port `0` and leading-zero forms are accepted when their numeric value is at most `65535`) | RFC 9110 Section 9.3.6 | `rest_v1_invalid_request_target_refused` |
| Missing Host field in HTTP/1.1 | RFC 9112 Section 3.2 | `rest_v1_missing_host_refused` |
| More than one Host field line | RFC 9112 Section 3.2 | `rest_v1_repeated_host_refused` |
| Host value is not a valid `uri-host [ ":" port ]` value | RFC 9112 Section 3.2; RFC 9110 Section 7.2 | `rest_v1_invalid_host_refused` |
| Transfer-Encoding has a final coding other than `chunked` | RFC 9112 Sections 6.1 and 6.3 | `rest_v1_nonfinal_transfer_coding_refused` |
| `chunked` is repeated inside one Transfer-Encoding field | RFC 9112 Sections 6.1, 6.3, and 7.1 | `rest_v1_repeated_chunked_refused` |
| `chunked` has a transfer parameter | RFC 9112 Section 7.1; RFC 9110 Section 10.1.4 | `rest_v1_chunked_parameters_refused` |
| Transfer-coding or transfer-parameter syntax is invalid (for example a parameter without `=` or a non-tchar byte in a coding); empty list elements are ignored as RFC 9110 Section 5.6.1.2 requires, bounded only by the header limit | RFC 9112 Section 6.1; RFC 9110 Sections 5.6.1.2, 5.6.2, and 10.1.4 | `rest_v1_invalid_transfer_coding_refused` |
| Transfer-Encoding combined with Content-Length | RFC 9112 Sections 6.1 and 6.3 | `rest_v1_transfer_encoding_with_content_length_refused` |
| Transfer-Encoding appears in an HTTP/1.0 request | RFC 9112 Section 6.1 | `rest_v1_http10_transfer_encoding_refused` |
| Obsolete line folding (obs-fold) in a field line | RFC 9112 Section 5.2 | `rest_v1_obs_fold_refused` |

Beast 1.91.0 owns the remaining parser refusals in `rest_v1_malformed_request_refused`, including
invalid request-line syntax, invalid field syntax, and invalid Content-Length syntax or value. The
adapter must not duplicate those cases. After the adapter's checks, it hands Beast a single canonical
`Transfer-Encoding: chunked` line. The adapter-owned list above was established by the RED probe
against the pinned Beast source and header parser behavior; a final report records the exact probe
command and representative output.

A `HEAD` response carries the status and headers of the `GET`-equivalent response and no content
(RFC 9110 Section 9.3.2).

The startup configuration and in-process composed-daemon smoke are covered by
`rest_v1_sitometrond_options`, which also exercises representative unknown, repeated, missing,
malformed, and out-of-range command-line values. The executable entrypoint smoke is
`rest_v1_sitometrond_entrypoint`: malformed options and their documented status `2` run on every
platform; the valid subprocess starts on loopback port `0`, answers health and readiness, and is
stopped with `SIGTERM` on POSIX. Windows runs only the malformed-option portion because the test
has no portable clean console-signal path that also verifies exit status `0`.

Owning design Issues add required test names to this table before production implementation. A test
name may change only with the corresponding Requirement review. A Planned check name is stable even
before its implementation; the owning Gate must record activation evidence before it closes.

## 5. Evidence

A production PR records structured RED evidence and final GREEN commands as required by
[the development workflow](development_workflow.md). Documentation-only changes provide link,
formatting, schema, and workflow validation instead of a RED test.

## 6. Dependency provisioning evidence

The canonical wrappers read the independent lowercase tool pin in
`tools/vcpkg-tool-commit.txt` and validate the official origin and exact clean checkout before
bootstrapping. The tool pin is separate from the manifest `builtin-baseline`; equality is recorded
evidence, not a required invariant. Linux uses `.cache/vcpkg/x64-linux`,
`build/vcpkg-installed/x64-linux`, `build/dev-linux`, and `build/bootstrap-locks/x64-linux.lock`;
Windows uses `.cache/vcpkg/x64-windows`, `build/vcpkg-installed/x64-windows`, `build/dev-windows`,
and `build/bootstrap-locks/x64-windows.lock`. First runs may contact the network for clone, bootstrap, pinned port sources,
configured caches, or proxies. Values that may contain credentials are never printed. Native host prerequisites are Bash/Git/CMake 3.28+/Ninja/GCC/G++/curl/zip/unzip/tar on Linux and PowerShell 7.3+/Git/CMake 3.28+/Ninja plus an initialized x64 MSVC Developer PowerShell on Windows; the Windows wrapper does not require curl or tar.

Each invocation explicitly provisions the manifest, then configures through the existing platform
preset with `CMAKE_TOOLCHAIN_FILE`, `VCPKG_TARGET_TRIPLET`, `VCPKG_INSTALLED_DIR`,
`VCPKG_MANIFEST_INSTALL=OFF`, and `VCPKG_APPLOCAL_DEPS=OFF`. Existing cache identity is checked
before and after configuration. Warm runs are non-destructive and repeat provisioning; failures
are fail-fast and safely retried by rerunning the same command. CI-equivalent means this C++
provision/configure/build/full-CTest path only; schema/core/format/cache/direct-port,
z-applocal, dependency-policy, and other repository evidence remains separate CI responsibility. The filesystem binary cache is
an optional `actions/cache` optimization; its keys include cache format, OS, architecture, triplet,
manifest hash, and runner image identity. A cache miss remains correct.

The direct manifest and closure/license evidence are recorded in
[`dependency_closure.md`](dependency_closure.md). The manifest, immutable baseline, and separately
verified tool checkout are the resolution authority; closure tables are audit evidence and not an
unofficial lock file.

When `SITOMETRON_ENABLE_HTTP=ON`, the dependency integration registers these CTests:
`http_dependency_allowlist`, `http_dependency_rejects_unapproved_include`,
`http_dependency_rejects_unapproved_target`, `http_public_api_dependency_isolation`, and
`http_dependency_api_smoke`. The HTTP checks are conditional on that option; all core dependency
checks remain unconditional. These scanners guard against accidental use of unapproved dependencies rather than deliberate
evasion, require literal `<...>` or `"..."` include directives, reject the dependency root words
`boost` and `nlohmann` outright in public headers, and track unusual directive spellings in
[Issue #100](https://github.com/tetsuh/sitometron/issues/100).

## 7. Packaging

> **Planned, not yet normative:** [Issue #35](https://github.com/tetsuh/sitometron/issues/35)
> tracks assignment of the future Phase 6 release Design Issue and ADR that will own this mechanism.
> Implementers must not treat this outline as a finalized contract.

The bootstrap creates no install package, release tag, or publication workflow. Phase 6 decides the
daemon, C++ SDK, Python SDK, symbols, signing, version coordination, and release automation units.
