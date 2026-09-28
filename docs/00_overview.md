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
[ADR-0004](adr/0004-allow-explicit-core-dependencies.md). The implemented closed direct allowlist
permits only nlohmann/json, Boost.UUID, and Boost.Hash2 behind Sitometron-owned public types. The
private synchronization facilities approved by
[ADR-0003](adr/0003-define-single-state-writer-ingress-contract.md) remain inside the core
implementation and do not leak into public APIs. The active Linux and Windows `NFR-005` checks
mechanically enforce these boundaries. HTTP, persistence, process, hardware-topology, Sitos, Zenoh,
Python, logging, and other I/O/framework dependencies remain in adapters composed by `sitometrond`.

See [the architecture](02_architecture.md) and
[the detailed dependency boundaries](architecture/boundaries.md).

## 4. Development phases

| Phase | Milestone | Result |
|---|---|---|
| 0A | `v0.1 / P0A Bootstrap` | Repository, core contracts, deterministic fake-driven lifecycle |
| 0B | `v0.1 / P0B Durability` | Durable JobJournal foundation and a qualified logger |

Later work (external REST and admission, the Worker protocol and local process supervision,
resources and scheduling, Sitos integration, Artifact REST, and cross-platform qualification) is
planned one working increment at a time. A Gate is created only when a contract must be frozen; see
[the development workflow](development_workflow.md#9-phase-and-milestone-gates) and
[the Issue breakdown](07_issue_breakdown.md).

## 5. Current status

Phase 0A closed on 2026-09-24 ([Gate #1](https://github.com/tetsuh/sitometron/issues/1)). The
dependency boundary, pure Job reducer, lifecycle capability ports and fakes, bounded private single
writer, complete logical JobJournal envelope and ordering, and the fake-driven lifecycle and
adverse/race qualification are implemented. Phase 0B is active under
[Gate #50](https://github.com/tetsuh/sitometron/issues/50): Accepted
[ADR-0006](adr/0006-define-physical-jobjournal-durability-contract.md) makes the physical JobJournal
contract normative. The record codec (Issue #57), the durable segment writer (Issue #59), and
startup validation and replay (Issue #61) are implemented; starting the writer from the replayed
state and pruning are Planned. A non-normative Linux walking skeleton under
`spike/` exercises the core end to end and is not a product component. Production adapters remain
Planned under their owners in the [Contract Registry](08_contract_registry.md). No production API or
compatibility guarantee exists.
