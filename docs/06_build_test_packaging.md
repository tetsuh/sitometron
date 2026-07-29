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
- clang-format dry-run;
- positive and negative core dependency-isolation checks.

Before the Phase 0A Gate closes, add clang-tidy, Linux ASan/UBSan, documentation/link validation,
and secret scanning. Later Gate Issues own the remaining qualification lanes.

## 3. Test policy

Production behavior follows RED, GREEN, REFACTOR. Intentional RED commits are permitted only in a
feature branch and must contain the expected acceptance or reproduction-test failure. A GREEN commit
must follow before merge.

Unit tests are deterministic. Use fake clocks, deterministic UUID generation, fake Journal and
ApplicationRunner ports, and explicit event injection. Do not use real sleeps to establish ordering.

## 4. Requirement-to-test mapping

Each normative Requirement ID maps directly to required test or check names. Test implementations
may iterate internal machine-readable vectors without creating a second stable fixture-ID registry.

| Requirement | Required tests or checks | Status |
|---|---|---|
| `NFR-001` | CI jobs `linux`, `windows` | Active |
| `NFR-002` | CTest `core_dependency_isolation`, `core_dependency_isolation_rejects_third_party` | Active |
| `NFR-003` | CI jobs `linux`, `windows`; CTest `in_source_build_rejected` | CI active; rejection test planned before the Phase 0A Gate closes |
| `NFR-004` | CTest `sitometron_core_tests`; policy check `unit_tests_reject_real_sleep` | Core test active; policy check planned before the Phase 0A Gate closes |

Owning design Issues add required test names to this table before production implementation. A test
name may change only with the corresponding Requirement review. A Planned check name is stable even
before its implementation; the owning Gate must record activation evidence before it closes.

## 5. Evidence

A production PR records structured RED evidence and final GREEN commands as required by
[the development workflow](development_workflow.md). Documentation-only changes provide link,
formatting, schema, and workflow validation instead of a RED test.

## 6. Packaging

> **Planned, not yet normative:** The Phase 6 release Design Issue and ADR own this mechanism.
> Implementers must not treat this outline as a finalized contract.

The bootstrap creates no install package, release tag, or publication workflow. Phase 6 decides the
daemon, C++ SDK, Python SDK, symbols, signing, version coordination, and release automation units.
