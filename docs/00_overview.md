# Sitometron overview

## 1. Purpose

Sitometron is a cross-platform control plane for admitting, scheduling, supervising, and observing
trusted local compute applications. An Application is opaque to Sitometron: the Application owns its
compute graph, parameter interpretation, input access, and compute implementation.

## 2. Responsibilities

Sitometron owns:

- Admission and Job creation;
- deterministic Job lifecycle and external controls;
- bounded scheduling and resource reservation;
- local Worker process supervision;
- Journaled audit and recovery decisions;
- the external REST control plane;
- application registration and launch qualification;
- lifecycle coordination with Sitos;
- reconstructed-output Artifact access in later Phases.

## 3. Boundaries

Sitometron does not construct compute graphs, interpret application parameters, move tensor or raw
input payloads through its Worker-control protocol, or execute application algorithms in its core.
Trusted Applications run in separate Worker processes.

`sitometron_core` is a C++20 dependency-minimal domain library under Accepted
[ADR-0004](adr/0004-allow-explicit-core-dependencies.md). Its closed direct allowlist permits only
nlohmann/json, Boost.UUID, and Boost.Hash2 behind Sitometron-owned public types. HTTP, persistence,
process, hardware-topology, Sitos, Zenoh, Python, logging, and other I/O/framework dependencies
remain in adapters composed by `sitometrond`.

The current implementation remains standard-library-only until
[dependency-integration Issue #17](https://github.com/tetsuh/sitometron/issues/17) activates the
Accepted boundary and its Linux/Windows checks.

See [the architecture](02_architecture.md) and
[the detailed dependency boundaries](architecture/boundaries.md).

## 4. Development phases

| Phase | Milestone | Result |
|---|---|---|
| 0A | `v0.1 / P0A Bootstrap` | Repository, core contracts, deterministic fake-driven lifecycle |
| 0B | `v0.1 / P0B Durability` | Qualified logger and durable JobJournal foundation |
| 1 | `v0.1 / P1 REST & Admission` | External REST, Admission, Application Registry |
| 2 | `v0.1 / P2 Local Worker` | Worker protocol and local process supervision |
| 3 | `v0.1 / P3 Resources` | Topology, scheduling, and resource reservations |
| 4 | `v0.1 / P4 Sitos Integration` | Sitos Session, parameter, and durable-buffer integration |
| 5 | `v0.1 / P5 Artifact REST` | Reconstructed-output Artifact REST |
| 6 | `v0.1 / P6 Qualification` | Cross-platform, fault, security, and release qualification |

Every Phase has one Gate Issue. See [the Issue breakdown](07_issue_breakdown.md).

## 5. Current status

Phase 0A is active. The bootstrap build and repository workflow exist, but no production API or
compatibility guarantee exists. Runtime contracts remain Planned until their owning Issue and ADR
make them normative in the [Contract Registry](08_contract_registry.md).
