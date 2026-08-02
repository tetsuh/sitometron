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
| `NFR-001` | MUST | Support Linux with GCC and Windows with MSVC using C++20. | [ADR-0004](adr/0004-allow-explicit-core-dependencies.md) |
| `NFR-003` | MUST | Use CMake 3.28 or later with Ninja presets and reject in-source builds. | [ADR-0004](adr/0004-allow-explicit-core-dependencies.md) |
| `NFR-004` | MUST | Keep unit tests deterministic and avoid real sleeps when a fake clock or explicit event can express the behavior. | [ADR-0004](adr/0004-allow-explicit-core-dependencies.md) |
| `NFR-005` | MUST | Keep `sitometron_core` dependency-minimal: permit only C++ standard headers named by the reviewed standard-header allowlist and third-party dependencies named by the accepted core dependency allowlist and pinned manifest, expose Sitometron-owned public types, treat transitive packages as opaque prerequisites, and mechanically reject unapproved standard headers, direct targets, dependency includes, and public dependency leakage. | [ADR-0004](adr/0004-allow-explicit-core-dependencies.md) under [Issue #15](https://github.com/tetsuh/sitometron/issues/15) |

## 4. Superseded requirements

| ID | Former level | Requirement | Supersession |
|---|---|---|---|
| `NFR-002` | MUST | Keep `sitometron_core` limited to the C++ standard library. | Superseded by `NFR-005` and [ADR-0004](adr/0004-allow-explicit-core-dependencies.md); the legacy implementation guard remains active until [Issue #17](https://github.com/tetsuh/sitometron/issues/17). |

## 5. Normative Phase 0A Job requirements

Accepted ADR-0002 makes these requirements binding on implementation.

| ID | Level | Requirement | Authority |
|---|---|---|---|
| `JOB-001` | MUST | Use the closed Job state set and terminal classification defined by the exhaustive core contract. | [ADR-0002](adr/0002-define-core-job-reducer-contract.md) |
| `JOB-002` | MUST | Decide every entity-position/event and entity-position/command pair as transition, audit, late audit, or stable rejection using a pure reducer. | [ADR-0002](adr/0002-define-core-job-reducer-contract.md) |
| `JOB-003` | MUST | Latch the first accepted stop or failure reason and permit later forced-stop escalation without changing that reason. | [ADR-0002](adr/0002-define-core-job-reducer-contract.md) |
| `JOB-004` | MUST | Treat successful Worker completion as a candidate and apply a terminal state only after a matching terminal Journal event is synced. | [ADR-0002](adr/0002-define-core-job-reducer-contract.md) |
| `JOB-005` | MUST | Keep preparation, execution, cooperative-stop, and process-exit-confirmation timeout behavior deterministic across `stopping`, `finalizing`, and terminal cleanup. | [ADR-0002](adr/0002-define-core-job-reducer-contract.md) |
| `JOB-006` | MUST | Keep terminal state and reason immutable while allowing checked process-exit, completion-mode, resource-release, cleanup, and late-Worker audit facts. | [ADR-0002](adr/0002-define-core-job-reducer-contract.md) |
| `JOB-007` | MUST | Resolve competing controls and Worker or process facts by single-writer acceptance order without scheduler timing or real sleeps. | [ADR-0002](adr/0002-define-core-job-reducer-contract.md) |
| `JRN-001` | MUST | Use the ADR-0002 minimum logical Job-event envelope, closed event kinds, payload discriminators, and global sequence ordering. | [ADR-0002](adr/0002-define-core-job-reducer-contract.md) |
| `JRN-002` | MUST | Append and disk-sync each accepted event before reducer apply, response, acknowledgment, or external post-sync effect. | [ADR-0002](adr/0002-define-core-job-reducer-contract.md) |
| `JRN-003` | MUST | Append no rejected input and fail closed without inferring a state transition when append or sync fails or has unknown outcome. | [ADR-0002](adr/0002-define-core-job-reducer-contract.md) |

## 6. Planned Phase 0A ingress requirements

> **Planned, not yet normative:** Proposed ADR-0003 under Issue #10 owns these requirements.
> Implementers must not treat this outline as a finalized contract.

| ID | Level | Requirement | Authority |
|---|---|---|---|
| `JOB-008` | MUST | Bound ingress through one single-writer FIFO with explicit admission linearization, a fixed resident Job population, critical-reserve isolation, delivery-identity-safe coalescing, and fail-closed capacity invariants; retain each successfully created Phase 0A Job snapshot until separate Normative deletion/retention authority exists. | Proposed [ADR-0003](adr/0003-define-single-state-writer-ingress-contract.md) under [Issue #10](https://github.com/tetsuh/sitometron/issues/10) |
| `OPS-001` | MUST | Close admission, latch persistence/readiness failure, quiesce producers and callbacks, and destroy writer state only in the bounded ingress/shutdown order defined for Phase 0A. | Proposed [ADR-0003](adr/0003-define-single-state-writer-ingress-contract.md) under [Issue #10](https://github.com/tetsuh/sitometron/issues/10) |

## 7. Planned domains

> **Planned, not yet normative:** The owning Phase Issues and ADRs own these mechanisms.
> Implementers must not treat this outline as a finalized contract.

The remaining `ADM`, `JRN`, `WRK`, `RES`, `APP`, `PAR`, `ART`, `SEC`, and `OPS` requirements are added
by their owning design Issues before implementation. Issue #3 owns the first `JOB` reducer and
logical `JRN` requirements; Phase 0B owns physical JobJournal durability requirements.
