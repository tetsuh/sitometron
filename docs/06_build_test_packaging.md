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

Production adapters are disabled by default until their owning Phase enables and qualifies them.
See [the dependency policy](09_dependency_policy.md).

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
- positive and negative core dependency-isolation checks.

The Linux clang-tidy step resolves the absolute `clang-tidy-18` application and runs the
standard-library-only `tools/run_clang_tidy.py` helper against
`build/dev-linux/compile_commands.json`. The helper intersects tracked `apps/**/*.cpp`,
`src/**/*.cpp`, and `tests/**/*.cpp` files with that database, requires the frozen 12-source set and
LLVM 18.1.3, and fails on diagnostics, tool errors, missing inputs, or selection drift.

The sanitizer step reuses the separately provisioned x64-linux dependency installation, configures
the existing `asan-ubsan` preset with manifest and applocal acquisition disabled, and requires exact
CTest-name parity with the 36-test `dev-linux` baseline. It runs without suppressions using
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

Before the Phase 0A Gate closes, add documentation/link validation. Later Gate Issues own the
remaining qualification lanes.

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

Development-time schema checks run with Python 3.12.13 and dependencies frozen by `uv.lock`:

```text
uv sync --frozen --only-group schema
uv run --frozen --only-group schema python -m unittest discover -s tests/tooling -p 'test_*.py' -v
uv run --frozen --only-group schema python tools/validate_core_contract.py
```

`tools/json_schema_validator.py` checks Draft 2020-12 schema definitions, an explicit local-only
schema registry, local reference resolution, known formats, and instances.
`tools/validate_core_contract.py` applies it to every core schema plus the contract and vector
documents. Contract-specific checks remain in their own validators; generic JSON Schema validation
does not interpret project extension keywords.

The same `unittest` discovery runs the network-free contract tests for `tools/run_clang_tidy.py`,
`tools/acquire_gitleaks.py`, and `tools/run_secret_scan.py` on Linux and native Windows. Those
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
| `JOB-008` | C++ tests `job_ingress_linearization_order`, `job_ingress_single_writer`, `job_ingress_source_classification`, `job_ingress_capacity_and_reserve`, `job_ingress_coalescing`, `job_ingress_fail_closed` | Normative under Accepted ADR-0003; implementation checks active under Issue #12 and deterministic adverse/race qualification active under Issue #13 |
| `OPS-001` | C++ tests `job_ingress_shutdown_quiescence`, `job_ingress_callback_lifetime`, `job_ingress_readiness_failure` | Normative under Accepted ADR-0003; implementation checks active under Issue #12 and deterministic adverse/race qualification active under Issue #13 |
| `JRN-001` | CTests `core_job_contract`, `job_fake_logical_commit_results`; C++ tests `job_journal_envelope_vectors`, `job_logical_sequence_exhaustion_fail_closed` | Contract, logical fake-result, complete writer-envelope construction, and non-wrapping sequence checks active; physical encoding remains Phase 0B |
| `JRN-002` | C++ test `job_logical_commit_order`; CTest `job_fake_effect_observation`; C++ test `job_physical_disk_sync_order` | Logical commit-before-activation/effect ordering active under Issue #12 and deterministically qualified under Issue #13; physical append/flush/disk-sync ordering remains planned in Phase 0B |
| `JRN-003` | C++ tests `job_rejected_input_no_append`, `job_logical_commit_failure_fail_closed`, `job_physical_commit_failure_fail_closed`; CTest `job_fake_logical_commit_results` | Reducer, fake logical-result, and writer fail-closed reaction checks active with Issue #13 adverse-path qualification; physical commit-failure reaction remains planned in Phase 0B |

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

## 7. Packaging

> **Planned, not yet normative:** [Issue #35](https://github.com/tetsuh/sitometron/issues/35)
> tracks assignment of the future Phase 6 release Design Issue and ADR that will own this mechanism.
> Implementers must not treat this outline as a finalized contract.

The bootstrap creates no install package, release tag, or publication workflow. Phase 6 decides the
daemon, C++ SDK, Python SDK, symbols, signing, version coordination, and release automation units.
