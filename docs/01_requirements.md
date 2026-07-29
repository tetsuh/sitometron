# Sitometron requirements

## 1. Requirement identity

A stable Requirement ID uses `<DOMAIN>-<three digits>`. IDs are never reused. Superseded or removed
requirements remain in history and point to their replacement or removal authority.

| Domain | Scope |
|---|---|
| `JOB` | Job lifecycle, outcomes, controls, and reducer behavior |
| `ADM` | Admission tickets, claims, bounds, and fairness |
| `JRN` | JobJournal records, durability, and persistence authority |
| `WRK` | Worker protocol, runner, process supervision, and containment |
| `RES` | Resource profiles, topology, reservation, and scheduling |
| `APP` | Application Registry, qualification, and launch identity |
| `PAR` | Parameter and Session facade behavior |
| `ART` | Artifact publication, access, retention, and deletion |
| `SEC` | Authentication, authorization, trust, and sensitive-data handling |
| `OPS` | Startup, shutdown, recovery, readiness, diagnostics, and maintenance |
| `NFR` | Cross-cutting platform, performance, build, and quality requirements |

Issues and PRs identify applicable IDs or give an N/A reason. Each normative Requirement maps to
required test or check names in [the build and test specification](06_build_test_packaging.md).

## 2. Requirement maturity

- **Planned**: registered for design ownership but not an implementation contract.
- **Normative**: approved by the named authority and binding on implementation.
- **Deprecated**: retained for traceability but no longer required.
- **Superseded**: replaced by named Requirement IDs or an ADR.

A Planned Requirement cannot authorize production implementation by itself.

## 3. Current normative requirements

| ID | Level | Requirement | Authority |
|---|---|---|---|
| `NFR-001` | MUST | Support Linux with GCC and Windows with MSVC using C++20. | [ADR-0001](adr/0001-bootstrap-a-stdlib-only-cpp20-core.md) |
| `NFR-002` | MUST | Keep `sitometron_core` limited to the C++ standard library. | [ADR-0001](adr/0001-bootstrap-a-stdlib-only-cpp20-core.md) |
| `NFR-003` | MUST | Use CMake 3.28 or later with Ninja presets and reject in-source builds. | [ADR-0001](adr/0001-bootstrap-a-stdlib-only-cpp20-core.md) |
| `NFR-004` | MUST | Keep unit tests deterministic and avoid real sleeps when a fake clock or explicit event can express the behavior. | [ADR-0001](adr/0001-bootstrap-a-stdlib-only-cpp20-core.md) |

## 4. Planned domains

> **Planned, not yet normative:** The owning Phase Issues and ADRs own these mechanisms.
> Implementers must not treat this outline as a finalized contract.

`JOB`, `ADM`, `JRN`, `WRK`, `RES`, `APP`, `PAR`, `ART`, `SEC`, and `OPS` requirements are added by
their owning design Issues before implementation. Issue #3 owns the first `JOB` reducer and state
requirements.
